/* lx_dev.c - the made-up parts of the file system: /dev, /proc, pipes,
 * eventfd and files whose contents are made at open (/proc's). */
#include "lx_core.h"
#include "lx_vfs.h"
#include "lx_dbt.h"

/* ---- files made at open ------------------------------------------------------ */

typedef struct mfile { char *data; int len; } mfile;

static int mf_pread(lx_thread *t, lx_file *f, char *buf, lx_u32 n, lx_s64 off)
{
	mfile *m = (mfile *)f->priv;
	(void)t;
	if (off >= m->len) return 0;
	if (off + n > (lx_u32)m->len) n = (lx_u32)(m->len - off);
	bk_memcpy(buf, m->data + off, n);
	return (int)n;
}
static int mf_read(lx_thread *t, lx_file *f, char *buf, lx_u32 n)
{
	int r = mf_pread(t, f, buf, n, f->pos);
	if (r > 0) f->pos += r;
	return r;
}
static lx_s64 mf_lseek(lx_file *f, lx_s64 off, int whence)
{
	mfile *m = (mfile *)f->priv;
	lx_s64 base = whence == LX_SEEK_CUR ? f->pos : whence == LX_SEEK_END ? m->len : 0;
	if (whence > 2 || base + off < 0) return -LX_EINVAL;
	f->pos = base + off;
	return f->pos;
}
static int mf_fstat(lx_file *f, struct lx_stat64 *st)
{
	lx_vn v;
	bk_memset(&v, 0, sizeof v);
	v.mode = f->mode;
	v.ino = f->ino;
	v.fs = LX_FS_PROC;
	v.mtime = bk_now_real();
	lx_vn_stat(&v, st);
	return 0;
}
static int mf_write(lx_thread *t, lx_file *f, const char *b, lx_u32 n) { (void)t; (void)f; (void)b; return (int)n; }
static int mf_poll(lx_file *f) { (void)f; return LX_POLLIN | LX_POLLOUT; }
static void mf_close(lx_file *f) { mfile *m = (mfile *)f->priv; bk_free(m->data); bk_free(m); }
static const lx_fops mfile_ops = { mf_read, mf_write, mf_pread, 0, mf_lseek, mf_fstat, 0, mf_poll, 0, mf_close, 0, 0 };

lx_file *lx_memfile_new(const char *data, int len)
{
	mfile *m = (mfile *)bk_alloc(sizeof *m);
	lx_file *f;
	if (!m) return 0;
	m->data = (char *)bk_alloc(len + 1);
	if (!m->data) { bk_free(m); return 0; }
	bk_memcpy(m->data, data, len);
	m->len = len;
	f = lx_file_new(&mfile_ops, m, LX_O_RDONLY);
	if (!f) { bk_free(m->data); bk_free(m); return 0; }
	f->mode = LX_S_IFREG | 0444;
	return f;
}

/* ---- /dev ----------------------------------------------------------------- */

enum { D_DIR = 1, D_NULL, D_ZERO, D_FULL, D_RANDOM, D_URANDOM, D_TTY, D_CONSOLE, D_TTYN, D_PTS };

typedef struct devent { const char *name; int dev; lx_u32 mode; lx_u32 rdev; const char *link; } devent;
static const devent devs[] = {
	{ "null", D_NULL, LX_S_IFCHR | 0666, 0x103, 0 },
	{ "zero", D_ZERO, LX_S_IFCHR | 0666, 0x105, 0 },
	{ "full", D_FULL, LX_S_IFCHR | 0666, 0x107, 0 },
	{ "random", D_RANDOM, LX_S_IFCHR | 0666, 0x108, 0 },
	{ "urandom", D_URANDOM, LX_S_IFCHR | 0666, 0x109, 0 },
	{ "tty", D_TTY, LX_S_IFCHR | 0666, 0x500, 0 },
	{ "console", D_CONSOLE, LX_S_IFCHR | 0600, 0x501, 0 },
	{ "tty1", D_TTYN, LX_S_IFCHR | 0620, 0x401, 0 },
	{ "tty2", D_TTYN, LX_S_IFCHR | 0620, 0x402, 0 },
	{ "tty3", D_TTYN, LX_S_IFCHR | 0620, 0x403, 0 },
	{ "tty4", D_TTYN, LX_S_IFCHR | 0620, 0x404, 0 },
	{ "stdin", 0, LX_S_IFLNK | 0777, 0, "/proc/self/fd/0" },
	{ "stdout", 0, LX_S_IFLNK | 0777, 0, "/proc/self/fd/1" },
	{ "stderr", 0, LX_S_IFLNK | 0777, 0, "/proc/self/fd/2" },
	{ "fd", 0, LX_S_IFLNK | 0777, 0, "/proc/self/fd" },
	{ "pts", D_PTS, LX_S_IFDIR | 0755, 0, 0 },
	{ "shm", 0, LX_S_IFLNK | 0777, 0, "/tmp" },
};
#define NDEVS (int)(sizeof devs / sizeof devs[0])

