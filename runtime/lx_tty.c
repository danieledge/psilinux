/* lx_tty.c - PsiLinux's terminals: the line discipline (canonical editing,
 * echo, ^C ^Z ^\ as signals, termios), job control's checks (SIGTTIN,
 * SIGTTOU), window size. The keys come from the PsiLinux window
 * (lx_k_tty_input); output goes to it through bk_tty_write. */
#include "lx_core.h"
#include "lx_vfs.h"

#define CC(t, i) ((t)->tio.c_cc[i])

void lx_tty_init(lx_tty *t, int index)
{
	bk_memset(t, 0, sizeof *t);
	t->index = index;
	t->tio.c_iflag = LX_ICRNL | LX_IXON | LX_IUTF8;
	t->tio.c_oflag = LX_OPOST | LX_ONLCR;
	t->tio.c_cflag = LX_B38400 | LX_CS8 | LX_CREAD;
	t->tio.c_lflag = LX_ISIG | LX_ICANON | LX_ECHO | LX_ECHOE | LX_ECHOK | LX_ECHOCTL | LX_ECHOKE | LX_IEXTEN;
	t->tio.c_ispeed = t->tio.c_ospeed = LX_B38400;
	CC(t, LX_VINTR) = 3;
	CC(t, LX_VQUIT) = 0x1c;
	CC(t, LX_VERASE) = 0x7f;
	CC(t, LX_VKILL) = 0x15;
	CC(t, LX_VEOF) = 4;
	CC(t, LX_VTIME) = 0;
	CC(t, LX_VMIN) = 1;
	CC(t, LX_VSTART) = 0x11;
	CC(t, LX_VSTOP) = 0x13;
	CC(t, LX_VSUSP) = 0x1a;
	CC(t, LX_VREPRINT) = 0x12;
	CC(t, LX_VWERASE) = 0x17;
	CC(t, LX_VLNEXT) = 0x16;
	t->ws.ws_row = 24;
	t->ws.ws_col = 80;
}

/* ---- output --------------------------------------------------------------- */

/* Writes with output processing (ONLCR). Returns the source bytes taken. */
static int out_bytes(lx_tty *t, const char *buf, int n, int must)
{
	char tmp[512];
	int i = 0, o, room, crlf = (t->tio.c_oflag & LX_OPOST) && (t->tio.c_oflag & LX_ONLCR);
	extern int bk_tty_room(lx_tty *tty);
	room = bk_tty_room(t);
	if (must && room < n * 2) room = n * 2;     /* (echo: never blocks; the back end drops what does not fit) */
	if (room > (int)sizeof tmp) room = sizeof tmp;
	o = 0;
	while (i < n) {
		char c = buf[i];
		if (c == '\n' && crlf) {
			if (o + 2 > room) break;
			tmp[o++] = '\r';
			tmp[o++] = '\n';
			t->column = 0;
		} else {
			if (o + 1 > room) break;
			tmp[o++] = c;
			if (c == '\r') t->column = 0;
			else if (c == '\b') { if (t->column) t->column--; }
			else if (c == '\t') t->column = (t->column | 7) + 1;
			else if ((unsigned char)c >= 32 && ((unsigned char)c & 0xc0) != 0x80) t->column++;
		}
		i++;
	}
	if (o) bk_tty_write(t, tmp, o);
	return i;
}

static void echo(lx_tty *t, const char *s, int n) { if (t->tio.c_lflag & LX_ECHO) out_bytes(t, s, n, 1); }

static void echo_char(lx_tty *t, unsigned char c)
{
	if (!(t->tio.c_lflag & LX_ECHO)) return;
	if ((t->tio.c_lflag & LX_ECHOCTL) && (c < 32 || c == 0x7f) && c != '\n' && c != '\t') {
		char s[2];
		s[0] = '^';
		s[1] = c == 0x7f ? '?' : (char)(c + 64);
		out_bytes(t, s, 2, 1);
	} else out_bytes(t, (const char *)&c, 1, 1);
}

/* ---- input --------------------------------------------------------------- */

static int in_count(lx_tty *t) { return (t->in_tail - t->in_head + LX_TTY_IN) % LX_TTY_IN; }

