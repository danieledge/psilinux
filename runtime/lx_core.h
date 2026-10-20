/* lx_core.h - PsiLinux's kernel: the part of the runner that is the same on
 * EPOC and on the host harness.
 *
 * The model (linux/docs/DESIGN.md):
 *  - Every Linux thread is a thread of the backend (an EPOC RThread in the
 *    runner, or a pthread in the host harness). It runs the program's code
 *    at full speed. A system call is a call (no svc: see psi_svc.s in the
 *    musl patch) into lx_entry.S, which hands it to the kernel thread and
 *    waits for the answer.
 *  - One kernel thread runs everything in this file, one request at a time,
 *    so the kernel has no locks. A call that would block is parked and tried
 *    again when anything changes; the kernel itself never blocks (except
 *    briefly on file I/O).
 *  - There is no MMU: every image is loaded at its own address in one address
 *    space. vfork (and posix_spawn's clone) run the child on the parent's
 *    thread and stack until it execs or exits, as on uClinux.
 */
#ifndef LX_CORE_H
#define LX_CORE_H

#include "lx_abi.h"
#include "lx_sysnr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LX_VERSION "0.24"
#define LX_MAXPROC 64
#define LX_MAXTHREAD 64
#define LX_MAXFD 128
#define LX_PATH_MAX 512
#define LX_ARG_MAX (64 * 1024)
#define LX_PIPE_SIZE 8192
#define LX_MAXTTY 4
#define LX_DEFAULT_STACK (128 * 1024)
#define LX_BLOCK (-100000)          /* a handler's "park me, try again later" */
#define LX_NOREPLY (-100001)        /* a handler has already answered (or the thread is gone) */

#ifdef __cplusplus
/* (gcc 3.0's C++ compiler crashes on "typedef struct x x;", and C++ does
 * not need it) */
struct lx_proc;
struct lx_thread;
struct lx_file;
struct lx_mm;
struct lx_tty;
struct lx_borrow;
#else
typedef struct lx_proc lx_proc;
typedef struct lx_thread lx_thread;
typedef struct lx_file lx_file;
typedef struct lx_mm lx_mm;
typedef struct lx_tty lx_tty;
#endif

/* ---- memory ------------------------------------------------------------ */

/* A piece of the address space the backend gave us. */
typedef struct lx_region {
	lx_u32 base;
	lx_u32 size;
	void *bk;                   /* the backend's handle (an RChunk) */
} lx_region;

enum { LX_REG_IMAGE = 1, LX_REG_STACK, LX_REG_ARENA };

/* Alpine mode: a program image processes share (lx_simg.c) */
typedef struct lx_simg {
	char path[LX_PATH_MAX];
	lx_s64 size, mtime;         /* the file's: which file it is */
	lx_u64 ino;
	int is_lib;                 /* mapped by the dynamic linker (or loaded by exec) */
	lx_region reg;              /* its memory */
	lx_u32 base, len;           /* the image */
	lx_u32 rw_lo, rw_hi;        /* its writable part: each process's own */
	char *pristine;             /* the writable part as the file has it */
	struct lx_mm *owner;        /* whose writable part is in place */
	int users;                  /* processes using it */
	lx_u32 used;                /* (to keep the most recent while unused) */
	lx_u32 entry;               /* (the ELF header's entry: the dynamic linker's start) */
} lx_simg;

/* One mapping made by mmap (in the arena, or a shared library's image). */
typedef struct lx_map {
	lx_u32 addr, len;
	lx_file *file;              /* MAP_SHARED + PROT_WRITE of a file: written back */
	lx_s64 off;
	int prot, flags;
	lx_simg *simg;              /* a shared library (Alpine mode): not the arena's */
	struct lx_map *next;
} lx_map;

/* An address space: the program image and its mappings. A vfork child
 * shares its parent's until it execs. */
struct lx_mm {
	int refs;
	lx_region image;            /* text, data and bss, loaded at image.base */
	lx_u32 entry;
	lx_u32 phdr, phnum;         /* for AT_PHDR */
	lx_u32 brk;                 /* (brk is never moved: musl falls back to mmap) */
	lx_map *maps;
	lx_u32 mapped;              /* bytes in maps */
	lx_u32 peak;                /* the most there were (/proc/PID/status VmHWM; logged with LX_DBT_STATS) */
	lx_u32 wlo, whi;            /* the image's writable part (data, bss) */
	int dbt;                    /* Alpine mode: its code is translated (lx_dbt.c) */
	lx_region interp;           /* the dynamic linker (PT_INTERP), if any */
	lx_u32 ilo, ihi;            /* ...and its writable part */
	lx_simg *img_s, *interp_s;  /* the image and the dynamic linker are shared images (not its own regions) */
#define LX_MM_SIMGS 24
	struct { lx_simg *s; char *store; int refs; } sw[LX_MM_SIMGS];   /* the shared images it uses */
	int nsw;
	struct lx_forksnap *fork_snap;   /* a fork child borrows this memory: what to put back */
	char exe[LX_PATH_MAX];      /* /proc/self/exe */
};

