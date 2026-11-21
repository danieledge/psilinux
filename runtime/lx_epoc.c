/* lx_epoc.c - /dev/epoc: Linux programs (the epoc command) asking EPOC to do
 * things: open a file in its program, start a program, the clipboard, an
 * infoprint, the task list... The format is common/lxepoc.h.
 *
 * A request is one write(), its answer one read(). The kernel checks the
 * request, makes its Linux paths EPOC names, answers a few itself (HELLO,
 * SYSWRITE, PATH) and hands the rest to the backend: on the Psion the runner
 * puts it in the shared chunk for PsiLinux.app, which does the work. One
 * request is at the app at a time; others wait their turn (their write
 * parks). A read parks until the answer is there; a signal interrupts it, and
 * an answer nobody waits for any more is dropped.
 *
 * Who may: only a program whose controlling terminal is the Psion's own
 * (the PsiLinux window), never one on a pseudo-terminal (an ssh login, say)
 * or with no terminal at all. Nothing here listens on the network. */
#include "lx_core.h"
#include "lx_vfs.h"
#include "../common/lxepoc.h"

#define HDR ((int)sizeof(LxeHeader))

typedef struct epf {
	int state;                  /* 0 idle, 1 waiting for the slot, 2 at the app, 3 answered */
	char *req;                  /* the request, as the backend gets it */
	int reqlen;
	char *ans;                  /* the answer, as read() gives it */
	int anslen;
} epf;

enum { EP_IDLE = 0, EP_QUEUED, EP_SENT, EP_DONE };

static epf *slot_owner;         /* whose request is at the app (0: none, or abandoned) */
static int slot_busy;

/* an answer made here */
static void answer(epf *e, const LxeHeader *rq, int status, int err, lx_u32 arg, const char *text)
{
	LxeHeader h;
	int n = text ? lx_strlen(text) : 0;
	if (n > LXE_MAX) n = LXE_MAX;
	if (!e->ans) e->ans = (char *)bk_alloc(HDR + LXE_MAX + 1);
	if (!e->ans) { e->state = EP_IDLE; return; }
	bk_memset(&h, 0, sizeof h);
	h.magic = LXE_MAGIC;
	h.version = LXE_VERSION;
	h.op = rq ? rq->op : 0;
	h.status = status;
	h.err = err;
	h.arg = arg;
	h.len = (unsigned)n;
	bk_memcpy(e->ans, &h, HDR);
	if (n) bk_memcpy(e->ans + HDR, text, n);
	e->anslen = HDR + n;
	e->state = EP_DONE;
}

/* the i'th NUL-ended string of a payload, or 0 */
static const char *arg_at(const char *p, int len, int i, int *at)
{
	int o = 0, k = 0;
	while (o < len) {
		int s = o;
		while (o < len && p[o]) o++;
		if (o >= len) return 0;               /* (not NUL-ended) */
		if (k == i) { if (at) *at = s; return p + s; }
		o++;
		k++;
	}
	return 0;
}

/* A Linux path -> the EPOC name of the file. The file must be on a Psion
 * disk (or in the root's writable layer, which is a folder on one); 'out'
 * may name a file that is not there yet (epoc shot's). */
static int epoc_name(lx_proc *p, const char *path, int may_be_new, char *native, int max, const char **why)
{
	char c[LX_PATH_MAX];
	lx_vn v;
	int r = lx_vfs_resolve(p, LX_AT_FDCWD, path, 1, c);
	if (r < 0) { *why = "no such folder"; return LXE_NOTFOUND; }
	r = lx_vn_lookup(p, c, &v);
	if (r < 0 && !(may_be_new && r == -LX_ENOENT)) { *why = "no such file"; return LXE_NOTFOUND; }
	/* (a file to be written - epoc shot's - not where Linux may not write: Z:, a \\System folder) */
	if (may_be_new && lx_vn_writable(&v) < 0) { *why = "read-only to Linux (the ROM, or a System folder)"; return LXE_REFUSED; }
	if (v.fs == LX_FS_NATIVE && v.native[0]) { lx_strlcpy(native, v.native, max); return LXE_OK; }
	if (v.fs == LX_FS_ROOT && r == 0 && v.upper && v.native[0]) { lx_strlcpy(native, v.native, max); return LXE_OK; }
	*why = "not on a Psion disk (use /psion/c, /psion/d...)";
	return LXE_REFUSED;
}

static void hello(epf *e, const LxeHeader *rq)
{
	char t[160];
	lx_snprintf(t, sizeof t, "proto\t%d\npsilinux\t%s\napp\t%d\nsyswrite\t%s\n", LXE_VERSION, LX_VERSION,
		bk_epoc_ready(), lx_k.sys_write ? "on" : "off");
	answer(e, rq, LXE_OK, 0, LXE_VERSION, t);
}