static void in_put(lx_tty *t, char c)
{
	int next = (t->in_tail + 1) % LX_TTY_IN;
	if (next == t->in_head) return;            /* full: dropped */
	t->in[t->in_tail] = c;
	t->in_tail = next;
}

static void flush_input(lx_tty *t)
{
	t->in_head = t->in_tail = 0;
	t->lines = 0;
	t->line_len = 0;
	t->eof = 0;
}

/* erase one character of the line being edited (a whole UTF-8 sequence) */
static void erase_one(lx_tty *t, int echo_it)
{
	int n = 1;
	unsigned char c;
	if (!t->line_len) return;
	while (n < t->line_len && ((unsigned char)t->line[t->line_len - n] & 0xc0) == 0x80) n++;
	c = (unsigned char)t->line[t->line_len - n];
	t->line_len -= n;
	if (!echo_it || !(t->tio.c_lflag & LX_ECHO)) return;
	if ((t->tio.c_lflag & LX_ECHOE) || 1) {
		int w = (c < 32 || c == 0x7f) && (t->tio.c_lflag & LX_ECHOCTL) && c != '\t' ? 2 : 1;
		while (w--) out_bytes(t, "\b \b", 3, 1);
	}
}

static lx_s64 last_intr;
static int last_intr_pgrp;

static void send_sig(lx_tty *t, int sig)
{
	if (!(t->tio.c_lflag & LX_NOFLSH)) flush_input(t);
	if (sig == LX_SIGINT && t->pgrp) {
		/* a second ^C while the first is still waiting to be taken (the
		 * program is busy and makes no calls): end the job */
		lx_s64 now = lx_now();
		int i, stuck = 0;
		if (t->pgrp == last_intr_pgrp && now - last_intr < 5000000) {
			for (i = 0; i < LX_MAXPROC; i++) {
				lx_proc *p = &lx_k.procs[i];
				if (p->state == LX_P_ALIVE && p->pgid == t->pgrp && (p->sigpending[0] & (1u << (LX_SIGINT - 1)))) stuck = 1;
			}
		}
		last_intr = now;
		last_intr_pgrp = t->pgrp;
		if (stuck) { lx_signal_pgrp(t->pgrp, LX_SIGKILL); return; }
	}
	if (t->pgrp) lx_signal_pgrp(t->pgrp, sig);
}

void lx_k_tty_input(lx_tty *t, const char *buf, int n)
{
	int i;
	struct lx_termios *tio = &t->tio;
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)buf[i];
		if (t->lnext) {
			t->lnext = 0;
			goto literal;
		}
		if (c == '\r') {
			if (tio->c_iflag & LX_IGNCR) continue;
			if (tio->c_iflag & LX_ICRNL) c = '\n';
		} else if (c == '\n' && (tio->c_iflag & LX_INLCR)) c = '\r';
		if (tio->c_lflag & LX_ISIG) {
			if (c == CC(t, LX_VINTR) && c) { echo_char(t, c); send_sig(t, LX_SIGINT); continue; }
			if (c == CC(t, LX_VQUIT) && c) { echo_char(t, c); send_sig(t, LX_SIGQUIT); continue; }
			if (c == CC(t, LX_VSUSP) && c) { echo_char(t, c); send_sig(t, LX_SIGTSTP); continue; }
		}
		if (tio->c_iflag & LX_IXON) {
			if (c == CC(t, LX_VSTOP) && c) { t->stopped = 1; continue; }
			if (c == CC(t, LX_VSTART) && c) { t->stopped = 0; lx_k_event(); continue; }
		}
		if (tio->c_lflag & LX_ICANON) {
			if ((tio->c_lflag & LX_IEXTEN) && c == CC(t, LX_VLNEXT) && c) { t->lnext = 1; continue; }
			if (c == CC(t, LX_VERASE) || (c == '\b' && CC(t, LX_VERASE) == 0x7f)) { erase_one(t, 1); continue; }
			if (c == CC(t, LX_VKILL) && c) { while (t->line_len) erase_one(t, 1); continue; }
			if ((tio->c_lflag & LX_IEXTEN) && c == CC(t, LX_VWERASE) && c) {
				while (t->line_len && t->line[t->line_len - 1] == ' ') erase_one(t, 1);
				while (t->line_len && t->line[t->line_len - 1] != ' ') erase_one(t, 1);
				continue;
			}
			if ((tio->c_lflag & LX_IEXTEN) && c == CC(t, LX_VREPRINT) && c) {
				echo_char(t, c);
				echo(t, "\n", 1);
				echo(t, t->line, t->line_len);
				continue;
			}
			if (c == CC(t, LX_VEOF) && c) {
				int j;
				for (j = 0; j < t->line_len; j++) in_put(t, t->line[j]);
				if (!t->line_len) t->eof = 1;
				else t->lines++;
				t->line_len = 0;
				lx_k_event();
				continue;
			}
			if (c == '\n' || (c == CC(t, LX_VEOL) && c) || (c == CC(t, LX_VEOL2) && c)) {
				int j;
				for (j = 0; j < t->line_len; j++) in_put(t, t->line[j]);
				in_put(t, (char)c);
				t->line_len = 0;
				t->lines++;
				if ((tio->c_lflag & LX_ECHO) || (tio->c_lflag & LX_ECHONL)) out_bytes(t, "\n", 1, 1);
				lx_k_event();
				continue;
			}
		literal:
			if (t->tio.c_lflag & LX_ICANON) {
				if (t->line_len < LX_TTY_IN - 2) {
					t->line[t->line_len++] = (char)c;
					echo_char(t, c);
				}
				continue;
			}
		}
		in_put(t, (char)c);
		echo_char(t, c);
		lx_k_event();
	}
	lx_k_event();
}

