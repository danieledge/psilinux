/* bk_host.c - the host backend: PsiLinux's kernel as an ordinary ARM Linux
 * program (psihost), run under qemu-arm on a PC. Linux threads are pthreads,
 * memory is mmap, files and sockets are the host's. It runs the same kernel
 * code (lx_*.c) and the same entry path (lx_entry.S) as the runner on the
 * Psion, so programs can be tried quickly without the emulator.
 *
 *   psihost [-t] [-r ROOTDIR] PROGRAM [ARGS...]
 * ROOTDIR holds ROOT.IMG and gets root/ and tmp/ (the writable layer and
 * /tmp), as <disk>:\Linux\ does on the Psion. -t traces every call.
 * LX_DRIVE_C=/some/dir (and _D...) shows a folder as /c (/d...). */
#define _GNU_SOURCE
#include "lx_core.h"
#include "lx_dbt.h"
#include <stdarg.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <termios.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/sysinfo.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

typedef struct hthread {
	pthread_t pt;
	sem_t start, reply, resume;
	int died_reported;
	int fault;
} hthread;

/* the kernel's queue: calls and deaths */
typedef struct qent { lx_thread *t; int kind; int reason; } qent;
#define QMAX 256
static qent queue[QMAX];
static int qhead, qtail;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static int wake_pipe[2];
static lx_s64 wake_at;
static __thread lx_thread *self;
static struct termios saved_tio;
static int raw_tty;

static void qpush(lx_thread *t, int kind, int reason)
{
	char c = 1;
	pthread_mutex_lock(&qlock);
	queue[qtail].t = t;
	queue[qtail].kind = kind;
	queue[qtail].reason = reason;
	qtail = (qtail + 1) % QMAX;
	pthread_mutex_unlock(&qlock);
	if (write(wake_pipe[1], &c, 1) < 0) {}
}

/* ---- threads ------------------------------------------------------------- */

static void on_kill(int sig) { (void)sig; pthread_exit(0); }
static void on_stop(int sig) { (void)sig; if (self) sem_wait(&((hthread *)self->bk)->resume); }

#include <ucontext.h>
static void on_fault(int sig, siginfo_t *si, void *uc)
{
	ucontext_t *u = (ucontext_t *)uc;
	if (lx_k.trace || !self)
		bk_log("signal %d at %p: pc %lx lr %lx sp %lx (%s thread)", sig, si->si_addr,
			(unsigned long)u->uc_mcontext.arm_pc, (unsigned long)u->uc_mcontext.arm_lr, (unsigned long)u->uc_mcontext.arm_sp, self ? "a Linux" : "the kernel");
	if (!self) { signal(sig, SIG_DFL); return; }
	if (self->dbt) {
		extern int dbt_block_of(lx_u32 h, lx_u32 *gpc, lx_u32 *host);
		lx_u32 pc = (lx_u32)u->uc_mcontext.arm_pc, g = 0, h = 0;
		int b = dbt_block_of(pc, &g, &h);
		bk_log("pid %d (%s): signal %d, address %p, at host %x = in the block of guest %x (+%u bytes); sp %lx lr %lx r0 %lx r1 %lx r2 %lx r3 %lx",
			self->proc ? self->proc->pid : 0, self->proc ? self->proc->comm : "", sig, si->si_addr, pc, b >= 0 ? g : 0, b >= 0 ? pc - h : 0,
			(unsigned long)u->uc_mcontext.arm_sp, (unsigned long)u->uc_mcontext.arm_lr, (unsigned long)u->uc_mcontext.arm_r0,
			(unsigned long)u->uc_mcontext.arm_r1, (unsigned long)u->uc_mcontext.arm_r2, (unsigned long)u->uc_mcontext.arm_r3);
	}
	self->fault_addr = (lx_u32)(long)si->si_addr;
	((hthread *)self->bk)->fault = sig == SIGSEGV ? LX_SIGSEGV : sig == SIGILL ? LX_SIGILL : sig == SIGBUS ? LX_SIGBUS : LX_SIGFPE;
	pthread_exit(0);
}

static void died(void *arg)
{
	lx_thread *t = (lx_thread *)arg;
	hthread *h = (hthread *)t->bk;
	if (h->died_reported++) return;
	qpush(t, 2, h->fault);
}

