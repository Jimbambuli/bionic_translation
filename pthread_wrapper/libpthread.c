#include <dlfcn.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <memory.h>
#include <pthread.h>
#include <semaphore.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <sys/mman.h>

// when __GLIBC__ (or some glibc specific symbol) is not defined, we're assuming musl; can't check to be sure because 🤡

#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP {{PTHREAD_MUTEX_RECURSIVE}}
#endif

#ifndef PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP {{PTHREAD_MUTEX_ERRORCHECK}}
#endif

typedef struct {
	union {
		struct {
			unsigned int count;
#ifdef __LP64__
			int __reserved[3];
#endif
		} bionic;
		sem_t *glibc;
	};
} bionic_sem_t;

typedef struct {
	union {
		struct {
			uint32_t flags;
			void* stack_base;
			size_t stack_size;
			size_t guard_size;
			int32_t sched_policy;
			int32_t sched_priority;
#ifdef __LP64__
			char __reserved[16];
#endif
		} bionic;
	};
} bionic_attr_t;

typedef struct {
	union {
#if defined(__LP64__)
		int32_t __private[10];
#else
		int32_t __private[1];
#endif
		pthread_mutex_t *glibc;
	};
} bionic_mutex_t;

typedef struct {
	union {
		long __private;
		pthread_mutexattr_t *glibc;
	};
} bionic_mutexattr_t;

typedef struct {
	union {
		#if defined(__LP64__)
		  int32_t __private[14];
		#else
		  int32_t __private[10];
		#endif
		pthread_rwlock_t *glibc;
	};
} bionic_rwlock_t;

static const struct {
	bionic_mutex_t bionic;
	pthread_mutex_t glibc;
} bionic_mutex_init_map[] = {
	{ .bionic = {{{ ((PTHREAD_MUTEX_NORMAL & 3) << 14) }}}, .glibc = PTHREAD_MUTEX_INITIALIZER },
	{ .bionic = {{{ ((PTHREAD_MUTEX_RECURSIVE & 3) << 14) }}}, .glibc = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP },
	{ .bionic = {{{ ((PTHREAD_MUTEX_ERRORCHECK & 3) << 14) }}}, .glibc = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP },
};

typedef struct {
	union {
#if defined(__LP64__)
		int32_t __private[12];
#else
		int32_t __private[1];
#endif
		pthread_cond_t *glibc;
	};
} bionic_cond_t;

typedef struct {
	union {
		long __private;
		pthread_condattr_t *glibc;
	};
} bionic_condattr_t;

typedef int bionic_key_t;
_Static_assert(sizeof(bionic_key_t) == sizeof(pthread_key_t), "bionic_key_t and pthread_key_t size mismatch");

typedef int bionic_once_t;
_Static_assert(sizeof(bionic_once_t) == sizeof(pthread_once_t), "bionic_once_t and pthread_once_t size mismatch");

typedef long bionic_pthread_t; // seems to be 32bit on 32bit musl, 64bit everywhere else, which is why long happens to work
_Static_assert(sizeof(bionic_pthread_t) == sizeof(pthread_t), "bionic_pthread_t and pthread_t size mismatch");

typedef uint64_t bionic_rwlockattr_t;
_Static_assert(sizeof(bionic_rwlockattr_t) == sizeof(pthread_rwlockattr_t), "bionic_rwlockattr_t and pthread_rwlockattr_t size mismatch");

// bionic's __pthread_cleanup_t: the caller keeps it on its own stack
struct bionic_pthread_cleanup_t {
	struct bionic_pthread_cleanup_t *prev;
	void (*routine)(void*);
	void *arg;
};

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

// For checking, if our glibc version is mapped to memory.
// Used for sanity checking and static initialization below.
#define IS_MAPPED(x) is_mapped(x->glibc, sizeof(*x))

// For handling static initialization.
#define INIT_IF_NOT_MAPPED(x, init) do { if (!IS_MAPPED(x)) init(x); } while(0)

