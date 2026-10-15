/* lx_dbt.h - Alpine mode: PsiLinux's ARM-to-ARM binary translator.
 *
 * Alpine Linux's armhf programs are built for ARMv6K with VFPv2 hardware
 * floating point; the 5mx's ARM710T is ARMv4T with no FPU, and an svc there
 * restarts the machine. So programs without the PsiLinux note are run
 * through this translator (linux/docs/DESIGN.md, "Alpine mode"):
 *
 *  - Guest code is translated a basic block at a time, when it first runs,
 *    into one code cache. Ordinary ARMv4 instructions are copied as they
 *    are; guest registers live in the same host registers, the guest's
 *    flags in the CPSR, and guest addresses are host addresses (one address
 *    space, no MMU).
 *  - What the ARM710T cannot run is rewritten: PC-relative operands become
 *    constants; branches go to translated blocks (chained once the target
 *    is known) or through the dispatcher; svc becomes a call into the
 *    kernel; v6 instructions become ARMv4 sequences or a call to the
 *    instruction interpreter (lx_dbt_rt.c); VFP runs on SoftFloat with the
 *    register file in the thread; anything unknown raises SIGILL.
 *  - Only one translated thread runs guest code at a time (it holds the
 *    "CPU", lx_cpu.cur). The kernel hands the CPU over at system calls and
 *    when the dispatcher is asked to yield. So the runtime's state is
 *    single-threaded, LDREX/STREX need no monitor, and the code cache can be
 *    flushed whenever the translator runs: every other translated thread is
 *    in the kernel, and goes back to guest code through the dispatcher.
 */
#ifndef LX_DBT_H
#define LX_DBT_H

#include "lx_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the frame of a translated thread (t->dframe): what dbt_enter saves */
enum { DF_SP = 13, DF_LR = 14, DF_PC = 15, DF_CPSR = 16, DF_WORDS = 17 };

/* why translated code enters the runtime: a descriptor's first word */
enum {
	DBT_SVC = 1,        /* { SVC, guest pc }: a system call */
	DBT_UNDEF,          /* { UNDEF, guest pc }: SIGILL */
	DBT_HELPER,         /* { HELPER, guest pc }: interpret one instruction, then on */
	DBT_CHAIN,          /* { CHAIN, guest target, branch to patch }: an exit not yet chained */
	DBT_MISS,           /* { MISS }: the dispatcher did not find lx_cpu.target */
	DBT_SEGV,           /* { SEGV, guest pc }: code fetched from no mapping */
	DBT_FIXUP           /* { FIXUP, host insn, host address to go on at }: an unaligned access (dbt_patch_unaligned) */
};

/* the CPU: the one translated thread running guest code, and what the
 * dispatcher (lx_dbt_entry.S) reads. Offsets are fixed: the assembler
 * uses them. */
typedef struct lx_cpu_t {
	lx_thread *cur;             /* 0  the thread holding the CPU */
	lx_u32 target;              /* 4  the dispatcher's guest target, on a miss */
	volatile lx_u32 yield;      /* 8  nonzero: enter the runtime at the next indirect branch */
	lx_u32 *hash;               /* 12 guest pc -> host code: {guest, host} pairs */
	lx_u32 critical;            /* 16 the runtime is changing shared state: kills wait */
	lx_u32 kill_req;            /* 20 a kill that waited for the end of critical */
	lx_u32 cache_base;          /* 24 the code cache */
	lx_u32 cache_end;           /* 28 */
	lx_u32 dispatch;            /* 32 the dispatcher (its copy in the cache) */
	lx_u32 enter;               /* 36 the veneer to dbt_enter (in the cache) */
	lx_u32 translations;        /* statistics */
	lx_u32 flushes;
	lx_u32 misses;
	lx_u32 helpers;
	lx_u32 syscalls;
	lx_u32 fixups;
	lx_u32 swaps;               /* a process's writable parts swapped in (lx_simg.c) */
	lx_u32 guest_insns;         /* guest instructions translated */
	lx_u32 host_words;          /* host words emitted for them */
	lx_u32 slow;                /* (debugging) classes of instruction to interpret: DBT_SLOW_* */
	void (*trace)(lx_thread *t, lx_u32 pc);   /* (debugging) every guest instruction run, one a block */
	void (*trace_note)(lx_thread *t, const char *what);   /* (debugging) ...and the system calls between */
	lx_u32 maxn;                /* (debugging) instructions in a block at most, 0: MAXN */
	lx_u32 nochain;             /* (debugging) 1: no chaining, 2: no dispatcher hash either */
	lx_u32 patched;             /* sites patched after an alignment fault */
	lx_u32 fixed_calls;         /* unaligned accesses done through a patched site */
} lx_cpu_t;

/* (debugging, psihost's LX_DBT_SLOW) instructions interpreted, not translated */
#define DBT_SLOW_DP    1
#define DBT_SLOW_LS    2
#define DBT_SLOW_EXTRA 4
#define DBT_SLOW_LDM   8
#define DBT_SLOW_MEDIA 16
#define DBT_SLOW_MUL   32
#define DBT_SLOW_MISC  64
#define DBT_SLOW_FIXUP 128             /* (not interpreted) loads and stores through DBT_FIXUP, as patched sites are */

extern lx_cpu_t lx_cpu;

#define LX_DBT_STACK (128 * 1024)       /* a translated program's main stack, at least (each is committed RAM) */

#define DBT_HASH_BITS 12
#define DBT_HASH_SIZE (1u << DBT_HASH_BITS)
#define DBT_EMPTY 0xffffffffu

/* the kernel's side */
int dbt_init(void);                           /* on the kernel thread, before the first translated program */
int dbt_available(void);
void dbt_invalidate(lx_u32 lo, lx_u32 hi);    /* guest code in [lo, hi) is gone (an image freed, a munmap) */
void dbt_stats(char *buf, int size);          /* /proc/psilinux/dbt */

/* the thread's side (on the Linux thread, not the kernel's) */
void dbt_thread_start(lx_thread *t);          /* from lx_thread_start: run t->ctx, never returns */
lx_u32 dbt_runtime(lx_u32 *desc, lx_u32 *frame);   /* from dbt_enter: where to go on */

/* lx_dbt_entry.S */
void dbt_enter(void);
void dbt_continue(lx_u32 host);               /* resume lx_cpu.cur's frame at host: never returns */
extern lx_u32 dbt_dispatch_start[], dbt_dispatch_end[];

/* lx_dbt_rt.c: the instruction interpreter and VFP */
int dbt_interp(lx_thread *t, lx_u32 *frame, lx_u32 pc, lx_u32 insn);  /* 0, or a signal */
int dbt_vfp(lx_thread *t, lx_u32 *frame, lx_u32 pc, lx_u32 insn);     /* 0, or a signal */
int dbt_fixup(lx_u32 *regs, lx_u32 sp, lx_u32 insn);   /* an unaligned load or store, done bytewise; 0 or -1 */
int dbt_patch_unaligned(lx_u32 host);         /* (after dbt_fixup) that site goes through the runtime from now on */

/* the kernel calls the runtime's translated threads make (not Linux's) */
#define LX_NR_dbt_yield  0x0fff01             /* give the CPU to another thread */
#define LX_NR_dbt_raise  0x0fff02             /* raise a signal (a[0]: kept; sig in t->dbt_sig) */

#ifdef __cplusplus
}
#endif
#endif