/* ---- files ------------------------------------------------------------ */

typedef struct lx_fops {
	int (*read)(lx_thread *t, lx_file *f, char *buf, lx_u32 n);
	int (*write)(lx_thread *t, lx_file *f, const char *buf, lx_u32 n);
	int (*pread)(lx_thread *t, lx_file *f, char *buf, lx_u32 n, lx_s64 off);
	int (*pwrite)(lx_thread *t, lx_file *f, const char *buf, lx_u32 n, lx_s64 off);
	lx_s64 (*lseek)(lx_file *f, lx_s64 off, int whence);
	int (*fstat)(lx_file *f, struct lx_stat64 *st);
	int (*ioctl)(lx_thread *t, lx_file *f, lx_u32 req, lx_u32 arg);
	int (*poll)(lx_file *f);    /* LX_POLLIN | LX_POLLOUT | LX_POLLHUP ... now */
	int (*getdents)(lx_file *f, char *buf, lx_u32 n);
	void (*close)(lx_file *f);
	int (*truncate)(lx_file *f, lx_s64 len);
	int (*sync)(lx_file *f);
} lx_fops;

struct lx_file {
	int refs;
	int flags;                  /* O_ACCMODE, O_APPEND, O_NONBLOCK... */
	lx_s64 pos;
	const lx_fops *ops;
	void *priv;
	char *path;                 /* the Linux path it was opened by (for fstat, /proc) */
	lx_u32 mode;                /* S_IF* and permission bits, as stat says */
	lx_u64 ino;
};

/* ---- terminals ---------------------------------------------------------- */

#define LX_TTY_IN 2048
struct lx_tty {
	int index;                  /* /dev/tty<index+1>, 0 = the console */
	int open;
	struct lx_termios tio;
	struct lx_winsize ws;
	int sid;                    /* the session it controls (0 = none) */
	int pgrp;                   /* the foreground process group */
	/* typed bytes, after input processing: what read() returns */
	char in[LX_TTY_IN];
	int in_head, in_tail;
	int lines;                  /* complete lines in 'in' (canonical mode) */
	char line[LX_TTY_IN];       /* the line being edited (canonical mode) */
	int line_len;
	int eof;                    /* ^D on an empty line: a 0-byte read */
	int hangup;
	int lnext;                  /* the next byte is literal (^V) */
	int column;                 /* the output column, for erase with tabs */
	int stopped;                /* ^S */
	void *bk;
};

/* ---- threads and processes ---------------------------------------------- */

enum {
	LX_T_FREE = 0,
	LX_T_USER,                  /* running the program's code */
	LX_T_SYSCALL,               /* in a system call, being handled */
	LX_T_PARKED,                /* in a system call that waits */
	LX_T_STOPPED,               /* in a system call, its process stopped */
	LX_T_DEAD
};

enum { LX_R_RET = 0, LX_R_RESUME, LX_R_EXIT };

/* the kernel's ARM rt signal frame: siginfo, ucontext, return code */
#define LX_SIGFRAME_SIZE (128 + 744 + 16)

/* the registers saved by lx_entry.S: r0-r12 and the return address */
#define LX_FRAME_WORDS 14
#define LX_FR_LR 13