int lx_dev_lookup(lx_proc *p, const char *rel, lx_vn *v)
{
	int i;
	(void)p;
	while (*rel == '/') rel++;
	if (!*rel) { v->mode = LX_S_IFDIR | 0755; v->dev = D_DIR; v->mtime = lx_k.boot_time; return 0; }
	for (i = 0; i < NDEVS; i++)
		if (lx_strcmp(devs[i].name, rel) == 0) {
			v->dev = devs[i].dev;
			v->mode = devs[i].mode;
			v->rdev = devs[i].rdev;
			v->mtime = lx_k.boot_time;
			v->ino = 0x1000 + i;
			if (devs[i].link) lx_strlcpy(v->link, devs[i].link, sizeof v->link);
			if (devs[i].dev == D_TTYN) {
				v->fd = rel[3] - '1';
				v->ino = 0x1100 + v->fd;          /* (as the open terminal's fstat says: ttyname compares them) */
			}
			return 0;
		}
	if (lx_strcmp(rel, "pts") > 0 && lx_strncmp(rel, "pts/", 4) == 0) return -LX_ENOENT;
	return -LX_ENOENT;
}

void lx_dev_list(lx_proc *p, lx_vn *v, void *dl)
{
	int i;
	(void)p;
	if (v->dev != D_DIR) return;
	for (i = 0; i < NDEVS; i++)
		lx_dlist_add(dl, devs[i].name, devs[i].link ? LX_DT_LNK : LX_S_ISDIR(devs[i].mode) ? LX_DT_DIR : LX_DT_CHR, 0x1000 + i);
}

static int dv_read(lx_thread *t, lx_file *f, char *buf, lx_u32 n)
{
	int d = (int)(long)f->priv;
	lx_u32 i;
	(void)t;
	if (d == D_NULL) return 0;
	if (d == D_ZERO || d == D_FULL) { bk_memset(buf, 0, n); return (int)n; }
	for (i = 0; i < n; i++) buf[i] = (char)(bk_random() >> 8);
	return (int)n;
}
static int dv_write(lx_thread *t, lx_file *f, const char *buf, lx_u32 n)
{
	(void)t; (void)buf;
	if ((int)(long)f->priv == D_FULL) return -LX_ENOSPC;
	return (int)n;
}
static int dv_pread(lx_thread *t, lx_file *f, char *buf, lx_u32 n, lx_s64 off) { (void)off; return dv_read(t, f, buf, n); }
static lx_s64 dv_lseek(lx_file *f, lx_s64 off, int whence) { (void)f; (void)off; (void)whence; return 0; }
static int dv_fstat(lx_file *f, struct lx_stat64 *st)
{
	lx_vn v;
	bk_memset(&v, 0, sizeof v);
	v.mode = f->mode;
	v.ino = f->ino;
	v.fs = LX_FS_DEV;
	lx_vn_stat(&v, st);
	return 0;
}
static int dv_poll(lx_file *f) { (void)f; return LX_POLLIN | LX_POLLOUT; }
static const lx_fops dev_ops = { dv_read, dv_write, dv_pread, 0, dv_lseek, dv_fstat, 0, dv_poll, 0, 0, 0, 0 };

lx_file *lx_dev_open(lx_proc *p, lx_vn *v, int flags, int *err)
{
	lx_file *f;
	lx_tty *tty = 0;
	if (v->dev == D_TTY) {
		tty = p->ctty;
		if (!tty) { *err = -LX_ENXIO; return 0; }
	} else if (v->dev == D_CONSOLE) tty = &lx_k.ttys[0];
	else if (v->dev == D_TTYN) tty = &lx_k.ttys[v->fd >= 0 && v->fd < LX_MAXTTY ? v->fd : 0];
	if (tty) {
		f = lx_tty_file(tty);
		if (!f) { *err = -LX_ENOMEM; return 0; }
		f->flags = flags;
		/* the first terminal a session leader opens becomes its own */
		if (!(flags & LX_O_NOCTTY) && !p->ctty && p->sid == p->pid && !tty->sid) {
			p->ctty = tty;
			tty->sid = p->sid;
			tty->pgrp = p->pgid;
		}
		return f;
	}
	f = lx_file_new(&dev_ops, (void *)(long)v->dev, flags);
	if (!f) { *err = -LX_ENOMEM; return 0; }
	f->mode = v->mode;
	f->ino = v->ino;
	return f;
}

