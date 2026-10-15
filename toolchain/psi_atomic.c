/* psi_atomic.c - GCC's __sync_* atomics for PsiLinux programs.
 *
 * On ARMv4T GCC turns __sync_* and __atomic_* builtins into calls, and
 * libgcc implements those (libgcc/config/arm/linux-atomic*.c) by calling the
 * Linux kernel's "kuser" helpers at 0xffff0fc0 and 0xffff0f60. EPOC has no
 * such page: a program using them faults (curl did). These are the same
 * functions, built on musl's compare-and-swap, which PsiLinux's patched musl
 * takes from the runner (AT_SYSINFO) and real Linux from the kuser page.
 *
 * lx-cc links this object into every program, ahead of libgcc, so libgcc's
 * versions are never pulled in. It must define everything they define.
 *
 * __clear_cache is here too: libgcc's (for JITs, MicroPython's native
 * emitter) is an inline svc, which on EPOC restarts the Psion. This one asks
 * through __psi_svc, like every other system call (the runner answers at
 * once: the ARM710T's one cache holds code and data alike). */
#include <stdint.h>

extern uintptr_t __a_cas_ptr, __a_barrier_ptr;    /* (hidden, in libc.a) */

/* (GCC knows __sync_* as builtins and will not have them defined in C by
 * those names: each is defined as psi_<name>, with the real name as its
 * symbol) */
#define SYNC(ret, name, args) ret psi_##name args __asm__(#name); ret psi_##name args

/* 0 if *p was oldv and is now newv */
static int kcas(int oldv, int newv, volatile int *p)
{
	register int r0 __asm__("r0") = oldv;
	register int r1 __asm__("r1") = newv;
	register volatile int *r2 __asm__("r2") = p;
	register uintptr_t r3 __asm__("r3") = __a_cas_ptr;
	__asm__ __volatile__("mov lr, pc\n\tbx r3"
		: "+r"(r0), "+r"(r3) : "r"(r1), "r"(r2) : "memory", "lr", "ip", "cc");
	return r0;
}

static void kbarrier(void)
{
	register uintptr_t ip __asm__("ip") = __a_barrier_ptr;
	__asm__ __volatile__("mov lr, pc\n\tbx ip" : "+r"(ip) : : "memory", "cc", "lr");
}

SYNC(void, __sync_synchronize, (void)) { kbarrier(); }

/* 32-bit */
#define OP4(name, expr, retnew) \
SYNC(int, name, (volatile int *p, int v)) \
{ \
	int old, n; \
	do { old = *p; n = (expr); } while (kcas(old, n, p)); \
	return retnew ? n : old; \
}
OP4(__sync_fetch_and_add_4, old + v, 0)
OP4(__sync_fetch_and_sub_4, old - v, 0)
OP4(__sync_fetch_and_or_4, old | v, 0)
OP4(__sync_fetch_and_and_4, old & v, 0)
OP4(__sync_fetch_and_xor_4, old ^ v, 0)
OP4(__sync_fetch_and_nand_4, ~(old & v), 0)
OP4(__sync_add_and_fetch_4, old + v, 1)
OP4(__sync_sub_and_fetch_4, old - v, 1)
OP4(__sync_or_and_fetch_4, old | v, 1)
OP4(__sync_and_and_fetch_4, old & v, 1)
OP4(__sync_xor_and_fetch_4, old ^ v, 1)
OP4(__sync_nand_and_fetch_4, ~(old & v), 1)
OP4(__sync_lock_test_and_set_4, v, 0)

SYNC(int, __sync_val_compare_and_swap_4, (volatile int *p, int oldv, int newv))
{
	int cur;
	for (;;) {
		cur = *p;
		if (cur != oldv) return cur;
		if (!kcas(oldv, newv, p)) return oldv;
	}
}

SYNC(_Bool, __sync_bool_compare_and_swap_4, (volatile int *p, int oldv, int newv))
{
	return !kcas(oldv, newv, p);
}

SYNC(void, __sync_lock_release_4, (volatile int *p)) { kbarrier(); *p = 0; }