void lx_k_tty_writable(lx_tty *t) { (void)t; lx_k_event(); }

void lx_k_tty_resize(lx_tty *t, int rows, int cols)
{
	if (t->ws.ws_row == rows && t->ws.ws_col == cols) return;
	t->ws.ws_row = (lx_u16)rows;
	t->ws.ws_col = (lx_u16)cols;
	if (t->pgrp) lx_signal_pgrp(t->pgrp, LX_SIGWINCH);
	lx_k_event();
}

void lx_k_tty_hangup(lx_tty *t)
{
	t->hangup = 1;
	if (t->pgrp) lx_signal_pgrp(t->pgrp, LX_SIGHUP);
	lx_k_event();
}

/* ---- the file ------------------------------------------------------------- */

/* a background job touching its terminal: SIGTTIN / SIGTTOU */
static int job_check(lx_thread *th, lx_tty *t, int sig)
{
	lx_proc *p;
	if (!th) return 0;
	p = th->proc;
	if (p->ctty != t || !t->pgrp || p->pgid == t->pgrp) return 0;
	{
		struct lx_ksigaction *sa = &p->sigact[sig];
		lx_u32 bit = 1u << (sig - 1);
		if (sa->handler == LX_SIG_IGN || (th->sigmask[0] & bit)) return sig == LX_SIGTTIN ? -LX_EIO : 0;
	}
	lx_signal_pgrp(p->pgid, sig);
	return LX_BLOCK;
}

static int tty_read(lx_thread *th, lx_file *f, char *buf, lx_u32 n)
{
	lx_tty *t = (lx_tty *)f->priv;
	lx_u32 done = 0;
	int r = job_check(th, t, LX_SIGTTIN);
	if (r) return r;
	if (t->hangup) return 0;
	if (t->tio.c_lflag & LX_ICANON) {
		if (!t->lines) {
			if (t->eof) { t->eof = 0; return 0; }
			return (f->flags & LX_O_NONBLOCK) ? -LX_EAGAIN : LX_BLOCK;
		}
		while (done < n && in_count(t)) {
			char c = t->in[t->in_head];
			t->in_head = (t->in_head + 1) % LX_TTY_IN;
			buf[done++] = c;
			if (c == '\n' || (c == (char)CC(t, LX_VEOL) && c)) { t->lines--; break; }
		}
		/* (a line ended by ^D has no newline: count it when it is all read) */
		if (done && buf[done - 1] != '\n' && !in_count(t) && t->lines) t->lines--;
		return (int)done;
	}
	if (!in_count(t)) {
		int vmin = CC(t, LX_VMIN), vtime = CC(t, LX_VTIME);
		if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
		if (vmin == 0 && vtime == 0) return 0;
		if (vmin == 0 && th) {
			if (th->timed_out) { th->timed_out = 0; th->deadline = 0; return 0; }
			if (!th->deadline) th->deadline = lx_now() + (lx_s64)vtime * 100000;
		}
		return LX_BLOCK;
	}
	while (done < n && in_count(t)) {
		buf[done++] = t->in[t->in_head];
		t->in_head = (t->in_head + 1) % LX_TTY_IN;
	}
	return (int)done;
}