/* ---- pipes -------------------------------------------------------------- */

typedef struct pipe {
	char buf[LX_PIPE_SIZE];
	int head, count;
	int readers, writers;
	lx_u64 ino;
} pipe;

static lx_u64 pipe_ino = 4000;
static const lx_fops pipe_w_ops;

/* Is q a parent that waits while t runs its fork (or vfork) child? The child
 * borrows its parent's thread until it execs or exits (lx_core.c), so such a
 * parent cannot run, and its other threads, if any, can. */
static int waits_on(lx_thread *t, lx_proc *q)
{
	lx_borrow *b;
	if (q == t->proc || q->nthreads > 1) return 0;
	if (q == t->owner) return 1;
	for (b = t->borrow; b; b = b->prev)
		if (b->child == q) return 1;
	return 0;
}

/* The pipe is empty, and only parents waiting on t's fork can write to it:
 * waiting would never end, so t reads the end of the file instead. bash's
 * job control does this: the first process of a job reads a pipe until the
 * shell has started the rest of the job and closed it (DECISIONS.md 49). */
static int pp_writers_wait(lx_thread *t, pipe *pp)
{
	int i, fd;
	if (!t || !t->borrow) return 0;
	for (i = 0; i < LX_MAXPROC; i++) {
		lx_proc *q = &lx_k.procs[i];
		if (q->state != LX_P_ALIVE) continue;
		for (fd = 0; fd < LX_MAXFD; fd++) {
			lx_file *w = q->fd[fd];
			if (w && w->ops == &pipe_w_ops && w->priv == pp) {
				if (!waits_on(t, q)) return 0;
				break;
			}
		}
	}
	return 1;
}

static int pp_read(lx_thread *t, lx_file *f, char *buf, lx_u32 n)
{
	pipe *pp = (pipe *)f->priv;
	lx_u32 done = 0;
	if (!pp->count) {
		if (!pp->writers || pp_writers_wait(t, pp)) return 0;
		if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
		return LX_BLOCK;
	}
	while (done < n && pp->count) {
		int chunk = LX_PIPE_SIZE - pp->head;
		if (chunk > pp->count) chunk = pp->count;
		if ((lx_u32)chunk > n - done) chunk = n - done;
		bk_memcpy(buf + done, pp->buf + pp->head, chunk);
		pp->head = (pp->head + chunk) % LX_PIPE_SIZE;
		pp->count -= chunk;
		done += chunk;
	}
	lx_k_event();
	return (int)done;
}

static int pp_write(lx_thread *t, lx_file *f, const char *buf, lx_u32 n)
{
	pipe *pp = (pipe *)f->priv;
	lx_u32 done = 0, space;
	if (!pp->readers) {
		if (t) lx_signal_thread(t, LX_SIGPIPE);
		return -LX_EPIPE;
	}
	space = LX_PIPE_SIZE - pp->count;
	/* writes of up to 4096 bytes are all or nothing (PIPE_BUF) */
	if (space == 0 || (n <= 4096 && space < n)) {
		if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
		return LX_BLOCK;
	}
	while (done < n && pp->count < LX_PIPE_SIZE) {
		int tail = (pp->head + pp->count) % LX_PIPE_SIZE;
		int chunk = LX_PIPE_SIZE - tail;
		if (chunk > LX_PIPE_SIZE - pp->count) chunk = LX_PIPE_SIZE - pp->count;
		if ((lx_u32)chunk > n - done) chunk = n - done;
		bk_memcpy(pp->buf + tail, buf + done, chunk);
		pp->count += chunk;
		done += chunk;
	}
	lx_k_event();
	return (int)done;
}