/* A request written: check it, and answer it or queue it. */
static int ep_write(lx_thread *t, lx_file *f, const char *buf, lx_u32 n)
{
	epf *e = (epf *)f->priv;
	LxeHeader h;
	lx_proc *p = t ? t->proc : 0;
	const char *pl;
	int plen, i;
	if (e->state == EP_QUEUED || e->state == EP_SENT) return -LX_EBUSY;
	if (n < (lx_u32)HDR || n > (lx_u32)(HDR + LXE_MAX)) return -LX_EINVAL;
	bk_memcpy(&h, buf, HDR);
	if (h.magic != LXE_MAGIC || h.len != n - HDR) return -LX_EINVAL;
	e->state = EP_IDLE;
	if (e->ans) e->anslen = 0;
	if (h.version != LXE_VERSION || h.op < 1 || h.op > LXE_OP_LAST || (h.op > LXE_OP_PATH && h.op < LXE_OP_OPEN)) {
		answer(e, &h, LXE_BADVERSION, 0, LXE_VERSION, "this PsiLinux does not know that request: update PsiLinux");
		return (int)n;
	}
	pl = buf + HDR;
	plen = (int)h.len;
	switch (h.op) {
	case LXE_OP_HELLO:
		hello(e, &h);
		return (int)n;
	case LXE_OP_SYSWRITE:
		if (h.arg == 1 && !(h.flags & LXE_F_FORCE)) {
			answer(e, &h, LXE_REFUSED, 0, 0, "writing to the System folders needs --force");
			return (int)n;
		}
		if (h.arg == 1) { lx_k.sys_write = 1; bk_log("epoc: pid %d allowed writing to \\System", p ? p->pid : 0); }
		if (h.arg == 2) lx_k.sys_write = 0;
		answer(e, &h, LXE_OK, 0, (lx_u32)lx_k.sys_write, lx_k.sys_write ? "on\n" : "off\n");
		return (int)n;
	}
	/* the Linux paths in it become EPOC names */
	if (!e->req) e->req = (char *)bk_alloc(HDR + LXE_MAX + LX_PATH_MAX + 1);
	if (!e->req) return -LX_ENOMEM;
	{
		int o = HDR, k = 0, at;
		const char *s;
		while ((s = arg_at(pl, plen, k, &at)) != 0) {
			char nat[LX_PATH_MAX];
			const char *why = 0;
			int len;
			if ((k == 0 && (h.flags & LXE_F_PATH)) || (k == 1 && (h.flags & LXE_F_PATH2))) {
				int st = epoc_name(p, s, h.op == LXE_OP_SHOT, nat, sizeof nat, &why);
				if (st != LXE_OK) {
					char msg[LX_PATH_MAX + 64];
					lx_snprintf(msg, sizeof msg, "%s: %s", s, why);
					answer(e, &h, st, 0, 0, msg);
					return (int)n;
				}
				s = nat;
			} else if (k == 0 && h.op == LXE_OP_SHOT) {
				/* (0.42) epoc shot's file given as an EPOC name (no
				 * LXE_F_PATH: "C:\\..." as it is): the same test as a Linux
				 * path's, on its Linux name (/psion/c/...), so a request
				 * written to /dev/epoc without the flag cannot write into
				 * Z: or a \\System folder either */
				char lp[LX_PATH_MAX];
				int st, j, o2;
				const char *why2 = 0;
				if (!(((s[0] | 0x20) >= 'a' && (s[0] | 0x20) <= 'z') && s[1] == ':' && s[2] == '\\')) {
					answer(e, &h, LXE_REFUSED, 0, 0, "epoc shot: a whole EPOC name (D:\\Screenshots\\a.mbm) or a Linux path");
					return (int)n;
				}
				lx_snprintf(lp, sizeof lp, "/psion/%c", s[0] | 0x20);
				o2 = lx_strlen(lp);
				for (j = 2; s[j] && o2 < (int)sizeof lp - 1; j++) {
					if (s[j] == '/') {           /* (not a separator to EPOC: not taken) */
						answer(e, &h, LXE_REFUSED, 0, 0, "epoc shot: a / in an EPOC name");
						return (int)n;
					}
					lp[o2++] = s[j] == '\\' ? '/' : s[j];
				}
				lp[o2] = 0;
				if (s[j]) return -LX_ENAMETOOLONG;
				st = epoc_name(p, lp, 1, nat, sizeof nat, &why2);
				if (st != LXE_OK) {
					char msg[LX_PATH_MAX + 64];
					lx_snprintf(msg, sizeof msg, "%s: %s", s, why2);
					answer(e, &h, st, 0, 0, msg);
					return (int)n;
				}
				s = nat;
			}
			len = lx_strlen(s) + 1;
			if (o + len > HDR + LXE_MAX) return -LX_E2BIG;
			bk_memcpy(e->req + o, s, len);
			o += len;
			k++;
		}
		if (k == 0 && plen) { bk_memcpy(e->req + HDR, pl, plen); o = HDR + plen; }   /* (not strings: as it is) */
		e->req[o] = 0;                           /* (PATH below reads it as a string, even with none sent) */
		h.len = (unsigned)(o - HDR);
		e->reqlen = o;
	}
	if (h.op == LXE_OP_PATH) {
		char t2[LX_PATH_MAX + 2];
		lx_snprintf(t2, sizeof t2, "%s\n", e->req + HDR);
		answer(e, &h, LXE_OK, 0, 0, t2);
		return (int)n;
	}
	h.status = 0;
	h.err = 0;
	bk_memcpy(e->req, &h, HDR);
	(void)i;
	if (!bk_epoc_ready()) {
		answer(e, &h, LXE_NOAPP, 0, 0, "PsiLinux's window does not answer EPOC requests");
		return (int)n;
	}
	e->state = EP_QUEUED;
	lx_k_event();                                /* (the slot may be free now) */
	return (int)n;
}