/* 8- and 16-bit: on the aligned word that holds them (little-endian) */
#define SUBWORD(N, T, MASK) \
static int shift_##N(volatile void *p) { return ((uintptr_t)p & 3) * 8; } \
static volatile int *word_##N(volatile void *p) { return (volatile int *)((uintptr_t)p & ~3u); } \
SYNC(T, __sync_val_compare_and_swap_##N, (volatile T *p, T oldv, T newv)) \
{ \
	volatile int *w = word_##N(p); \
	int s = shift_##N(p), mask = MASK << s, cur, n; \
	for (;;) { \
		cur = *w; \
		if ((T)((cur & mask) >> s) != oldv) return (T)((cur & mask) >> s); \
		n = (cur & ~mask) | (((int)(unsigned)(newv) << s) & mask); \
		if (!kcas(cur, n, w)) return oldv; \
	} \
} \
SYNC(_Bool, __sync_bool_compare_and_swap_##N, (volatile T *p, T oldv, T newv)) \
{ \
	return psi___sync_val_compare_and_swap_##N(p, oldv, newv) == oldv; \
} \
SYNC(void, __sync_lock_release_##N, (volatile T *p)) { kbarrier(); *p = 0; }

#define SUBOP(N, T, MASK, name, expr, retnew) \
SYNC(T, name, (volatile T *p, T v)) \
{ \
	volatile int *w = (volatile int *)((uintptr_t)p & ~3u); \
	int s = ((uintptr_t)p & 3) * 8, mask = MASK << s, cur, n; \
	T old, nv; \
	do { \
		cur = *w; \
		old = (T)((cur & mask) >> s); \
		nv = (T)(expr); \
		n = (cur & ~mask) | (((int)(unsigned)nv << s) & mask); \
	} while (kcas(cur, n, w)); \
	return retnew ? nv : old; \
}

SUBWORD(1, unsigned char, 0xff)
SUBWORD(2, unsigned short, 0xffff)
#define SUBOPS(N, T, M) \
SUBOP(N, T, M, __sync_fetch_and_add_##N, old + v, 0) \
SUBOP(N, T, M, __sync_fetch_and_sub_##N, old - v, 0) \
SUBOP(N, T, M, __sync_fetch_and_or_##N, old | v, 0) \
SUBOP(N, T, M, __sync_fetch_and_and_##N, old & v, 0) \
SUBOP(N, T, M, __sync_fetch_and_xor_##N, old ^ v, 0) \
SUBOP(N, T, M, __sync_fetch_and_nand_##N, ~(old & v), 0) \
SUBOP(N, T, M, __sync_add_and_fetch_##N, old + v, 1) \
SUBOP(N, T, M, __sync_sub_and_fetch_##N, old - v, 1) \
SUBOP(N, T, M, __sync_or_and_fetch_##N, old | v, 1) \
SUBOP(N, T, M, __sync_and_and_fetch_##N, old & v, 1) \
SUBOP(N, T, M, __sync_xor_and_fetch_##N, old ^ v, 1) \
SUBOP(N, T, M, __sync_nand_and_fetch_##N, ~(old & v), 1) \
SUBOP(N, T, M, __sync_lock_test_and_set_##N, v, 0)
SUBOPS(1, unsigned char, 0xff)
SUBOPS(2, unsigned short, 0xffff)

/* 64-bit: under a lock word (kuser's cmpxchg64 is not there either) */
static volatile int lock64;
static void lock(void) { while (kcas(0, 1, &lock64)) ; }
static void unlock(void) { kbarrier(); lock64 = 0; }

SYNC(long long, __sync_val_compare_and_swap_8, (volatile long long *p, long long oldv, long long newv))
{
	long long cur;
	lock();
	cur = *p;
	if (cur == oldv) *p = newv;
	unlock();
	return cur;
}

SYNC(_Bool, __sync_bool_compare_and_swap_8, (volatile long long *p, long long oldv, long long newv))
{
	return psi___sync_val_compare_and_swap_8(p, oldv, newv) == oldv;
}

#define OP8(name, expr, retnew) \
SYNC(long long, name, (volatile long long *p, long long v)) \
{ \
	long long old, n; \
	lock(); \
	old = *p; \
	n = (expr); \
	*p = n; \
	unlock(); \
	return retnew ? n : old; \
}
OP8(__sync_fetch_and_add_8, old + v, 0)
OP8(__sync_fetch_and_sub_8, old - v, 0)
OP8(__sync_fetch_and_or_8, old | v, 0)
OP8(__sync_fetch_and_and_8, old & v, 0)
OP8(__sync_fetch_and_xor_8, old ^ v, 0)
OP8(__sync_fetch_and_nand_8, ~(old & v), 0)
OP8(__sync_add_and_fetch_8, old + v, 1)
OP8(__sync_sub_and_fetch_8, old - v, 1)
OP8(__sync_or_and_fetch_8, old | v, 1)
OP8(__sync_and_and_fetch_8, old & v, 1)
OP8(__sync_xor_and_fetch_8, old ^ v, 1)
OP8(__sync_nand_and_fetch_8, ~(old & v), 1)
OP8(__sync_lock_test_and_set_8, v, 0)
SYNC(void, __sync_lock_release_8, (volatile long long *p)) { lock(); *p = 0; unlock(); }

/* __clear_cache(begin, end): the ARM cacheflush call (0xf0002) */
extern void __psi_svc(void);
SYNC(void, __clear_cache, (char *begin, char *end))
{
	register long r0 __asm__("r0") = (long)begin;
	register long r1 __asm__("r1") = (long)end;
	register long r2 __asm__("r2") = 0;
	register long r7 __asm__("r7") = 0xf0002;
	__asm__ __volatile__("bl __psi_svc" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory", "cc", "lr", "ip");
}