static int pp_poll_r(lx_file *f)
{
	pipe *pp = (pipe *)f->priv;
	int r = 0;
	if (pp->count) r |= LX_POLLIN;
	if (!pp->writers) r |= LX_POLLHUP;
	return r;
}
static int pp_poll_w(lx_file *f)
{
	pipe *pp = (pipe *)f->priv;
	int r = 0;
	if (LX_PIPE_SIZE - pp->count >= 4096 || (pp->count < LX_PIPE_SIZE && 0)) r |= LX_POLLOUT;
	if (!pp->readers) r |= LX_POLLERR;
	return r;
}
static int pp_fstat(lx_file *f, struct lx_stat64 *st)
{
	lx_vn v;
	bk_memset(&v, 0, sizeof v);
	v.mode = LX_S_IFIFO | 0600;
	v.ino = ((pipe *)f->priv)->ino;
	v.size = ((pipe *)f->priv)->count;
	v.mtime = bk_now_real();
	lx_vn_stat(&v, st);
	return 0;
}
static int pp_ioctl(lx_thread *t, lx_file *f, lx_u32 req, lx_u32 arg)
{
	if (req == LX_FIONREAD) {
		if (!lx_uaccess(t, arg, 4)) return -LX_EFAULT;
		*(lx_u32 *)arg = ((pipe *)f->priv)->count;
		return 0;
	}
	return -LX_ENOTTY;
}
static lx_s64 pp_lseek(lx_file *f, lx_s64 o, int w) { (void)f; (void)o; (void)w; return -LX_ESPIPE; }
static void pp_close_r(lx_file *f) { pipe *pp = (pipe *)f->priv; pp->readers--; lx_k_event(); if (!pp->readers && !pp->writers) bk_free(pp); }
static void pp_close_w(lx_file *f) { pipe *pp = (pipe *)f->priv; pp->writers--; lx_k_event(); if (!pp->readers && !pp->writers) bk_free(pp); }
static int pp_bad_w(lx_thread *t, lx_file *f, const char *b, lx_u32 n) { (void)t; (void)f; (void)b; (void)n; return -LX_EBADF; }
static int pp_bad_r(lx_thread *t, lx_file *f, char *b, lx_u32 n) { (void)t; (void)f; (void)b; (void)n; return -LX_EBADF; }

static const lx_fops pipe_r_ops = { pp_read, pp_bad_w, 0, 0, pp_lseek, pp_fstat, pp_ioctl, pp_poll_r, 0, pp_close_r, 0, 0 };
static const lx_fops pipe_w_ops = { pp_bad_r, pp_write, 0, 0, pp_lseek, pp_fstat, pp_ioctl, pp_poll_w, 0, pp_close_w, 0, 0 };

int lx_pipe_new(lx_file **r, lx_file **w)
{
	pipe *pp = (pipe *)bk_alloc(sizeof *pp);
	if (!pp) return -LX_ENOMEM;
	pp->readers = pp->writers = 1;
	pp->ino = pipe_ino++;
	*r = lx_file_new(&pipe_r_ops, pp, LX_O_RDONLY);
	*w = lx_file_new(&pipe_w_ops, pp, LX_O_WRONLY);
	if (!*r || !*w) {
		if (*r) bk_free(*r);
		if (*w) bk_free(*w);
		bk_free(pp);
		return -LX_ENOMEM;
	}
	(*r)->mode = (*w)->mode = LX_S_IFIFO | 0600;
	(*r)->ino = (*w)->ino = pp->ino;
	return 0;
}

int lx_is_pipe(lx_file *f) { return f->ops == &pipe_r_ops || f->ops == &pipe_w_ops; }

/* ---- eventfd -------------------------------------------------------------- */

typedef struct evfd { lx_u64 count; int sem; } evfd;
static int ev_read(lx_thread *t, lx_file *f, char *buf, lx_u32 n)
{
	evfd *e = (evfd *)f->priv;
	lx_u64 v;
	(void)t;
	if (n < 8) return -LX_EINVAL;
	if (!e->count) return (f->flags & LX_O_NONBLOCK) ? -LX_EAGAIN : LX_BLOCK;
	v = e->sem ? 1 : e->count;
	e->count -= v;
	bk_memcpy(buf, &v, 8);
	lx_k_event();
	return 8;
}
static int ev_write(lx_thread *t, lx_file *f, const char *buf, lx_u32 n)
{
	evfd *e = (evfd *)f->priv;
	lx_u64 v;
	(void)t;
	if (n < 8) return -LX_EINVAL;
	bk_memcpy(&v, buf, 8);
	e->count += v;
	lx_k_event();
	return 8;
}
static int ev_poll(lx_file *f) { return (((evfd *)f->priv)->count ? LX_POLLIN : 0) | LX_POLLOUT; }
static void ev_close(lx_file *f) { bk_free(f->priv); }
static const lx_fops ev_ops = { ev_read, ev_write, 0, 0, 0, 0, 0, ev_poll, 0, ev_close, 0, 0 };

lx_file *lx_eventfd_new(lx_u32 initval, int flags)
{
	evfd *e = (evfd *)bk_alloc(sizeof *e);
	lx_file *f;
	if (!e) return 0;
	e->count = initval;
	e->sem = flags & 1;
	f = lx_file_new(&ev_ops, e, LX_O_RDWR | (flags & LX_O_NONBLOCK));
	if (!f) bk_free(e);
	else f->mode = LX_S_IFREG | 0600;
	return f;
}

/* ---- /proc ------------------------------------------------------------------ */

