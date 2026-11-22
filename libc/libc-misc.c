#include <errno.h>
#include <locale.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* System properties. Android's init owns them and every process reads the same values. Here the host
 * runs no property service: the framework side (ATL's SystemProperties) publishes the ones native code
 * has to agree with it on (ro.build.version.*) through btl_system_property_set(), and native code
 * reads them with __system_property_get() / __system_property_find() + __system_property_read()
 * like on a device. Properties nobody published are unset: get() gives "" and 0, as in bionic. */
#define PROP_NAME_MAX  32
#define PROP_VALUE_MAX 92

struct stored_property {
	char *name;
	char value[PROP_VALUE_MAX];
};

#define MAX_STORED_PROPERTIES 16
static struct stored_property stored_properties[MAX_STORED_PROPERTIES];
static pthread_mutex_t stored_properties_lock = PTHREAD_MUTEX_INITIALIZER;

// the lock must be held
static struct stored_property *find_stored_property(const char *name)
{
	for (int i = 0; i < MAX_STORED_PROPERTIES && stored_properties[i].name; i++) {
		if (!strcmp(stored_properties[i].name, name))
			return &stored_properties[i];
	}
	return NULL;
}

// not part of bionic: called by the host (ATL) to publish a property; false when the table is full
bool btl_system_property_set(const char *name, const char *value)
{
	bool ok = true;

	pthread_mutex_lock(&stored_properties_lock);
	struct stored_property *prop = find_stored_property(name);
	for (int i = 0; !prop && i < MAX_STORED_PROPERTIES; i++) {
		if (!stored_properties[i].name) {
			stored_properties[i].name = strdup(name);
			prop = &stored_properties[i];
		}
	}
	if (prop) {
		strncpy(prop->value, value, PROP_VALUE_MAX - 1);
		prop->value[PROP_VALUE_MAX - 1] = 0;
	} else {
		ok = false;
	}
	pthread_mutex_unlock(&stored_properties_lock);
	return ok;
}

static int get_stored_property(const char *name, char *value)
{
	int len = 0;

	pthread_mutex_lock(&stored_properties_lock);
	struct stored_property *prop = find_stored_property(name);
	if (prop) {
		strcpy(value, prop->value);
		len = strlen(value);
	} else {
		*value = 0;
	}
	pthread_mutex_unlock(&stored_properties_lock);
	return len;
}

int __system_property_get(const char *name, char *value)
{
	return get_stored_property(name, value);
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

// this seems to not be a stable ABI, so hopefully nobody treats it as such
// FIXME: move this out into it's own file whenever we get to implementing it more properly
struct prop_info {
	const char *name;
};

const struct prop_info *bionic___system_property_find(const char *name)
{
	struct prop_info *ret = malloc(sizeof(struct prop_info));
	ret->name = strdup(name);

	return ret;
}

int bionic___system_property_read(const struct prop_info *prop_info, char *name, char *value)
{
	if (name)
		strncpy(name, prop_info->name, PROP_NAME_MAX);

	if (!value)
		return 0;

	int len = get_stored_property(prop_info->name, value);
	if (!len && strcmp(prop_info->name, "ro.build.fingerprint")) // there is no good reason that apps should need the fingerprint, so it stays empty
		printf("__system_property_find: >%s< not handled yet\n", prop_info->name);

	return len;
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