struct lx_thread {
	/* Alpine mode (lx_dbt.h): first, at fixed offsets (lx_dbt_entry.S) */
	lx_u32 dframe[17];          /* r0-r12, sp, lr, pc, cpsr while the runtime has it */
	lx_u32 hostsp;              /* its own stack (the EPOC thread's), for the runtime */
	int dbt;                    /* it runs translated code */
	lx_u32 ctx_cpsr;            /* the flags, with ctx (LX_R_RESUME) */
	lx_u32 vfp[32];             /* VFP: s0-s31 (d0-d15) */
	lx_u32 fpscr;
	lx_u32 tpidrurw;
	lx_u32 dbt_ge;              /* the GE flags of the v6 parallel instructions */
	lx_u32 dbt_q;               /* the sticky Q flag (not in the ARM710T's CPSR) */
	int dbt_kcall;              /* a call of the runtime's own (LX_NR_dbt_*), not the program's */
	int dbt_sig;                /* LX_NR_dbt_raise: the signal */
	int cpu_wait;               /* answered, waiting for the CPU: 1 to reply, 2 to start */
	lx_u32 cpu_seq;             /* (the order they wait in) */
	lx_s64 slice_start;         /* when it last got the CPU */
	struct lx_borrow *borrow;   /* vfork and fork children running on it, newest first */
	int tid;
	int state;
	lx_proc *proc;              /* the process this thread runs as now (a vfork child, while it borrows it) */
	lx_proc *owner;             /* the process it belongs to */
	void *bk;                   /* the backend's thread */
	lx_u32 *frame;              /* the registers on the program's stack, in a call */
	lx_u32 nr;                  /* the call */
	int reply;                  /* LX_R_* */
	lx_u32 ctx[16];             /* LX_R_RESUME: r0-r12, sp, lr, pc */
	/* a signal frame to put on the program's stack as it resumes. The
	 * kernel cannot write it there itself: the thread is still running its
	 * side of the call (lx_trap) on that very part of the stack. */
	lx_u32 sigbuf[LX_SIGFRAME_SIZE / 4];
	lx_u32 sigframe_at;
	lx_u32 sigframe_len;
	lx_u32 sigframe_src;        /* where it is copied from (sigbuf, or a fork's saved stack) */
	lx_region fork_store;       /* a fork's saved memory, freed at this thread's next call */
	lx_u32 tp;                  /* the thread pointer (TLS) */
	lx_u32 stack_lo, stack_hi;  /* where its stack is (gettp, lx_trap find the thread by sp) */
	lx_region stack;            /* its own stack, if the kernel made it */
	lx_u32 clear_child_tid;
	lx_u32 sigmask[2];
	lx_u32 sigpending[2];
	lx_u32 altstack_sp, altstack_size;
	int altstack_on;
	/* what it waits for, while parked */
	lx_s64 deadline;            /* in microseconds of lx_now(), 0 = none */
	int timed_out;
	int intr;                   /* a signal came: the call returns EINTR */
	int restart;                /* ...or is made again after the handler (SA_RESTART) */
	lx_u32 restart_r0;
	lx_u32 saved_sigmask[2];    /* rt_sigsuspend, ppoll, pselect: the mask before */
	int sigmask_saved;
	int fault;                  /* the exception handler reported a fault (LX_SIGSEGV...) */
	lx_u32 fault_addr;
	lx_u64 syscalls;            /* how many calls it made (/proc) */
	lx_s64 ran_since;           /* when its call was last answered: it has run since */
};

/* a vfork or fork child running on its parent's thread (lx_core.c) */
typedef struct lx_borrow {
	struct lx_borrow *prev;
	lx_proc *child;
	lx_u32 frame[17];           /* the parent's registers at the call (LX_FRAME_WORDS or DF_WORDS) */
	lx_u32 sp;                  /* ...and its sp */
	lx_u32 tp, tpidrurw, fpscr; /* (a translated parent's: the child may change them) */
	lx_u32 vfp[32];
	lx_u32 sigmask[2], saved_sigmask[2];   /* (the child's signal state is its own) */
	int sigmask_saved;
	lx_u32 altstack_sp, altstack_size, clear_child_tid;
	struct lx_forksnap *snap;   /* fork: the parent's memory, to put back */
} lx_borrow;

enum { LX_P_FREE = 0, LX_P_ALIVE, LX_P_ZOMBIE };

struct lx_proc {
	int pid, ppid, pgid, sid;
	int state;
	int stopped;                /* stopped by a signal (job control) */
	int wait_status;            /* for wait4 */
	int wait_report;            /* 1: a stop, 2: a continue, 3: an exit, not yet collected */
	int exit_signal;            /* the signal sent to the parent at exit (SIGCHLD) */
	lx_mm *mm;
	lx_file *fd[LX_MAXFD];
	lx_u8 fdflags[LX_MAXFD];    /* FD_CLOEXEC */
	char cwd[LX_PATH_MAX];
	lx_u32 umask;
	struct lx_ksigaction sigact[LX_NSIG];
	lx_u32 sigpending[2];       /* for the process, not one thread */
	lx_thread *vfork_parent;    /* this is a vfork child, running on that thread */
	lx_tty *ctty;
	int nthreads;
	lx_s64 alarm_at;            /* ITIMER_REAL, microseconds, 0 = off */
	lx_s64 alarm_interval;
	lx_s64 start_time;
	lx_s64 cpu_us;              /* time its threads ran their own code (between calls) */
	lx_u32 uid, gid;
	char comm[16];
	char cmdline[256];          /* argv, NUL separated, for ps */
	int cmdline_len;
	int child_exited;           /* something changed among the children (wait4) */
	int exit_pending;           /* exit status + 1: to end it when its thread leaves the translator */
};