enum {
	P_DIR = 1, P_SELF, P_PIDDIR, P_CMDLINE, P_COMM, P_STAT, P_STATUS, P_STATM, P_ENVIRON, P_MAPS,
	P_EXE, P_CWD, P_ROOT, P_FDDIR, P_FD, P_MEMINFO, P_CPUINFO, P_UPTIME, P_LOADAVG, P_VERSION,
	P_MOUNTS, P_FILESYSTEMS, P_KSTAT, P_KCMDLINE, P_SYSDIR, P_HOSTNAME, P_THREADSELF, P_MOUNTINFO,
	P_PSIDIR, P_PSIDBT, P_PSIIMAGES, P_PSIMEM
};

typedef struct pent { const char *name; int kind; lx_u32 mode; } pent;
static const pent proc_top[] = {
	{ "self", P_SELF, LX_S_IFLNK | 0777 },
	{ "meminfo", P_MEMINFO, LX_S_IFREG | 0444 },
	{ "cpuinfo", P_CPUINFO, LX_S_IFREG | 0444 },
	{ "uptime", P_UPTIME, LX_S_IFREG | 0444 },
	{ "loadavg", P_LOADAVG, LX_S_IFREG | 0444 },
	{ "version", P_VERSION, LX_S_IFREG | 0444 },
	{ "mounts", P_MOUNTS, LX_S_IFREG | 0444 },
	{ "filesystems", P_FILESYSTEMS, LX_S_IFREG | 0444 },
	{ "stat", P_KSTAT, LX_S_IFREG | 0444 },
	{ "cmdline", P_KCMDLINE, LX_S_IFREG | 0444 },
	{ "sys", P_SYSDIR, LX_S_IFDIR | 0555 },
	{ "psilinux", P_PSIDIR, LX_S_IFDIR | 0555 },
};
static const pent proc_pid[] = {
	{ "cmdline", P_CMDLINE, LX_S_IFREG | 0444 },
	{ "comm", P_COMM, LX_S_IFREG | 0644 },
	{ "stat", P_STAT, LX_S_IFREG | 0444 },
	{ "status", P_STATUS, LX_S_IFREG | 0444 },
	{ "statm", P_STATM, LX_S_IFREG | 0444 },
	{ "environ", P_ENVIRON, LX_S_IFREG | 0400 },
	{ "maps", P_MAPS, LX_S_IFREG | 0444 },
	{ "mounts", P_MOUNTS, LX_S_IFREG | 0444 },
	{ "mountinfo", P_MOUNTINFO, LX_S_IFREG | 0444 },
	{ "exe", P_EXE, LX_S_IFLNK | 0777 },
	{ "cwd", P_CWD, LX_S_IFLNK | 0777 },
	{ "root", P_ROOT, LX_S_IFLNK | 0777 },
	{ "fd", P_FDDIR, LX_S_IFDIR | 0500 },
};
#define NTOP (int)(sizeof proc_top / sizeof proc_top[0])
#define NPID (int)(sizeof proc_pid / sizeof proc_pid[0])

static int parse_num(const char **s)
{
	int n = 0, any = 0;
	while (**s >= '0' && **s <= '9') { n = n * 10 + (**s - '0'); (*s)++; any = 1; }
	return any ? n : -1;
}

