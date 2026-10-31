/* lx_pty.c - pseudo-terminals: /dev/ptmx and /dev/pts/N (xterm, ssh's
 * server, script, tmux).
 *
 * The slave is an ordinary terminal (lx_tty.c: the line discipline, job
 * control, termios) whose output goes into the master's buffer instead of
 * the PsiLinux window; what the master writes is the slave's typed input.
 * The master: read takes the slave's output (EIO once the slave side has
 * been closed by everyone, as Linux), write types, TIOCGPTN names the
 * slave, TIOCSPTLCK unlocks it, TIOCSWINSZ sizes it (SIGWINCH). Closing the
 * master hangs the slave up (SIGHUP to its session).
 */
#include "lx_core.h"
#include "lx_vfs.h"

#define LX_TIOCGPTN  0x80045430
#define LX_TIOCSPTLCK 0x40045431
#define PTY_OUT 4096

struct lx_pty {
	lx_tty tty;                 /* the slave (tty.pty points back here) */
	int num;                    /* /dev/pts/<num> */
	int master;                 /* the master is open */
	int locked;                 /* TIOCSPTLCK: the slave cannot be opened yet */
	int slave_seen;             /* the slave has been opened (EIO only after that) */
	int head, count;            /* the slave's output, for the master to read */
	char out[PTY_OUT];
};

static lx_pty *ptys[LX_MAXPTY];

/* ---- the slave's output (lx_tty.c calls these for a pty) ----------------------- */

int lx_pty_room(lx_tty *t)
{
	return PTY_OUT - t->pty->count;
}

int lx_pty_write(lx_tty *t, const char *buf, int n)
{
	lx_pty *p = t->pty;
	int i;
	if (!p->master) return n;                /* (nobody reads: gone) */
	if (n > PTY_OUT - p->count) n = PTY_OUT - p->count;
	for (i = 0; i < n; i++) p->out[(p->head + p->count + i) % PTY_OUT] = buf[i];
	p->count += n;
	if (n) lx_k_event();
	return n;
}

static void pty_free_if_unused(lx_pty *p)
{
	if (p->master || p->tty.open) return;
	ptys[p->num] = 0;
	bk_free(p);
}

/* a slave file closed (lx_tty.c) */
void lx_pty_slave_closed(lx_tty *t)
{
	lx_pty *p = t->pty;
	lx_k_event();                            /* (the master's read: EIO, maybe) */
	if (!p->master) pty_free_if_unused(p);
}

/* ---- the master -------------------------------------------------------------- */

static int pm_read(lx_thread *th, lx_file *f, char *buf, lx_u32 n)
{
	lx_pty *p = (lx_pty *)f->priv;
	lx_u32 done = 0;
	(void)th;
	if (!p->count) {
		if (p->slave_seen && !p->tty.open) return -LX_EIO;
		return (f->flags & LX_O_NONBLOCK) ? -LX_EAGAIN : LX_BLOCK;
	}
	while (done < n && p->count) {
		buf[done++] = p->out[p->head];
		p->head = (p->head + 1) % PTY_OUT;
		p->count--;
	}
	lx_k_tty_writable(&p->tty);              /* (room: the slave's writers go on) */
	return (int)done;
}

static int pm_write(lx_thread *th, lx_file *f, const char *buf, lx_u32 n)
{
	lx_pty *p = (lx_pty *)f->priv;
	(void)th; (void)f;
	lx_k_tty_input(&p->tty, buf, (int)n);
	return (int)n;
}

static int pm_poll(lx_file *f)
{
	lx_pty *p = (lx_pty *)f->priv;
	int r = LX_POLLOUT;
	if (p->count) r |= LX_POLLIN;
	else if (p->slave_seen && !p->tty.open) r |= LX_POLLIN | LX_POLLHUP;
	return r;
}

static int pm_ioctl(lx_thread *th, lx_file *f, lx_u32 req, lx_u32 arg)
{
	lx_pty *p = (lx_pty *)f->priv;
	switch (req) {
	case LX_TIOCGPTN:
		if (!lx_uaccess(th, arg, 4)) return -LX_EFAULT;
		*(lx_u32 *)arg = (lx_u32)p->num;
		return 0;
	case LX_TIOCSPTLCK:
		if (!lx_uaccess(th, arg, 4)) return -LX_EFAULT;
		p->locked = *(lx_s32 *)arg != 0;
		return 0;
	case LX_FIONREAD:
		if (!lx_uaccess(th, arg, 4)) return -LX_EFAULT;
		*(lx_s32 *)arg = p->count;
		return 0;
	case LX_TIOCSCTTY: case LX_TIOCNOTTY:
		return -LX_ENOTTY;                   /* (only the slave can be a controlling terminal) */
	}
	/* termios, window size, process groups: the slave's */
	return lx_tty_ioctl(th, &p->tty, req, arg);
}

static int pm_fstat(lx_file *f, struct lx_stat64 *st)
{
	lx_vn v;
	bk_memset(&v, 0, sizeof v);
	v.mode = f->mode;
	v.ino = f->ino;
	v.rdev = 0x0502;            /* (5, 2): /dev/ptmx */
	v.fs = LX_FS_DEV;
	lx_vn_stat(&v, st);
	return 0;
}

static lx_s64 pm_lseek(lx_file *f, lx_s64 o, int w) { (void)f; (void)o; (void)w; return -LX_ESPIPE; }

static void pm_close(lx_file *f)
{
	lx_pty *p = (lx_pty *)f->priv;
	p->master = 0;
	p->count = 0;
	if (p->tty.open) lx_k_tty_hangup(&p->tty);
	pty_free_if_unused(p);
}

static const lx_fops pm_ops = { pm_read, pm_write, 0, 0, pm_lseek, pm_fstat, pm_ioctl, pm_poll, 0, pm_close, 0, 0, 0 };

/* /dev/ptmx: a new pair, its master open */
lx_file *lx_ptmx_open(int flags, int *err)
{
	lx_pty *p;
	lx_file *f;
	int i;
	for (i = 0; i < LX_MAXPTY && ptys[i]; i++) ;
	if (i == LX_MAXPTY) { *err = -LX_ENOSPC; return 0; }
	p = (lx_pty *)bk_alloc(sizeof *p);
	if (!p) { *err = -LX_ENOMEM; return 0; }
	lx_tty_init(&p->tty, LX_PTY_INDEX + i);
	p->tty.pty = p;
	p->num = i;
	p->master = 1;
	p->locked = 1;
	f = lx_file_new(&pm_ops, p, flags);
	if (!f) { bk_free(p); *err = -LX_ENOMEM; return 0; }
	f->mode = LX_S_IFCHR | 0666;
	f->ino = 0x1400 + i;
	ptys[i] = p;
	return f;
}

/* /dev/pts/<num>: the slave terminal (0 if there is none) */
lx_tty *lx_pts_find(int num, int *locked)
{
	lx_pty *p = num >= 0 && num < LX_MAXPTY ? ptys[num] : 0;
	if (!p || !p->master) return 0;
	if (locked) *locked = p->locked;
	return &p->tty;
}

void lx_pts_opened(lx_tty *t) { t->pty->slave_seen = 1; }

int lx_pts_list(int *nums)
{
	int i, n = 0;
	for (i = 0; i < LX_MAXPTY; i++) if (ptys[i] && ptys[i]->master) nums[n++] = i;
	return n;
}