/* ---- the kernel state ---------------------------------------------------- */

typedef struct lx_kernel {
	lx_proc procs[LX_MAXPROC];
	lx_thread threads[LX_MAXTHREAD];
	lx_tty ttys[LX_MAXTTY];
	int next_pid;
	int init_exited;            /* the first program ended (the session is over) */
	int first_status;           /* the first program's pid */
	int first_exit;             /* ...and its wait status, when it has ended */
	int real_init;              /* the first program is pid 1 (Alpine mode's /sbin/init): it reaps orphans */
	lx_s64 boot_time;           /* realtime at start, in microseconds */
	lx_s64 start_mono;          /* bk_now_mono() at start */
	char root_native[LX_PATH_MAX];   /* e.g. D:\Linux\  or  /tmp/lxroot/ */
	char hostname[65];
	int trace;                  /* log every call (debugging) */
	int memlog;                 /* log each program's peak memory at its end */
	int execlog;                /* Alpine mode: 1 a status line every 30 s, 2 also each program run, with the time */
	int no_cache;               /* (debugging: no program cache) */
	lx_u32 reserve_refused;     /* growths the memory reserve refused (lx_reserve_ok) */
	lx_u32 reserve_refused_kb;  /* ...and how much they asked for */
	lx_u32 vdso[4];             /* AT_SYSINFO: entry, cas, gettp, barrier */
	lx_u32 random_seed;
} lx_kernel;

extern lx_kernel lx_k;

/* ---- the backend: EPOC (bk_epoc.cpp) or the host (bk_host.c) ---------------- */

/* threads */
int bk_thread_new(lx_thread *t);              /* make it, suspended; start runs lx_thread_start(t) */
void bk_thread_start(lx_thread *t);
void bk_thread_reply(lx_thread *t);           /* wake it: its call has an answer (t->reply) */
void bk_thread_kill(lx_thread *t);            /* end it now, wherever it is */
void bk_thread_suspend(lx_thread *t);
void bk_thread_resume(lx_thread *t);
void bk_thread_free(lx_thread *t);            /* after it has died */
/* ...and on the Linux thread itself */
void bk_thread_call(lx_thread *t);            /* post t's call to the kernel thread and wait for the answer */
void bk_thread_exit_self(lx_thread *t);       /* end this thread (LX_R_EXIT); never returns */
void bk_cas_yield(void);                      /* the compare-and-swap lock is held by another thread */
/* memory */
int bk_region_new(lx_region *r, lx_u32 size, int kind);
void bk_region_free(lx_region *r);
int bk_arena_grow(lx_u32 size);               /* make the arena this big (committed) */
int bk_code_region(lx_region *r, lx_u32 max, lx_u32 size);   /* Alpine mode's code cache: max of address space, size committed */
int bk_code_grow(lx_region *r, lx_u32 size);  /* commit this much of it */
lx_u32 bk_arena_base(void);
lx_u32 bk_arena_max(void);
void *bk_alloc(lx_u32 n);                     /* kernel memory (zeroed) */
void bk_free(void *p);
void bk_memcpy(void *d, const void *s, lx_u32 n);
void bk_memmove(void *d, const void *s, lx_u32 n);
void bk_memset(void *d, int c, lx_u32 n);
/* time, in microseconds */
lx_s64 bk_now_real(void);                     /* since 1970 */
lx_s64 bk_now_mono(void);                     /* since the runner started */
void bk_wake_at(lx_s64 mono);                 /* call lx_k_timer() at (or after) this time; 0 = never */
lx_u32 bk_random(void);
void bk_mem_info(lx_u32 *total, lx_u32 *free_);   /* the Psion's RAM, and how much is free */
lx_u32 bk_mem_reserve(void);                  /* bytes to leave free for EPOC's programs (Preferences), 0 = none */
/* terminals */
int bk_tty_write(lx_tty *tty, const char *buf, int n);   /* how much was taken; 0 = full (lx_k_tty_writable later) */
int bk_tty_room(lx_tty *tty);                 /* how much bk_tty_write would take now */
void bk_tty_status(lx_tty *tty, int running, int status);
/* logging */
void bk_log(const char *fmt, ...);