int lx_proc_lookup(lx_proc *p, const char *rel, lx_vn *v)
{
	int i, pid;
	lx_proc *q;
	while (*rel == '/') rel++;
	v->mtime = bk_now_real();
	if (!*rel) { v->kind = P_DIR; v->mode = LX_S_IFDIR | 0555; return 0; }
	if (lx_strcmp(rel, "sys/kernel/hostname") == 0) { v->kind = P_HOSTNAME; v->mode = LX_S_IFREG | 0644; return 0; }
	if (lx_strcmp(rel, "sys") == 0 || lx_strcmp(rel, "sys/kernel") == 0) { v->kind = P_SYSDIR; v->mode = LX_S_IFDIR | 0555; return 0; }
	/* PsiLinux's own: the translator (Alpine mode) and its shared images */
	if (lx_strcmp(rel, "psilinux/dbt") == 0) { v->kind = P_PSIDBT; v->mode = LX_S_IFREG | 0444; return 0; }
	if (lx_strcmp(rel, "psilinux/images") == 0) { v->kind = P_PSIIMAGES; v->mode = LX_S_IFREG | 0444; return 0; }
	if (lx_strcmp(rel, "psilinux/mem") == 0) { v->kind = P_PSIMEM; v->mode = LX_S_IFREG | 0444; return 0; }
	for (i = 0; i < NTOP; i++)
		if (lx_strcmp(proc_top[i].name, rel) == 0) {
			v->kind = proc_top[i].kind;
			v->mode = proc_top[i].mode;
			if (v->kind == P_SELF) lx_snprintf(v->link, sizeof v->link, "%d", p->pid);
			return 0;
		}
	pid = parse_num(&rel);
	if (pid <= 0) return -LX_ENOENT;
	q = lx_proc_by_pid(pid);
	if (!q) return -LX_ENOENT;
	v->pid = pid;
	if (!*rel) { v->kind = P_PIDDIR; v->mode = LX_S_IFDIR | 0555; return 0; }
	if (*rel++ != '/') return -LX_ENOENT;
	if (lx_strncmp(rel, "fd/", 3) == 0) {
		lx_file *f;
		const char *s = rel + 3;
		int fd = parse_num(&s);
		if (fd < 0 || *s) return -LX_ENOENT;
		f = lx_fd_get(q, fd);
		if (!f) return -LX_ENOENT;
		v->kind = P_FD;
		v->fd = fd;
		v->mode = LX_S_IFLNK | 0700;
		lx_file_describe(f, v->link, sizeof v->link);
		return 0;
	}
	for (i = 0; i < NPID; i++)
		if (lx_strcmp(proc_pid[i].name, rel) == 0) {
			v->kind = proc_pid[i].kind;
			v->mode = proc_pid[i].mode;
			if (v->kind == P_EXE) lx_strlcpy(v->link, q->mm ? q->mm->exe : "/", sizeof v->link);
			if (v->kind == P_CWD) lx_strlcpy(v->link, q->cwd, sizeof v->link);
			if (v->kind == P_ROOT) lx_strlcpy(v->link, "/", sizeof v->link);
			if (q->state == LX_P_ZOMBIE && (v->kind == P_EXE || v->kind == P_CWD)) return -LX_ENOENT;
			return 0;
		}
	return -LX_ENOENT;
}

void lx_proc_list(lx_proc *p, lx_vn *v, void *dl)
{
	int i;
	char n[16];
	(void)p;
	if (v->kind == P_DIR) {
		for (i = 0; i < NTOP; i++)
			lx_dlist_add(dl, proc_top[i].name, LX_S_ISLNK(proc_top[i].mode) ? LX_DT_LNK : LX_S_ISDIR(proc_top[i].mode) ? LX_DT_DIR : LX_DT_REG, 0x2000 + i);
		for (i = 0; i < LX_MAXPROC; i++) {
			lx_proc *q = &lx_k.procs[i];
			if (q->state == LX_P_FREE) continue;
			lx_snprintf(n, sizeof n, "%d", q->pid);
			lx_dlist_add(dl, n, LX_DT_DIR, 0x10000 + q->pid);
		}
	} else if (v->kind == P_PIDDIR) {
		for (i = 0; i < NPID; i++)
			lx_dlist_add(dl, proc_pid[i].name, LX_S_ISLNK(proc_pid[i].mode) ? LX_DT_LNK : LX_S_ISDIR(proc_pid[i].mode) ? LX_DT_DIR : LX_DT_REG, 0);
	} else if (v->kind == P_FDDIR) {
		lx_proc *q = lx_proc_by_pid(v->pid);
		if (!q) return;
		for (i = 0; i < LX_MAXFD; i++)
			if (q->fd[i]) { lx_snprintf(n, sizeof n, "%d", i); lx_dlist_add(dl, n, LX_DT_LNK, 0); }
	} else if (v->kind == P_PSIDIR) {
		lx_dlist_add(dl, "dbt", LX_DT_REG, 0);
		lx_dlist_add(dl, "images", LX_DT_REG, 0);
		lx_dlist_add(dl, "mem", LX_DT_REG, 0);
	} else if (v->kind == P_SYSDIR) {
		if (lx_strcmp(v->path, "/proc/sys") == 0) lx_dlist_add(dl, "kernel", LX_DT_DIR, 0);
		else lx_dlist_add(dl, "hostname", LX_DT_REG, 0);
	}
}

int lx_file_describe(lx_file *f, char *buf, int size)
{
	extern int lx_is_pipe(lx_file *f);
	if (lx_is_pipe(f)) return lx_snprintf(buf, size, "pipe:[%u]", (lx_u32)f->ino);
	if ((f->mode & LX_S_IFMT) == LX_S_IFSOCK) return lx_snprintf(buf, size, "socket:[%u]", (lx_u32)f->ino);
	if (f->path) { lx_strlcpy(buf, f->path, size); return lx_strlen(buf); }
	return lx_snprintf(buf, size, "anon_inode:[%u]", (lx_u32)f->ino);
}