// The glibc objects all come from mmap(), so they are page aligned and lie above mmap_min_addr.
// What else can be in the pointer slot is small: a bionic static initializer (0, or the type bits
// << 14 for mutexes) or DESTROYED. This check runs on every lock, unlock, wait and signal, so it
// must not cost a syscall: with mincore() here, JW Library's V8/ICU start-up spent 323 208
// syscalls (~240 ms on a Ryzen 9 7900X) in one thread. AOSP bionic keeps the state inline and
// takes a lock without entering the kernel (android-14 bionic/libc/bionic/pthread_mutex.cpp,
// NormalMutexTryLock()).
#define MAPPED_MIN_ADDR 0x10000
#define MAPPED_ALIGN 0x1000
static bool is_mapped(void *mem, const size_t sz)
{
	(void)sz;
	const uintptr_t addr = (uintptr_t)mem;
	return addr >= MAPPED_MIN_ADDR && !(addr & (MAPPED_ALIGN - 1));
}

// After munmap() the slot must no longer look mapped. 0xffff is what AOSP stores in a destroyed
// mutex (pthread_mutex_destroy()); it matches no static initializer, so using a destroyed mutex
// still fails the assertion in default_pthread_mutex_init() as it did with mincore().
#define DESTROYED ((void *)0xffff)
#define UNMAP(x) do { munmap(x->glibc, sizeof(*x->glibc)); x->glibc = DESTROYED; } while(0)

/* Like bionic (pthread_internal_t::cleanup_stack), the handlers are a per-thread list of the callers'
 * structs; pthread_exit() runs what is left. They must not go through glibc's cancellation buffers
 * (__pthread_register_cancel() with a sigsetjmp() in this function): pthread_exit() would
 * longjmp back into the frame of __pthread_cleanup_push(), long gone by then, and call a
 * `routine` read from an overwritten stack slot (seen as a jump to a heap address in libvlc's
 * pthread_exit() when it ends a cancelled thread). bionic has no pthread_cancel(). */
static __thread struct bionic_pthread_cleanup_t *cleanup_stack;

void bionic___pthread_cleanup_push(struct bionic_pthread_cleanup_t *c, void (*routine)(void*), void *arg)
{
	assert(c && routine);
	c->routine = routine;
	c->arg = arg;
	c->prev = cleanup_stack;
	cleanup_stack = c;
}

void bionic___pthread_cleanup_pop(struct bionic_pthread_cleanup_t *c, int execute)
{
	assert(c);
	cleanup_stack = c->prev;
	if (execute)
		c->routine(c->arg);
}

/* Go's cgo runtime for Android (runtime/cgo/gcc_android.c, inittls()) does not find
 * android_get_device_api_level() here, so it creates a key, stores a magic value in it and looks
 * for that value in the first 384 words of the thread control block. glibc keeps only keys
 * 0-31 there (specific_1stblock); a higher key lives in a block of its own and Go aborts with
 * "runtime/cgo: inittls: could not find pthread key" (Feeder, signal 6). Whether a low key is
 * still free when the app's library loads depends on what the process did before. So the free
 * keys below 32 are reserved before main() and handed to bionic callers first.
 * glibc fixes a key's destructor at creation, so each reserved key gets a trampoline that calls
 * the destructor its bionic caller asks for later. pthread_key_delete() needs no wrapper: a
 * deleted key goes back to glibc and is not handed out from here again. */
#define LOW_KEYS 32 // glibc's PTHREAD_KEY_2NDLEVEL_SIZE
static pthread_key_t low_keys[LOW_KEYS];
static void (*low_key_destructors[LOW_KEYS])(void *);
static uint8_t low_keys_reserved;
static uint8_t low_keys_given;
static pthread_mutex_t low_keys_lock = PTHREAD_MUTEX_INITIALIZER;

#define LOW_KEY_DTOR(i) \
	static void low_key_dtor_##i(void *value) \
	{ \
		void (*destructor)(void *) = __atomic_load_n(&low_key_destructors[i], __ATOMIC_ACQUIRE); \
		if (destructor) \
			destructor(value); \
	}
#define LOW_KEY_DTOR8(a, b, c, d, e, f, g, h) \
	LOW_KEY_DTOR(a) LOW_KEY_DTOR(b) LOW_KEY_DTOR(c) LOW_KEY_DTOR(d) \
	LOW_KEY_DTOR(e) LOW_KEY_DTOR(f) LOW_KEY_DTOR(g) LOW_KEY_DTOR(h)
LOW_KEY_DTOR8(0, 1, 2, 3, 4, 5, 6, 7)
LOW_KEY_DTOR8(8, 9, 10, 11, 12, 13, 14, 15)
LOW_KEY_DTOR8(16, 17, 18, 19, 20, 21, 22, 23)
LOW_KEY_DTOR8(24, 25, 26, 27, 28, 29, 30, 31)
#define LOW_KEY_REF8(a, b, c, d, e, f, g, h) \
	low_key_dtor_##a, low_key_dtor_##b, low_key_dtor_##c, low_key_dtor_##d, \
	low_key_dtor_##e, low_key_dtor_##f, low_key_dtor_##g, low_key_dtor_##h