static int tty_write(lx_thread *th, lx_file *f, const char *buf, lx_u32 n)
{
	lx_tty *t = (lx_tty *)f->priv;
	int done;
	if (t->tio.c_lflag & LX_TOSTOP) {
		int r = job_check(th, t, LX_SIGTTOU);
		if (r) return r;
	}
	if (t->hangup) return -LX_EIO;
	if (!n) return 0;
	if (t->stopped) return (f->flags & LX_O_NONBLOCK) ? -LX_EAGAIN : LX_BLOCK;
	done = out_bytes(t, buf, (int)n, 0);
	if (!done) return (f->flags & LX_O_NONBLOCK) ? -LX_EAGAIN : LX_BLOCK;
	return done;
}

static int tty_poll(lx_file *f)
{
	lx_tty *t = (lx_tty *)f->priv;
	extern int bk_tty_room(lx_tty *tty);
	int r = 0;
	if (t->hangup) return LX_POLLIN | LX_POLLHUP;
	if (t->tio.c_lflag & LX_ICANON) { if (t->lines || t->eof) r |= LX_POLLIN; }
	else if (in_count(t)) r |= LX_POLLIN;
	if (bk_tty_room(t) > 0 && !t->stopped) r |= LX_POLLOUT;
	return r;
}

int lx_tty_ioctl(lx_thread *th, lx_tty *t, lx_u32 req, lx_u32 arg)
{
	lx_proc *p = th->proc;
	switch (req) {
	case LX_TCGETS:
		if (!lx_uaccess(th, arg, sizeof(struct lx_termios))) return -LX_EFAULT;
		bk_memcpy((void *)arg, &t->tio, sizeof t->tio);
		return 0;
	case LX_TCSETSF: flush_input(t); /* fall through */
	case LX_TCSETS: case LX_TCSETSW: {
		int was_canon = t->tio.c_lflag & LX_ICANON;
		if (!lx_uaccess(th, arg, sizeof(struct lx_termios))) return -LX_EFAULT;
		bk_memcpy(&t->tio, (const void *)arg, sizeof t->tio);
		if (was_canon && !(t->tio.c_lflag & LX_ICANON)) {
			/* the line being edited becomes input */
			int j;
			for (j = 0; j < t->line_len; j++) in_put(t, t->line[j]);
			t->line_len = 0;
		}
		if (!(t->tio.c_lflag & LX_ICANON)) t->lines = 0;
		lx_k_event();
		return 0;
	}
	case LX_TIOCGWINSZ:
		if (!lx_uaccess(th, arg, sizeof(struct lx_winsize))) return -LX_EFAULT;
		bk_memcpy((void *)arg, &t->ws, sizeof t->ws);
		return 0;
	case LX_TIOCSWINSZ:
		if (!lx_uaccess(th, arg, sizeof(struct lx_winsize))) return -LX_EFAULT;
		{
			struct lx_winsize *w = (struct lx_winsize *)arg;
			lx_k_tty_resize(t, w->ws_row, w->ws_col);
		}
		return 0;
	case LX_TIOCGPGRP:
		if (p->ctty != t) return -LX_ENOTTY;
		if (!lx_uaccess(th, arg, 4)) return -LX_EFAULT;
		*(lx_s32 *)arg = t->pgrp ? t->pgrp : 0x7fffffff;
		return 0;
	case LX_TIOCSPGRP: {
		int pg, i, ok = 0;
		if (p->ctty != t || t->sid != p->sid) return -LX_ENOTTY;
		if (!lx_uaccess(th, arg, 4)) return -LX_EFAULT;
		pg = *(lx_s32 *)arg;
		if (pg <= 0) return -LX_EINVAL;
		for (i = 0; i < LX_MAXPROC; i++)
			if (lx_k.procs[i].state == LX_P_ALIVE && lx_k.procs[i].pgid == pg && lx_k.procs[i].sid == p->sid) ok = 1;
		if (!ok) return -LX_EPERM;
		/* (a background job may not: SIGTTOU, unless it is ignored or blocked) */
		if (p->pgid != t->pgrp) {
			struct lx_ksigaction *sa = &p->sigact[LX_SIGTTOU];
			if (sa->handler != LX_SIG_IGN && !(th->sigmask[0] & (1u << (LX_SIGTTOU - 1)))) {
				lx_signal_pgrp(p->pgid, LX_SIGTTOU);
				return LX_BLOCK;
			}
		}
		t->pgrp = pg;
		lx_k_event();
		return 0;
	}
	case LX_TIOCGSID:
		if (p->ctty != t) return -LX_ENOTTY;
		if (!lx_uaccess(th, arg, 4)) return -LX_EFAULT;
		*(lx_s32 *)arg = t->sid;
		return 0;
	case LX_TIOCSCTTY:
		if (p->sid != p->pid) return -LX_EPERM;
		if (t->sid && t->sid != p->sid && !arg) return -LX_EPERM;
		p->ctty = t;
		t->sid = p->sid;
		t->pgrp = p->pgid;
		return 0;
	case LX_TIOCNOTTY:
		if (p->ctty != t) return -LX_ENOTTY;
		if (p->sid == p->pid) { if (t->pgrp) lx_signal_pgrp(t->pgrp, LX_SIGHUP); t->sid = 0; t->pgrp = 0; }
		p->ctty = 0;
		return 0;
	case LX_FIONREAD:
		if (!lx_uaccess(th, arg, 4)) return -LX_EFAULT;
		*(lx_s32 *)arg = (t->tio.c_lflag & LX_ICANON) && !t->lines ? 0 : in_count(t);
		return 0;
	case LX_TIOCOUTQ:
		if (!lx_uaccess(th, arg, 4)) return -LX_EFAULT;
		*(lx_s32 *)arg = 0;
		return 0;
	case LX_TCFLSH:
		if (arg == 0 || arg == 2) flush_input(t);
		return 0;
	case LX_TCXONC:
		if (arg == 0) t->stopped = 1;
		if (arg == 1) { t->stopped = 0; lx_k_event(); }
		return 0;
	case LX_TCSBRK:
		return 0;
	}
	return -LX_ENOTTY;
}

