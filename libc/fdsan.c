#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>

/* fdsan, the file descriptor ownership checker of bionic (libc/bionic/fdsan.cpp, Android 16).
 * Owners tag the fds they own; closing or re-tagging an fd with the wrong tag is reported at the
 * process' error level. Differences to a device, both because the host's libc is not bionic:
 * - close() of glibc code does not go through fdsan, so only android_fdsan_close_with_tag() checks.
 * - there is no debuggerd: warnings are logged to stderr instead of raising the debugger signal. */

enum android_fdsan_error_level {
	ANDROID_FDSAN_ERROR_LEVEL_DISABLED,
	ANDROID_FDSAN_ERROR_LEVEL_WARN_ONCE,
	ANDROID_FDSAN_ERROR_LEVEL_WARN_ALWAYS,
	ANDROID_FDSAN_ERROR_LEVEL_FATAL,
};

enum android_fdsan_owner_type {
	ANDROID_FDSAN_OWNER_TYPE_GENERIC_00 = 0,
	ANDROID_FDSAN_OWNER_TYPE_GENERIC_FF = 255,
	ANDROID_FDSAN_OWNER_TYPE_FILE = 1,
	ANDROID_FDSAN_OWNER_TYPE_DIR = 2,
	ANDROID_FDSAN_OWNER_TYPE_UNIQUE_FD = 3,
	ANDROID_FDSAN_OWNER_TYPE_SQLITE = 4,
	ANDROID_FDSAN_OWNER_TYPE_FILEINPUTSTREAM = 5,
	ANDROID_FDSAN_OWNER_TYPE_FILEOUTPUTSTREAM = 6,
	ANDROID_FDSAN_OWNER_TYPE_RANDOMACCESSFILE = 7,
	ANDROID_FDSAN_OWNER_TYPE_PARCELFILEDESCRIPTOR = 8,
	ANDROID_FDSAN_OWNER_TYPE_ART_FDFILE = 9,
	ANDROID_FDSAN_OWNER_TYPE_DATAGRAMSOCKETIMPL = 10,
	ANDROID_FDSAN_OWNER_TYPE_SOCKETIMPL = 11,
	ANDROID_FDSAN_OWNER_TYPE_ZIPARCHIVE = 12,
	ANDROID_FDSAN_OWNER_TYPE_NATIVE_HANDLE = 13,
	ANDROID_FDSAN_OWNER_TYPE_PARCEL = 14,
};

/* bionic keeps 128 entries inline and the rest in an overflow table sized to RLIMIT_NOFILE;
 * here the table grows in chunks, allocated on first use */
#define FD_CHUNK 1024
#define FD_CHUNKS 1024
static _Atomic(_Atomic uint64_t *) fd_chunks[FD_CHUNKS];
static _Atomic int error_level = ANDROID_FDSAN_ERROR_LEVEL_DISABLED;

static _Atomic uint64_t *get_fd_entry(int fd)
{
	if (fd < 0 || fd >= FD_CHUNK * FD_CHUNKS)
		return NULL;
	_Atomic(_Atomic uint64_t *) *slot = &fd_chunks[fd / FD_CHUNK];
	_Atomic uint64_t *chunk = atomic_load(slot);
	if (!chunk) {
		_Atomic uint64_t *fresh = calloc(FD_CHUNK, sizeof(*fresh));
		if (!fresh)
			return NULL;
		if (atomic_compare_exchange_strong(slot, &chunk, fresh))
			chunk = fresh;
		else
			free(fresh);
	}
	return &chunk[fd % FD_CHUNK];
}

static void fdsan_error(const char *fmt, ...)
{
	int level = atomic_load(&error_level);
	if (level == ANDROID_FDSAN_ERROR_LEVEL_DISABLED)
		return;

	va_list va;
	va_start(va, fmt);
	fprintf(stderr, "fdsan: ");
	vfprintf(stderr, fmt, va);
	fprintf(stderr, "\n");
	va_end(va);

	switch (level) {
		case ANDROID_FDSAN_ERROR_LEVEL_WARN_ONCE:
			atomic_compare_exchange_strong(&error_level, &level, ANDROID_FDSAN_ERROR_LEVEL_DISABLED);
			break;
		case ANDROID_FDSAN_ERROR_LEVEL_FATAL:
			abort();
	}
}

uint64_t android_fdsan_create_owner_tag(enum android_fdsan_owner_type type, uint64_t tag)
{
	if (tag == 0)
		return 0;
	if ((type & 0xff) != type) {
		fprintf(stderr, "fdsan: invalid android_fdsan_owner_type value: %x\n", type);
		abort();
	}
	uint64_t mask = ((uint64_t)1 << 56) - 1;
	return ((uint64_t)type << 56) | (tag & mask);
}

