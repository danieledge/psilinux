/* lx_core.c - PsiLinux's kernel: processes, threads, system call dispatch,
 * signals, vfork/clone/exec/exit/wait and job control. Runs on the kernel
 * thread only (see lx_core.h). Files are lx_vfs.c, memory lx_mem.c, program
 * loading lx_elf.c, terminals lx_tty.c, sockets lx_net.c. */
#include "lx_core.h"
#include "lx_dbt.h"
#include <stdarg.h>

lx_kernel lx_k;

/* ---- a call's registers ----------------------------------------------------
 * A native thread's frame (lx_entry.S) is r0-r12 and the return address, on
 * its own stack, with sp just above it. A translated thread's (Alpine mode,
 * lx_dbt.h) is r0-r12, sp, lr, pc and the flags, in the thread. */
static int fr_words(lx_thread *t) { return t->dbt ? DF_WORDS : LX_FRAME_WORDS; }
static lx_u32 fr_sp(lx_thread *t) { return t->dbt ? t->frame[DF_SP] : (lx_u32)t->frame + LX_FRAME_WORDS * 4; }
static lx_u32 fr_pc(lx_thread *t) { return t->dbt ? t->frame[DF_PC] : t->frame[LX_FR_LR]; }
static lx_u32 fr_lr(lx_thread *t) { return t->dbt ? t->frame[DF_LR] : t->frame[LX_FR_LR]; }
static lx_u32 fr_flags(lx_thread *t) { return t->dbt ? (t->frame[DF_CPSR] & 0xF8000000u) : 0; }

extern lx_u32 lx_sigtramp;
static int need_retry;
static int retrying;
static int in_call;             /* a call is being handled: retries wait until it is done */

#define SIGBIT(s) (1u << (((s) - 1) & 31))
#define SIGWORD(s) (((s) - 1) >> 5)
#define SIGHAS(set, s) ((set)[SIGWORD(s)] & SIGBIT(s))
#define SIGADD(set, s) ((set)[SIGWORD(s)] |= SIGBIT(s))
#define SIGDEL(set, s) ((set)[SIGWORD(s)] &= ~SIGBIT(s))

/* ---- small helpers ------------------------------------------------------ */