static int tty_ioctl(lx_thread *th, lx_file *f, lx_u32 req, lx_u32 arg) { return lx_tty_ioctl(th, (lx_tty *)f->priv, req, arg); }

static int tty_fstat(lx_file *f, struct lx_stat64 *st)
{
	lx_vn v;
	lx_tty *t = (lx_tty *)f->priv;
	bk_memset(&v, 0, sizeof v);
	v.mode = LX_S_IFCHR | 0620;
	v.rdev = 0x400 + t->index + 1;
	v.ino = 0x1100 + t->index;
	v.fs = LX_FS_DEV;
	v.mtime = bk_now_real();
	lx_vn_stat(&v, st);
	return 0;
}

static lx_s64 tty_lseek(lx_file *f, lx_s64 o, int w) { (void)f; (void)o; (void)w; return -LX_ESPIPE; }
static void tty_close(lx_file *f) { lx_tty *t = (lx_tty *)f->priv; if (t->open > 0) t->open--; }

static const lx_fops tty_ops = { tty_read, tty_write, 0, 0, tty_lseek, tty_fstat, tty_ioctl, tty_poll, 0, tty_close, 0, 0 };

lx_file *lx_tty_file(lx_tty *t)
{
	lx_file *f = lx_file_new(&tty_ops, t, LX_O_RDWR);
	char name[16];
	if (!f) return 0;
	t->open++;
	f->mode = LX_S_IFCHR | 0620;
	f->ino = 0x1100 + t->index;
	lx_snprintf(name, sizeof name, "/dev/tty%d", t->index + 1);
	f->path = lx_strdup(name);
	return f;
}

int lx_is_tty(lx_file *f) { return f->ops == &tty_ops; }