static void (*const low_key_trampolines[LOW_KEYS])(void *) = {
	LOW_KEY_REF8(0, 1, 2, 3, 4, 5, 6, 7),
	LOW_KEY_REF8(8, 9, 10, 11, 12, 13, 14, 15),
	LOW_KEY_REF8(16, 17, 18, 19, 20, 21, 22, 23),
	LOW_KEY_REF8(24, 25, 26, 27, 28, 29, 30, 31),
};

__attribute__((constructor)) static void reserve_low_keys(void)
{
	while (low_keys_reserved < LOW_KEYS) {
		pthread_key_t key;
		if (pthread_key_create(&key, low_key_trampolines[low_keys_reserved]) != 0)
			break;
		if (key >= LOW_KEYS) {
			pthread_key_delete(key);
			break;
		}
		low_keys[low_keys_reserved++] = key;
	}
}

int bionic_pthread_key_create(bionic_key_t *key, void (*destructor)(void *))
{
	pthread_mutex_lock(&low_keys_lock);
	if (low_keys_given < low_keys_reserved) {
		const uint8_t i = low_keys_given++;
		__atomic_store_n(&low_key_destructors[i], destructor, __ATOMIC_RELEASE);
		*key = (bionic_key_t)low_keys[i];
		pthread_mutex_unlock(&low_keys_lock);
		return 0;
	}
	pthread_mutex_unlock(&low_keys_lock);
	return pthread_key_create((pthread_key_t *)key, destructor);
}