int lx_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
int lx_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (unsigned char)*a - (unsigned char)*b;
}
int lx_strncmp(const char *a, const char *b, int n)
{
	while (n > 0 && *a && *a == *b) { a++; b++; n--; }
	return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
void lx_strlcpy(char *d, const char *s, int size)
{
	int i = 0;
	if (size <= 0) return;
	while (s[i] && i < size - 1) { d[i] = s[i]; i++; }
	d[i] = 0;
}
char *lx_strdup(const char *s)
{
	int n = lx_strlen(s);
	char *d = (char *)bk_alloc(n + 1);
	if (d) bk_memcpy(d, s, n + 1);
	return d;
}

/* a small snprintf: %d %u %x %s %c %lld %llu %%, with widths and '0' */
static int fmt_num(char *tmp, unsigned long long v, int base, int neg)
{
	int n = 0, i;
	char rev[24];
	do { int d = (int)(v % base); rev[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v);
	i = 0;
	if (neg) tmp[i++] = '-';
	while (n) tmp[i++] = rev[--n];
	return i;
}

static int lx_vsnprintf(char *buf, int size, const char *fmt, va_list ap)
{
	int o = 0;
#define PUT(c) do { if (o < size - 1) buf[o] = (c); o++; } while (0)
	while (*fmt) {
		char c = *fmt++;
		int width = 0, zero = 0, ll = 0, left = 0, i, n;
		char tmp[32];
		const char *s;
		if (c != '%') { PUT(c); continue; }
		if (*fmt == '-') { left = 1; fmt++; }
		if (*fmt == '0') { zero = 1; fmt++; }
		while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l') { ll++; fmt++; }
		c = *fmt++;
		s = tmp; n = 0;
		switch (c) {
		case 'd': {
			long long v = ll >= 2 ? va_arg(ap, long long) : va_arg(ap, int);
			n = fmt_num(tmp, v < 0 ? -(unsigned long long)v : (unsigned long long)v, 10, v < 0);
			break; }
		case 'u': {
			unsigned long long v = ll >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
			n = fmt_num(tmp, v, 10, 0); break; }
		case 'o': {
			unsigned long long v = ll >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
			n = fmt_num(tmp, v, 8, 0); break; }
		case 'x': case 'p': {
			unsigned long long v = ll >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
			n = fmt_num(tmp, v, 16, 0); break; }
		case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; n = lx_strlen(s); break;
		case 'c': tmp[0] = (char)va_arg(ap, int); n = 1; break;
		case '%': tmp[0] = '%'; n = 1; break;
		default: tmp[0] = c; n = 1; break;
		}
		if (!left) for (i = n; i < width; i++) PUT(zero ? '0' : ' ');
		for (i = 0; i < n; i++) PUT(s[i]);
		if (left) for (i = n; i < width; i++) PUT(' ');
	}
	if (size > 0) buf[o < size ? o : size - 1] = 0;
#undef PUT
	return o;
}

int lx_vsnprintf_pub(char *buf, int size, const char *fmt, va_list ap)
{
	return lx_vsnprintf(buf, size, fmt, ap);
}

int lx_snprintf(char *buf, int size, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = lx_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

lx_s64 lx_now(void) { return bk_now_mono(); }

/* ---- user memory --------------------------------------------------------- */

/* Is [addr, addr+len) memory of the calling program? (There is no MMU to
 * fault for us, and a fault on the kernel thread would take every program
 * down: so pointers are checked against what the program owns.) */
int lx_uaccess(lx_thread *t, lx_u32 addr, lx_u32 len)
{
	lx_mm *mm = t->proc ? t->proc->mm : 0;
	lx_map *m;
	int i;
	lx_u32 end = addr + len;
	if (len == 0) return 1;
	if (end < addr) return 0;
	if (mm) {
		if (addr >= mm->image.base && end <= mm->image.base + mm->image.size) return 1;
		if (mm->interp.size && addr >= mm->interp.base && end <= mm->interp.base + mm->interp.size) return 1;
		for (m = mm->maps; m; m = m->next)
			if (addr >= m->addr && end <= m->addr + m->len) return 1;
	}
	/* any thread's stack (a vfork child writes into its parent's) */
	for (i = 0; i < LX_MAXTHREAD; i++) {
		lx_thread *o = &lx_k.threads[i];
		if (o->state == LX_T_FREE || o->state == LX_T_DEAD) continue;
		if (o->stack.size && addr >= o->stack.base && end <= o->stack.base + o->stack.size) return 1;
	}
	return 0;
}

int lx_ustr(lx_thread *t, lx_u32 addr, char *out, int max)
{
	int i;
	if (!lx_uaccess(t, addr, 1)) return -LX_EFAULT;
	for (i = 0; i < max; i++) {
		if (!lx_uaccess(t, addr + i, 1)) return -LX_EFAULT;
		out[i] = ((const char *)addr)[i];
		if (!out[i]) return i;
	}
	return -LX_ENAMETOOLONG;
}

/* ---- threads -------------------------------------------------------------- */

static int count_threads(lx_proc *p)
{
	int i, n = 0;
	for (i = 0; i < LX_MAXTHREAD; i++) {
		lx_thread *t = &lx_k.threads[i];
		if (t->state != LX_T_FREE && t->state != LX_T_DEAD && t->owner == p) n++;
	}
	return n;
}

static lx_thread *thread_new(lx_proc *p, int tid)
{
	int i;
	for (i = 0; i < LX_MAXTHREAD; i++) {
		lx_thread *t = &lx_k.threads[i];
		if (t->state == LX_T_FREE) {
			bk_memset(t, 0, sizeof *t);
			t->state = LX_T_USER;
			t->tid = tid;
			t->proc = t->owner = p;
			return t;
		}
	}
	return 0;
}

/* the thread whose stack holds sp (lx_trap and gettp, on the Linux thread) */
lx_thread *lx_thread_by_sp(lx_u32 sp)
{
	static lx_thread *last;
	lx_thread *best = 0;
	int i;
	lx_thread *t = last;
	if (t && t->state != LX_T_FREE && sp >= t->stack_lo && sp < t->stack_hi) return t;
	for (i = 0; i < LX_MAXTHREAD; i++) {
		t = &lx_k.threads[i];
		if (t->state == LX_T_FREE || t->state == LX_T_DEAD) continue;
		if (sp >= t->stack_lo && sp < t->stack_hi) {
			if (!best || t->stack_hi < best->stack_hi) best = t;
		}
	}
	if (best) last = best;
	return best;
}

lx_u32 lx_gettp_c(lx_u32 sp)
{
	lx_thread *t = lx_thread_by_sp(sp);
	return t ? t->tp : 0;
}

/* ---- the CPU (Alpine mode) ---------------------------------------------------
 * One translated thread runs guest code at a time: it holds lx_cpu.cur
 * (lx_dbt.h). The kernel hands the CPU over when the holder's call waits,
 * when it ends, and when another thread has waited a time slice and the
 * holder makes a call or reaches the dispatcher (lx_cpu.yield). A thread
 * whose call is answered while another holds the CPU waits for it
 * (cpu_wait), in order. Native threads never wait for it. */
#define CPU_SLICE 40000                 /* microseconds */
static lx_u32 cpu_order;

static void cpu_give(lx_thread *t)
{
	int start = t->cpu_wait == 2;
	lx_cpu.cur = t;
	lx_cpu.yield = 0;
	lx_cpu.critical = 0;
	if (t->proc) lx_mm_resident(t->proc->mm);   /* (its writable parts of shared images in place: lx_simg.c) */
	t->cpu_wait = 0;
	t->slice_start = lx_now();
	if (start) bk_thread_start(t);
	else bk_thread_reply(t);
}

static int deferred;                    /* parked calls not retried: their memory was not in place */

static int cpu_waiters(void)
{
	int i;
	if (deferred) return 1;
	for (i = 0; i < LX_MAXTHREAD; i++) if (lx_k.threads[i].cpu_wait) return 1;
	return 0;
}

/* Can t's parked call be tried now? Its process's writable parts of shared
 * images must be in place (it may write into them), and they can be put in
 * place only while no translated thread is running guest code. */
static int retry_ready(lx_thread *t)
{
	lx_mm *mm = t->proc ? t->proc->mm : 0;
	if (!t->dbt || !mm || lx_mm_is_resident(mm)) return 1;
	if (lx_cpu.cur && lx_cpu.cur->state == LX_T_USER) { deferred = 1; return 0; }
	lx_mm_resident(mm);
	return 1;
}

/* the CPU is free: the thread that has waited longest gets it */
static void cpu_next(void)
{
	int i;
	lx_thread *best = 0;
	if (lx_cpu.cur) return;
	if (deferred && !retrying) {
		/* calls that could not be tried while guest code ran: now they can */
		need_retry = 1;
		lx_retry_parked();
		if (lx_cpu.cur) return;
	}
	for (i = 0; i < LX_MAXTHREAD; i++) {
		lx_thread *t = &lx_k.threads[i];
		if (!t->cpu_wait || t->state == LX_T_FREE || t->state == LX_T_DEAD) continue;
		if (t->proc && t->proc->stopped) continue;
		if (!best || (lx_s32)(t->cpu_seq - best->cpu_seq) < 0) best = t;
	}
	if (best) cpu_give(best);
}

/* t gives the CPU up (its call waits, it ends, it stops) */
static void cpu_drop(lx_thread *t)
{
	if (lx_cpu.cur != t) return;
	lx_cpu.cur = 0;
	lx_cpu.yield = 0;
	cpu_next();
}

/* t may run guest code now (a reply, or start = 2): at once if the CPU is
 * free or already t's, or when it comes round */
static void cpu_run(lx_thread *t, int start)
{
	if (lx_cpu.cur == t && cpu_waiters() && lx_now() - t->slice_start >= CPU_SLICE) {
		/* its slice is over and others wait: to the back of the queue */
		lx_cpu.cur = 0;
		t->cpu_wait = start ? 2 : 1;
		t->cpu_seq = ++cpu_order;
		cpu_next();
		return;
	}
	if (!lx_cpu.cur || lx_cpu.cur == t) { t->cpu_wait = start ? 2 : 1; cpu_give(t); return; }
	t->cpu_wait = start ? 2 : 1;
	t->cpu_seq = ++cpu_order;
}

/* starts a new thread (native at once; translated when it has the CPU) */
static void thread_go(lx_thread *t)
{
	if (t->dbt) cpu_run(t, 2);
	else bk_thread_start(t);
}

/* wakes a thread whose call is answered */
static void thread_wake(lx_thread *t)
{
	if (t->dbt) cpu_run(t, 0);
	else bk_thread_reply(t);
}

/* The Linux thread's side of a call (lx_entry.S calls this on the program's
 * stack). */
void lx_trap(lx_u32 *frame)
{
	lx_thread *t = lx_thread_by_sp((lx_u32)frame);
	if (!t) {
		frame[0] = (lx_u32)-LX_ENOSYS;   /* (cannot happen: every thread is known) */
		return;
	}
	t->frame = frame;
	bk_thread_call(t);
	if (t->reply == LX_R_RESUME) {
		if (t->sigframe_len) {
			lx_u32 n = t->sigframe_len;
			t->sigframe_len = 0;
			lx_resume_copy(t->ctx, t->sigframe_at, (const lx_u32 *)t->sigframe_src, n);
		}
		lx_resume(t->ctx);
	}
	if (t->reply == LX_R_EXIT) bk_thread_exit_self(t);
}

void lx_thread_start(lx_thread *t)
{
	if (t->dbt) dbt_thread_start(t);      /* (never returns) */
	if (t->sigframe_len) {
		/* (a parent going on in a new thread: its stack as it was) */
		lx_u32 n = t->sigframe_len;
		t->sigframe_len = 0;
		lx_resume_copy(t->ctx, t->sigframe_at, (const lx_u32 *)t->sigframe_src, n);
	}
	lx_resume(t->ctx);
}

/* ---- processes ------------------------------------------------------------ */

lx_proc *lx_proc_by_pid(int pid)
{
	int i;
	if (pid <= 0) return 0;
	for (i = 0; i < LX_MAXPROC; i++)
		if (lx_k.procs[i].state != LX_P_FREE && lx_k.procs[i].pid == pid) return &lx_k.procs[i];
	return 0;
}

static lx_thread *main_thread(lx_proc *p)
{
	int i;
	for (i = 0; i < LX_MAXTHREAD; i++) {
		lx_thread *t = &lx_k.threads[i];
		if (t->state != LX_T_FREE && t->state != LX_T_DEAD && t->owner == p) return t;
	}
	return 0;
}

static int pid_in_use(int pid)
{
	int i;
	for (i = 0; i < LX_MAXPROC; i++) {
		lx_proc *p = &lx_k.procs[i];
		if (p->state == LX_P_FREE) continue;
		if (p->pid == pid || p->pgid == pid || p->sid == pid) return 1;
	}
	return 0;
}

static lx_proc *proc_new(lx_proc *parent)
{
	int i, pid;
	lx_proc *p = 0;
	for (i = 0; i < LX_MAXPROC; i++)
		if (lx_k.procs[i].state == LX_P_FREE) { p = &lx_k.procs[i]; break; }
	if (!p) return 0;
	do {
		pid = lx_k.next_pid++;
		if (lx_k.next_pid > 32000) lx_k.next_pid = 2;
	} while (pid_in_use(pid));
	bk_memset(p, 0, sizeof *p);
	p->pid = pid;
	p->state = LX_P_ALIVE;
	p->exit_signal = LX_SIGCHLD;
	p->start_time = lx_now();
	if (parent) {
		p->ppid = parent->pid;
		p->pgid = parent->pgid;
		p->sid = parent->sid;
		p->ctty = parent->ctty;
		p->umask = parent->umask;
		p->uid = parent->uid;
		p->gid = parent->gid;
		bk_memcpy(p->cwd, parent->cwd, sizeof p->cwd);
		bk_memcpy(p->sigact, parent->sigact, sizeof p->sigact);
		bk_memcpy(p->comm, parent->comm, sizeof p->comm);
		bk_memcpy(p->cmdline, parent->cmdline, sizeof p->cmdline);
		p->cmdline_len = parent->cmdline_len;
		for (i = 0; i < LX_MAXFD; i++)
			if (parent->fd[i]) { p->fd[i] = lx_file_get(parent->fd[i]); p->fdflags[i] = parent->fdflags[i]; }
	} else {
		p->umask = 022;
		lx_strlcpy(p->cwd, "/", sizeof p->cwd);
	}
	return p;
}

static void proc_free(lx_proc *p)
{
	p->state = LX_P_FREE;
	p->pid = 0;
}

/* ---- answers ----------------------------------------------------------------- */

static int deliver_signal(lx_thread *t);
static void stop_proc(lx_proc *p, int sig);

static void schedule_timer(void)
{
	lx_s64 next = 0;
	int i;
	for (i = 0; i < LX_MAXTHREAD; i++) {
		lx_thread *t = &lx_k.threads[i];
		if (t->state == LX_T_PARKED && t->deadline && (!next || t->deadline < next)) next = t->deadline;
	}
	for (i = 0; i < LX_MAXPROC; i++) {
		lx_proc *p = &lx_k.procs[i];
		if (p->state == LX_P_ALIVE && p->alarm_at && (!next || p->alarm_at < next)) next = p->alarm_at;
	}
	if (lx_cpu.cur && !lx_cpu.yield && cpu_waiters()) {
		/* the end of the holder's slice: ask it to give way */
		lx_s64 at = lx_cpu.cur->slice_start + CPU_SLICE;
		if (!next || at < next) next = at;
	}
	bk_wake_at(next);
}

/* Answers t's call with r, delivering a signal on the way if one is due. */
void lx_reply(lx_thread *t, int r)
{
	t->frame[0] = (lx_u32)r;
	t->reply = LX_R_RET;
	t->deadline = 0;
	t->timed_out = 0;
	t->intr = 0;
	if (lx_k.trace) bk_log("[%d] = %d", t->tid, r);
	if (deliver_signal(t)) return;            /* the process ended */
	if (t->sigmask_saved) {                   /* rt_sigsuspend & co: the old mask is back */
		t->sigmask[0] = t->saved_sigmask[0];
		t->sigmask[1] = t->saved_sigmask[1];
		t->sigmask_saved = 0;
	}
	if (t->proc->stopped) {
		if (lx_k.trace) bk_log("[%d] stopped", t->tid);
		t->state = LX_T_STOPPED;          /* answered when it continues */
		if (t->dbt) cpu_drop(t);
		return;
	}
	t->state = LX_T_USER;
	t->ran_since = lx_now();
	thread_wake(t);
}

/* Goes on to ctx (all registers) instead of returning from the call. */
static void reply_resume(lx_thread *t)
{
	t->reply = LX_R_RESUME;
	t->deadline = 0;
	t->state = LX_T_USER;
	t->ran_since = lx_now();
	thread_wake(t);
}

static void reply_exit(lx_thread *t)
{
	t->reply = LX_R_EXIT;
	t->state = LX_T_DEAD;
	t->cpu_wait = 0;
	if (t->dbt) cpu_drop(t);              /* (it ends without running guest code) */
	bk_thread_reply(t);
}

/* ---- dispatch -------------------------------------------------------------- */

static int do_syscall(lx_thread *t, lx_u32 nr, lx_u32 *a);

static int restartable(lx_u32 nr)
{
	switch (nr) {
	case LX_NR_nanosleep: case LX_NR_clock_nanosleep: case LX_NR_clock_nanosleep_time64:
	case LX_NR_poll: case LX_NR_ppoll: case LX_NR_ppoll_time64:
	case LX_NR__newselect: case LX_NR_pselect6: case LX_NR_pselect6_time64:
	case LX_NR_rt_sigsuspend: case LX_NR_pause: case LX_NR_rt_sigtimedwait: case LX_NR_rt_sigtimedwait_time64:
		return 0;
	}
	return 1;
}

static void dispatch(lx_thread *t)
{
	int r;
	t->state = LX_T_SYSCALL;
	if (t->intr) {
		/* a signal came while it waited: EINTR, or (SA_RESTART) the
		 * handler runs and the call is made again */
		int s, sig = 0;
		lx_proc *p = t->proc;
		t->intr = 0;
		for (s = 1; s < LX_NSIG; s++)
			if ((SIGHAS(p->sigpending, s) || SIGHAS(t->sigpending, s)) && !SIGHAS(t->sigmask, s)) { sig = s; break; }
		if (sig) {
			struct lx_ksigaction *sa = &p->sigact[sig];
			if (sa->handler > LX_SIG_IGN && (sa->flags & LX_SA_RESTART) && restartable(t->nr)) {
				t->restart = 1;
				t->restart_r0 = t->frame[0];
				lx_reply(t, (int)t->frame[0]);
				return;
			}
			lx_reply(t, -LX_EINTR);
			return;
		}
		/* (the signal went away: carry on waiting) */
	}
	in_call++;
	r = do_syscall(t, t->nr, t->frame);
	in_call--;
	if (r == LX_NOREPLY) return;
	if (r == LX_BLOCK) {
		if (lx_k.trace && t->state != LX_T_PARKED) bk_log("[%d] waits", t->tid);
		t->state = LX_T_PARKED;
		if (t->dbt) cpu_drop(t);
		return;
	}
	if (r == -LX_ERESTARTSYS) r = -LX_EINTR;
	lx_reply(t, r);
}

void lx_retry_parked(void)
{
	int i, rounds = 0;
	if (retrying) { need_retry = 1; return; }
	retrying = 1;
	do {
		need_retry = 0;
		deferred = 0;
		for (i = 0; i < LX_MAXTHREAD; i++) {
			lx_thread *t = &lx_k.threads[i];
			if (t->state == LX_T_PARKED && retry_ready(t)) dispatch(t);
		}
	} while (need_retry && ++rounds < 50);
	retrying = 0;
	/* the holder's writable parts back in place, if a retry moved them */
	if (lx_cpu.cur && lx_cpu.cur->proc) lx_mm_resident(lx_cpu.cur->proc->mm);
	schedule_timer();
}

void lx_k_event(void)
{
	need_retry = 1;
	if (!retrying && !in_call) lx_retry_parked();
}

void lx_k_syscall(lx_thread *t)
{
	if (!t->frame) return;
	t->nr = t->dbt_kcall ? (lx_u32)t->dbt_kcall : t->frame[7];
	if (t->dbt && t->proc && t->proc->exit_pending) {
		/* it was to end while it was in the translator (lx_proc_exit) */
		int st = t->proc->exit_pending - 1;
		t->state = LX_T_SYSCALL;
		t->proc->exit_pending = 0;
		lx_proc_exit(t->proc, st);
		if (need_retry) lx_retry_parked(); else schedule_timer();
		return;
	}
	t->syscalls++;
	if (t->fork_store.size) bk_region_free(&t->fork_store);
	if (t->ran_since && t->proc) { t->proc->cpu_us += lx_now() - t->ran_since; t->ran_since = 0; }
	if (lx_k.trace) bk_log("[%d] %u(%x, %x, %x, %x)", t->tid, t->nr, t->frame[0], t->frame[1], t->frame[2], t->frame[3]);
	if (t->dbt && lx_cpu.trace_note) lx_cpu.trace_note(t, "syscall");

	dispatch(t);
	if (need_retry) lx_retry_parked();
	else schedule_timer();
}

void lx_k_timer(void)
{
	lx_s64 now = lx_now();
	int i;
	if (lx_cpu.cur && cpu_waiters() && now - lx_cpu.cur->slice_start >= CPU_SLICE) lx_cpu.yield = 1;
	for (i = 0; i < LX_MAXPROC; i++) {
		lx_proc *p = &lx_k.procs[i];
		if (p->state == LX_P_ALIVE && p->alarm_at && p->alarm_at <= now) {
			p->alarm_at = p->alarm_interval ? now + p->alarm_interval : 0;
			lx_signal_proc(p, LX_SIGALRM);
		}
	}
	for (i = 0; i < LX_MAXTHREAD; i++) {
		lx_thread *t = &lx_k.threads[i];
		if (t->state == LX_T_PARKED && t->deadline && t->deadline <= now) t->timed_out = 1;
	}
	need_retry = 1;
	lx_retry_parked();
}

/* A parked call asks to wait until 'until' (microseconds of lx_now). */
static int park_until(lx_thread *t, lx_s64 until)
{
	if (t->timed_out) { t->timed_out = 0; t->deadline = 0; return 1; }
	t->deadline = until;
	return 0;
}

/* ---- signals ---------------------------------------------------------------- */

static int sig_default_ignored(int sig)
{
	return sig == LX_SIGCHLD || sig == LX_SIGURG || sig == LX_SIGWINCH || sig == LX_SIGCONT;
}
static int sig_default_stops(int sig)
{
	return sig == LX_SIGSTOP || sig == LX_SIGTSTP || sig == LX_SIGTTIN || sig == LX_SIGTTOU;
}
static int sig_core(int sig)
{
	return sig == LX_SIGQUIT || sig == LX_SIGILL || sig == LX_SIGTRAP || sig == LX_SIGABRT ||
		sig == LX_SIGBUS || sig == LX_SIGFPE || sig == LX_SIGSEGV || sig == LX_SIGSYS;
}

static void wake_parent(lx_proc *p)
{
	lx_proc *pp = lx_proc_by_pid(p->ppid);
	if (pp) {
		pp->child_exited = 1;
		need_retry = 1;
	}
}

static void stop_proc(lx_proc *p, int sig)
{
	int i;
	lx_proc *pp;
	if (p->stopped) return;
	p->stopped = sig;
	p->wait_status = 0x7f | (sig << 8);
	p->wait_report = 1;
	for (i = 0; i < LX_MAXTHREAD; i++) {
		lx_thread *t = &lx_k.threads[i];
		if (t->state == LX_T_FREE || t->state == LX_T_DEAD || t->owner != p) continue;
		if (t->proc != p) continue;          /* lent to a vfork child */
		if (t->state != LX_T_USER) continue;
		if (t->dbt) { if (lx_cpu.cur == t) lx_cpu.yield = 1; }   /* (it stops at its next call or indirect branch) */
		else bk_thread_suspend(t);
	}
	pp = lx_proc_by_pid(p->ppid);
	if (pp && !(pp->sigact[LX_SIGCHLD].flags & LX_SA_NOCLDSTOP)) lx_signal_proc(pp, LX_SIGCHLD);
	wake_parent(p);
}

static void continue_proc(lx_proc *p)
{
	int i;
	if (!p->stopped) return;
	p->stopped = 0;
	p->wait_status = 0xffff;
	p->wait_report = 2;
	for (i = 0; i < LX_MAXTHREAD; i++) {
		lx_thread *t = &lx_k.threads[i];
		if (t->state == LX_T_FREE || t->state == LX_T_DEAD || t->owner != p || t->proc != p) continue;
		if (t->state == LX_T_STOPPED) { t->state = LX_T_USER; thread_wake(t); }
		else if (t->state == LX_T_USER && !t->dbt) bk_thread_resume(t);
	}
	cpu_next();
	wake_parent(p);
}

/* the threads that run as p now (its own, or the one a vfork child borrows) */
static lx_thread *proc_thread(lx_proc *p)
{
	int i;
	for (i = 0; i < LX_MAXTHREAD; i++) {
		lx_thread *t = &lx_k.threads[i];
		if (t->state == LX_T_FREE || t->state == LX_T_DEAD) continue;
		if (t->proc == p) return t;
	}
	return 0;
}

void lx_signal_thread(lx_thread *t, int sig)
{
	SIGADD(t->sigpending, sig);
	lx_signal_proc(t->proc, 0);
}

/* Sends sig to p (0: just look at what is pending). */
void lx_signal_proc(lx_proc *p, int sig)
{
	struct lx_ksigaction *sa;
	lx_thread *t;
	if (!p || p->state != LX_P_ALIVE) return;
	if (sig) {
		if (sig == LX_SIGKILL) { lx_proc_exit(p, LX_SIGKILL); return; }
		if (sig == LX_SIGCONT) {
			p->sigpending[0] &= ~(SIGBIT(LX_SIGSTOP) | SIGBIT(LX_SIGTSTP) | SIGBIT(LX_SIGTTIN) | SIGBIT(LX_SIGTTOU));
			continue_proc(p);
		}
		if (sig_default_stops(sig)) SIGDEL(p->sigpending, LX_SIGCONT);
		sa = &p->sigact[sig];
		if (sa->handler == LX_SIG_IGN && sig != LX_SIGSTOP) return;
		if (sa->handler == LX_SIG_DFL && sig_default_ignored(sig)) return;
		if (sig == LX_SIGSTOP) { stop_proc(p, sig); return; }
		SIGADD(p->sigpending, sig);
	}
	/* who takes it? a thread in a call is interrupted; a thread running the
	 * program's code takes it at its next call, unless the default action
	 * ends or stops the process: that is done now */
	t = proc_thread(p);
	if (!t) return;
	{
		int s, any = 0;
		for (s = 1; s < LX_NSIG; s++) {
			if (!(SIGHAS(p->sigpending, s) || SIGHAS(t->sigpending, s))) continue;
			if (SIGHAS(t->sigmask, s)) continue;
			any = s;
			break;
		}
		if (!any) return;
		sa = &p->sigact[any];
		if (sa->handler == LX_SIG_DFL && sig_default_stops(any)) {
			SIGDEL(p->sigpending, any);
			SIGDEL(t->sigpending, any);
			stop_proc(p, any);
			return;
		}
		if (t->state == LX_T_PARKED) {
			t->intr = 1;
			need_retry = 1;
		} else if (t->state == LX_T_USER && sa->handler == LX_SIG_DFL) {
			SIGDEL(p->sigpending, any);
			SIGDEL(t->sigpending, any);
			lx_proc_exit(p, any | (sig_core(any) ? 0x80 : 0));
		}
		/* a handled signal for a thread in its own code waits for its next call */
	}
}

void lx_signal_pgrp(int pgrp, int sig)
{
	int i;
	if (pgrp <= 0) return;
	for (i = 0; i < LX_MAXPROC; i++) {
		lx_proc *p = &lx_k.procs[i];
		if (p->state == LX_P_ALIVE && p->pgid == pgrp) lx_signal_proc(p, sig);
	}
}

/* The kernel's ARM signal frame: siginfo (128 bytes), then the ucontext
 * (744 bytes: flags, link, stack, the 21-word sigcontext, a 128-byte mask,
 * the register space), then 4 words of return code. */
#define SF_INFO 0
#define SF_UC 128
#define SF_SIZE LX_SIGFRAME_SIZE
#define UC_STACK 8
#define UC_MCTX 20
#define UC_MASK 104
#define UC_REGSPACE 232                 /* the VFP registers (a translated thread's), as Linux keeps them */
#define VFP_MAGIC 0x56465001u

static int deliver_signal(lx_thread *t)
{
	lx_proc *p = t->proc;
	struct lx_ksigaction *sa;
	int s, sig = 0;
	lx_u32 sp, fr, *mc, *uc, *info;
	for (s = 1; s < LX_NSIG; s++) {
		if (!(SIGHAS(p->sigpending, s) || SIGHAS(t->sigpending, s))) continue;
		if (SIGHAS(t->sigmask, s) && s != LX_SIGKILL && s != LX_SIGSTOP) continue;
		sig = s;
		break;
	}
	if (!sig) return 0;
	SIGDEL(p->sigpending, sig);
	SIGDEL(t->sigpending, sig);
	sa = &p->sigact[sig];
	if (sa->handler == LX_SIG_IGN) return deliver_signal(t);
	if (sa->handler == LX_SIG_DFL) {
		if (sig_default_ignored(sig)) return deliver_signal(t);
		if (sig_default_stops(sig)) { stop_proc(p, sig); return 0; }
		lx_proc_exit(p, sig | (sig_core(sig) ? 0x80 : 0));
		return 1;
	}
	/* a handler: build the frame on the program's stack */
	sp = fr_sp(t);
	if ((sa->flags & LX_SA_ONSTACK) && t->altstack_size && !t->altstack_on)
		sp = t->altstack_sp + t->altstack_size;
	fr = (sp - SF_SIZE) & ~7u;
	if (!lx_uaccess(t, fr, SF_SIZE)) {
		lx_proc_exit(p, LX_SIGSEGV | 0x80);
		return 1;
	}
	bk_memset(t->sigbuf, 0, SF_SIZE);
	info = (lx_u32 *)((lx_u32)t->sigbuf + SF_INFO);
	info[0] = sig;
	info[2] = 0;                         /* si_code: SI_USER */
	uc = (lx_u32 *)((lx_u32)t->sigbuf + SF_UC);
	uc[UC_STACK / 4] = t->altstack_sp;
	uc[UC_STACK / 4 + 1] = t->altstack_size ? (t->altstack_on ? LX_SS_ONSTACK : 0) : LX_SS_DISABLE;
	uc[UC_STACK / 4 + 2] = t->altstack_size;
	mc = (lx_u32 *)((lx_u32)t->sigbuf + SF_UC + UC_MCTX);
	mc[0] = 0;                            /* trap_no */
	mc[1] = 0;                            /* error_code */
	mc[2] = t->sigmask[0];                /* oldmask */
	for (s = 0; s <= 12; s++) mc[3 + s] = t->frame[s];   /* r0-r10, fp, ip */
	mc[16] = fr_sp(t);                    /* sp */
	mc[17] = fr_lr(t);                    /* lr (a native program's was used by the call) */
	mc[18] = fr_pc(t);                    /* pc */
	if (t->restart) {
		/* SA_RESTART: when the handler returns, the call is made again
		 * (the bl __psi_svc, or the svc, before the return address, with r0 as it was) */
		mc[18] = fr_pc(t) - 4;
		mc[3] = t->restart_r0;
		t->restart = 0;
	}
	mc[19] = 0x10 | fr_flags(t);          /* cpsr: user mode */
	if (t->dbt) {
		/* the VFP registers too: a handler may use them */
		lx_u32 *rs = (lx_u32 *)((lx_u32)t->sigbuf + SF_UC + UC_REGSPACE);
		rs[0] = VFP_MAGIC;
		rs[1] = 280;
		for (s = 0; s < 32; s++) rs[2 + s] = t->vfp[s];
		rs[2 + 64] = t->fpscr;
	}
	mc[20] = t->fault_addr;
	((lx_u32 *)((lx_u32)t->sigbuf + SF_UC + UC_MASK))[0] = t->sigmask[0];
	((lx_u32 *)((lx_u32)t->sigbuf + SF_UC + UC_MASK))[1] = t->sigmask[1];
	if (t->sigmask_saved) {
		/* rt_sigsuspend: the handler returns to the mask from before */
		((lx_u32 *)((lx_u32)t->sigbuf + SF_UC + UC_MASK))[0] = t->saved_sigmask[0];
		((lx_u32 *)((lx_u32)t->sigbuf + SF_UC + UC_MASK))[1] = t->saved_sigmask[1];
		mc[2] = t->saved_sigmask[0];
		t->sigmask_saved = 0;
	}
	/* the mask while the handler runs */
	t->sigmask[0] |= sa->mask[0];
	t->sigmask[1] |= sa->mask[1];
	if (!(sa->flags & LX_SA_NODEFER)) SIGADD(t->sigmask, sig);
	SIGDEL(t->sigmask, LX_SIGKILL);
	SIGDEL(t->sigmask, LX_SIGSTOP);
	if (sa->flags & LX_SA_ONSTACK && t->altstack_size) t->altstack_on = 1;
	t->ctx[0] = sig;
	t->ctx[1] = fr + SF_INFO;
	t->ctx[2] = fr + SF_UC;
	for (s = 3; s <= 12; s++) t->ctx[s] = t->frame[s];
	t->ctx[13] = fr;
	t->ctx[14] = (sa->flags & LX_SA_RESTORER) && sa->restorer ? sa->restorer : (lx_u32)&lx_sigtramp;
	t->ctx[15] = sa->handler;
	t->ctx_cpsr = 0x10 | fr_flags(t);
	if (t->dbt && !((sa->flags & LX_SA_RESTORER) && sa->restorer)) {
		/* (no restorer: lx_sigtramp is host code; Alpine's musl always gives one) */
		bk_log("pid %d: a signal handler without SA_RESTORER", p->pid);
	}
	if (sa->flags & LX_SA_RESETHAND) { sa->handler = LX_SIG_DFL; sa->flags &= ~LX_SA_SIGINFO; }
	if (lx_k.trace) bk_log("[%d] signal %d: handler %x, return to %x, frame %x", t->tid, sig, t->ctx[15], t->ctx[14], fr);
	t->sigframe_at = fr;
	t->sigframe_len = SF_SIZE;
	t->sigframe_src = (lx_u32)t->sigbuf;
	reply_resume(t);
	return 1;
}

static int sys_rt_sigreturn(lx_thread *t)
{
	lx_u32 fr = fr_sp(t);
	lx_u32 *mc = (lx_u32 *)(fr + SF_UC + UC_MCTX);
	lx_u32 *mask = (lx_u32 *)(fr + SF_UC + UC_MASK);
	lx_u32 *uc = (lx_u32 *)(fr + SF_UC);
	int i;
	if (!lx_uaccess(t, fr, SF_SIZE - 16)) { lx_proc_exit(t->proc, LX_SIGSEGV | 0x80); return LX_NOREPLY; }
	for (i = 0; i <= 12; i++) t->ctx[i] = mc[3 + i];
	t->ctx[13] = mc[16];
	t->ctx[14] = mc[17];
	t->ctx[15] = mc[18];
	t->ctx_cpsr = mc[19];
	if (t->dbt) {
		lx_u32 *rs = (lx_u32 *)(fr + SF_UC + UC_REGSPACE);
		if (rs[0] == VFP_MAGIC) {
			for (i = 0; i < 32; i++) t->vfp[i] = rs[2 + i];
			t->fpscr = rs[2 + 64];
		}
	}
	t->sigmask[0] = mask[0];
	t->sigmask[1] = mask[1];
	SIGDEL(t->sigmask, LX_SIGKILL);
	SIGDEL(t->sigmask, LX_SIGSTOP);
	t->altstack_on = (uc[UC_STACK / 4 + 1] & LX_SS_ONSTACK) ? 1 : 0;
	if (!(uc[UC_STACK / 4 + 1] & LX_SS_ONSTACK)) t->altstack_on = 0;
	/* anything else pending is delivered at the next call; but a signal
	 * that came while the handler ran (and was blocked) should not wait:
	 * deliver it now by answering through lx_reply's path */
	reply_resume(t);
	return LX_NOREPLY;
}

/* ---- exit and wait ----------------------------------------------------------- */

static void close_all(lx_proc *p)
{
	int i;
	for (i = 0; i < LX_MAXFD; i++)
		if (p->fd[i]) { lx_file_put(p->fd[i]); p->fd[i] = 0; }
}

/* ---- fork, without an MMU ----------------------------------------------------
 * fork is a vfork with a snapshot: the child borrows the parent's thread and
 * memory, as a vfork child does, and the parent's writable memory (its data
 * and bss, its mappings, the used part of its stack) is saved first. When
 * the child execs or exits, everything is put back as it was and the parent
 * goes on: so a program that forks and then execs or exits (by far the most
 * common use: git, make, popen-like code, a shell's pipelines and $(...))
 * works. One whose parent and child must both run on cannot: the parent
 * waits until the child execs or exits.
 *
 * Children nest: a fork child (a subshell) may fork again. Each child on a
 * thread has a record (lx_borrow), newest first, with the parent's
 * registers and snapshot; the newest is the one running.
 *
 * The stack is put back by the thread itself as it resumes (lx_resume_copy,
 * or the translator's runtime), because a native thread may still be
 * running on that part of it. */
#define FORK_RANGES 96
typedef struct lx_forksnap {
	struct lx_forksnap *prev;   /* an outer fork's snapshot of the same memory */
	lx_region store;            /* the saved bytes, back to back */
	int n;
	struct { lx_u32 addr, len, at; } r[FORK_RANGES];
	lx_u32 stack_lo, stack_hi, stack_at;
	int nmaps;
	lx_map **maps;              /* the mappings there were */
} lx_forksnap;

int lx_fork_holds(lx_mm *mm, lx_u32 addr, lx_u32 len)
{
	lx_forksnap *s;
	int i;
	for (s = mm ? mm->fork_snap : 0; s; s = s->prev)
		for (i = 0; i < s->nmaps; i++)
			if (addr < s->maps[i]->addr + s->maps[i]->len && addr + len > s->maps[i]->addr) return 1;
	return 0;
}

static void snap_free(lx_forksnap *s)
{
	if (!s) return;
	if (s->store.size) bk_region_free(&s->store);
	bk_free(s->maps);
	bk_free(s);
}

static int start_vfork(lx_thread *t, lx_u32 newsp, int exit_signal);

static void snap_range(lx_forksnap *s, lx_u32 lo, lx_u32 hi)
{
	if (hi <= lo || s->n >= FORK_RANGES) return;
	s->r[s->n].addr = lo;
	s->r[s->n].len = hi - lo;
	s->n++;
}

static int start_fork(lx_thread *t, int exit_signal)
{
	lx_proc *p = t->proc;
	lx_mm *mm = p->mm;
	lx_forksnap *s;
	lx_map *m;
	lx_u32 total = 0, at = 0, sp = t->dbt ? t->frame[DF_SP] : (lx_u32)t->frame;
	int i, r, nmaps = 0;
	if (!mm) return -LX_EAGAIN;
	s = (lx_forksnap *)bk_alloc(sizeof *s);
	if (!s) return -LX_ENOMEM;
	for (m = mm->maps; m; m = m->next) nmaps++;
	if (nmaps + 2 > FORK_RANGES) { snap_free(s); return -LX_ENOMEM; }
	snap_range(s, mm->wlo, mm->whi);
	snap_range(s, mm->ilo, mm->ihi);
	for (m = mm->maps; m; m = m->next) {
		/* (a shared library: only its writable part can change) */
		if (m->simg) snap_range(s, m->simg->rw_lo, m->simg->rw_hi);
		else snap_range(s, m->addr, m->addr + m->len);
		s->nmaps++;
	}
	if (sp < t->stack_lo || sp > t->stack_hi) sp = t->stack_lo;
	s->stack_lo = sp & ~3u;
	s->stack_hi = t->stack_hi;
	for (i = 0; i < s->n; i++) total += s->r[i].len;
	total += s->stack_hi - s->stack_lo;
	if (total && lx_region_new(&s->store, total, LX_REG_IMAGE) < 0) { snap_free(s); return -LX_ENOMEM; }
	s->maps = (lx_map **)bk_alloc((s->nmaps + 1) * sizeof(lx_map *));
	if (!s->maps) { snap_free(s); return -LX_ENOMEM; }
	for (i = 0, m = mm->maps; m; m = m->next) s->maps[i++] = m;
	for (i = 0; i < s->n; i++) {
		s->r[i].at = at;
		bk_memcpy((void *)(s->store.base + at), (const void *)s->r[i].addr, s->r[i].len);
		at += s->r[i].len;
	}
	s->stack_at = at;
	bk_memcpy((void *)(s->store.base + at), (const void *)s->stack_lo, s->stack_hi - s->stack_lo);
	r = start_vfork(t, 0, exit_signal);
	if (r < 0) { snap_free(s); return r; }
	t->borrow->snap = s;
	s->prev = mm->fork_snap;
	mm->fork_snap = s;
	if (lx_k.trace) {
		bk_log("[%d] fork: %u bytes saved, stack %x-%x", t->tid, total, s->stack_lo, s->stack_hi);
		for (i = 0; i < s->n; i++) {
			lx_u32 k, sum = 0;
			for (k = 0; k < s->r[i].len; k += 4) sum = sum * 31 + *(lx_u32 *)(s->r[i].addr + k);
			bk_log("   range %x+%x sum %x", s->r[i].addr, s->r[i].len, sum);
		}
	}
	return r;
}

/* a fork child is done with its parent's memory: everything back as it was */
static void fork_restore(lx_thread *t, lx_forksnap *s)
{
	lx_mm *mm = t->proc ? t->proc->mm : 0;
	lx_map *m, *next;
	int i, j;
	if (!mm) mm = t->owner ? t->owner->mm : 0;
	if (mm && mm->fork_snap == s) mm->fork_snap = s->prev;
	if (mm) {
		/* the child's own mappings go */
		for (m = mm->maps; m; m = next) {
			int had = 0;
			next = m->next;
			for (j = 0; j < s->nmaps; j++) if (s->maps[j] == m) had = 1;
			if (!had) lx_mm_unmap(mm, m->addr, m->len);
		}
	}
	for (i = 0; i < s->n; i++)
		lx_mm_write(mm, s->r[i].addr, (const void *)(s->store.base + s->r[i].at), s->r[i].len);
	if (lx_k.trace) {
		bk_log("[%d] fork restore: stack %x-%x", t->tid, s->stack_lo, s->stack_hi);
		for (i = 0; i < s->n; i++) {
			lx_u32 k, sum = 0;
			for (k = 0; k < s->r[i].len; k += 4) sum = sum * 31 + *(lx_u32 *)(s->r[i].addr + k);
			bk_log("   range %x+%x sum %x", s->r[i].addr, s->r[i].len, sum);
		}
	}
	/* the stack: by the thread, as it resumes (a later restore replaces an earlier one) */
	if (t->fork_store.size) bk_region_free(&t->fork_store);
	t->sigframe_at = s->stack_lo;
	t->sigframe_len = s->stack_hi - s->stack_lo;
	t->sigframe_src = s->store.base + s->stack_at;
	t->fork_store = s->store;              /* (freed at its next call) */
	s->store.size = 0;
	snap_free(s);
}

/* The newest child on t is done: t goes back to that child's parent,
 * whose vfork (fork, clone) returns the child's pid. */
static void borrow_pop(lx_thread *t)
{
	lx_borrow *b = t->borrow;
	int i;
	if (!b) return;
	t->borrow = b->prev;
	t->proc = t->borrow ? t->borrow->child : t->owner;
	if (b->snap) fork_restore(t, b->snap);
	for (i = 0; i < 16; i++) t->ctx[i] = 0;
	for (i = 0; i < 13; i++) t->ctx[i] = b->frame[i];
	t->ctx[0] = b->child->pid;
	t->ctx[13] = b->sp;
	if (t->dbt) {
		t->ctx[14] = b->frame[DF_LR];
		t->ctx[15] = b->frame[DF_PC];
		t->ctx_cpsr = b->frame[DF_CPSR];
		t->tp = b->tp;
		t->tpidrurw = b->tpidrurw;
		t->fpscr = b->fpscr;
		for (i = 0; i < 32; i++) t->vfp[i] = b->vfp[i];
	} else {
		t->ctx[14] = b->frame[LX_FR_LR];
		t->ctx[15] = b->frame[LX_FR_LR];
	}
	t->sigmask[0] = b->sigmask[0]; t->sigmask[1] = b->sigmask[1];
	t->saved_sigmask[0] = b->saved_sigmask[0]; t->saved_sigmask[1] = b->saved_sigmask[1];
	t->sigmask_saved = b->sigmask_saved;
	t->altstack_sp = b->altstack_sp; t->altstack_size = b->altstack_size;
	t->clear_child_tid = b->clear_child_tid;
	t->restart = 0;
	t->intr = 0;
	if (b->child->vfork_parent == t) b->child->vfork_parent = 0;
	bk_free(b);
}

static void vfork_release(lx_proc *child)
{
	lx_thread *t = child->vfork_parent;
	if (!t || !t->borrow) return;
	if (t->borrow->child != child) {
		/* an older child ended (killed) while a newer one runs: it goes when
		 * the newer one is done */
		return;
	}
	borrow_pop(t);
	/* parents that ended meanwhile give way to theirs */
	while (t->borrow && t->proc->state != LX_P_ALIVE) borrow_pop(t);
	if (t->state == LX_T_DEAD || t->state == LX_T_FREE) return;
	if (t->proc->state != LX_P_ALIVE) return;
	reply_resume(t);
}

/* every child running on t loses it (its owner ends) */
static void borrow_drop_all(lx_thread *t)
{
	while (t->borrow) {
		lx_borrow *b = t->borrow;
		lx_proc *c = b->child;
		t->borrow = b->prev;
		if (c->vfork_parent == t) c->vfork_parent = 0;
		if (c->state == LX_P_ALIVE) {
			c->state = LX_P_ZOMBIE;
			c->wait_status = LX_SIGKILL;
			c->wait_report = 3;
			close_all(c);
			if (c->mm) { lx_mm_put(c->mm); c->mm = 0; }
			wake_parent(c);
		}
		if (b->snap) {
			lx_mm *mm = t->owner ? t->owner->mm : 0;
			if (mm && mm->fork_snap == b->snap) mm->fork_snap = b->snap->prev;
			snap_free(b->snap);
		}
		bk_free(b);
	}
	t->proc = t->owner;
}

/* the translated thread holding the CPU, if it is p's and is in the
 * translator's own code (it must leave it before the process can go) */
static lx_thread *busy_translating(lx_proc *p)
{
	lx_thread *t = lx_cpu.cur;
	if (!t || !lx_cpu.critical) return 0;
	if (t->proc != p && t->owner != p) return 0;
	if (t->state != LX_T_USER) return 0;
	return t;
}

void lx_proc_exit(lx_proc *p, int status)
{
	int i;
	lx_proc *pp;
	if (p->state != LX_P_ALIVE) return;
	if (busy_translating(p)) {
		/* it ends at its next call into the kernel, very soon */
		if (!p->exit_pending) p->exit_pending = status + 1;
		lx_cpu.kill_req = 1;
		lx_cpu.yield = 1;
		return;
	}
	p->state = LX_P_ZOMBIE;
	p->wait_status = status;
	p->wait_report = 3;
	p->stopped = 0;
	/* a vfork child gives the thread back */
	if (p->vfork_parent) vfork_release(p);
	/* its own threads end, and any children running on them */
	for (i = 0; i < LX_MAXTHREAD; i++) {
		lx_thread *t = &lx_k.threads[i];
		if (t->state == LX_T_FREE || t->state == LX_T_DEAD) continue;
		if (t->owner != p) continue;
		if (t->borrow) borrow_drop_all(t);
		t->cpu_wait = 0;
		if (t->state == LX_T_SYSCALL) reply_exit(t);   /* (the caller of exit: it ends itself) */
		else {
			t->state = LX_T_DEAD;
			bk_thread_kill(t);
			if (t->dbt) cpu_drop(t);
		}
	}
	close_all(p);
	if (p->mm) { lx_mm_put(p->mm); p->mm = 0; }
	/* its children go to init */
	for (i = 0; i < LX_MAXPROC; i++) {
		lx_proc *c = &lx_k.procs[i];
		if (c->state != LX_P_FREE && c->ppid == p->pid && c != p) {
			c->ppid = 1;
			if (c->state == LX_P_ZOMBIE) {
				if (lx_k.real_init && p->pid != 1) wake_parent(c);   /* (init collects it) */
				else proc_free(c);
			}
		}
	}
	/* a session leader takes its terminal with it */
	if (p->ctty && p->ctty->sid == p->pid && p->sid == p->pid) {
		lx_tty *tty = p->ctty;
		if (tty->pgrp) lx_signal_pgrp(tty->pgrp, LX_SIGHUP);
		tty->sid = 0;
		tty->pgrp = 0;
	}
	pp = lx_proc_by_pid(p->ppid);
	if (!pp || (p->ppid == 1 && !lx_k.real_init) || p->pid == 1) {
		if (p->pid == lx_k.first_status) {
			lx_k.init_exited = 1;
			lx_k.first_exit = status;
			bk_tty_status(p->ctty ? p->ctty : &lx_k.ttys[0], 0, status);
		}
		proc_free(p);
	} else {
		if (pp->sigact[LX_SIGCHLD].handler == LX_SIG_IGN || (pp->sigact[LX_SIGCHLD].flags & LX_SA_NOCLDWAIT)) {
			proc_free(p);
			pp->child_exited = 1;
		} else {
			wake_parent(p);
			if (p->exit_signal) lx_signal_proc(pp, p->exit_signal);
		}
	}
	need_retry = 1;
}

/* A thread lent to a fork or vfork child died with the child (a fault in
 * the child): the parent's side goes on in a new backend thread, with the
 * same stack and registers (the child's fork returned in the parent). */
static void thread_respawn(lx_thread *t)
{
	lx_thread *n;
	int i;
	if (!t->owner || t->owner->state != LX_P_ALIVE) return;
	for (i = 0, n = 0; i < LX_MAXTHREAD; i++)
		if (lx_k.threads[i].state == LX_T_FREE) { n = &lx_k.threads[i]; break; }
	if (!n) return;
	bk_memcpy(n, t, sizeof *n);
	n->bk = 0;
	n->state = LX_T_USER;
	n->cpu_wait = 0;
	if (!lx_reserve_ok(0x4000) || bk_thread_new(n) < 0) { n->state = LX_T_FREE; return; }
	/* (the stack, the fork's saved stack and the children now belong to n) */
	t->stack.size = 0;
	t->fork_store.size = 0;
	t->borrow = 0;
	for (i = 0; i < LX_MAXPROC; i++)
		if (lx_k.procs[i].state != LX_P_FREE && lx_k.procs[i].vfork_parent == t) lx_k.procs[i].vfork_parent = n;
	if (lx_k.trace) bk_log("[%d] goes on in a new thread", n->tid);
	thread_go(n);
}

/* Called by the backend when a Linux thread has ended on its own terms:
 * killed, or it reported a fault from its exception handler. */
void lx_k_thread_died(lx_thread *t, int reason)
{
	lx_proc *p = t->proc;
	int lent = t->borrow != 0;
	if (t->state == LX_T_FREE) return;
	if (t->dbt) { t->cpu_wait = 0; cpu_drop(t); }
	if (reason && t->state != LX_T_DEAD && p && p->state == LX_P_ALIVE) {
		/* a fault: the process gets the signal's default action */
		bk_log("pid %d (%s): signal %d at %x", p->pid, p->comm, reason, t->fault_addr);
		t->state = LX_T_DEAD;
		lx_proc_exit(p, reason | 0x80);
		/* a child running on its parent's thread: the parent goes on, in a
		 * new thread (its fork returned) */
		if (lent && p != t->owner && t->proc && t->proc != p && t->proc->state == LX_P_ALIVE) thread_respawn(t);
	}
	t->state = LX_T_DEAD;
	if (t->borrow) borrow_drop_all(t);
	{
		/* killed inside the compare-and-swap's lock (it holds the holder's
		 * sp): let it go, or every other thread would wait for it forever */
		extern volatile lx_u32 lx_cas_lock;
		lx_u32 o = lx_cas_lock;
		if (o && o >= t->stack_lo && o <= t->stack_hi) lx_cas_lock = 0;
	}
	bk_thread_free(t);
	if (t->stack.size) bk_region_free(&t->stack);
	if (t->fork_store.size) bk_region_free(&t->fork_store);
	t->state = LX_T_FREE;
	lx_retry_parked();
}

static int wait_match(lx_proc *parent, lx_proc *c, int pid)
{
	if (c->ppid != parent->pid || c->state == LX_P_FREE) return 0;
	if (pid > 0) return c->pid == pid;
	if (pid == 0) return c->pgid == parent->pgid;
	if (pid < -1) return c->pgid == -pid;
	return 1;
}

/* What wait4 and waitid have in common: a child to report (its pid, and
 * *kind 3 exit, 1 stop, 2 continue, *status the wait status), 0 (none yet),
 * or -ECHILD. 'reap' frees an exited child. */
static int wait_find(lx_thread *t, int pid, int options, int reap, int *status, int *kind)
{
	lx_proc *p = t->proc;
	int i, any = 0;
	for (i = 0; i < LX_MAXPROC; i++) {
		lx_proc *c = &lx_k.procs[i];
		if (!wait_match(p, c, pid)) continue;
		any = 1;
		if (c->wait_report == 3 && c->state == LX_P_ZOMBIE && (options & LX_WEXITED)) {
			int cpid = c->pid;
			*status = c->wait_status;
			*kind = 3;
			if (reap) proc_free(c);
			return cpid;
		}
		if (c->wait_report == 1 && (options & LX_WUNTRACED)) {
			if (reap) c->wait_report = 0;
			*status = c->wait_status;
			*kind = 1;
			return c->pid;
		}
		if (c->wait_report == 2 && (options & LX_WCONTINUED)) {
			if (reap) c->wait_report = 0;
			*status = 0xffff;
			*kind = 2;
			return c->pid;
		}
	}
	return any ? 0 : -LX_ECHILD;
}

static int sys_wait4(lx_thread *t, int pid, lx_u32 statusp, int options, lx_u32 rusage)
{
	int status = 0, kind = 0, r;
	if (statusp && !lx_uaccess(t, statusp, 4)) return -LX_EFAULT;
	if (rusage && !lx_uaccess(t, rusage, sizeof(struct lx_rusage))) return -LX_EFAULT;
	r = wait_find(t, pid, options | LX_WEXITED, 1, &status, &kind);
	if (r > 0) {
		if (statusp) *(lx_u32 *)statusp = status;
		if (rusage) bk_memset((void *)rusage, 0, sizeof(struct lx_rusage));
		return r;
	}
	if (r < 0) return r;
	if (options & LX_WNOHANG) return 0;
	return LX_BLOCK;
}

/* waitid(idtype, id, siginfo, options): WEXITED 4, WSTOPPED 2, WCONTINUED 8, WNOWAIT 0x1000000 */
static int sys_waitid(lx_thread *t, lx_u32 type, lx_u32 id, lx_u32 info, lx_u32 options)
{
	int pid = type == LX_P_PID ? (int)id : type == LX_P_PGID ? -(int)id : -1;
	int status = 0, kind = 0, r;
	int opts = (options & LX_WEXITED) | ((options & 2) ? LX_WUNTRACED : 0) | (options & LX_WCONTINUED);
	if (info && !lx_uaccess(t, info, 128)) return -LX_EFAULT;
	r = wait_find(t, pid, opts, !(options & 0x1000000), &status, &kind);
	if (r < 0) return r;
	if (r == 0 && !(options & LX_WNOHANG)) return LX_BLOCK;
	if (info) {
		lx_u32 *si = (lx_u32 *)info;
		bk_memset(si, 0, 128);
		if (r > 0) {
			si[0] = LX_SIGCHLD;
			si[3] = r;                           /* si_pid */
			if (kind == 3) {
				if ((status & 0x7f) == 0) { si[2] = 1; si[5] = (status >> 8) & 0xff; }   /* CLD_EXITED */
				else { si[2] = (status & 0x80) ? 3 : 2; si[5] = status & 0x7f; }         /* CLD_DUMPED / KILLED */
			} else if (kind == 1) { si[2] = 5; si[5] = (status >> 8) & 0xff; }        /* CLD_STOPPED */
			else { si[2] = 6; si[5] = LX_SIGCONT; }                                     /* CLD_CONTINUED */
		}
	}
	return 0;
}

/* ---- vfork, clone, exec --------------------------------------------------- */

/* vfork and clone(CLONE_VM|CLONE_VFORK): the child runs on this thread, on
 * the same memory, until it execs or exits. newsp: the child's stack (0:
 * the parent's). */
static int start_vfork(lx_thread *t, lx_u32 newsp, int exit_signal)
{
	lx_proc *p = t->proc;
	lx_proc *c;
	lx_borrow *b;
	int i;
	b = (lx_borrow *)bk_alloc(sizeof *b);
	if (!b) return -LX_ENOMEM;
	c = proc_new(p);
	if (!c) { bk_free(b); return -LX_EAGAIN; }
	c->mm = p->mm;
	if (c->mm) c->mm->refs++;
	c->exit_signal = exit_signal;
	c->vfork_parent = t;
	/* the parent's registers, for when it goes on */
	b->child = c;
	for (i = 0; i < fr_words(t); i++) b->frame[i] = t->frame[i];
	b->sp = fr_sp(t);
	if (t->dbt) {
		b->tp = t->tp;
		b->tpidrurw = t->tpidrurw;
		b->fpscr = t->fpscr;
		for (i = 0; i < 32; i++) b->vfp[i] = t->vfp[i];
	}
	b->sigmask[0] = t->sigmask[0]; b->sigmask[1] = t->sigmask[1];
	b->saved_sigmask[0] = t->saved_sigmask[0]; b->saved_sigmask[1] = t->saved_sigmask[1];
	b->sigmask_saved = t->sigmask_saved;
	b->altstack_sp = t->altstack_sp; b->altstack_size = t->altstack_size;
	b->clear_child_tid = t->clear_child_tid;
	b->prev = t->borrow;
	t->borrow = b;
	t->proc = c;
	if (newsp) {
		/* clone: the child goes on from the call, on its own stack */
		for (i = 0; i < 13; i++) t->ctx[i] = t->frame[i];
		t->ctx[0] = 0;
		t->ctx[13] = newsp;
		t->ctx[14] = fr_lr(t);
		t->ctx[15] = fr_pc(t);
		t->ctx_cpsr = 0x10 | fr_flags(t);
		reply_resume(t);
		return LX_NOREPLY;
	}
	return 0;
}

static int sys_clone(lx_thread *t, lx_u32 flags, lx_u32 newsp, lx_u32 ptid, lx_u32 tls, lx_u32 ctid)
{
	if ((flags & (LX_CLONE_VM | LX_CLONE_VFORK)) == (LX_CLONE_VM | LX_CLONE_VFORK))
		return start_vfork(t, newsp, flags & 0xff);
	if (flags & LX_CLONE_THREAD) {
		/* a thread of this process: a new backend thread on newsp */
		lx_proc *p = t->proc;
		lx_thread *n;
		int i;
		lx_map *m;
		if (!(flags & LX_CLONE_VM) || !newsp) return -LX_EINVAL;
		n = thread_new(p, 0);
		if (!n) return -LX_EAGAIN;
		do { n->tid = lx_k.next_pid++; } while (pid_in_use(n->tid));
		for (i = 0; i < 13; i++) n->ctx[i] = t->frame[i];
		n->ctx[0] = 0;
		n->ctx[13] = newsp;
		n->ctx[14] = fr_lr(t);
		n->ctx[15] = fr_pc(t);
		n->ctx_cpsr = 0x10 | fr_flags(t);
		n->dbt = t->dbt;
		n->fpscr = t->fpscr;
		n->tp = (flags & LX_CLONE_SETTLS) ? tls : t->tp;
		n->sigmask[0] = t->sigmask[0];
		n->sigmask[1] = t->sigmask[1];
		/* its stack is the mapping newsp is in */
		n->stack_hi = newsp;
		n->stack_lo = newsp - 4096;
		for (m = p->mm ? p->mm->maps : 0; m; m = m->next)
			if (newsp > m->addr && newsp <= m->addr + m->len) { n->stack_lo = m->addr; n->stack_hi = m->addr + m->len; }
		if (flags & LX_CLONE_CHILD_CLEARTID) n->clear_child_tid = ctid;
		if ((flags & LX_CLONE_PARENT_SETTID) && lx_uaccess(t, ptid, 4)) *(lx_u32 *)ptid = n->tid;
		if ((flags & LX_CLONE_CHILD_SETTID) && lx_uaccess(t, ctid, 4)) *(lx_u32 *)ctid = n->tid;
		if (!lx_reserve_ok(0x4000) || bk_thread_new(n) < 0) { n->state = LX_T_FREE; return -LX_EAGAIN; }
		thread_go(n);
		return n->tid;
	}
	/* fork: a vfork with the parent's memory saved and put back */
	return start_fork(t, flags & 0xff);
}

/* argv or envp from the program: count them, and copy into kernel memory */
static int copy_strv(lx_thread *t, lx_u32 up, char ***out, int *count, int *bytes)
{
	int n = 0, i, total = 0;
	char **v;
	*out = 0; *count = 0;
	if (!up) { *out = (char **)bk_alloc(sizeof(char *)); return 0; }
	for (;;) {
		if (!lx_uaccess(t, up + n * 4, 4)) return -LX_EFAULT;
		if (!((lx_u32 *)up)[n]) break;
		if (++n > 4096) return -LX_E2BIG;
	}
	v = (char **)bk_alloc((n + 1) * sizeof(char *));
	if (!v) return -LX_ENOMEM;
	for (i = 0; i < n; i++) {
		char tmp[LX_PATH_MAX];
		lx_u32 s = ((lx_u32 *)up)[i];
		int len;
		/* strings can be long: measure first */
		len = 0;
		while (lx_uaccess(t, s + len, 1) && ((char *)s)[len]) {
			len++;
			if (total + len > LX_ARG_MAX) break;
		}
		if (!lx_uaccess(t, s + len, 1) || total + len > LX_ARG_MAX) {
			while (i-- > 0) bk_free(v[i]);
			bk_free(v);
			return total + len > LX_ARG_MAX ? -LX_E2BIG : -LX_EFAULT;
		}
		(void)tmp;
		v[i] = (char *)bk_alloc(len + 1);
		if (!v[i]) { while (i-- > 0) bk_free(v[i]); bk_free(v); return -LX_ENOMEM; }
		bk_memcpy(v[i], (const void *)s, len + 1);
		total += len + 1;
	}
	*out = v;
	*count = n;
	*bytes += total;
	return 0;
}

static void free_strv(char **v, int n)
{
	int i;
	if (!v) return;
	for (i = 0; i < n; i++) bk_free(v[i]);
	bk_free(v);
}

static void set_cmdline(lx_proc *p, char **argv, int argc, const char *path)
{
	int i, o = 0;
	const char *base = path;
	for (i = 0; path[i]; i++) if (path[i] == '/') base = path + i + 1;
	lx_strlcpy(p->comm, base, sizeof p->comm);
	for (i = 0; i < argc && o < (int)sizeof p->cmdline - 1; i++) {
		int n = lx_strlen(argv[i]);
		if (o + n + 1 > (int)sizeof p->cmdline) n = sizeof p->cmdline - o - 1;
		bk_memcpy(p->cmdline + o, argv[i], n);
		o += n;
		p->cmdline[o++] = 0;
	}
	p->cmdline_len = o;
}

/* the new program starts: the old thread goes (or back to the vfork parent) */
static int sys_execve(lx_thread *t, lx_u32 upath, lx_u32 uargv, lx_u32 uenvp)
{
	char path[LX_PATH_MAX];
	char **argv = 0, **envp = 0;
	int argc = 0, envc = 0, bytes = 0, r, i;
	lx_proc *p = t->proc;
	lx_thread *nt = 0;
	r = lx_ustr(t, upath, path, sizeof path);
	if (r < 0) return r;
	r = copy_strv(t, uargv, &argv, &argc, &bytes);
	if (r < 0) return r;
	r = copy_strv(t, uenvp, &envp, &envc, &bytes);
	if (r < 0) { free_strv(argv, argc); return r; }
	if (lx_k.trace) bk_log("[%d] execve %s [%s %s %s]", t->tid, path, argc > 0 ? argv[0] : "", argc > 1 ? argv[1] : "", argc > 2 ? argv[2] : "");
	else if (lx_k.execlog > 1) {
		lx_s64 us = bk_now_mono() - lx_k.start_mono;
		bk_log("%d.%d s: pid %d runs %s %s %s", (int)(us / 1000000), (int)(us / 100000 % 10), p->pid, path, argc > 1 ? argv[1] : "", argc > 2 ? argv[2] : "");
	}
	r = lx_exec_load(t, p, path, argv, argc, envp, envc, &nt);
	if (r < 0) { free_strv(argv, argc); free_strv(envp, envc); return r; }
	set_cmdline(p, argv, argc, path);      /* (comm: the name it was run by, as Linux) */
	free_strv(argv, argc);
	free_strv(envp, envc);
	/* close-on-exec files, and handlers back to the default */
	for (i = 0; i < LX_MAXFD; i++)
		if (p->fd[i] && (p->fdflags[i] & LX_FD_CLOEXEC)) { lx_file_put(p->fd[i]); p->fd[i] = 0; p->fdflags[i] = 0; }
	for (i = 1; i < LX_NSIG; i++) {
		if (p->sigact[i].handler != LX_SIG_IGN) p->sigact[i].handler = LX_SIG_DFL;
		p->sigact[i].flags = 0;
		p->sigact[i].mask[0] = p->sigact[i].mask[1] = 0;
	}
	nt->sigmask[0] = t->sigmask[0];
	nt->sigmask[1] = t->sigmask[1];
	if (p->vfork_parent == t) {
		/* a vfork child: the thread goes back to the parent */
		nt->owner = p;
		vfork_release(p);
	} else {
		/* the old thread ends; the new one takes its place */
		nt->tid = p->pid;
		t->tid = 0;
		reply_exit(t);
	}
	thread_go(nt);
	return LX_NOREPLY;
}

/* The first program, on terminal tty: pid 2, its session's leader (pid 1 is
 * the kernel's init, which only collects orphans). */
int lx_k_spawn_first(const char *path, char *const argv[], char *const envp[], int tty)
{
	lx_proc *init = &lx_k.procs[0];
	lx_proc *p;
	lx_thread *nt = 0;
	lx_file *f;
	int argc = 0, envc = 0, r;
	if (lx_strcmp(path, "/sbin/init") == 0 && init->state == LX_P_FREE) {
		/* Alpine mode's boot: BusyBox init must be pid 1 itself */
		bk_memset(init, 0, sizeof *init);
		init->pid = 1;
		init->state = LX_P_ALIVE;
		init->exit_signal = 0;
		init->start_time = lx_now();
		init->umask = 022;
		lx_strlcpy(init->cwd, "/", sizeof init->cwd);
		init->pgid = init->sid = 1;
		lx_k.next_pid = 2;
		lx_k.real_init = 1;
		p = init;
		goto have_proc;
	}
	if (init->state == LX_P_FREE) {
		bk_memset(init, 0, sizeof *init);
		init->pid = 1;
		init->state = LX_P_ALIVE;
		init->pgid = init->sid = 1;
		lx_strlcpy(init->comm, "init", sizeof init->comm);
		lx_strlcpy(init->cmdline, "init", sizeof init->cmdline);
		init->cmdline_len = 5;
		init->start_time = lx_now();
		init->umask = 022;
		lx_strlcpy(init->cwd, "/", sizeof init->cwd);
		lx_k.next_pid = 2;
	}
	p = proc_new(init);
	if (!p) return -LX_EAGAIN;
	p->ppid = 1;
	p->pgid = p->sid = p->pid;
have_proc:
	p->ctty = &lx_k.ttys[tty];
	p->ctty->hangup = 0;
	f = lx_tty_file(p->ctty);
	if (p->pid == 1) p->ctty = 0;         /* (init has no controlling terminal: its getty or login takes it) */
	else {
		p->ctty->sid = p->pid;
		p->ctty->pgrp = p->pid;
	}
	lx_fd_alloc(p, f, 0, 0);
	lx_fd_alloc(p, lx_file_get(f), 1, 0);
	lx_fd_alloc(p, lx_file_get(f), 2, 0);
	while (argv[argc]) argc++;
	while (envp[envc]) envc++;
	{
		lx_thread fake;
		bk_memset(&fake, 0, sizeof fake);
		fake.proc = fake.owner = p;
		r = lx_exec_load(&fake, p, path, (char **)argv, argc, (char **)envp, envc, &nt);
	}
	if (r < 0) {
		close_all(p);
		proc_free(p);
		return r;
	}
	set_cmdline(p, (char **)argv, argc, p->mm->exe);
	nt->tid = p->pid;
	lx_k.first_status = p->pid;
	lx_k.init_exited = 0;
	thread_go(nt);
	return p->pid;
}

/* Used by lx_elf.c: a new thread for p, to start at ctx, with its stack. */
lx_thread *lx_thread_for_exec(lx_proc *p)
{
	lx_thread *n = thread_new(p, p->pid);
	if (!n) return 0;
	if (!lx_reserve_ok(0x4000) || bk_thread_new(n) < 0) { n->state = LX_T_FREE; return 0; }
	return n;
}

/* ---- the system calls not about files, memory or sockets ------------------- */

static int put_u32(lx_thread *t, lx_u32 addr, lx_u32 v)
{
	if (!lx_uaccess(t, addr, 4)) return -LX_EFAULT;
	*(lx_u32 *)addr = v;
	return 0;
}

static int sys_rt_sigaction(lx_thread *t, int sig, lx_u32 act, lx_u32 oact, lx_u32 size)
{
	lx_proc *p = t->proc;
	if (sig < 1 || sig >= LX_NSIG) return -LX_EINVAL;
	if (oact) {
		if (!lx_uaccess(t, oact, sizeof(struct lx_ksigaction))) return -LX_EFAULT;
		bk_memcpy((void *)oact, &p->sigact[sig], sizeof(struct lx_ksigaction));
	}
	if (act) {
		if (sig == LX_SIGKILL || sig == LX_SIGSTOP) return -LX_EINVAL;
		if (!lx_uaccess(t, act, sizeof(struct lx_ksigaction))) return -LX_EFAULT;
		bk_memcpy(&p->sigact[sig], (const void *)act, sizeof(struct lx_ksigaction));
		if (p->sigact[sig].handler == LX_SIG_IGN || (p->sigact[sig].handler == LX_SIG_DFL && sig_default_ignored(sig))) {
			SIGDEL(p->sigpending, sig);
			SIGDEL(t->sigpending, sig);
		}
	}
	(void)size;
	return 0;
}

static int sys_rt_sigprocmask(lx_thread *t, int how, lx_u32 set, lx_u32 oset)
{
	lx_u32 n[2];
	if (oset) {
		if (!lx_uaccess(t, oset, 8)) return -LX_EFAULT;
		((lx_u32 *)oset)[0] = t->sigmask[0];
		((lx_u32 *)oset)[1] = t->sigmask[1];
	}
	if (set) {
		if (!lx_uaccess(t, set, 8)) return -LX_EFAULT;
		n[0] = ((lx_u32 *)set)[0];
		n[1] = ((lx_u32 *)set)[1];
		if (how == LX_SIG_BLOCK) { t->sigmask[0] |= n[0]; t->sigmask[1] |= n[1]; }
		else if (how == LX_SIG_UNBLOCK) { t->sigmask[0] &= ~n[0]; t->sigmask[1] &= ~n[1]; }
		else if (how == LX_SIG_SETMASK) { t->sigmask[0] = n[0]; t->sigmask[1] = n[1]; }
		else return -LX_EINVAL;
		SIGDEL(t->sigmask, LX_SIGKILL);
		SIGDEL(t->sigmask, LX_SIGSTOP);
		need_retry = 1;
	}
	return 0;
}

static int sys_kill(lx_thread *t, int pid, int sig)
{
	lx_proc *p;
	int i, n = 0;
	if (sig < 0 || sig >= LX_NSIG) return -LX_EINVAL;
	if (pid > 0) {
		p = lx_proc_by_pid(pid);
		if (!p || p->state != LX_P_ALIVE) return -LX_ESRCH;
		if (sig) lx_signal_proc(p, sig);
		return 0;
	}
	for (i = 0; i < LX_MAXPROC; i++) {
		p = &lx_k.procs[i];
		if (p->state != LX_P_ALIVE || p->pid == 1) continue;
		if (pid == 0 && p->pgid != t->proc->pgid) continue;
		if (pid < -1 && p->pgid != -pid) continue;
		if (pid == -1 && p == t->proc) continue;
		n++;
		if (sig) lx_signal_proc(p, sig);
	}
	return n ? 0 : -LX_ESRCH;
}

static lx_s64 ts_to_us(lx_s64 sec, lx_s32 nsec) { return sec * 1000000 + nsec / 1000; }

static int sys_nanosleep(lx_thread *t, lx_u32 req, lx_u32 rem, int is64, int clock, int flags)
{
	lx_s64 us;
	if (is64) {
		struct lx_timespec64 *ts = (struct lx_timespec64 *)req;
		if (!lx_uaccess(t, req, sizeof *ts)) return -LX_EFAULT;
		us = ts_to_us(ts->tv_sec, ts->tv_nsec);
	} else {
		struct lx_timespec32 *ts = (struct lx_timespec32 *)req;
		if (!lx_uaccess(t, req, sizeof *ts)) return -LX_EFAULT;
		us = ts_to_us(ts->tv_sec, ts->tv_nsec);
	}
	if (us < 0) return -LX_EINVAL;
	if (!t->deadline) {
		if (flags & LX_TIMER_ABSTIME) {
			lx_s64 now = clock == LX_CLOCK_REALTIME ? bk_now_real() : lx_now();
			t->deadline = lx_now() + (us - now);
		} else t->deadline = lx_now() + us;
		if (t->deadline <= lx_now()) { t->deadline = 0; return 0; }
		return LX_BLOCK;
	}
	if (t->timed_out) { t->timed_out = 0; t->deadline = 0; return 0; }
	return LX_BLOCK;
	(void)rem;
}

static int sys_clock_gettime(lx_thread *t, int clock, lx_u32 tsp, int is64)
{
	lx_s64 us;
	switch (clock) {
	case LX_CLOCK_REALTIME: case LX_CLOCK_REALTIME_COARSE: us = bk_now_real(); break;
	case LX_CLOCK_PROCESS_CPUTIME_ID: case LX_CLOCK_THREAD_CPUTIME_ID:
		us = t->proc->cpu_us + (t->ran_since ? 0 : 0); break;
	default: us = lx_now() + 1000000; break;     /* monotonic: never 0 */
	}
	if (is64) {
		struct lx_timespec64 *ts = (struct lx_timespec64 *)tsp;
		if (!lx_uaccess(t, tsp, sizeof *ts)) return -LX_EFAULT;
		ts->tv_sec = us / 1000000;
		ts->tv_nsec = (lx_s32)(us % 1000000) * 1000;
		ts->pad = 0;
	} else {
		struct lx_timespec32 *ts = (struct lx_timespec32 *)tsp;
		if (!lx_uaccess(t, tsp, sizeof *ts)) return -LX_EFAULT;
		ts->tv_sec = (lx_s32)(us / 1000000);
		ts->tv_nsec = (lx_s32)(us % 1000000) * 1000;
	}
	return 0;
}

static int sys_uname(lx_thread *t, lx_u32 buf)
{
	struct lx_utsname *u = (struct lx_utsname *)buf;
	if (!lx_uaccess(t, buf, sizeof *u)) return -LX_EFAULT;
	bk_memset(u, 0, sizeof *u);
	lx_strlcpy(u->s[0], "Linux", 65);
	lx_strlcpy(u->s[1], lx_k.hostname, 65);
	lx_strlcpy(u->s[2], "5.15.0-psilinux", 65);
	lx_strlcpy(u->s[3], "#1 PsiLinux " LX_VERSION " on EPOC R5", 65);
	lx_strlcpy(u->s[4], t->dbt ? "armv6l" : "armv4tl", 65);
	lx_strlcpy(u->s[5], "(none)", 65);
	return 0;
}

static int sys_sysinfo(lx_thread *t, lx_u32 buf)
{
	struct lx_sysinfo *s = (struct lx_sysinfo *)buf;
	lx_u32 total, freeb;
	int i, n = 0;
	if (!lx_uaccess(t, buf, sizeof *s)) return -LX_EFAULT;
	bk_memset(s, 0, sizeof *s);
	bk_mem_info(&total, &freeb);
	for (i = 0; i < LX_MAXPROC; i++) if (lx_k.procs[i].state == LX_P_ALIVE) n++;
	s->uptime = (lx_s32)(lx_now() / 1000000);
	s->totalram = total;
	s->freeram = freeb;
	s->procs = (lx_u16)n;
	s->mem_unit = 1;
	return 0;
}

static int sys_setpgid(lx_thread *t, int pid, int pgid)
{
	lx_proc *p = pid ? lx_proc_by_pid(pid) : t->proc;
	if (!p || p->state != LX_P_ALIVE) return -LX_ESRCH;
	if (p != t->proc && p->ppid != t->proc->pid) return -LX_ESRCH;
	if (pgid < 0) return -LX_EINVAL;
	if (!pgid) pgid = p->pid;
	if (p->sid == p->pid) return p->pgid == pgid ? 0 : -LX_EPERM;
	p->pgid = pgid;
	return 0;
}

static int sys_setsid(lx_thread *t)
{
	lx_proc *p = t->proc;
	int i;
	for (i = 0; i < LX_MAXPROC; i++)
		if (lx_k.procs[i].state == LX_P_ALIVE && lx_k.procs[i].pgid == p->pid && &lx_k.procs[i] != p) return -LX_EPERM;
	if (p->pgid == p->pid) return -LX_EPERM;
	p->sid = p->pgid = p->pid;
	p->ctty = 0;
	return p->pid;
}

static int sys_prlimit(lx_thread *t, int res, lx_u32 newl, lx_u32 old)
{
	if (old) {
		struct lx_rlimit64 *r = (struct lx_rlimit64 *)old;
		if (!lx_uaccess(t, old, sizeof *r)) return -LX_EFAULT;
		r->cur = r->max = ~0ull;
		if (res == LX_RLIMIT_NOFILE) r->cur = r->max = LX_MAXFD;
		if (res == LX_RLIMIT_STACK) r->cur = r->max = LX_DEFAULT_STACK;
	}
	(void)newl;
	return 0;
}

static int sys_getrandom(lx_thread *t, lx_u32 buf, lx_u32 len)
{
	lx_u32 i;
	if (!lx_uaccess(t, buf, len)) return -LX_EFAULT;
	for (i = 0; i < len; i++) ((lx_u8 *)buf)[i] = (lx_u8)(bk_random() >> 8);
	return (int)len;
}

static int sys_setitimer(lx_thread *t, int which, lx_u32 nv, lx_u32 ov)
{
	lx_proc *p = t->proc;
	lx_s64 now = lx_now();
	if (which != 0) return -LX_EINVAL;     /* ITIMER_REAL only */
	if (ov) {
		lx_s32 *o = (lx_s32 *)ov;
		lx_s64 left = p->alarm_at ? p->alarm_at - now : 0;
		if (!lx_uaccess(t, ov, 16)) return -LX_EFAULT;
		if (left < 0) left = 0;
		o[0] = (lx_s32)(p->alarm_interval / 1000000); o[1] = (lx_s32)(p->alarm_interval % 1000000);
		o[2] = (lx_s32)(left / 1000000); o[3] = (lx_s32)(left % 1000000);
	}
	if (nv) {
		lx_s32 *n = (lx_s32 *)nv;
		lx_s64 val;
		if (!lx_uaccess(t, nv, 16)) return -LX_EFAULT;
		p->alarm_interval = (lx_s64)n[0] * 1000000 + n[1];
		val = (lx_s64)n[2] * 1000000 + n[3];
		p->alarm_at = val ? now + val : 0;
	}
	return 0;
}

static int sys_sigaltstack(lx_thread *t, lx_u32 ss, lx_u32 oss)
{
	if (oss) {
		lx_u32 *o = (lx_u32 *)oss;
		if (!lx_uaccess(t, oss, 12)) return -LX_EFAULT;
		o[0] = t->altstack_sp;
		o[1] = t->altstack_size ? (t->altstack_on ? LX_SS_ONSTACK : 0) : LX_SS_DISABLE;
		o[2] = t->altstack_size;
	}
	if (ss) {
		lx_u32 *s = (lx_u32 *)ss;
		if (!lx_uaccess(t, ss, 12)) return -LX_EFAULT;
		if (t->altstack_on) return -LX_EPERM;
		if (s[1] & LX_SS_DISABLE) { t->altstack_sp = t->altstack_size = 0; }
		else { t->altstack_sp = s[0]; t->altstack_size = s[2]; }
	}
	return 0;
}

static int sys_times(lx_thread *t, lx_u32 buf)
{
	lx_s64 ticks = lx_now() / 10000;
	if (buf) {
		struct lx_tms *m = (struct lx_tms *)buf;
		if (!lx_uaccess(t, buf, sizeof *m)) return -LX_EFAULT;
		m->utime = (lx_s32)(t->proc->cpu_us / 10000);
		m->stime = m->cutime = m->cstime = 0;
	}
	return (int)ticks;
}

static int do_syscall(lx_thread *t, lx_u32 nr, lx_u32 *a)
{
	lx_proc *p = t->proc;
	int r;
	switch (nr) {
	case LX_NR_exit:
		if (count_threads(p) > 1 && !p->vfork_parent) {
			/* one thread of several ends */
			if (t->clear_child_tid && lx_uaccess(t, t->clear_child_tid, 4)) *(lx_u32 *)t->clear_child_tid = 0;
			reply_exit(t);
			need_retry = 1;
			return LX_NOREPLY;
		}
		/* fall through */
	case LX_NR_exit_group:
		lx_proc_exit(p, (a[0] & 0xff) << 8);
		return LX_NOREPLY;
	case LX_NR_getpid: return p->pid;
	case LX_NR_getppid: return p->ppid;
	case LX_NR_gettid: return p->vfork_parent ? p->pid : t->tid;
	case LX_NR_set_tid_address: t->clear_child_tid = a[0]; return p->vfork_parent ? p->pid : t->tid;
	case LX_NR_ARM_set_tls: t->tp = a[0]; return 0;
	case LX_NR_set_robust_list: return 0;
	case LX_NR_vfork: return start_vfork(t, 0, LX_SIGCHLD);
	case LX_NR_fork: return start_fork(t, LX_SIGCHLD);
	case LX_NR_clone: return sys_clone(t, a[0], a[1], a[2], a[3], a[4]);
	case LX_NR_execve: return sys_execve(t, a[0], a[1], a[2]);
	case LX_NR_wait4: return sys_wait4(t, (int)a[0], a[1], (int)a[2], a[3]);
	case LX_NR_waitid: return sys_waitid(t, a[0], a[1], a[2], a[3]);
	case LX_NR_kill: return sys_kill(t, (int)a[0], (int)a[1]);
	case LX_NR_tkill:
	case LX_NR_tgkill: {
		int tid = (int)(nr == LX_NR_tkill ? a[0] : a[1]);
		int sig = (int)(nr == LX_NR_tkill ? a[1] : a[2]);
		int i;
		for (i = 0; i < LX_MAXTHREAD; i++) {
			lx_thread *o = &lx_k.threads[i];
			if (o->state != LX_T_FREE && o->state != LX_T_DEAD && (o->tid == tid || (o->proc && o->proc->pid == tid && o->proc->vfork_parent == o))) {
				if (sig) {
					if (o == t) SIGADD(t->sigpending, sig);
					else lx_signal_thread(o, sig);
				}
				return 0;
			}
		}
		return -LX_ESRCH;
	}
	case LX_NR_rt_sigaction: return sys_rt_sigaction(t, (int)a[0], a[1], a[2], a[3]);
	case LX_NR_rt_sigprocmask: return sys_rt_sigprocmask(t, (int)a[0], a[1], a[2]);
	case LX_NR_rt_sigreturn: case LX_NR_sigreturn: return sys_rt_sigreturn(t);
	case LX_NR_rt_sigpending: {
		if (!lx_uaccess(t, a[0], 8)) return -LX_EFAULT;
		((lx_u32 *)a[0])[0] = (p->sigpending[0] | t->sigpending[0]) & t->sigmask[0];
		((lx_u32 *)a[0])[1] = (p->sigpending[1] | t->sigpending[1]) & t->sigmask[1];
		return 0;
	}
	case LX_NR_rt_sigsuspend:
		if (!t->sigmask_saved) {
			if (!lx_uaccess(t, a[0], 8)) return -LX_EFAULT;
			t->saved_sigmask[0] = t->sigmask[0];
			t->saved_sigmask[1] = t->sigmask[1];
			t->sigmask[0] = ((lx_u32 *)a[0])[0];
			t->sigmask[1] = ((lx_u32 *)a[0])[1];
			t->sigmask_saved = 1;
			lx_signal_proc(p, 0);
		}
		return LX_BLOCK;      /* until a signal: EINTR */
	case LX_NR_pause: return LX_BLOCK;
	case LX_NR_rt_sigtimedwait:
	case LX_NR_rt_sigtimedwait_time64: {
		/* sigwaitinfo: take a pending signal from the set */
		lx_u32 set0, set1;
		int s;
		if (!lx_uaccess(t, a[0], 8)) return -LX_EFAULT;
		set0 = ((lx_u32 *)a[0])[0]; set1 = ((lx_u32 *)a[0])[1];
		for (s = 1; s < LX_NSIG; s++) {
			lx_u32 b = SIGBIT(s), w = SIGWORD(s) ? set1 : set0;
			if (!(w & b)) continue;
			if (SIGHAS(p->sigpending, s) || SIGHAS(t->sigpending, s)) {
				SIGDEL(p->sigpending, s); SIGDEL(t->sigpending, s);
				if (a[1] && lx_uaccess(t, a[1], 128)) { bk_memset((void *)a[1], 0, 128); ((lx_u32 *)a[1])[0] = s; }
				return s;
			}
		}
		if (a[2]) {
			lx_s64 us;
			if (nr == LX_NR_rt_sigtimedwait_time64) { struct lx_timespec64 *ts = (struct lx_timespec64 *)a[2]; if (!lx_uaccess(t, a[2], 16)) return -LX_EFAULT; us = ts_to_us(ts->tv_sec, ts->tv_nsec); }
			else { struct lx_timespec32 *ts = (struct lx_timespec32 *)a[2]; if (!lx_uaccess(t, a[2], 8)) return -LX_EFAULT; us = ts_to_us(ts->tv_sec, ts->tv_nsec); }
			if (!t->deadline) t->deadline = lx_now() + us;
			if (t->timed_out) { t->timed_out = 0; t->deadline = 0; return -LX_EAGAIN; }
		}
		return LX_BLOCK;
	}
	case LX_NR_sigaltstack: return sys_sigaltstack(t, a[0], a[1]);
	case LX_NR_getuid32: case LX_NR_geteuid32: case LX_NR_getuid: case LX_NR_geteuid: return p->uid;
	case LX_NR_getgid32: case LX_NR_getegid32: case LX_NR_getgid: case LX_NR_getegid: return p->gid;
	case LX_NR_getresuid32: case LX_NR_getresgid32: {
		lx_u32 v = nr == LX_NR_getresuid32 ? p->uid : p->gid;
		if (put_u32(t, a[0], v) || put_u32(t, a[1], v) || put_u32(t, a[2], v)) return -LX_EFAULT;
		return 0;
	}
	case LX_NR_setuid32: case LX_NR_setgid32: case LX_NR_setreuid32: case LX_NR_setregid32:
	case LX_NR_setresuid32: case LX_NR_setresgid32: case LX_NR_setfsuid32: case LX_NR_setfsgid32:
		return 0;               /* one user: anything goes */
	case LX_NR_getgroups32: return 0;
	case LX_NR_setgroups32: return 0;
	case LX_NR_getpgid: {
		lx_proc *q = a[0] ? lx_proc_by_pid((int)a[0]) : p;
		return q ? q->pgid : -LX_ESRCH;
	}
	case LX_NR_getpgrp: return p->pgid;
	case LX_NR_setpgid: return sys_setpgid(t, (int)a[0], (int)a[1]);
	case LX_NR_getsid: {
		lx_proc *q = a[0] ? lx_proc_by_pid((int)a[0]) : p;
		return q ? q->sid : -LX_ESRCH;
	}
	case LX_NR_setsid: return sys_setsid(t);
	case LX_NR_umask: r = p->umask; p->umask = a[0] & 0777; return r;
	case LX_NR_uname: return sys_uname(t, a[0]);
	case LX_NR_sethostname:
		if (a[1] > 64) return -LX_EINVAL;
		if (!lx_uaccess(t, a[0], a[1])) return -LX_EFAULT;
		bk_memcpy(lx_k.hostname, (const void *)a[0], a[1]);
		lx_k.hostname[a[1]] = 0;
		return 0;
	case LX_NR_sysinfo: return sys_sysinfo(t, a[0]);
	case LX_NR_prlimit64: return sys_prlimit(t, (int)a[1], a[2], a[3]);
	case LX_NR_ugetrlimit: {
		struct lx_rlimit32 *rl = (struct lx_rlimit32 *)a[1];
		if (!lx_uaccess(t, a[1], sizeof *rl)) return -LX_EFAULT;
		rl->cur = rl->max = ~0u;
		if (a[0] == LX_RLIMIT_NOFILE) rl->cur = rl->max = LX_MAXFD;
		if (a[0] == LX_RLIMIT_STACK) rl->cur = rl->max = LX_DEFAULT_STACK;
		return 0;
	}
	case LX_NR_setrlimit: return 0;
	case LX_NR_getrusage:
		if (!lx_uaccess(t, a[1], sizeof(struct lx_rusage))) return -LX_EFAULT;
		bk_memset((void *)a[1], 0, sizeof(struct lx_rusage));
		return 0;
	case LX_NR_times: return sys_times(t, a[0]);
	case LX_NR_sched_yield: return 0;
	case LX_NR_sched_getaffinity:
		if (a[1] < 4 || !lx_uaccess(t, a[2], 4)) return -LX_EINVAL;
		*(lx_u32 *)a[2] = 1;
		return 4;
	case LX_NR_sched_getparam: case LX_NR_sched_getscheduler: case LX_NR_sched_setscheduler:
	case LX_NR_sched_setparam: case LX_NR_setpriority: case LX_NR_getpriority:
		return 0;
	case LX_NR_prctl:
		if (a[0] == 15 && lx_ustr(t, a[1], p->comm, sizeof p->comm) >= 0) return 0;   /* PR_SET_NAME */
		if (a[0] == 16 && lx_uaccess(t, a[1], 16)) { bk_memcpy((void *)a[1], p->comm, 16); return 0; }
		return 0;
	case LX_NR_nanosleep: return sys_nanosleep(t, a[0], a[1], 0, 0, 0);
	case LX_NR_clock_nanosleep: return sys_nanosleep(t, a[2], a[3], 0, (int)a[0], (int)a[1]);
	case LX_NR_clock_nanosleep_time64: return sys_nanosleep(t, a[2], a[3], 1, (int)a[0], (int)a[1]);
	case LX_NR_clock_gettime: return sys_clock_gettime(t, (int)a[0], a[1], 0);
	case LX_NR_clock_gettime64: return sys_clock_gettime(t, (int)a[0], a[1], 1);
	case LX_NR_clock_getres: case LX_NR_clock_getres_time64:
		if (a[1]) {
			int n = nr == LX_NR_clock_getres ? 8 : 16;
			if (!lx_uaccess(t, a[1], n)) return -LX_EFAULT;
			bk_memset((void *)a[1], 0, n);
			if (n == 8) ((lx_s32 *)a[1])[1] = 1000; else ((lx_s32 *)a[1])[2] = 1000;
		}
		return 0;
	case LX_NR_gettimeofday:
		if (a[0]) {
			lx_s64 us = bk_now_real();
			struct lx_timeval32 *tv = (struct lx_timeval32 *)a[0];
			if (!lx_uaccess(t, a[0], sizeof *tv)) return -LX_EFAULT;
			tv->tv_sec = (lx_s32)(us / 1000000);
			tv->tv_usec = (lx_s32)(us % 1000000);
		}
		if (a[1] && lx_uaccess(t, a[1], 8)) bk_memset((void *)a[1], 0, 8);
		return 0;
	case LX_NR_settimeofday: case LX_NR_clock_settime: case LX_NR_clock_settime64: return -LX_EPERM;
	case LX_NR_setitimer: return sys_setitimer(t, (int)a[0], a[1], a[2]);
	case LX_NR_getitimer: return sys_setitimer(t, (int)a[0], 0, a[1]);
	case LX_NR_getrandom: return sys_getrandom(t, a[0], a[1]);
	case LX_NR_ARM_cacheflush: return 0;       /* one unified cache: nothing to do */
	case LX_NR_membarrier: return 0;
	case LX_NR_mmap2: return lx_sys_mmap(t, a);
	case LX_NR_munmap: return lx_sys_munmap(t, a);
	case LX_NR_mremap: return lx_sys_mremap(t, a);
	case LX_NR_msync: return lx_sys_msync(t, a);
	case LX_NR_mprotect: case LX_NR_madvise: case LX_NR_mlock: case LX_NR_munlock: case LX_NR_mlockall: case LX_NR_munlockall:
		return 0;
	case LX_NR_brk:
		/* no brk: musl's malloc uses mmap when brk does not move */
		return p->mm ? (int)p->mm->brk : 0;
	case LX_NR_futex: case LX_NR_futex_time64: {
		/* threads: FUTEX_WAIT parks while *uaddr == val; FUTEX_WAKE wakes */
		lx_u32 op = a[1] & 127;
		if (op == 0 || op == 9) {           /* WAIT, WAIT_BITSET */
			if (!lx_uaccess(t, a[0], 4)) return -LX_EFAULT;
			if (*(volatile lx_u32 *)a[0] != a[2]) return -LX_EAGAIN;
			if (t->timed_out) { t->timed_out = 0; t->deadline = 0; return -LX_ETIMEDOUT; }
			if (a[3] && !t->deadline) {
				lx_s64 us;
				if (nr == LX_NR_futex) { struct lx_timespec32 *ts = (struct lx_timespec32 *)a[3]; if (!lx_uaccess(t, a[3], 8)) return -LX_EFAULT; us = ts_to_us(ts->tv_sec, ts->tv_nsec); }
				else { struct lx_timespec64 *ts = (struct lx_timespec64 *)a[3]; if (!lx_uaccess(t, a[3], 16)) return -LX_EFAULT; us = ts_to_us(ts->tv_sec, ts->tv_nsec); }
				t->deadline = lx_now() + us;
			}
			return LX_BLOCK;     /* (every change makes parked calls look again) */
		}
		if (op == 1 || op == 10) { need_retry = 1; return (int)a[2]; }   /* WAKE */
		return -LX_ENOSYS;
	}
	case LX_NR_dbt_yield: return (int)t->frame[0];      /* (the CPU changed hands in lx_reply) */
	case LX_NR_dbt_raise: {
		/* a fault in translated code: like Linux's force_sig, it cannot be
		 * blocked or ignored */
		int sig = t->dbt_sig;
		if (sig < 1 || sig >= LX_NSIG) return (int)t->frame[0];
		if (p->sigact[sig].handler == LX_SIG_IGN || SIGHAS(t->sigmask, sig)) {
			p->sigact[sig].handler = LX_SIG_DFL;
			SIGDEL(t->sigmask, sig);
		}
		SIGADD(t->sigpending, sig);
		t->fault_addr = fr_pc(t);
		return (int)t->frame[0];
	}
	case LX_NR_reboot:
		/* reboot, halt, poweroff: Linux on the Psion ends (the window says so) */
		if (a[0] != 0xfee1dead) return -LX_EINVAL;
		if (a[2] == 0 || a[2] == 0x89abcdef) return 0;          /* (CAD on/off) */
		bk_log("reboot (%x): the session ends", a[2]);
		lx_k_shutdown();
		if (!lx_k.init_exited) {
			lx_k.init_exited = 1;
			lx_k.first_exit = 0;
			bk_tty_status(&lx_k.ttys[0], 0, 0);
		}
		return LX_NOREPLY;
	case LX_NR_personality: return 0;
	case LX_NR_ioprio_get: case LX_NR_ioprio_set: return 0;
	case LX_NR_capget: case LX_NR_capset: return 0;
	default:
		break;
	}
	r = lx_fs_syscall(t, nr, a);
	if (r != LX_NOSYS) return r;
	r = lx_net_syscall(t, nr, a);
	if (r != LX_NOSYS) return r;
	bk_log("pid %d: system call %u is not there", p->pid, nr);
	return -LX_ENOSYS;
}

/* ---- start --------------------------------------------------------------- */

void lx_k_init(const char *root_native)
{
	int i;
	bk_memset(&lx_k, 0, sizeof lx_k);
	lx_strlcpy(lx_k.root_native, root_native, sizeof lx_k.root_native);
	lx_strlcpy(lx_k.hostname, "psion", sizeof lx_k.hostname);
	lx_k.next_pid = 2;
	lx_k.boot_time = bk_now_real();
	lx_k.start_mono = bk_now_mono();
	lx_k.vdso[0] = (lx_u32)&lx_entry_sys;
	lx_k.vdso[1] = (lx_u32)&lx_entry_cas;
	lx_k.vdso[2] = (lx_u32)&lx_entry_gettp;
	lx_k.vdso[3] = (lx_u32)&lx_entry_barrier;
	for (i = 0; i < LX_MAXTTY; i++) lx_tty_init(&lx_k.ttys[i], i);
	lx_vfs_init();
}

void lx_k_shutdown(void)
{
	int i;
	for (i = 0; i < LX_MAXPROC; i++)
		if (lx_k.procs[i].state == LX_P_ALIVE && lx_k.procs[i].pid != 1) lx_proc_exit(&lx_k.procs[i], LX_SIGKILL);
}

int lx_first_status_value(void) { return lx_k.first_exit; }