/* files on the backend's disks. Paths are native (EPOC: D:\Linux\x; host: /x) */
typedef struct bk_stat {
	int is_dir;
	lx_s64 size;
	lx_s64 mtime;               /* microseconds since 1970 */
	int readonly;
} bk_stat;
void *bk_file_open(const char *path, int flags, int *err);   /* LX_O_* flags */
int bk_file_read(void *h, lx_s64 off, char *buf, lx_u32 n);
int bk_file_write(void *h, lx_s64 off, const char *buf, lx_u32 n);
int bk_file_size(void *h, lx_s64 *size);
int bk_file_truncate(void *h, lx_s64 size);
int bk_file_sync(void *h);
void bk_file_close(void *h);
int bk_stat_path(const char *path, bk_stat *st);
int bk_mkdir(const char *path);
int bk_rmdir(const char *path);
int bk_unlink(const char *path);
int bk_rename(const char *from, const char *to);
int bk_set_mtime(const char *path, lx_s64 mtime);
/* directories: bk_dir_next gives one name at a time, NULL at the end */
void *bk_dir_open(const char *path, int *err);
const char *bk_dir_next(void *d, bk_stat *st);
void bk_dir_close(void *d);
int bk_disk_info(const char *path, lx_u64 *size, lx_u64 *free_);
int bk_drive_list(char *drives);              /* "CD" etc. (EPOC), "" on the host */
char bk_native_sep(void);                     /* '\\' or '/' */
/* sockets (lx_net.c) */
#include "lx_bknet.h"

/* ---- the kernel's entry points (called by the backend, on the kernel thread) --- */

void lx_k_init(const char *root_native);
int lx_k_spawn_first(const char *path, char *const argv[], char *const envp[], int tty);
void lx_k_syscall(lx_thread *t);              /* t posted a call (t->frame) */
void lx_k_timer(void);
void lx_k_tty_input(lx_tty *tty, const char *buf, int n);
void lx_k_tty_writable(lx_tty *tty);
void lx_k_tty_resize(lx_tty *tty, int rows, int cols);
void lx_k_tty_hangup(lx_tty *tty);
void lx_k_thread_died(lx_thread *t, int reason);   /* it faulted or was killed outside the kernel */
/* lx_rand.c: the kernel's random numbers (getrandom, /dev/urandom, AT_RANDOM) */
void lx_rand_bytes(void *buf, lx_u32 n);
void lx_rand_stir(const void *p, lx_u32 n);     /* (what is written to /dev/urandom) */
void lx_rand_event(lx_u32 what);                /* (a key, a network event: its time) */
void lx_k_event(void);                        /* something changed (a socket...): retry the parked calls */
void lx_k_shutdown(void);

/* the kernel's own helpers, shared between its files */
lx_s64 lx_now(void);
lx_thread *lx_thread_by_sp(lx_u32 sp);
void lx_reply(lx_thread *t, int r);
void lx_retry_parked(void);
int lx_uaccess(lx_thread *t, lx_u32 addr, lx_u32 len);  /* 1 if the range is the program's */
int lx_ustr(lx_thread *t, lx_u32 addr, char *out, int max);  /* copy a string in: length or -errno */
lx_proc *lx_proc_by_pid(int pid);
void lx_signal_proc(lx_proc *p, int sig);
void lx_signal_pgrp(int pgrp, int sig);
void lx_signal_thread(lx_thread *t, int sig);
void lx_proc_exit(lx_proc *p, int status);    /* status: the wait status */
int lx_fd_alloc(lx_proc *p, lx_file *f, int min, int cloexec);
lx_file *lx_fd_get(lx_proc *p, int fd);
lx_file *lx_file_new(const lx_fops *ops, void *priv, int flags);
void lx_file_put(lx_file *f);
lx_file *lx_file_get(lx_file *f);
int lx_snprintf(char *buf, int size, const char *fmt, ...);
void lx_strlcpy(char *d, const char *s, int size);
int lx_strlen(const char *s);
int lx_strcmp(const char *a, const char *b);
int lx_strncmp(const char *a, const char *b, int n);
char *lx_strdup(const char *s);