void bionic_pthread_exit(void *retval)
{
	while (cleanup_stack) {
		struct bionic_pthread_cleanup_t *c = cleanup_stack;
		cleanup_stack = c->prev;
		c->routine(c->arg);
	}
	pthread_exit(retval);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* sem */

int bionic_sem_destroy(bionic_sem_t *sem)
{
	assert(sem);
	int ret = 0;
	if (IS_MAPPED(sem)) {
		ret = sem_destroy(sem->glibc);
		UNMAP(sem);
	}
	return ret;
}

static void default_sem_init(bionic_sem_t *sem)
{
	// Apparently some android apps (hearthstone) do not call sem_init()
	assert(sem);
	sem->glibc = mmap(NULL, sizeof(*sem->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	memset(sem->glibc, 0, sizeof(*sem->glibc));
}

int bionic_sem_init(bionic_sem_t *sem, int pshared, unsigned int value)
{
	assert(sem);
	// From SEM_INIT(3)
	// Initializing a semaphore that has already been initialized results in underined behavior.
	*sem = (bionic_sem_t){0};
	sem->glibc = mmap(NULL, sizeof(*sem->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return sem_init(sem->glibc, pshared, value);
}

int bionic_sem_post(bionic_sem_t *sem)
{
	assert(sem);
	INIT_IF_NOT_MAPPED(sem, default_sem_init);
	return sem_post(sem->glibc);
}

int bionic_sem_wait(bionic_sem_t *sem)
{
	assert(sem);
	INIT_IF_NOT_MAPPED(sem, default_sem_init);
	return sem_wait(sem->glibc);
}

int bionic_sem_trywait(bionic_sem_t *sem)
{
	assert(sem);
	INIT_IF_NOT_MAPPED(sem, default_sem_init);
	return sem_trywait(sem->glibc);
}

int bionic_sem_timedwait(bionic_sem_t *sem, const struct timespec *abs_timeout)
{
	assert(sem && abs_timeout);
	INIT_IF_NOT_MAPPED(sem, default_sem_init);
	return sem_timedwait(sem->glibc, abs_timeout);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* rwlock */

int bionic_pthread_rwlock_destroy(bionic_rwlock_t *rwlock)
{
	assert(rwlock);
	int ret = 0;
	if (IS_MAPPED(rwlock)) {
		ret = pthread_rwlock_destroy(rwlock->glibc);
		UNMAP(rwlock);
	}
	return ret;

}

static void default_rwlock_init(bionic_rwlock_t *rwlock)
{
	// Apparently some android apps/libs (Qt5) do not call pthread_rwlock_init()
	assert(rwlock);
	rwlock->glibc = mmap(NULL, sizeof(*rwlock->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	memset(rwlock->glibc, 0, sizeof(*rwlock->glibc));
}

int bionic_pthread_rwlock_init(bionic_rwlock_t *restrict rwlock, const bionic_rwlockattr_t *restrict attr)
{
	assert(rwlock);
	rwlock->glibc = mmap(NULL, sizeof(*rwlock->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return pthread_rwlock_init(rwlock->glibc, (pthread_rwlockattr_t *)attr);
}

int bionic_pthread_rwlock_rdlock(bionic_rwlock_t *rwlock)
{
	assert(rwlock);
	INIT_IF_NOT_MAPPED(rwlock, default_rwlock_init);
	return pthread_rwlock_rdlock(rwlock->glibc);
}

int bionic_pthread_rwlock_unlock(bionic_rwlock_t *rwlock)
{
	assert(rwlock);
	INIT_IF_NOT_MAPPED(rwlock, default_rwlock_init);
	return pthread_rwlock_unlock(rwlock->glibc);
}

int bionic_pthread_rwlock_wrlock(bionic_rwlock_t *rwlock)
{
	assert(rwlock);
	INIT_IF_NOT_MAPPED(rwlock, default_rwlock_init);
	return pthread_rwlock_wrlock(rwlock->glibc);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* attr */

/* pthread_attr_t is a public struct in bionic (<pthread.h>): its fields are part of the ABI, and bionic code
 * reads and writes them directly (e.g. Berberis' pthread_create for a guest thread). So the attributes live in
 * bionic's layout, handled as bionic/libc/bionic/pthread_attr.cpp does; pthread_create() and
 * pthread_getattr_np() translate to and from glibc's attributes. */
#define BIONIC_PTHREAD_ATTR_FLAG_DETACHED 0x00000001
#define BIONIC_PTHREAD_ATTR_FLAG_INHERIT  0x00000004
#define BIONIC_PTHREAD_ATTR_FLAG_EXPLICIT 0x00000008
#define BIONIC_PTHREAD_CREATE_JOINABLE 0
#define BIONIC_PTHREAD_CREATE_DETACHED 1
#define BIONIC_PTHREAD_INHERIT_SCHED  0
#define BIONIC_PTHREAD_EXPLICIT_SCHED 1
#define BIONIC_PTHREAD_SCOPE_SYSTEM  0
#define BIONIC_PTHREAD_SCOPE_PROCESS 1
#if defined(__LP64__)
#define BIONIC_PTHREAD_STACK_MIN 16384
#define BIONIC_SIGNAL_STACK_SIZE_WITHOUT_GUARD (32 * 1024)
#else
#define BIONIC_PTHREAD_STACK_MIN 8192
#define BIONIC_SIGNAL_STACK_SIZE_WITHOUT_GUARD (16 * 1024)
#endif
#define BIONIC_PTHREAD_STACK_SIZE_DEFAULT ((1 * 1024 * 1024) - BIONIC_SIGNAL_STACK_SIZE_WITHOUT_GUARD)

int bionic_pthread_attr_init(bionic_attr_t *attr)
{
	attr->bionic.flags = 0;
	attr->bionic.stack_base = NULL;
	attr->bionic.stack_size = BIONIC_PTHREAD_STACK_SIZE_DEFAULT;
	attr->bionic.guard_size = sysconf(_SC_PAGESIZE);
	attr->bionic.sched_policy = SCHED_OTHER; /* SCHED_NORMAL */
	attr->bionic.sched_priority = 0;
	return 0;
}

int bionic_pthread_attr_destroy(bionic_attr_t *attr)
{
	memset(attr, 0x42, sizeof(*attr));
	return 0;
}

int bionic_pthread_attr_setinheritsched(bionic_attr_t *attr, int flag)
{
	if (flag == BIONIC_PTHREAD_EXPLICIT_SCHED) {
		attr->bionic.flags &= ~BIONIC_PTHREAD_ATTR_FLAG_INHERIT;
		attr->bionic.flags |= BIONIC_PTHREAD_ATTR_FLAG_EXPLICIT;
	} else if (flag == BIONIC_PTHREAD_INHERIT_SCHED) {
		attr->bionic.flags |= BIONIC_PTHREAD_ATTR_FLAG_INHERIT;
		attr->bionic.flags &= ~BIONIC_PTHREAD_ATTR_FLAG_EXPLICIT;
	} else {
		return EINVAL;
	}
	return 0;
}

int bionic_pthread_attr_getinheritsched(const bionic_attr_t *attr, int *flag)
{
	if (attr->bionic.flags & BIONIC_PTHREAD_ATTR_FLAG_INHERIT)
		*flag = BIONIC_PTHREAD_INHERIT_SCHED;
	else if (attr->bionic.flags & BIONIC_PTHREAD_ATTR_FLAG_EXPLICIT)
		*flag = BIONIC_PTHREAD_EXPLICIT_SCHED;
	else /* the behaviour before setinheritsched existed */
		*flag = (attr->bionic.sched_policy != SCHED_OTHER) ? BIONIC_PTHREAD_EXPLICIT_SCHED : BIONIC_PTHREAD_INHERIT_SCHED;
	return 0;
}

int bionic_pthread_attr_setdetachstate(bionic_attr_t *attr, int state)
{
	if (state == BIONIC_PTHREAD_CREATE_DETACHED)
		attr->bionic.flags |= BIONIC_PTHREAD_ATTR_FLAG_DETACHED;
	else if (state == BIONIC_PTHREAD_CREATE_JOINABLE)
		attr->bionic.flags &= ~BIONIC_PTHREAD_ATTR_FLAG_DETACHED;
	else
		return EINVAL;
	return 0;
}

int bionic_pthread_attr_getdetachstate(const bionic_attr_t *attr, int *state)
{
	*state = (attr->bionic.flags & BIONIC_PTHREAD_ATTR_FLAG_DETACHED) ? BIONIC_PTHREAD_CREATE_DETACHED : BIONIC_PTHREAD_CREATE_JOINABLE;
	return 0;
}

int bionic_pthread_attr_setschedpolicy(bionic_attr_t *attr, int policy)
{
	attr->bionic.sched_policy = policy;
	return 0;
}

int bionic_pthread_attr_getschedpolicy(const bionic_attr_t *attr, int *policy)
{
	*policy = attr->bionic.sched_policy;
	return 0;
}

int bionic_pthread_attr_setschedparam(bionic_attr_t *attr, const struct sched_param *param)
{
	attr->bionic.sched_priority = param->sched_priority;
	return 0;
}

int bionic_pthread_attr_getschedparam(const bionic_attr_t *attr, struct sched_param *param)
{
	param->sched_priority = attr->bionic.sched_priority;
	return 0;
}

int bionic_pthread_attr_setstacksize(bionic_attr_t *attr, size_t stack_size)
{
	if (stack_size < BIONIC_PTHREAD_STACK_MIN)
		return EINVAL;
	attr->bionic.stack_size = stack_size;
	return 0;
}

int bionic_pthread_attr_getstack(const bionic_attr_t *attr, void **stack_base, size_t *stack_size)
{
	*stack_base = attr->bionic.stack_base;
	*stack_size = attr->bionic.stack_size;
	return 0;
}

int bionic_pthread_attr_getstacksize(const bionic_attr_t *attr, size_t *stack_size)
{
	void *unused;
	return bionic_pthread_attr_getstack(attr, &unused, stack_size);
}

int bionic_pthread_attr_setstack(bionic_attr_t *attr, void *stack_base, size_t stack_size)
{
	size_t page_size = sysconf(_SC_PAGESIZE);
	if ((stack_size & (page_size - 1)) || stack_size < BIONIC_PTHREAD_STACK_MIN)
		return EINVAL;
	if ((uintptr_t)stack_base & (page_size - 1))
		return EINVAL;
	attr->bionic.stack_base = stack_base;
	attr->bionic.stack_size = stack_size;
	return 0;
}

int bionic_pthread_attr_setguardsize(bionic_attr_t *attr, size_t guard_size)
{
	attr->bionic.guard_size = guard_size;
	return 0;
}

int bionic_pthread_attr_getguardsize(const bionic_attr_t *attr, size_t *guard_size)
{
	*guard_size = attr->bionic.guard_size;
	return 0;
}

int bionic_pthread_attr_setscope(bionic_attr_t *attr, int scope)
{
	(void)attr;
	if (scope == BIONIC_PTHREAD_SCOPE_SYSTEM)
		return 0;
	if (scope == BIONIC_PTHREAD_SCOPE_PROCESS)
		return ENOTSUP;
	return EINVAL;
}

int bionic_pthread_attr_getscope(const bionic_attr_t *attr, int *scope)
{
	(void)attr;
	*scope = BIONIC_PTHREAD_SCOPE_SYSTEM;
	return 0;
}

int bionic_pthread_getattr_np(bionic_pthread_t thread, bionic_attr_t *attr)
{
	pthread_attr_t glibc_attr;
	int ret = pthread_getattr_np((pthread_t)thread, &glibc_attr);
	if (ret)
		return ret;
	int detach_state, policy;
	struct sched_param param;
	bionic_pthread_attr_init(attr);
	pthread_attr_getstack(&glibc_attr, &attr->bionic.stack_base, &attr->bionic.stack_size);
	pthread_attr_getguardsize(&glibc_attr, &attr->bionic.guard_size);
	if (!pthread_attr_getdetachstate(&glibc_attr, &detach_state) && detach_state == PTHREAD_CREATE_DETACHED)
		attr->bionic.flags |= BIONIC_PTHREAD_ATTR_FLAG_DETACHED;
	if (!pthread_attr_getschedpolicy(&glibc_attr, &policy))
		attr->bionic.sched_policy = policy;
	if (!pthread_attr_getschedparam(&glibc_attr, &param))
		attr->bionic.sched_priority = param.sched_priority;
	pthread_attr_destroy(&glibc_attr);
	return 0;
}

/* glibc attributes for a thread with these bionic ones (bionic's pthread_create: user stack, guard,
 * detach state, and the scheduling it sets on the new thread) */
static int to_glibc_attr(const bionic_attr_t *attr, pthread_attr_t *glibc_attr)
{
	int ret = pthread_attr_init(glibc_attr);
	if (ret)
		return ret;
	pthread_attr_setdetachstate(glibc_attr, (attr->bionic.flags & BIONIC_PTHREAD_ATTR_FLAG_DETACHED) ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);
	if (attr->bionic.stack_base) {
		ret = pthread_attr_setstack(glibc_attr, attr->bionic.stack_base, attr->bionic.stack_size);
	} else {
		ret = pthread_attr_setstacksize(glibc_attr, attr->bionic.stack_size < PTHREAD_STACK_MIN ? PTHREAD_STACK_MIN : attr->bionic.stack_size);
		if (!ret)
			ret = pthread_attr_setguardsize(glibc_attr, attr->bionic.guard_size);
	}
	bool explicit_sched = (attr->bionic.flags & BIONIC_PTHREAD_ATTR_FLAG_EXPLICIT) ||
	                      (!(attr->bionic.flags & BIONIC_PTHREAD_ATTR_FLAG_INHERIT) && attr->bionic.sched_policy != SCHED_OTHER);
	if (!ret && explicit_sched) {
		struct sched_param param = { .sched_priority = attr->bionic.sched_priority };
		pthread_attr_setinheritsched(glibc_attr, PTHREAD_EXPLICIT_SCHED);
		pthread_attr_setschedpolicy(glibc_attr, attr->bionic.sched_policy);
		pthread_attr_setschedparam(glibc_attr, &param);
	}
	if (ret)
		pthread_attr_destroy(glibc_attr);
	return ret;
}

int bionic_pthread_create(bionic_pthread_t *thread, const bionic_attr_t *attr, void* (*start)(void*), void *arg)
{
	if (!attr)
		return pthread_create((pthread_t *)thread, NULL, start, arg);
	pthread_attr_t glibc_attr;
	int ret = to_glibc_attr(attr, &glibc_attr);
	if (ret)
		return ret;
	ret = pthread_create((pthread_t *)thread, &glibc_attr, start, arg);
	pthread_attr_destroy(&glibc_attr);
	return ret;
}

/* bionic pthread_gettid_np(): the kernel thread id of a thread. glibc has it from 2.42 on; before, the
 * id is read from glibc's struct pthread (pthread_t points to it), at the offset where the calling
 * thread keeps its own id. */
static pid_t (*glibc_gettid_np)(pthread_t);
static ssize_t tid_offset = -1;

static void find_tid(void)
{
	glibc_gettid_np = dlsym(RTLD_DEFAULT, "pthread_gettid_np");
	if (glibc_gettid_np)
		return;
	const pid_t *self = (const pid_t *)pthread_self();
	pid_t tid = gettid();
	for (size_t i = 0; i < 1024 / sizeof(pid_t); i++) {
		if (self[i] == tid) {
			tid_offset = i * sizeof(pid_t);
			break;
		}
	}
}

pid_t bionic_pthread_gettid_np(bionic_pthread_t thread)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	pthread_once(&once, find_tid);

	if (glibc_gettid_np)
		return glibc_gettid_np((pthread_t)thread);
	if ((pthread_t)thread == pthread_self())
		return gettid();
	if (tid_offset < 0)
		return -1;
	return *(const pid_t *)((const char *)thread + tid_offset);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* mutexattr */

int bionic_pthread_mutexattr_settype(bionic_mutexattr_t *attr, int type)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_mutexattr_settype(attr->glibc, type);
}

int bionic_pthread_mutexattr_destroy(bionic_mutexattr_t *attr)
{
	assert(attr);
	int ret = 0;
	if (IS_MAPPED(attr)) {
		ret = pthread_mutexattr_destroy(attr->glibc);
		UNMAP(attr);
	}
	return ret;
}

int bionic_pthread_mutexattr_init(bionic_mutexattr_t *attr)
{
	assert(attr);
	// From PTHREAD_MUTEXATTR_INIT(3)
	// The results of initializing an already initialized mutex attributes object are undefined.
	*attr = (bionic_mutexattr_t){0};
	attr->glibc = mmap(NULL, sizeof(*attr->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return pthread_mutexattr_init(attr->glibc);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* mutex */

static void default_pthread_mutex_init(bionic_mutex_t *mutex)
{
	assert(mutex);

	for (size_t i = 0; i < ARRAY_SIZE(bionic_mutex_init_map); i++) {
		if (memcmp(&bionic_mutex_init_map[i].bionic, mutex, sizeof(*mutex)))
			continue;

		mutex->glibc = mmap(NULL, sizeof(*mutex->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		memcpy(mutex->glibc, &bionic_mutex_init_map[i].glibc, sizeof(bionic_mutex_init_map[i].glibc));
		return;
	}

	// We might have been mapped by another thread in the meantime, otherwise fail
	assert(IS_MAPPED(mutex) && "no such default initializer???");
}

int bionic_pthread_mutex_destroy(bionic_mutex_t *mutex)
{
	assert(mutex);
	int ret = 0;
	if (IS_MAPPED(mutex)) {
		ret = pthread_mutex_destroy(mutex->glibc);
		UNMAP(mutex);
	}
	return ret;
}

int bionic_pthread_mutex_init(bionic_mutex_t *mutex, const bionic_mutexattr_t *attr)
{
	assert(mutex && (!attr || IS_MAPPED(attr)));
	// From PTHREAD_MUTEX_INIT(3)
	// Attempting to initialize an already initialized mutex result in undefined behavior.
	*mutex = (bionic_mutex_t){0};
	mutex->glibc = mmap(NULL, sizeof(*mutex->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return pthread_mutex_init(mutex->glibc, (attr ? attr->glibc : NULL));
}

int
bionic_pthread_mutex_lock(bionic_mutex_t *mutex)
{
	assert(mutex);
	INIT_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	return pthread_mutex_lock(mutex->glibc);
}

int bionic_pthread_mutex_trylock(bionic_mutex_t *mutex)
{
	assert(mutex);
	INIT_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	return pthread_mutex_trylock(mutex->glibc);
}

int bionic_pthread_mutex_unlock(bionic_mutex_t *mutex)
{
	assert(mutex);
	INIT_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	return pthread_mutex_unlock(mutex->glibc);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* condattr */

int bionic_pthread_condattr_destroy(bionic_condattr_t *attr)
{
	assert(attr);
	int ret = 0;
	if (IS_MAPPED(attr)) {
		ret = pthread_condattr_destroy(attr->glibc);
		UNMAP(attr);
	}
	return ret;
}

int bionic_pthread_condattr_init(bionic_condattr_t *attr)
{
	*attr = (bionic_condattr_t){0};
	attr->glibc = mmap(NULL, sizeof(*attr->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return pthread_condattr_init(attr->glibc);
}

int bionic_pthread_condattr_setclock(bionic_condattr_t *attr, clockid_t clock_id)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_condattr_setclock(attr->glibc, clock_id);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* cond */

static void default_pthread_cond_init(bionic_cond_t *cond)
{
	assert(cond);
	cond->glibc = mmap(NULL, sizeof(*cond->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	memset(cond->glibc, 0, sizeof(*cond->glibc));
}

int bionic_pthread_cond_destroy(bionic_cond_t *cond)
{
	assert(cond);
	int ret = 0;
	if (IS_MAPPED(cond)) {
		ret = pthread_cond_destroy(cond->glibc);
		UNMAP(cond);
	}
	return ret;
}

int bionic_pthread_cond_init(bionic_cond_t *cond, const bionic_condattr_t *attr)
{
	// SUS // assert(cond && (!attr || IS_MAPPED(attr)));
	// From PTHREAD_COND_INIT(3)
	// Attempting to initialize an already initialized mutex result in undefined behavior.
	*cond = (bionic_cond_t){0};
	cond->glibc = mmap(NULL, sizeof(*cond->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return pthread_cond_init(cond->glibc, (attr ? attr->glibc : NULL));
}

int bionic_pthread_cond_broadcast(bionic_cond_t *cond)
{
	assert(cond);
	INIT_IF_NOT_MAPPED(cond, default_pthread_cond_init);
	return pthread_cond_broadcast(cond->glibc);
}

int bionic_pthread_cond_signal(bionic_cond_t *cond)
{
	assert(cond);
	INIT_IF_NOT_MAPPED(cond, default_pthread_cond_init);
	return pthread_cond_signal(cond->glibc);
}

int
bionic_pthread_cond_wait(bionic_cond_t *cond, bionic_mutex_t *mutex) {
	assert(cond && mutex);
	INIT_IF_NOT_MAPPED(cond, default_pthread_cond_init);
	INIT_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	return pthread_cond_wait(cond->glibc, mutex->glibc);
}

int bionic_pthread_cond_clockwait(bionic_cond_t *cond, bionic_mutex_t *mutex, clockid_t clock_id, const struct timespec *abs_timeout) {
	assert(cond && mutex);
	INIT_IF_NOT_MAPPED(cond, default_pthread_cond_init);
	INIT_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
#ifdef __GLIBC__
	return pthread_cond_clockwait(cond->glibc, mutex->glibc, clock_id, abs_timeout);
#else /* musl */
	/*
	 * FIXME: This is a bit ugly but musl doesn't implement _clockwait yet.
	 * Instead we just patch the cond manually to update the clock type,
	 * which is a TERRIBLE idea but what else can we do...
	 */
	clockid_t old_clock = cond->glibc->__u.__i[4]; /* _c_clock */
	cond->glibc->__u.__i[4] = clock_id;
	int ret = pthread_cond_timedwait(cond->glibc, mutex->glibc, abs_timeout);
	cond->glibc->__u.__i[4] = old_clock;
	return ret;
#endif
}

int bionic_pthread_cond_timedwait(bionic_cond_t *cond, bionic_mutex_t *mutex, const struct timespec *abs_timeout)
{
	assert(cond && mutex);
	INIT_IF_NOT_MAPPED(cond, default_pthread_cond_init);
	INIT_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	return pthread_cond_timedwait(cond->glibc, mutex->glibc, abs_timeout);
}

int bionic_pthread_cond_timedwait_relative_np(bionic_cond_t *cond, bionic_mutex_t *mutex, const struct timespec *reltime)
{
	assert(cond && mutex && reltime);
	struct timespec tv;
	clock_gettime(CLOCK_REALTIME, &tv);
	tv.tv_sec += reltime->tv_sec;
	tv.tv_nsec += reltime->tv_nsec;
	if (tv.tv_nsec >= 1000000000) {
		++tv.tv_sec;
		tv.tv_nsec -= 1000000000;
	}
	return bionic_pthread_cond_timedwait(cond, mutex, &tv);
}

int bionic_pthread_cond_timedwait_monotonic_np(bionic_cond_t *cond, bionic_mutex_t *mutex, const struct timespec *abstime)
{
	assert(cond && mutex && abstime);
	struct timespec tv;
	clock_gettime(CLOCK_MONOTONIC, &tv);
	tv.tv_sec += abstime->tv_sec;
	tv.tv_nsec += abstime->tv_nsec;
	if (tv.tv_nsec >= 1000000000) {
		++tv.tv_sec;
		tv.tv_nsec -= 1000000000;
	}
	return bionic_pthread_cond_timedwait(cond, mutex, &tv);
}

int bionic_pthread_cond_timedwait_monotonic(bionic_cond_t *cond, bionic_mutex_t *mutex, const struct timespec *abstime)
{
	return bionic_pthread_cond_timedwait_monotonic_np(cond, mutex, abstime);
}

int bionic_pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
	return pthread_atfork(prepare, parent, child);
}