static void *thread_main(void *arg)
{
	lx_thread *t = (lx_thread *)arg;
	hthread *h = (hthread *)t->bk;
	self = t;
	pthread_cleanup_push(died, t);
	sem_wait(&h->start);
	lx_thread_start(t);
	pthread_cleanup_pop(1);
	return 0;
}

int bk_thread_new(lx_thread *t)
{
	hthread *h = (hthread *)calloc(1, sizeof *h);
	pthread_attr_t a;
	if (!h) return -LX_ENOMEM;
	sem_init(&h->start, 0, 0);
	sem_init(&h->reply, 0, 0);
	sem_init(&h->resume, 0, 0);
	t->bk = h;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, 128 * 1024);
	if (pthread_create(&h->pt, &a, thread_main, t)) { free(h); t->bk = 0; return -LX_EAGAIN; }
	return 0;
}

void bk_thread_start(lx_thread *t) { sem_post(&((hthread *)t->bk)->start); }
void bk_thread_reply(lx_thread *t) { sem_post(&((hthread *)t->bk)->reply); }
void bk_thread_kill(lx_thread *t) { if (t->bk) pthread_kill(((hthread *)t->bk)->pt, SIGUSR2); }
void bk_thread_suspend(lx_thread *t) { if (t->bk) pthread_kill(((hthread *)t->bk)->pt, SIGUSR1); }
void bk_thread_resume(lx_thread *t) { if (t->bk) sem_post(&((hthread *)t->bk)->resume); }
void bk_thread_free(lx_thread *t)
{
	hthread *h = (hthread *)t->bk;
	if (!h) return;
	/* (it told us it was dying from inside, still on its stack: wait until
	 * it has really gone before that stack is freed) */
	pthread_join(h->pt, 0);
	sem_destroy(&h->start);
	sem_destroy(&h->reply);
	sem_destroy(&h->resume);
	free(h);
	t->bk = 0;
}

void bk_thread_call(lx_thread *t)
{
	qpush(t, 1, 0);
	while (sem_wait(&((hthread *)t->bk)->reply) < 0 && errno == EINTR) ;
}

void bk_thread_exit_self(lx_thread *t) { (void)t; pthread_exit(0); }
void bk_cas_yield(void) { sched_yield(); }

/* ---- memory --------------------------------------------------------------- */

static char *arena;
#define ARENA_MAX (32u * 1024 * 1024)

/* LX_SIM_FREE=KB: pretend the Psion has this much free RAM before Linux
 * starts, less what Linux has taken (regions, the arena, the code cache), to
 * test the memory reserve (LX_RESERVE=KB) and running short */
static lx_u32 sim_used, arena_now, code_now;
static lx_u32 sim_free(void)
{
	static long kb = -1;
	if (kb < 0) kb = getenv("LX_SIM_FREE") ? atol(getenv("LX_SIM_FREE")) : 0;
	if (!kb) return 0xffffffffu;
	return (lx_u32)kb * 1024 > sim_used ? (lx_u32)kb * 1024 - sim_used : 0;
}
lx_u32 bk_mem_reserve(void) { return getenv("LX_RESERVE") ? (lx_u32)atol(getenv("LX_RESERVE")) * 1024 : 0; }

/* stacks get 16 KB of no access below them, as on the Psion (bk_epoc.cpp),
 * so a program that runs off its stack faults here too */