lx_thread *lx_thread_for_exec(lx_proc *p);   /* a new thread for p (lx_elf.c) */

/* lx_simg.c */
lx_simg *lx_simg_find(const char *path, lx_s64 size, lx_u64 ino, lx_s64 mtime, int lib);
lx_simg *lx_simg_add(const char *path, lx_s64 size, lx_u64 ino, lx_s64 mtime, int lib, lx_region *reg, lx_u32 base, lx_u32 len, lx_u32 rw_lo, lx_u32 rw_hi);
void lx_simg_trim(int all);
lx_u32 lx_simg_unused_bytes(void);
lx_u32 lx_exec_cache_bytes(void);
void lx_exec_cache_drop(void);
void lx_mem_reclaim(void);                    /* every cache gives its memory back */
lx_u32 lx_mem_cached(void);                   /* memory the caches hold, which they give back when it is needed */
int lx_mm_share(lx_mm *mm, lx_simg *s);
void lx_mm_unshare(lx_mm *mm, lx_simg *s);
void lx_mm_unshare_all(lx_mm *mm);
int lx_mm_is_resident(lx_mm *mm);
int lx_reserve_ok(lx_u32 grow);               /* may Linux take grow more bytes of RAM (the reserve)? */
int lx_reserve_check(lx_u32 grow, int may_drop);
lx_u32 lx_mem_avail(void);                    /* free RAM less the reserve */
const char *lx_root_missing(void);            /* a part of the root image that is not there, or 0 */
int lx_region_new(lx_region *r, lx_u32 size, int kind);   /* bk_region_new, within the reserve */
void lx_mm_resident(lx_mm *mm);
void lx_mm_write(lx_mm *mm, lx_u32 addr, const void *src, lx_u32 len);
void lx_simg_stats(char *buf, int size);

/* lx_mem.c */
lx_u32 lx_arena_alloc(lx_u32 len);
void lx_arena_free(lx_u32 addr, lx_u32 len);
lx_mm *lx_mm_new(void);
void lx_mm_put(lx_mm *mm);
int lx_sys_mmap(lx_thread *t, lx_u32 *a);
int lx_sys_munmap(lx_thread *t, lx_u32 *a);
int lx_sys_mremap(lx_thread *t, lx_u32 *a);
int lx_sys_msync(lx_thread *t, lx_u32 *a);
int lx_mm_unmap(lx_mm *mm, lx_u32 addr, lx_u32 len);
int lx_fork_holds(lx_mm *mm, lx_u32 addr, lx_u32 len);   /* a fork's snapshot covers this mapping */

/* lx_elf.c */
int lx_exec_load(lx_thread *t, lx_proc *p, const char *path, char **argv, int argc, char **envp, int envc, lx_thread **nt);

/* lx_vfs.c */
int lx_vfs_init(void);
lx_file *lx_vfs_open(lx_proc *p, int dirfd, const char *path, int flags, lx_u32 mode, int *err);
int lx_vfs_stat(lx_proc *p, int dirfd, const char *path, int follow, struct lx_stat64 *st);
int lx_vfs_readlink(lx_proc *p, int dirfd, const char *path, char *buf, int size);
int lx_vfs_resolve(lx_proc *p, int dirfd, const char *path, int follow, char *out);
int lx_fs_syscall(lx_thread *t, lx_u32 nr, lx_u32 *a);   /* LX_NOSYS if not a file call */
#define LX_NOSYS (-100002)
int lx_pipe_new(lx_file **r, lx_file **w);
lx_file *lx_tty_file(lx_tty *tty);
void lx_tty_init(lx_tty *tty, int index);
int lx_tty_ioctl(lx_thread *t, lx_tty *tty, lx_u32 req, lx_u32 arg);

/* lx_net.c */
int lx_net_syscall(lx_thread *t, lx_u32 nr, lx_u32 *a);  /* LX_NOSYS if not a socket call */

/* lx_entry.S (the program's side) */
void lx_trap(lx_u32 *frame);
void lx_resume(const lx_u32 *ctx);
void lx_resume_copy(const lx_u32 *ctx, lx_u32 dst, const lx_u32 *src, lx_u32 len);
extern lx_u32 lx_entry_sys, lx_entry_cas, lx_entry_gettp, lx_entry_barrier;
void lx_thread_start(lx_thread *t);           /* on the new thread: jump into the program */

#ifdef __cplusplus
}
#endif
#endif