/* the slot: the next queued request goes to the app; an answer comes back */
static void ep_take(void);
static void ep_pump(epf *e)
{
	ep_take();
	if (!slot_busy && e->state == EP_QUEUED) {
		int r = bk_epoc_send(e->req, e->reqlen);
		if (r == 0) { slot_busy = 1; slot_owner = e; e->state = EP_SENT; ep_take(); }   /* (the host's answers at once) */
		else {
			LxeHeader h;
			bk_memcpy(&h, e->req, HDR);
			answer(e, &h, LXE_NOAPP, 0, 0, "PsiLinux's window does not answer EPOC requests");
		}
	}
}

/* an answer from the app, to whoever asked (dropped if they have gone) */
static void ep_take(void)
{
	if (slot_busy) {
		/* (0.44, MEM-100) taken at the first answer (the epoc command's
		   sessions only): 16 KB of statics in every session before */
		static char *tmp;
		int n;
		if (!tmp && !(tmp = (char *)bk_alloc(HDR + LXE_MAX + 1))) return;
		n = bk_epoc_answer(tmp, HDR + LXE_MAX + 1);
		if (n > 0) {
			epf *o = slot_owner;
			slot_busy = 0;
			slot_owner = 0;
			if (o && o->state == EP_SENT) {
				if (!o->ans) o->ans = (char *)bk_alloc(HDR + LXE_MAX + 1);
				if (o->ans) { bk_memcpy(o->ans, tmp, n); o->anslen = n; o->state = EP_DONE; }
				else o->state = EP_IDLE;
			}
		}
	}
}

static int ep_read(lx_thread *t, lx_file *f, char *buf, lx_u32 n)
{
	epf *e = (epf *)f->priv;
	int k;
	(void)t;
	if (e->state == EP_IDLE) return 0;
	ep_pump(e);
	if (e->state != EP_DONE) return (f->flags & LX_O_NONBLOCK) ? -LX_EAGAIN : LX_BLOCK;
	if (n < (lx_u32)e->anslen) return -LX_EINVAL;    /* (the epoc command reads HDR + LXE_MAX) */
	k = e->anslen;
	bk_memcpy(buf, e->ans, k);
	e->state = EP_IDLE;
	e->anslen = 0;
	return k;
}

static int ep_poll(lx_file *f)
{
	epf *e = (epf *)f->priv;
	if (e->state == EP_QUEUED || e->state == EP_SENT) ep_pump(e);
	if (e->state == EP_DONE) return LX_POLLIN;
	if (e->state == EP_IDLE) return LX_POLLOUT;
	return 0;
}

static int ep_fstat(lx_file *f, struct lx_stat64 *st)
{
	lx_vn v;
	bk_memset(&v, 0, sizeof v);
	v.mode = f->mode;
	v.ino = f->ino;
	v.rdev = 0xa00 + 250;
	v.fs = LX_FS_DEV;
	lx_vn_stat(&v, st);
	return 0;
}

static void ep_close(lx_file *f)
{
	epf *e = (epf *)f->priv;
	if (slot_owner == e) slot_owner = 0;       /* (its answer, when it comes, is dropped) */
	if (e->req) bk_free(e->req);
	if (e->ans) bk_free(e->ans);
	bk_free(e);
}

static const lx_fops epoc_ops = { ep_read, ep_write, 0, 0, 0, ep_fstat, 0, ep_poll, 0, ep_close, 0, 0 };

/* Is p at the Psion's own keyboard? Its terminal is the PsiLinux window. */
int lx_epoc_allowed(lx_proc *p)
{
	return p && p->ctty && !p->ctty->pty && p->ctty->index >= 0 && p->ctty->index < LX_MAXTTY;
}

lx_file *lx_epoc_open(lx_proc *p, int flags, lx_u64 ino, int *err)
{
	epf *e;
	lx_file *f;
	if (!lx_epoc_allowed(p)) { *err = -LX_EACCES; return 0; }
	e = (epf *)bk_alloc(sizeof *e);
	if (!e) { *err = -LX_ENOMEM; return 0; }
	f = lx_file_new(&epoc_ops, e, flags);
	if (!f) { bk_free(e); *err = -LX_ENOMEM; return 0; }
	f->mode = LX_S_IFCHR | 0600;
	f->ino = ino;
	return f;
}

/* (the backend, when the app has answered: the parked reads look again) */
void lx_k_epoc_answered(void)
{
	lx_k_event();
}