static char proc_state(lx_proc *q)
{
	int i;
	if (q->state == LX_P_ZOMBIE) return 'Z';
	if (q->stopped) return 'T';
	if (q->pid == 1) return 'S';
	for (i = 0; i < LX_MAXTHREAD; i++) {
		lx_thread *t = &lx_k.threads[i];
		if (t->state != LX_T_FREE && t->state != LX_T_DEAD && t->proc == q)
			return t->state == LX_T_USER || t->state == LX_T_SYSCALL ? 'R' : 'S';
	}
	return 'S';
}

static lx_u32 proc_mem(lx_proc *q)
{
	lx_u32 n = 0;
	int i;
	if (q->mm) n = q->mm->image.size + q->mm->mapped;
	for (i = 0; i < LX_MAXTHREAD; i++)
		if (lx_k.threads[i].state != LX_T_FREE && lx_k.threads[i].owner == q) n += lx_k.threads[i].stack.size;
	return n;
}

static int count_threads_of(lx_proc *q)
{
	int i, n = 0;
	for (i = 0; i < LX_MAXTHREAD; i++)
		if (lx_k.threads[i].state != LX_T_FREE && lx_k.threads[i].state != LX_T_DEAD && lx_k.threads[i].owner == q) n++;
	return n ? n : 1;
}

