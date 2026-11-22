#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* System properties. Android's init owns them and every process reads the same values. Here the host
 * runs no property service: the framework side (ATL's SystemProperties) publishes the ones native code
 * has to agree with it on (ro.build.version.*) through btl_system_property_set(), and native code
 * reads them with __system_property_get() / __system_property_find() + __system_property_read()
 * like on a device. Properties nobody published are unset: find() gives NULL, get() gives "" and 0,
 * as in bionic.
 *
 * The API follows bionic's system_property_api.cpp / system_properties.cpp (Android 16): every
 * prop_info carries a serial (value length in the top 8 bits, bumped by 2 per update, bit 0 marks
 * an update in progress) and the area has a global serial that every add/update bumps; waiters
 * sleep on these serials with futexes. __system_property_set() is a request to init's property
 * service, which does not exist here, so it fails like a rejected request. */
#define PROP_NAME_MAX  32
#define PROP_VALUE_MAX 92
#define SERIAL_VALUE_LEN(serial) ((serial) >> 24)

/* bionic's prop_info also starts with the serial; the rest is private to libc */
struct prop_info {
	_Atomic uint32_t serial;
	char value[PROP_VALUE_MAX];
	char *name;
};

#define MAX_PROPERTIES 64
static struct prop_info properties[MAX_PROPERTIES];
static _Atomic unsigned property_count;
static _Atomic uint32_t area_serial;
/* writers serialize on the lock; readers take it to copy a value that is being updated */
static pthread_mutex_t properties_lock = PTHREAD_MUTEX_INITIALIZER;

static void futex_wake_all(_Atomic uint32_t *addr)
{
	syscall(SYS_futex, addr, FUTEX_WAKE, INT32_MAX, NULL, NULL, 0);
}

static void bump_area_serial(void)
{
	atomic_fetch_add_explicit(&area_serial, 1, memory_order_release);
	futex_wake_all(&area_serial);
}

const struct prop_info *__system_property_find(const char *name)
{
	unsigned count = atomic_load_explicit(&property_count, memory_order_acquire);
	for (unsigned i = 0; i < count; i++) {
		if (!strcmp(properties[i].name, name))
			return &properties[i];
	}
	return NULL;
}

int __system_property_update(struct prop_info *pi, const char *value, unsigned int len)
{
	if (len >= PROP_VALUE_MAX)
		return -1;

	pthread_mutex_lock(&properties_lock);
	uint32_t serial = atomic_load_explicit(&pi->serial, memory_order_relaxed);
	atomic_store_explicit(&pi->serial, serial | 1, memory_order_relaxed);
	memcpy(pi->value, value, len);
	pi->value[len] = 0;
	atomic_store_explicit(&pi->serial, (len << 24) | ((serial + 1) & 0xffffff), memory_order_release);
	pthread_mutex_unlock(&properties_lock);
	futex_wake_all(&pi->serial);
	bump_area_serial();
	return 0;
}

int __system_property_add(const char *name, unsigned int namelen, const char *value, unsigned int valuelen)
{
	if (namelen < 1 || valuelen >= PROP_VALUE_MAX)
		return -1;

	int ret = 0;
	pthread_mutex_lock(&properties_lock);
	char *key = strndup(name, namelen);
	/* as bionic's prop_area::add(): an existing property keeps its value */
	if (!__system_property_find(key)) {
		unsigned count = atomic_load_explicit(&property_count, memory_order_relaxed);
		if (count < MAX_PROPERTIES) {
			struct prop_info *pi = &properties[count];
			pi->name = key;
			key = NULL;
			memcpy(pi->value, value, valuelen);
			pi->value[valuelen] = 0;
			atomic_store_explicit(&pi->serial, valuelen << 24, memory_order_relaxed);
			atomic_store_explicit(&property_count, count + 1, memory_order_release);
		} else {
			ret = -1;
		}
	}
	free(key);
	pthread_mutex_unlock(&properties_lock);
	if (ret == 0)
		bump_area_serial();
	return ret;
}

// not part of bionic: called by the host (ATL) to publish a property; false when the table is full
bool btl_system_property_set(const char *name, const char *value)
{
	size_t len = strnlen(value, PROP_VALUE_MAX - 1);
	struct prop_info *pi = (struct prop_info *)__system_property_find(name);
	if (pi)
		return __system_property_update(pi, value, len) == 0;
	return __system_property_add(name, strlen(name), value, len) == 0;
}

int __system_property_read(const struct prop_info *pi, char *name, char *value)
{
	pthread_mutex_lock(&properties_lock);
	size_t len = strlen(pi->value);
	if (value)
		memcpy(value, pi->value, len + 1);
	pthread_mutex_unlock(&properties_lock);
	if (name) {
		/* legacy interface: names are truncated to PROP_NAME_MAX */
		strncpy(name, pi->name, PROP_NAME_MAX - 1);
		name[PROP_NAME_MAX - 1] = 0;
	}
	return len;
}

void __system_property_read_callback(const struct prop_info *pi,
                                     void (*callback)(void *cookie, const char *name, const char *value, uint32_t serial),
                                     void *cookie)
{
	char value[PROP_VALUE_MAX];
	pthread_mutex_lock(&properties_lock);
	uint32_t serial = atomic_load_explicit(&pi->serial, memory_order_relaxed);
	memcpy(value, pi->value, SERIAL_VALUE_LEN(serial) + 1);
	pthread_mutex_unlock(&properties_lock);
	callback(cookie, pi->name, value, serial);
}

int __system_property_get(const char *name, char *value)
{
	const struct prop_info *pi = __system_property_find(name);
	if (!pi) {
		*value = 0;
		return 0;
	}
	return __system_property_read(pi, NULL, value);
}

uint32_t __system_property_serial(const struct prop_info *pi)
{
	return atomic_load_explicit(&pi->serial, memory_order_acquire);
}

uint32_t __system_property_area_serial(void)
{
	return atomic_load_explicit(&area_serial, memory_order_acquire);
}

int __system_property_foreach(void (*propfn)(const struct prop_info *pi, void *cookie), void *cookie)
{
	unsigned count = atomic_load_explicit(&property_count, memory_order_acquire);
	for (unsigned i = 0; i < count; i++)
		propfn(&properties[i], cookie);
	return 0;
}

const struct prop_info *__system_property_find_nth(unsigned n)
{
	unsigned count = atomic_load_explicit(&property_count, memory_order_acquire);
	return n < count ? &properties[n] : NULL;
}

bool __system_property_wait(const struct prop_info *pi, uint32_t old_serial, uint32_t *new_serial_ptr,
                            const struct timespec *relative_timeout)
{
	_Atomic uint32_t *serial_ptr = pi ? (_Atomic uint32_t *)&pi->serial : &area_serial;
	uint32_t new_serial;
	do {
		if (syscall(SYS_futex, serial_ptr, FUTEX_WAIT, old_serial, relative_timeout, NULL, 0) != 0 &&
		    errno == ETIMEDOUT)
			return false;
		new_serial = atomic_load_explicit(serial_ptr, memory_order_acquire);
	} while (new_serial == old_serial);
	*new_serial_ptr = new_serial;
	return true;
}

uint32_t __system_property_wait_any(uint32_t old_serial)
{
	uint32_t new_serial;
	__system_property_wait(NULL, old_serial, &new_serial, NULL);
	return new_serial;
}

/* the property area is this table, there is nothing to map */
int __system_properties_init(void)
{
	return 0;
}

/* init creates the property area files; this process is not init */
int __system_property_area_init(void)
{
	return -1;
}

int __system_property_set_filename(const char *filename)
{
	(void)filename;
	return -1;
}

/* a request to init's property service; there is none, so it fails like a rejected request */
int __system_property_set(const char *key, const char *value)
{
	(void)value;
	if (!key)
		return -1;
	errno = ENOENT; /* connect() to /dev/socket/property_service */
	return -1;
}