#define GUARD 0x4000
int bk_region_new(lx_region *r, lx_u32 size, int kind)
{
	lx_u32 g = kind == LX_REG_STACK ? GUARD : 0;
	char *p = (char *)mmap(0, size + g, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) return -LX_ENOMEM;
	if (g) mprotect(p, g, PROT_NONE);
	r->base = (lx_u32)(long)(p + g);
	r->size = size;
	r->bk = (void *)(long)g;
	sim_used += (size + 4095) & ~4095u;
	return 0;
}
void bk_region_free(lx_region *r)
{
	lx_u32 g = (lx_u32)(long)r->bk;
	if (r->size) { munmap((void *)(long)(r->base - g), r->size + g); sim_used -= (r->size + 4095) & ~4095u; }
	r->size = 0;
}
lx_u32 bk_arena_base(void)
{
	if (!arena) {
		arena = (char *)mmap(0, ARENA_MAX, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (arena == MAP_FAILED) arena = 0;
	}
	return (lx_u32)(long)arena;
}
lx_u32 bk_arena_max(void) { return arena ? ARENA_MAX : 0; }
/* Alpine mode's code cache: all of it at once (the host has room) */
int bk_code_region(lx_region *r, lx_u32 max, lx_u32 size)
{
	char *p = (char *)mmap(0, max, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) return -LX_ENOMEM;
	code_now = size; sim_used += size;
	r->base = (lx_u32)(long)p;
	r->size = max;
	r->bk = 0;
	return 0;
}
int bk_code_grow(lx_region *r, lx_u32 size)
{
	if (size > r->size || (size > code_now && size - code_now > sim_free())) return -LX_ENOMEM;
	sim_used += size - code_now; code_now = size;
	return 0;
}
int bk_arena_grow(lx_u32 size)
{
	size = (size + 4095) & ~4095u;
	if (size > ARENA_MAX || (size > arena_now && size - arena_now > sim_free())) return -LX_ENOMEM;
	sim_used += size - arena_now; arena_now = size;
	return 0;
}
void *bk_alloc(lx_u32 n) { return calloc(1, n ? n : 1); }
void bk_free(void *p) { free(p); }
void bk_memcpy(void *d, const void *s, lx_u32 n) { memcpy(d, s, n); }
void bk_memmove(void *d, const void *s, lx_u32 n) { memmove(d, s, n); }
void bk_memset(void *d, int c, lx_u32 n) { memset(d, c, n); }

/* ---- time ----------------------------------------------------------------- */

static lx_s64 mono0;
lx_s64 bk_now_real(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (lx_s64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
lx_s64 bk_now_mono(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (lx_s64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 - mono0;
}
void bk_wake_at(lx_s64 mono) { wake_at = mono; }
lx_u32 bk_random(void) { return (lx_u32)random() ^ ((lx_u32)random() << 16); }
void bk_mem_info(lx_u32 *total, lx_u32 *freeb)
{
	struct sysinfo si;
	sysinfo(&si);
	*total = 16 * 1024 * 1024;   /* (pretend to be a 5mx) */
	*freeb = si.freeram > 9u * 1024 * 1024 ? 9u * 1024 * 1024 : (lx_u32)si.freeram;
	if (getenv("LX_SIM_FREE")) *freeb = sim_free();
}

/* ---- the terminal ------------------------------------------------------------ */

int bk_tty_write(lx_tty *tty, const char *buf, int n)
{
	int done = 0;
	(void)tty;
	while (done < n) {
		int r = (int)write(1, buf + done, n - done);
		if (r <= 0) break;
		done += r;
	}
	return done;
}
int bk_tty_room(lx_tty *tty) { (void)tty; return 4096; }
void bk_tty_status(lx_tty *tty, int running, int status) { (void)tty; (void)running; (void)status; }

void bk_log(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
	buf[n++] = '\n';
	if (raw_tty) { if (write(2, "\r", 1) < 0) {} }
	if (write(2, buf, n) < 0) {}
}

/* ---- files --------------------------------------------------------------------- */

static int lxerr(int e) { return -(e == ENOENT ? LX_ENOENT : e == EEXIST ? LX_EEXIST : e == ENOTDIR ? LX_ENOTDIR :
	e == EISDIR ? LX_EISDIR : e == ENOTEMPTY ? LX_ENOTEMPTY : e == EACCES ? LX_EACCES : e == ENOSPC ? LX_ENOSPC :
	e == EXDEV ? LX_EXDEV : e == EINVAL ? LX_EINVAL : e == EBUSY ? LX_EBUSY : e == EROFS ? LX_EROFS : LX_EIO); }

/* "C:/x" -> $LX_DRIVE_C/x */
static const char *host_path(const char *p, char *buf)
{
	if (p[0] >= 'A' && p[0] <= 'Z' && p[1] == ':') {
		char var[16];
		const char *d;
		snprintf(var, sizeof var, "LX_DRIVE_%c", p[0]);
		d = getenv(var);
		snprintf(buf, LX_PATH_MAX, "%s%s", d ? d : "/nonexistent", p + 2);
		return buf;
	}
	return p;
}

void *bk_file_open(const char *path, int flags, int *err)
{
	char hb[LX_PATH_MAX];
	int f = 0, fd;
	int acc = flags & LX_O_ACCMODE;
	f = acc == LX_O_RDONLY ? O_RDONLY : acc == LX_O_WRONLY ? O_WRONLY : O_RDWR;
	if (flags & LX_O_CREAT) f |= O_CREAT;
	if (flags & LX_O_EXCL) f |= O_EXCL;
	fd = open(host_path(path, hb), f | O_CLOEXEC, 0644);
	if (fd < 0) { *err = lxerr(errno); return 0; }
	return (void *)(long)(fd + 1);
}
#define HFD(h) ((int)(long)(h) - 1)
int bk_file_read(void *h, lx_s64 off, char *buf, lx_u32 n) { ssize_t r = pread(HFD(h), buf, n, off); return r < 0 ? lxerr(errno) : (int)r; }
int bk_file_write(void *h, lx_s64 off, const char *buf, lx_u32 n) { ssize_t r = pwrite(HFD(h), buf, n, off); return r < 0 ? lxerr(errno) : (int)r; }
int bk_file_size(void *h, lx_s64 *size) { struct stat st; if (fstat(HFD(h), &st)) return lxerr(errno); *size = st.st_size; return 0; }
int bk_file_truncate(void *h, lx_s64 size) { return ftruncate(HFD(h), size) ? lxerr(errno) : 0; }
int bk_file_sync(void *h) { return fsync(HFD(h)) ? lxerr(errno) : 0; }
void bk_file_close(void *h) { close(HFD(h)); }
int bk_stat_path(const char *path, bk_stat *st)
{
	char hb[LX_PATH_MAX];
	struct stat s;
	if (stat(host_path(path, hb), &s)) return lxerr(errno);
	st->is_dir = S_ISDIR(s.st_mode);
	st->size = s.st_size;
	st->mtime = (lx_s64)s.st_mtim.tv_sec * 1000000 + s.st_mtim.tv_nsec / 1000;
	st->readonly = !(s.st_mode & 0200);
	return 0;
}
int bk_mkdir(const char *path) { char hb[LX_PATH_MAX]; return mkdir(host_path(path, hb), 0755) ? lxerr(errno) : 0; }
int bk_rmdir(const char *path) { char hb[LX_PATH_MAX]; return rmdir(host_path(path, hb)) ? lxerr(errno) : 0; }
int bk_unlink(const char *path) { char hb[LX_PATH_MAX]; return unlink(host_path(path, hb)) ? lxerr(errno) : 0; }
int bk_rename(const char *a, const char *b) { char h1[LX_PATH_MAX], h2[LX_PATH_MAX]; return rename(host_path(a, h1), host_path(b, h2)) ? lxerr(errno) : 0; }
int bk_set_mtime(const char *path, lx_s64 mtime)
{
	char hb[LX_PATH_MAX];
	struct timeval tv[2];
	tv[0].tv_sec = tv[1].tv_sec = (time_t)(mtime / 1000000);
	tv[0].tv_usec = tv[1].tv_usec = (suseconds_t)(mtime % 1000000);
	return utimes(host_path(path, hb), tv) ? lxerr(errno) : 0;
}

typedef struct hdir { DIR *d; char path[LX_PATH_MAX]; } hdir;
void *bk_dir_open(const char *path, int *err)
{
	char hb[LX_PATH_MAX];
	hdir *h = (hdir *)calloc(1, sizeof *h);
	if (!h) { *err = -LX_ENOMEM; return 0; }
	snprintf(h->path, sizeof h->path, "%s", host_path(path, hb));
	h->d = opendir(h->path);
	if (!h->d) { *err = lxerr(errno); free(h); return 0; }
	return h;
}
const char *bk_dir_next(void *dp, bk_stat *st)
{
	hdir *h = (hdir *)dp;
	struct dirent *e;
	while ((e = readdir(h->d)) != 0) {
		char full[LX_PATH_MAX * 2];
		struct stat s;
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		snprintf(full, sizeof full, "%s/%s", h->path, e->d_name);
		if (stat(full, &s)) continue;
		st->is_dir = S_ISDIR(s.st_mode);
		st->size = s.st_size;
		st->mtime = (lx_s64)s.st_mtime * 1000000;
		st->readonly = 0;
		return e->d_name;
	}
	return 0;
}
void bk_dir_close(void *dp) { hdir *h = (hdir *)dp; closedir(h->d); free(h); }
int bk_disk_info(const char *path, lx_u64 *size, lx_u64 *freeb)
{
	char hb[LX_PATH_MAX];
	struct statvfs s;
	if (statvfs(host_path(path, hb), &s)) return lxerr(errno);
	*size = (lx_u64)s.f_blocks * s.f_frsize;
	*freeb = (lx_u64)s.f_bavail * s.f_frsize;
	return 0;
}
int bk_drive_list(char *drives)
{
	int n = 0;
	char c, var[16];
	for (c = 'A'; c <= 'Z'; c++) {
		snprintf(var, sizeof var, "LX_DRIVE_%c", c);
		if (getenv(var)) drives[n++] = c;
	}
	drives[n] = 0;
	return n;
}
char bk_native_sep(void) { return '/'; }

/* ---- sockets ------------------------------------------------------------------- */

typedef struct hsock { int fd; int err; } hsock;
#define MAXSOCK 64
static hsock *socks[MAXSOCK];

static void track(hsock *s, int on)
{
	int i;
	for (i = 0; i < MAXSOCK; i++) {
		if (on && !socks[i]) { socks[i] = s; return; }
		if (!on && socks[i] == s) { socks[i] = 0; return; }
	}
}

void *bk_sock_new(int kind, int *err)
{
	hsock *s;
	int fd = socket(AF_INET, (kind == BK_SOCK_TCP ? SOCK_STREAM : SOCK_DGRAM) | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) { *err = -LX_ENETDOWN; return 0; }
	s = (hsock *)calloc(1, sizeof *s);
	s->fd = fd;
	track(s, 1);
	return s;
}
void bk_sock_close(void *p) { hsock *s = (hsock *)p; track(s, 0); close(s->fd); free(s); }
static int neterr(int e)
{
	switch (e) {
	case EAGAIN: return -LX_EAGAIN;
	case EINPROGRESS: return -LX_EINPROGRESS;
	case ECONNREFUSED: return -LX_ECONNREFUSED;
	case ECONNRESET: return -LX_ECONNRESET;
	case ETIMEDOUT: return -LX_ETIMEDOUT;
	case ENETUNREACH: return -LX_ENETUNREACH;
	case EHOSTUNREACH: return -LX_EHOSTUNREACH;
	case EADDRINUSE: return -LX_EADDRINUSE;
	case EPIPE: return -LX_EPIPE;
	case ENOTCONN: return -LX_ENOTCONN;
	case EISCONN: return -LX_EISCONN;
	}
	return -LX_EIO;
}
static void mkaddr(struct sockaddr_in *sa, lx_u32 addr, lx_u16 port)
{
	memset(sa, 0, sizeof *sa);
	sa->sin_family = AF_INET;
	sa->sin_addr.s_addr = addr;
	sa->sin_port = port;
}
int bk_sock_connect(void *p, lx_u32 addr, lx_u16 port)
{
	struct sockaddr_in sa;
	mkaddr(&sa, addr, port);
	if (connect(((hsock *)p)->fd, (struct sockaddr *)&sa, sizeof sa) == 0) return 0;
	return neterr(errno);
}
int bk_sock_bind(void *p, lx_u32 addr, lx_u16 port)
{
	struct sockaddr_in sa;
	int one = 1;
	mkaddr(&sa, addr, port);
	setsockopt(((hsock *)p)->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	return bind(((hsock *)p)->fd, (struct sockaddr *)&sa, sizeof sa) ? neterr(errno) : 0;
}
int bk_sock_listen(void *p, int backlog) { return listen(((hsock *)p)->fd, backlog) ? neterr(errno) : 0; }
void *bk_sock_accept(void *p, int *err, lx_u32 *addr, lx_u16 *port)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof sa;
	hsock *s;
	int fd = accept4(((hsock *)p)->fd, (struct sockaddr *)&sa, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0) { *err = neterr(errno); return 0; }
	s = (hsock *)calloc(1, sizeof *s);
	s->fd = fd;
	track(s, 1);
	*addr = sa.sin_addr.s_addr;
	*port = sa.sin_port;
	return s;
}
int bk_sock_send(void *p, const char *buf, lx_u32 n) { ssize_t r = send(((hsock *)p)->fd, buf, n, MSG_NOSIGNAL); return r < 0 ? neterr(errno) : (int)r; }
int bk_sock_recv(void *p, char *buf, lx_u32 n, int peek) { ssize_t r = recv(((hsock *)p)->fd, buf, n, peek ? MSG_PEEK : 0); return r < 0 ? neterr(errno) : (int)r; }
int bk_sock_sendto(void *p, const char *buf, lx_u32 n, lx_u32 addr, lx_u16 port)
{
	struct sockaddr_in sa;
	ssize_t r;
	mkaddr(&sa, addr, port);
	r = sendto(((hsock *)p)->fd, buf, n, MSG_NOSIGNAL, (struct sockaddr *)&sa, sizeof sa);
	return r < 0 ? neterr(errno) : (int)r;
}
int bk_sock_recvfrom(void *p, char *buf, lx_u32 n, lx_u32 *addr, lx_u16 *port)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof sa;
	ssize_t r = recvfrom(((hsock *)p)->fd, buf, n, 0, (struct sockaddr *)&sa, &len);
	if (r < 0) return neterr(errno);
	if (addr) *addr = sa.sin_addr.s_addr;
	if (port) *port = sa.sin_port;
	return (int)r;
}
int bk_sock_shutdown(void *p, int how) { return shutdown(((hsock *)p)->fd, how) ? neterr(errno) : 0; }
int bk_sock_poll(void *p)
{
	struct pollfd pf;
	int r = 0;
	pf.fd = ((hsock *)p)->fd;
	pf.events = POLLIN | POLLOUT;
	pf.revents = 0;
	poll(&pf, 1, 0);
	if (pf.revents & POLLIN) r |= LX_POLLIN;
	if (pf.revents & POLLOUT) r |= LX_POLLOUT;
	if (pf.revents & POLLERR) r |= LX_POLLERR;
	if (pf.revents & POLLHUP) r |= LX_POLLHUP | LX_POLLIN;
	return r;
}
int bk_sock_error(void *p)
{
	int e = 0;
	socklen_t len = sizeof e;
	getsockopt(((hsock *)p)->fd, SOL_SOCKET, SO_ERROR, &e, &len);
	return e ? neterr(e) : 0;
}
int bk_sock_name(void *p, int peer, lx_u32 *addr, lx_u16 *port)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof sa;
	int r = peer ? getpeername(((hsock *)p)->fd, (struct sockaddr *)&sa, &len) : getsockname(((hsock *)p)->fd, (struct sockaddr *)&sa, &len);
	if (r) return neterr(errno);
	*addr = sa.sin_addr.s_addr;
	*port = sa.sin_port;
	return 0;
}

typedef struct hres { int n; lx_u32 a[8]; int err; } hres;
void *bk_resolve_start(const char *name, int *err)
{
	struct addrinfo hints, *res = 0, *ai;
	hres *r = (hres *)calloc(1, sizeof *r);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(name, 0, &hints, &res)) { r->err = -LX_ENOENT; return r; }
	for (ai = res; ai && r->n < 8; ai = ai->ai_next)
		r->a[r->n++] = ((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr;
	freeaddrinfo(res);
	(void)err;
	return r;
}
int bk_resolve_poll(void *p, lx_u32 *addrs, int max)
{
	hres *r = (hres *)p;
	int i;
	if (r->err) return r->err;
	for (i = 0; i < r->n && i < max; i++) addrs[i] = r->a[i];
	return i;
}
void bk_resolve_free(void *p) { free(p); }
int bk_dns_server(lx_u32 *addr) { (void)addr; return -1; }

/* ---- the kernel thread ----------------------------------------------------------- */

static volatile int want_dump;
static void on_quit(int sig) { (void)sig; want_dump = 1; if (write(wake_pipe[1], "q", 1) < 0) {} }
static void dump(void)
{
	int i;
	for (i = 0; i < LX_MAXTHREAD; i++) {
		lx_thread *t = &lx_k.threads[i];
		if (t->state == LX_T_FREE) continue;
		bk_log("thread %d: state %d nr %u proc %d owner %d", t->tid, t->state, t->nr, t->proc ? t->proc->pid : -1, t->owner ? t->owner->pid : -1);
	}
	for (i = 0; i < LX_MAXPROC; i++) {
		lx_proc *p = &lx_k.procs[i];
		if (p->state == LX_P_FREE) continue;
		bk_log("proc %d: state %d ppid %d report %d status %x vfork %p", p->pid, p->state, p->ppid, p->wait_report, p->wait_status, (void *)p->vfork_parent);
	}
}

/* (debugging) LX_DBT_TRACE=file: every guest instruction run, as
 * "image+offset", "interp+offset" or "map:base+offset", with r0-r3 */
static FILE *trace_file;
static void trace_pc(lx_thread *t, lx_u32 pc)
{
	lx_mm *mm = t->proc ? t->proc->mm : 0;
	lx_u32 *f = t->dframe;
	if (!trace_file || !mm) return;
	{
		static lx_u32 wa, last0, last1;
		const char *e = getenv("LX_DBT_WATCH");
		if (e && !wa) wa = (lx_u32)strtoul(e, 0, 16);
		if (wa && lx_uaccess(t, wa, 8) && (*(lx_u32 *)wa != last0 || *(lx_u32 *)(wa + 4) != last1)) {
			last0 = *(lx_u32 *)wa; last1 = *(lx_u32 *)(wa + 4);
			fprintf(trace_file, "WATCH %x: %08x %08x (before the next)\n", wa, last0, last1);
		}
	}
	if (pc >= mm->image.base && pc < mm->image.base + mm->image.size) fprintf(trace_file, "I+%x", pc - mm->image.base);
	else if (mm->interp.size && pc >= mm->interp.base && pc < mm->interp.base + mm->interp.size) fprintf(trace_file, "L+%x", pc - mm->interp.base);
	else fprintf(trace_file, "?%x", pc);
	fprintf(trace_file, " %x %x %x %x %x sp %x lr %x p%d\n", f[0], f[1], f[2], f[3], f[16] >> 28, f[13], f[14], t->proc ? t->proc->pid : 0);
}

void dbt_trace_note(lx_thread *t, const char *what)
{
	if (trace_file) fprintf(trace_file, "-- %s %u (pid %d)\n", what, t->dbt_kcall ? (lx_u32)t->dbt_kcall : t->frame[7], t->proc ? t->proc->pid : 0);
}

static void restore_tty(void) { if (raw_tty) tcsetattr(0, TCSANOW, &saved_tio); }

int main(int argc, char **argv)
{
	const char *root = "lxroot";
	char rootn[LX_PATH_MAX];
	static char *envp[] = { "HOME=/root", "PATH=/bin:/usr/bin:/sbin:/usr/sbin", "TERM=xterm-256color", "USER=root", "LOGNAME=root", "SHELL=/bin/sh", "PS1=\\w \\$ ", 0 };
	struct sigaction sa;
	int i = 1, r, stdin_open = 1;
	{
		/* the host's vDSO is Thumb-2 code: hide it from musl, so that psihost
		 * runs under qemu-arm -cpu ti925t (an ARMv4T, like the ARM710T),
		 * where any v5 or later instruction it or the translator's output
		 * has is an illegal instruction, as on the 5mx */
		extern char **environ;
		char **e = environ;
		unsigned long *av;
		while (*e) e++;
		for (av = (unsigned long *)(e + 1); av[0]; av += 2)
			if (av[0] == 33) av[0] = 1;              /* AT_SYSINFO_EHDR -> AT_IGNORE */
	}
	while (i < argc && argv[i][0] == '-') {
		if (!strcmp(argv[i], "-t")) lx_k.trace = 1;
		else if (!strcmp(argv[i], "-r") && i + 1 < argc) root = argv[++i];
		else break;
		i++;
	}
	if (i >= argc) { fprintf(stderr, "usage: psihost [-t] [-r ROOTDIR] PROGRAM [ARGS...]\n"); return 2; }
	snprintf(rootn, sizeof rootn, "%s/", root);
	mkdir(root, 0755);
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		mono0 = (lx_s64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
	}
	srandom((unsigned)time(0));
	if (pipe(wake_pipe)) return 1;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_kill;
	sigaction(SIGUSR2, &sa, 0);
	sa.sa_handler = on_stop;
	sigaction(SIGUSR1, &sa, 0);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, 0);
	sa.sa_handler = on_quit;
	sigaction(SIGQUIT, &sa, 0);
	sa.sa_handler = 0;
	sa.sa_sigaction = on_fault;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigaction(SIGSEGV, &sa, 0);
	sigaction(SIGILL, &sa, 0);
	sigaction(SIGBUS, &sa, 0);
	sigaction(SIGFPE, &sa, 0);
	if (isatty(0) && tcgetattr(0, &saved_tio) == 0) {
		struct termios t = saved_tio;
		cfmakeraw(&t);
		t.c_oflag |= OPOST;
		tcsetattr(0, TCSANOW, &t);
		raw_tty = 1;
		atexit(restore_tty);
	}
	{
		struct lx_k_save { int trace; } keep;
		keep.trace = lx_k.trace;
		lx_k_init(rootn);
		lx_k.trace = keep.trace;
	}
	{
		struct winsize ws;
		if (raw_tty && ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row) { lx_k.ttys[0].ws.ws_row = ws.ws_row; lx_k.ttys[0].ws.ws_col = ws.ws_col; }
	}
	if (getenv("LX_DBT_SLOW")) lx_cpu.slow = (lx_u32)strtoul(getenv("LX_DBT_SLOW"), 0, 0);
	if (getenv("LX_DBT_MAXN")) lx_cpu.maxn = (lx_u32)atoi(getenv("LX_DBT_MAXN"));
	if (getenv("LX_DBT_STATS")) lx_k.memlog = 1;
	if (getenv("LX_DBT_NOCHAIN")) lx_cpu.nochain = (lx_u32)atoi(getenv("LX_DBT_NOCHAIN"));
	if (getenv("LX_DBT_TRACE")) {
		trace_file = fopen(getenv("LX_DBT_TRACE"), "w");
		if (trace_file) { lx_cpu.trace = trace_pc; lx_cpu.trace_note = dbt_trace_note; }
	}
	r = lx_k_spawn_first(argv[i], argv + i, envp, 0);
	if (r < 0) { fprintf(stderr, "psihost: cannot start %s: error %d\n", argv[i], -r); return 127; }
	while (!lx_k.init_exited) {
		struct pollfd pf[2];
		int n = 0, timeout = -1, j, nsock = 0;
		pf[n].fd = wake_pipe[0]; pf[n].events = POLLIN; n++;
		if (stdin_open) { pf[n].fd = 0; pf[n].events = POLLIN; n++; }
		for (j = 0; j < MAXSOCK; j++) if (socks[j]) nsock++;
		if (nsock) timeout = 20;              /* (sockets: look at them again soon) */
		if (wake_at) {
			lx_s64 d = wake_at - bk_now_mono();
			int ms = d <= 0 ? 0 : (int)(d / 1000) + 1;
			if (timeout < 0 || ms < timeout) timeout = ms;
		}
		poll(pf, n, timeout);
		if (pf[0].revents) { char b[64]; if (read(wake_pipe[0], b, sizeof b) < 0) {} }
		if (want_dump) { want_dump = 0; dump(); }
		if (stdin_open && n > 1 && pf[1].revents) {
			char b[256];
			ssize_t k = read(0, b, sizeof b);
			if (k > 0) lx_k_tty_input(&lx_k.ttys[0], b, (int)k);
			else { stdin_open = 0; lx_k_tty_input(&lx_k.ttys[0], "\004", 1); }
		}
		for (;;) {
			qent e;
			pthread_mutex_lock(&qlock);
			if (qhead == qtail) { pthread_mutex_unlock(&qlock); break; }
			e = queue[qhead];
			qhead = (qhead + 1) % QMAX;
			pthread_mutex_unlock(&qlock);
			if (e.kind == 1) lx_k_syscall(e.t);
			else lx_k_thread_died(e.t, e.reason);
		}
		if (wake_at && bk_now_mono() >= wake_at) { wake_at = 0; lx_k_timer(); }
		if (nsock) lx_k_event();
	}
	restore_tty();
	if (getenv("LX_DBT_STATS")) {
		char buf[600];
		extern void dbt_profile_log(void);
		dbt_stats(buf, sizeof buf);
		fprintf(stderr, "%s", buf);
		dbt_profile_log();
	}
	r = lx_k.ttys[0].index;     /* (unused) */
	{
		extern int lx_first_status_value(void);
		r = lx_first_status_value();
	}
	if (r & 0x7f) return 128 + (r & 0x7f);
	return (r >> 8) & 0xff;
}