/* makes /proc file v's contents into buf; the length */
static int proc_text(lx_proc *p, lx_vn *v, char *buf, int size)
{
	lx_proc *q = v->pid ? lx_proc_by_pid(v->pid) : p;
	lx_s64 now = lx_now();
	lx_u32 total, freeb;
	int o = 0, i;
	if (!q) q = p;
	switch (v->kind) {
	case P_CMDLINE:
		o = q->cmdline_len < size ? q->cmdline_len : size;
		bk_memcpy(buf, q->cmdline, o);
		return o;
	case P_COMM: return lx_snprintf(buf, size, "%s\n", q->comm);
	case P_STAT: {
		lx_u32 mem = proc_mem(q);
		int tpgid = q->ctty ? q->ctty->pgrp : -1;
		lx_s64 ticks = q->cpu_us / 10000;
		lx_s64 start = q->start_time / 10000;
		return lx_snprintf(buf, size, "%d (%s) %c %d %d %d %d %d 4194304 0 0 0 0 %u 0 0 0 20 0 %d 0 %u %u %u 4294967295 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
			q->pid, q->comm, proc_state(q), q->ppid, q->pgid, q->sid, q->ctty ? 0x400 + q->ctty->index + 1 : 0, tpgid,
			(lx_u32)ticks, count_threads_of(q), (lx_u32)start, mem, mem / 4096);
	}
	case P_STATUS: {
		lx_u32 mem = proc_mem(q) / 1024;
		return lx_snprintf(buf, size,
			"Name:\t%s\nState:\t%c\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\n"
			"VmSize:\t%u kB\nVmRSS:\t%u kB\nThreads:\t%d\nSigQ:\t0/0\n",
			q->comm, proc_state(q), q->pid, q->pid, q->ppid, mem, mem, count_threads_of(q));
	}
	case P_STATM: {
		lx_u32 pages = proc_mem(q) / 4096;
		return lx_snprintf(buf, size, "%u %u 0 %u 0 %u 0\n", pages, pages, q->mm ? q->mm->image.size / 4096 : 0, pages);
	}
	case P_ENVIRON: return 0;
	case P_MAPS: {
		lx_map *m;
		if (q->mm) {
			o += lx_snprintf(buf + o, size - o, "%08x-%08x r-xp 00000000 08:01 0 %s\n", q->mm->image.base, q->mm->image.base + q->mm->image.size, q->mm->exe);
			for (m = q->mm->maps; m && o < size - 80; m = m->next)
				o += lx_snprintf(buf + o, size - o, "%08x-%08x rw-p 00000000 00:00 0\n", m->addr, m->addr + m->len);
		}
		for (i = 0; i < LX_MAXTHREAD && o < size - 80; i++) {
			lx_thread *t = &lx_k.threads[i];
			if (t->state != LX_T_FREE && t->owner == q && t->stack.size)
				o += lx_snprintf(buf + o, size - o, "%08x-%08x rw-p 00000000 00:00 0 [stack]\n", t->stack.base, t->stack.base + t->stack.size);
		}
		return o;
	}
	case P_MEMINFO:
		bk_mem_info(&total, &freeb);
		/* (Cached: programs and unused libraries kept for their next run,
		 * given back when memory is needed; available: free and cached,
		 * less the memory kept for other programs, if any) */
		{
			lx_u32 cached = lx_mem_cached(), res = bk_mem_reserve();
			lx_u32 avail = freeb + cached > res ? freeb + cached - res : 0;
			return lx_snprintf(buf, size, "MemTotal:\t%u kB\nMemFree:\t%u kB\nMemAvailable:\t%u kB\nBuffers:\t0 kB\nCached:\t%u kB\nSwapTotal:\t0 kB\nSwapFree:\t0 kB\n",
				total / 1024, freeb / 1024, avail / 1024, cached / 1024);
		}
	case P_PSIMEM: {
		lx_u32 res = bk_mem_reserve();
		bk_mem_info(&total, &freeb);
		return lx_snprintf(buf, size, "free\t%u KB (the Psion's free RAM)\ncached\t%u KB (programs and libraries kept for their next run)\nreserve\t%u KB%s\navailable\t%u KB (for Linux: free less the reserve)\nrefused\t%u (%u KB)\n",
			freeb / 1024, lx_mem_cached() / 1024, res / 1024, res ? "" : " (off)", lx_mem_avail() / 1024, lx_k.reserve_refused, lx_k.reserve_refused_kb);
	}
	case P_CPUINFO:
		return lx_snprintf(buf, size, "processor\t: 0\nmodel name\t: ARM710T (ARMv4T, Cirrus/Psion Windermere)\nBogoMIPS\t: 36.86\n"
			"Features\t: swp half thumb fastmult\nCPU implementer\t: 0x41\nCPU architecture: 4T\nCPU part\t: 0x710\n\n"
			"Hardware\t: Psion Series 5mx\nRevision\t: 0000\n");
	case P_UPTIME: return lx_snprintf(buf, size, "%u.%02u 0.00\n", (lx_u32)(now / 1000000), (lx_u32)(now / 10000 % 100));
	case P_LOADAVG: {
		int run = 0, all = 0;
		for (i = 0; i < LX_MAXPROC; i++) if (lx_k.procs[i].state == LX_P_ALIVE) { all++; if (proc_state(&lx_k.procs[i]) == 'R') run++; }
		return lx_snprintf(buf, size, "0.00 0.00 0.00 %d/%d %d\n", run, all, lx_k.next_pid - 1);
	}
	case P_VERSION: return lx_snprintf(buf, size, "Linux version 5.15.0-psilinux (PsiLinux %s, EPOC R5) #1\n", LX_VERSION);
	case P_MOUNTS: return lx_mount_list(buf, size);
	case P_MOUNTINFO: return lx_mount_info(buf, size);
	case P_FILESYSTEMS: return lx_snprintf(buf, size, "nodev\tproc\nnodev\tdevtmpfs\n\tvfat\n\tpsiroot\n");
	case P_KSTAT: {
		lx_s64 t = now / 10000;
		return lx_snprintf(buf, size, "cpu  %u 0 0 0 0 0 0 0 0 0\ncpu0 %u 0 0 0 0 0 0 0 0 0\nbtime %u\nprocesses %d\nprocs_running 1\nprocs_blocked 0\n",
			(lx_u32)t, (lx_u32)t, (lx_u32)(lx_k.boot_time / 1000000), lx_k.next_pid);
	}
	case P_KCMDLINE: return lx_snprintf(buf, size, "psilinux\n");
	case P_HOSTNAME: return lx_snprintf(buf, size, "%s\n", lx_k.hostname);
	case P_PSIDBT:
		if (!dbt_available()) return lx_snprintf(buf, size, "the translator has not run (no Alpine program yet)\n");
		dbt_stats(buf, size);
		return lx_strlen(buf);
	case P_PSIIMAGES:
		buf[0] = 0;
		lx_simg_stats(buf, size);
		return lx_strlen(buf);
	}
	return 0;
}

/* /proc/N/fd/<fd> for a pipe or socket: its link names no file
 * ("pipe:[4001]"), so opening it gives the same open file (bash's <(...)
 * opens /dev/fd/63). 0: not such a file. */
lx_file *lx_proc_fd_file(lx_vn *v)
{
	lx_proc *q;
	lx_file *f;
	if (v->fs != LX_FS_PROC || v->kind != P_FD || v->link[0] == '/') return 0;
	q = lx_proc_by_pid(v->pid);
	f = q ? lx_fd_get(q, v->fd) : 0;
	return f ? lx_file_get(f) : 0;
}

lx_file *lx_proc_open(lx_proc *p, lx_vn *v, int flags, int *err)
{
	int size = 16384, n;
	char *buf;
	lx_file *f;
	(void)flags;
	if (LX_S_ISLNK(v->mode)) { *err = -LX_ELOOP; return 0; }
	buf = (char *)bk_alloc(size);
	if (!buf) { *err = -LX_ENOMEM; return 0; }
	n = proc_text(p, v, buf, size);
	if (n > size) n = size;
	f = lx_memfile_new(buf, n);
	bk_free(buf);
	if (!f) { *err = -LX_ENOMEM; return 0; }
	f->mode = v->mode;
	return f;
}