const char *android_fdsan_get_tag_type(uint64_t tag)
{
	switch (tag >> 56) {
		case ANDROID_FDSAN_OWNER_TYPE_FILE: return "FILE*";
		case ANDROID_FDSAN_OWNER_TYPE_DIR: return "DIR*";
		case ANDROID_FDSAN_OWNER_TYPE_UNIQUE_FD: return "unique_fd";
		case ANDROID_FDSAN_OWNER_TYPE_FILEINPUTSTREAM: return "FileInputStream";
		case ANDROID_FDSAN_OWNER_TYPE_FILEOUTPUTSTREAM: return "FileOutputStream";
		case ANDROID_FDSAN_OWNER_TYPE_RANDOMACCESSFILE: return "RandomAccessFile";
		case ANDROID_FDSAN_OWNER_TYPE_PARCELFILEDESCRIPTOR: return "ParcelFileDescriptor";
		case ANDROID_FDSAN_OWNER_TYPE_SQLITE: return "sqlite";
		case ANDROID_FDSAN_OWNER_TYPE_ART_FDFILE: return "ART FdFile";
		case ANDROID_FDSAN_OWNER_TYPE_DATAGRAMSOCKETIMPL: return "DatagramSocketImpl";
		case ANDROID_FDSAN_OWNER_TYPE_SOCKETIMPL: return "SocketImpl";
		case ANDROID_FDSAN_OWNER_TYPE_ZIPARCHIVE: return "ZipArchive";
		case ANDROID_FDSAN_OWNER_TYPE_NATIVE_HANDLE: return "native_handle_t";
		case ANDROID_FDSAN_OWNER_TYPE_PARCEL: return "Parcel";
		case ANDROID_FDSAN_OWNER_TYPE_GENERIC_FF:
			if ((tag >> 48) == (1 << 16) - 1)
				return "native object of unknown type";
			return "Java object of unknown type";
		case ANDROID_FDSAN_OWNER_TYPE_GENERIC_00:
		default:
			return "native object of unknown type";
	}
}

uint64_t android_fdsan_get_tag_value(uint64_t tag)
{
	/* lop off the type byte and sign-extend */
	return (uint64_t)((int64_t)(tag << 8) >> 8);
}

/* bionic reports a mismatch between the expected tag and the one found in four ways */
static void report_mismatch(const char *what, int fd, uint64_t expected_tag, uint64_t tag)
{
	if (expected_tag && tag)
		fdsan_error("%s %d, expected to be owned by %s 0x%" PRIx64 ", actually owned by %s 0x%" PRIx64, what, fd,
		            android_fdsan_get_tag_type(expected_tag), android_fdsan_get_tag_value(expected_tag),
		            android_fdsan_get_tag_type(tag), android_fdsan_get_tag_value(tag));
	else if (expected_tag)
		fdsan_error("%s %d, expected to be owned by %s 0x%" PRIx64 ", actually unowned", what, fd,
		            android_fdsan_get_tag_type(expected_tag), android_fdsan_get_tag_value(expected_tag));
	else
		fdsan_error("%s %d, expected to be unowned, actually owned by %s 0x%" PRIx64, what, fd,
		            android_fdsan_get_tag_type(tag), android_fdsan_get_tag_value(tag));
}

int android_fdsan_close_with_tag(int fd, uint64_t expected_tag)
{
	_Atomic uint64_t *entry = get_fd_entry(fd);
	if (!entry)
		return close(fd);

	uint64_t tag = expected_tag;
	if (!atomic_compare_exchange_strong(entry, &tag, 0))
		report_mismatch("attempted to close file descriptor", fd, expected_tag, tag);

	int rc = close(fd);
	if (expected_tag && rc == -1 && errno == EBADF)
		fdsan_error("double-close of file descriptor %d detected", fd);
	return rc;
}

uint64_t android_fdsan_get_owner_tag(int fd)
{
	_Atomic uint64_t *entry = get_fd_entry(fd);
	return entry ? atomic_load(entry) : 0;
}

void android_fdsan_exchange_owner_tag(int fd, uint64_t expected_tag, uint64_t new_tag)
{
	_Atomic uint64_t *entry = get_fd_entry(fd);
	if (!entry)
		return;

	uint64_t tag = expected_tag;
	if (!atomic_compare_exchange_strong(entry, &tag, new_tag))
		report_mismatch("failed to exchange ownership of file descriptor", fd, expected_tag, tag);
}

enum android_fdsan_error_level android_fdsan_get_error_level(void)
{
	return atomic_load(&error_level);
}

enum android_fdsan_error_level android_fdsan_set_error_level(enum android_fdsan_error_level new_level)
{
	return atomic_exchange(&error_level, new_level);
}

int __system_property_get(const char *name, char *value);

enum android_fdsan_error_level android_fdsan_set_error_level_from_property(enum android_fdsan_error_level default_level)
{
	char value[92];
	if (__system_property_get("debug.fdsan", value) == 0)
		return android_fdsan_set_error_level(default_level);
	if (!strcasecmp(value, "1") || !strcasecmp(value, "fatal"))
		return android_fdsan_set_error_level(ANDROID_FDSAN_ERROR_LEVEL_FATAL);
	if (!strcasecmp(value, "warn"))
		return android_fdsan_set_error_level(ANDROID_FDSAN_ERROR_LEVEL_WARN_ALWAYS);
	if (!strcasecmp(value, "warn_once"))
		return android_fdsan_set_error_level(ANDROID_FDSAN_ERROR_LEVEL_WARN_ONCE);
	if (strcasecmp(value, "0"))
		fprintf(stderr, "libc: debug.fdsan set to unknown value '%s', disabling\n", value);
	return android_fdsan_set_error_level(default_level);
}
