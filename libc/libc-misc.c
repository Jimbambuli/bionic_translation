#include <errno.h>
#include <locale.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int __system_property_get(const char *name, char *value);
#define PROP_VALUE_MAX 92

// bionic's api-level.h: the inline before API 29 and libc's function since do the same.
// Not on x86/x86_64: there libc exporting it says "Android 10 TLS layout". Go's cgo runtime
// (runtime/cgo/gcc_android.c, inittls(): "android_get_device_api_level is introduced in Android Q,
// so its mere presence is enough") then keeps g in TLS_SLOT_APP, (tp + 2), which on glibc is
// tcbhead_t.self (see main_executable/bionic_compat.c), and the first cgo callback on a thread Go
// did not create reads glibc's self pointer as its g (Feeder: SIGSEGV in runtime.cgocallback).
// Without it Go looks for a pthread key slot instead (libpthread.c, low keys). On x86 the host program
// exports it for libraries that link against it (ATL: src/main-executable/bionic_compat.c), outside libc's scope.
#if !defined(__i386__) && !defined(__x86_64__)
int android_get_device_api_level(void)
{
	char value[PROP_VALUE_MAX] = { 0 };
	if (__system_property_get("ro.build.version.sdk", value) < 1)
		return -1;
	int api_level = atoi(value);
	return (api_level > 0) ? api_level : -1;
}
#endif

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

/* BSD's getprogname() as bionic has it: the name the process was started with, without the path */
const char *bionic_getprogname(void)
{
	return program_invocation_short_name;
}

/* android_mallopt (bionic/libc/bionic/android_mallopt.cpp, Android 16) on top of the host's malloc:
 * there is no malloc debug, heapprofd, GWP-ASan, MTE or allocation limit, so the queries answer
 * "off" and the requests that need one of them fail with ENOTSUP, as bionic does without them */
enum {
	M_INIT_ZYGOTE_CHILD_PROFILING = 1,
	M_RESET_HOOKS = 2,
	M_SET_ALLOCATION_LIMIT_BYTES = 3,
	M_SET_ZYGOTE_CHILD = 4,
	M_WRITE_MALLOC_LEAK_INFO_TO_FILE = 5,
	M_GET_MALLOC_LEAK_INFO = 6,
	M_FREE_MALLOC_LEAK_INFO = 7,
	M_GET_PROCESS_PROFILEABLE = 9,
	M_INITIALIZE_GWP_ASAN = 10,
	M_MEMTAG_STACK_IS_ON = 11,
	M_GET_DECAY_TIME_ENABLED = 12,
};

typedef struct {
	uint8_t *buffer;
	size_t overall_size;
	size_t info_size;
	size_t total_memory;
	size_t backtrace_size;
} android_mallopt_leak_info_t;

typedef struct {
	const char *program_name;
	int desire;
} android_mallopt_gwp_asan_options_t;

static bool zygote_child;
static _Atomic bool zygote_child_profileable;

bool android_mallopt(int opcode, void *arg, size_t arg_size)
{
	switch (opcode) {
		case M_GET_DECAY_TIME_ENABLED:
		case M_MEMTAG_STACK_IS_ON:
			if (!arg || arg_size != sizeof(bool)) {
				errno = EINVAL;
				return false;
			}
			*(bool *)arg = false;
			return true;
		case M_INITIALIZE_GWP_ASAN:
			if (!arg || arg_size != sizeof(android_mallopt_gwp_asan_options_t)) {
				errno = EINVAL;
				return false;
			}
			return false; /* GWP-ASan was not enabled */
		case M_SET_ZYGOTE_CHILD:
			if (arg || arg_size != 0) {
				errno = EINVAL;
				return false;
			}
			zygote_child = true;
			return true;
		case M_INIT_ZYGOTE_CHILD_PROFILING:
			if (arg || arg_size != 0) {
				errno = EINVAL;
				return false;
			}
			zygote_child_profileable = true;
			return true;
		case M_GET_PROCESS_PROFILEABLE:
			if (!arg || arg_size != sizeof(bool)) {
				errno = EINVAL;
				return false;
			}
			*(bool *)arg = !zygote_child || zygote_child_profileable;
			return true;
		case M_WRITE_MALLOC_LEAK_INFO_TO_FILE:
			if (!arg || arg_size != sizeof(FILE *)) {
				errno = EINVAL;
				return false;
			}
			fprintf((FILE *)arg, "Native heap dump not available. To enable, run these commands (requires root):\n"
			                     "# adb shell stop\n"
			                     "# adb shell setprop libc.debug.malloc.options backtrace\n"
			                     "# adb shell start\n");
			errno = ENOTSUP;
			return false;
		case M_GET_MALLOC_LEAK_INFO:
		case M_FREE_MALLOC_LEAK_INFO:
			if (!arg || arg_size != sizeof(android_mallopt_leak_info_t)) {
				errno = EINVAL;
				return false;
			}
			errno = ENOTSUP;
			return false;
		case M_SET_ALLOCATION_LIMIT_BYTES: /* needs bionic's malloc dispatch */
		default:
			errno = ENOTSUP;
			return false;
	}
}

/* crash details (bionic/libc/bionic/android_crash_detail.cpp) are read by debuggerd into the
 * tombstone; there is no debuggerd, but the handles behave the same: name and data are not copied */
typedef struct crash_detail_t {
	const char *name;
	size_t name_size;
	const char *data;
	size_t data_size;
} crash_detail_t;

crash_detail_t *android_crash_detail_register(const void *name, size_t name_size, const void *data, size_t data_size)
{
	crash_detail_t *detail = malloc(sizeof(*detail));
	if (!detail)
		return NULL;
	detail->name = name;
	detail->name_size = name_size;
	detail->data = data;
	detail->data_size = data_size;
	return detail;
}

void android_crash_detail_unregister(crash_detail_t *crash_detail)
{
	free(crash_detail);
}

void android_crash_detail_replace_data(crash_detail_t *crash_detail, const void *data, size_t data_size)
{
	crash_detail->data = data;
	crash_detail->data_size = data_size;
}

void android_crash_detail_replace_name(crash_detail_t *crash_detail, const void *name, size_t name_size)
{
	crash_detail->name = name;
	crash_detail->name_size = name_size;
}

/* bionic's heap inspection API (malloc_common.cpp: malloc_disable, malloc_enable, malloc_iterate), used by heap
 * walkers such as libmemunreachable. The host's malloc (glibc) can neither be locked from outside nor walked, so
 * disable/enable have nothing to do and malloc_iterate fails like bionic without such an allocator. */
void malloc_disable(void)
{
}

void malloc_enable(void)
{
}

int malloc_iterate(uintptr_t base, size_t size, void (*callback)(uintptr_t base, size_t size, void *arg), void *arg)
{
	(void)base;
	(void)size;
	(void)callback;
	(void)arg;
	errno = ENOTSUP;
	return -1;
}
