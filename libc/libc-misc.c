#include <errno.h>
#include <linux/futex.h>
#include <locale.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* System properties. Android's init owns them and every process reads the same values. Here the host
 * runs no property service: the framework side (ATL's SystemProperties) publishes the ones native code
 * has to agree with it on (ro.build.version.*) through btl_system_property_set(), and native code
 * reads them with bionic's read API (sys/system_properties.h) like on a device. Properties nobody
 * published do not exist: find() gives NULL, get() gives "" and 0, foreach() does not see them.
 * As in bionic (system_properties.cpp), a prop_info stays where it is once added, its serial holds the
 * value length in the top 8 bits and a change counter below, and __system_property_wait() sleeps on the
 * serial with a futex. */
#define PROP_NAME_MAX  32
#define PROP_VALUE_MAX 92

#define SERIAL_VALUE_LEN(serial) ((serial) >> 24)

struct prop_info {
	char *name;
	char value[PROP_VALUE_MAX];
	_Atomic uint32_t serial;
};

#define MAX_STORED_PROPERTIES 16
static struct prop_info stored_properties[MAX_STORED_PROPERTIES];
static _Atomic uint32_t stored_properties_count;
static _Atomic uint32_t area_serial; // the serial of the whole property area: changes on every add and update
static pthread_mutex_t stored_properties_lock = PTHREAD_MUTEX_INITIALIZER;

static void futex_wake_all(_Atomic uint32_t *serial)
{
	syscall(SYS_futex, serial, FUTEX_WAKE, INT32_MAX, NULL, NULL, 0);
}

const struct prop_info *bionic___system_property_find(const char *name);

// not part of bionic: called by the host (ATL) to publish a property; false when the table is full
bool btl_system_property_set(const char *name, const char *value)
{
	bool ok = true;
	uint32_t len = strnlen(value, PROP_VALUE_MAX - 1);

	pthread_mutex_lock(&stored_properties_lock);
	struct prop_info *pi = (struct prop_info *)bionic___system_property_find(name);
	if (pi) {
		uint32_t serial = atomic_load(&pi->serial) | 1; // dirty while the value changes
		atomic_store(&pi->serial, serial);
		memcpy(pi->value, value, len);
		pi->value[len] = 0;
		atomic_store(&pi->serial, (len << 24) | ((serial + 1) & 0xffffff));
		futex_wake_all(&pi->serial);
	} else if (atomic_load(&stored_properties_count) < MAX_STORED_PROPERTIES) {
		pi = &stored_properties[atomic_load(&stored_properties_count)];
		pi->name = strdup(name);
		memcpy(pi->value, value, len);
		pi->value[len] = 0;
		atomic_store(&pi->serial, len << 24);
		atomic_fetch_add(&stored_properties_count, 1); // readers see the entry only once it is complete
	} else {
		ok = false;
	}
	if (ok) {
		atomic_fetch_add(&area_serial, 1);
		futex_wake_all(&area_serial);
	}
	pthread_mutex_unlock(&stored_properties_lock);
	return ok;
}

// the value and its serial, read again when a writer changed it meanwhile (ReadMutablePropertyValue)
static uint32_t read_property_value(const struct prop_info *pi, char *value)
{
	uint32_t new_serial = atomic_load(&pi->serial);
	uint32_t serial;
	do {
		serial = new_serial;
		pthread_mutex_lock(&stored_properties_lock);
		memcpy(value, pi->value, SERIAL_VALUE_LEN(serial) + 1);
		pthread_mutex_unlock(&stored_properties_lock);
		new_serial = atomic_load(&pi->serial);
	} while (serial != new_serial || (serial & 1));
	return serial;
}

const struct prop_info *bionic___system_property_find(const char *name)
{
	uint32_t count = atomic_load(&stored_properties_count);
	for (uint32_t i = 0; i < count; i++) {
		if (!strcmp(stored_properties[i].name, name))
			return &stored_properties[i];
	}
	return NULL;
}

int bionic___system_property_read(const struct prop_info *pi, char *name, char *value)
{
	uint32_t serial = read_property_value(pi, value);
	if (name) {
		strncpy(name, pi->name, PROP_NAME_MAX - 1);
		name[PROP_NAME_MAX - 1] = 0;
	}
	return SERIAL_VALUE_LEN(serial);
}

void bionic___system_property_read_callback(const struct prop_info *pi,
                                            void (*callback)(void *cookie, const char *name, const char *value, uint32_t serial),
                                            void *cookie)
{
	char value[PROP_VALUE_MAX];
	uint32_t serial = read_property_value(pi, value);
	callback(cookie, pi->name, value, serial);
}

int __system_property_get(const char *name, char *value)
{
	const struct prop_info *pi = bionic___system_property_find(name);
	if (pi)
		return bionic___system_property_read(pi, NULL, value);
	value[0] = 0;
	return 0;
}

int bionic___system_property_foreach(void (*propfn)(const struct prop_info *pi, void *cookie), void *cookie)
{
	uint32_t count = atomic_load(&stored_properties_count);
	for (uint32_t i = 0; i < count; i++)
		propfn(&stored_properties[i], cookie);
	return 0;
}

const struct prop_info *bionic___system_property_find_nth(unsigned n)
{
	return n < atomic_load(&stored_properties_count) ? &stored_properties[n] : NULL;
}

uint32_t bionic___system_property_serial(const struct prop_info *pi)
{
	return atomic_load(&pi->serial);
}

uint32_t bionic___system_property_area_serial(void)
{
	return atomic_load(&area_serial);
}

// pi NULL: wait for any property to change
bool bionic___system_property_wait(const struct prop_info *pi, uint32_t old_serial, uint32_t *new_serial_ptr,
                                   const struct timespec *relative_timeout)
{
	_Atomic uint32_t *serial_ptr = pi ? (_Atomic uint32_t *)&pi->serial : &area_serial;
	uint32_t new_serial;
	do {
		if (syscall(SYS_futex, serial_ptr, FUTEX_WAIT, old_serial, relative_timeout, NULL, 0) == -1 && errno == ETIMEDOUT)
			return false;
		new_serial = atomic_load(serial_ptr);
	} while (new_serial == old_serial);
	*new_serial_ptr = new_serial;
	return true;
}

uint32_t bionic___system_property_wait_any(uint32_t old_serial)
{
	uint32_t new_serial;
	bionic___system_property_wait(NULL, old_serial, &new_serial, NULL);
	return new_serial;
}

// bionic's api-level.h: the inline before API 29 and libc's function since do the same
int android_get_device_api_level(void)
{
	char value[PROP_VALUE_MAX] = { 0 };
	if (__system_property_get("ro.build.version.sdk", value) < 1)
		return -1;
	int api_level = atoi(value);
	return (api_level > 0) ? api_level : -1;
}

// actually misc stuff

void android_set_abort_message(const char *msg)
{
	printf("android_set_abort_message called: '%s'\n", msg);
	//	exit(1);
}

// setlocale is a bit special on bionic, try to mimic the behavior
char *bionic_setlocale(int category, const char *locale)
{
	if(!locale)
		return setlocale(category, NULL);

	// list of allowed locales from bionic
	if (!strcmp(locale, "") ||
	    !strcmp(locale, "C.UTF-8") ||
	    !strcmp(locale, "en_US.UTF-8")) {
		return setlocale(category, "C.UTF-8");
	} else if (!strcmp(locale, "C") ||
		   !strcmp(locale, "POSIX")) {
		return setlocale(category, "C");
	} else {
		errno = ENOENT;
		return NULL;
	}
}

/* Android apps run without HOME: the desktop's HOME is hidden from them. A HOME the app sets itself
 * (Firefox's GeckoLoader puts its files folder there, Gecko finds its profile under it) is theirs. */
static char *host_home;

__attribute__((constructor)) static void keep_host_home(void)
{
	const char *home = getenv("HOME");
	host_home = home ? strdup(home) : NULL;
}

char *bionic_getenv(const char *name) {
	if(!name) {
		printf("!!! NULL passed to %s\n", __func__);
	}

	if(!strcmp(name, "HOME")) {
		char *home = getenv("HOME");
		if (!home || (host_home && !strcmp(home, host_home)))
			return NULL;
		return home;
	}

	/* no point returning anything else, apps mix this with using hardcoded paths */
	if(!strcmp(name, "ANDROID_ROOT"))
		return "/system";

	return getenv(name);
}

/* __gnu_strerror_r emulates glibc's strerror_r.
 * to make sure we get that, we define _GNU_SOURCE.
 * musl doesn't have any way to get the glibc behavior. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

char* bionic___gnu_strerror_r(int error_number, char* buf, size_t buf_len) {
#ifdef __GLIBC__
	return strerror_r(error_number, buf, buf_len);
#else // musl, or something else POSIX compliant
	int saved_errno = errno;
	strerror_r(error_number, buf, buf_len);
	errno = saved_errno;
	return buf;
#endif
}
