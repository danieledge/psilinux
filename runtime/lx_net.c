/* lx_net.c - sockets for PsiLinux's programs.
 *
 *  - AF_INET TCP and UDP go to the backend (ESOCK on EPOC: the Psion's own
 *    Internet connection, over PPP; BSD sockets on the host).
 *  - DNS: /etc/resolv.conf names 127.0.0.53. A UDP query sent there is
 *    answered here, by asking the backend's resolver (RHostResolver on EPOC),
 *    so musl's resolver works without knowing the PPP link's DNS server.
 *  - AF_UNIX: stream sockets made of two pipes: socketpair, and bind,
 *    listen, accept and connect by name (a path or an abstract name). The
 *    names live here only: a bound path makes no file (X's clients, the
 *    ones that need them, just connect).
 */
#include "lx_core.h"
#include "lx_vfs.h"

#define DNS_ADDR 0x3500007fu       /* 127.0.0.53, as it lies in memory (network order) */

/* the DNS answerer's questions on one socket: musl asks for A and AAAA at
 * once, on the same socket, and matches the answers by their ids, so each
 * question keeps its own lookup and answer until it is read */
#define DNS_SLOTS 4
typedef struct dns_slot {
	int used;
	void *req;                  /* the backend's lookup, while it runs */
	int qlen, alen;             /* alen: an answer is ready */
	char q[512];
	char a[512];
	unsigned seq;               /* (answers are read in the order asked) */
} dns_slot;

typedef struct sock {
	void *bk;
	int type;                   /* LX_SOCK_STREAM / LX_SOCK_DGRAM */
	int connecting;
	int connected;
	int listening;
	int err;                    /* SO_ERROR */
	int shut_wr;
	/* the DNS answerer (a UDP socket that sent to 127.0.0.53) */
	int dns;
	dns_slot *dq;               /* DNS_SLOTS of them, made at the first question */
	unsigned dns_seq;
	/* AF_UNIX: two pipes once connected */
	lx_file *pr, *pw;
	int unix_;                  /* an AF_UNIX socket */
	int namelen;                /* its bound name (sun_path's bytes), 0 = none */
	char name[108];
	int pid;                    /* (SO_PEERCRED: the process at the other end) */
	struct sock *lnext;         /* the listeners, by name */
	struct sock *q[8];          /* a listener's connections, not yet accepted */
	int nq;
} sock;

static sock *listeners;

static lx_u64 sock_ino = 9000;

static lx_u16 swap16(lx_u16 v) { return (lx_u16)((v >> 8) | (v << 8)); }

/* ---- the DNS answerer ------------------------------------------------------ */

/* the question's name in dotted form; the bytes it took (from 12), or -1 */
static int dns_qname(const char *q, int len, char *out, int max)
{
	int i = 12, o = 0;
	while (i < len) {
		int n = (unsigned char)q[i++];
		if (!n) break;
		if (n > 63 || i + n > len || o + n + 2 > max) return -1;
		if (o) out[o++] = '.';
		bk_memcpy(out + o, q + i, n);
		o += n;
		i += n;
	}
	out[o] = 0;
	return i + 4 <= len ? i : -1;
}

/* the answer to the slot's question, with these addresses */
static void dns_answer(dns_slot *d, const lx_u32 *addrs, int n, int rcode)
{
	char *a = d->a;
	char name[256];
	int qend = dns_qname(d->q, d->qlen, name, sizeof name), o, i;
	int qtype;
	if (qend < 0) { d->used = 0; return; }   /* (not a question: no answer) */
	qtype = ((unsigned char)d->q[qend] << 8) | (unsigned char)d->q[qend + 1];
	if (qtype != 1) n = 0;           /* (A only: AAAA and the rest get no answers) */
	o = qend + 4;
	bk_memcpy(a, d->q, o);
	a[2] = (char)0x81;               /* a response, recursion desired */
	a[3] = (char)(0x80 | rcode);     /* recursion available */
	a[4] = 0; a[5] = 1;              /* one question */
	a[6] = 0; a[7] = (char)n;        /* answers */
	a[8] = a[9] = a[10] = a[11] = 0;
	for (i = 0; i < n && o + 16 <= 512; i++) {
		a[o++] = (char)0xc0; a[o++] = 12;          /* the question's name */
		a[o++] = 0; a[o++] = 1;                    /* A */
		a[o++] = 0; a[o++] = 1;                    /* IN */
		a[o++] = 0; a[o++] = 0; a[o++] = 0x0e; a[o++] = 0x10;   /* TTL 3600 */
		a[o++] = 0; a[o++] = 4;
		bk_memcpy(a + o, &addrs[i], 4);
		o += 4;
	}
	d->alen = o;
}

/* the lookups that have finished get their answers; the first answer ready */
static dns_slot *dns_poll(sock *s)
{
	dns_slot *best = 0;
	int k;
	if (!s->dq) return 0;
	for (k = 0; k < DNS_SLOTS; k++) {
		dns_slot *d = &s->dq[k];
		if (!d->used) continue;
		if (d->req) {
			lx_u32 addrs[8];
			int n = bk_resolve_poll(d->req, addrs, 8);
			if (n == -LX_EAGAIN) continue;
			bk_resolve_free(d->req);
			d->req = 0;
			if (n == -LX_ENOENT) dns_answer(d, 0, 0, 3);   /* NXDOMAIN: no such name */
			else if (n < 0) dns_answer(d, 0, 0, 2);   /* SERVFAIL: the network failed */
			else dns_answer(d, addrs, n, 0);
		}
		if (d->used && d->alen && (!best || d->seq < best->seq)) best = d;
	}
	return best;
}

static int same(const char *a, const char *b, int n)
{
	while (n--) if (*a++ != *b++) return 0;
	return 1;
}

static void dns_free(dns_slot *d)
{
	if (d->req) bk_resolve_free(d->req);
	d->req = 0;
	d->used = 0;
	d->alen = 0;
}

static int dns_query(sock *s, const char *buf, int len)
{
	char name[256];
	int err = 0, qend, k;
	dns_slot *d = 0;
	if (len < 17 || len > 512) return -LX_EINVAL;
	if (!s->dq && !(s->dq = (dns_slot *)bk_alloc(DNS_SLOTS * sizeof(dns_slot)))) return -LX_ENOMEM;
	for (k = 0; k < DNS_SLOTS; k++) {
		dns_slot *e = &s->dq[k];
		/* (the same question again: musl asking again after a timeout) */
		if (e->used && e->qlen == len && same(e->q, buf, len)) { d = e; break; }
		if (!e->used && !d) d = e;
	}
	if (!d) {                        /* all in use: the oldest goes */
		d = &s->dq[0];
		for (k = 1; k < DNS_SLOTS; k++) if (s->dq[k].seq < d->seq) d = &s->dq[k];
	}
	if (d->used && d->qlen == len && same(d->q, buf, len)) return len;   /* (its lookup goes on) */
	dns_free(d);
	bk_memcpy(d->q, buf, len);
	d->qlen = len;
	d->used = 1;
	d->seq = ++s->dns_seq;
	qend = dns_qname(buf, len, name, sizeof name);
	if (qend < 0) { d->used = 0; return len; }
	if ((((unsigned char)buf[qend] << 8) | (unsigned char)buf[qend + 1]) != 1) {
		dns_answer(d, 0, 0, 0);                  /* not A: an empty answer at once */
		return len;
	}
	d->req = bk_resolve_start(name, &err);
	if (!d->req) dns_answer(d, 0, 0, 2);   /* SERVFAIL */
	return len;
}

/* ---- the socket file --------------------------------------------------------- */

static int sk_poll(lx_file *f)
{
	sock *s = (sock *)f->priv;
	if (s->unix_ && !s->pr) return s->listening ? (s->nq ? LX_POLLIN : 0) : LX_POLLOUT;
	if (s->pr) {
		int r = s->pr->ops->poll(s->pr) & (LX_POLLIN | LX_POLLHUP);
		return r | (s->pw->ops->poll(s->pw) & (LX_POLLOUT | LX_POLLERR));
	}
	if (s->dns) return (dns_poll(s) ? LX_POLLIN : 0) | LX_POLLOUT;
	if (!s->bk) return LX_POLLOUT;
	return bk_sock_poll(s->bk);
}

static int sk_recv(lx_thread *t, lx_file *f, char *buf, lx_u32 n, int flags, lx_u32 *addr, lx_u16 *port)
{
	sock *s = (sock *)f->priv;
	int r;
	int nonblock = (f->flags & LX_O_NONBLOCK) || (flags & LX_MSG_DONTWAIT);
	if (s->unix_ && !s->pr) return -LX_ENOTCONN;
	if (s->pr) {
		r = s->pr->ops->read(t, s->pr, buf, n);
		if (r == LX_BLOCK && nonblock) return -LX_EAGAIN;
		return r;
	}
	if (s->dns) {
		dns_slot *d = dns_poll(s);
		if (!d) return nonblock ? -LX_EAGAIN : LX_BLOCK;
		r = d->alen < (int)n ? d->alen : (int)n;
		bk_memcpy(buf, d->a, r);
		if (!(flags & LX_MSG_PEEK)) dns_free(d);
		if (addr) *addr = DNS_ADDR;
		if (port) *port = swap16(53);
		return r;
	}
	if (!s->bk) return -LX_ENOTCONN;
	if (s->type == LX_SOCK_DGRAM) r = bk_sock_recvfrom(s->bk, buf, n, addr, port);
	else r = bk_sock_recv(s->bk, buf, n, flags & LX_MSG_PEEK);
	if (r == -LX_EAGAIN && !nonblock) return LX_BLOCK;
	return r;
}

static int sk_send(lx_thread *t, lx_file *f, const char *buf, lx_u32 n, int flags, int to, lx_u32 addr, lx_u16 port)
{
	sock *s = (sock *)f->priv;
	int r;
	int nonblock = (f->flags & LX_O_NONBLOCK) || (flags & LX_MSG_DONTWAIT);
	if (s->unix_ && !s->pr) return -LX_ENOTCONN;
	if (s->pr) {
		r = s->pw->ops->write(t, s->pw, buf, n);
		if (r == LX_BLOCK && nonblock) return -LX_EAGAIN;
		if (r == -LX_EPIPE && (flags & LX_MSG_NOSIGNAL)) t->sigpending[0] &= ~(1u << (LX_SIGPIPE - 1));
		return r;
	}
	if (s->type == LX_SOCK_DGRAM && to && addr == DNS_ADDR && port == swap16(53)) {
		s->dns = 1;
		return dns_query(s, buf, (int)n);
	}
	if (s->dns && !to) return dns_query(s, buf, (int)n);
	if (!s->bk) return -LX_ENOTCONN;
	if (s->shut_wr) { if (!(flags & LX_MSG_NOSIGNAL)) lx_signal_thread(t, LX_SIGPIPE); return -LX_EPIPE; }
	if (s->type == LX_SOCK_DGRAM && to) r = bk_sock_sendto(s->bk, buf, n, addr, port);
	else r = bk_sock_send(s->bk, buf, n);
	if (r == -LX_EAGAIN && !nonblock) return LX_BLOCK;
	if (r == -LX_EPIPE && !(flags & LX_MSG_NOSIGNAL)) lx_signal_thread(t, LX_SIGPIPE);
	return r;
}

static int sk_read(lx_thread *t, lx_file *f, char *buf, lx_u32 n) { return sk_recv(t, f, buf, n, 0, 0, 0); }
static int sk_write(lx_thread *t, lx_file *f, const char *buf, lx_u32 n) { return sk_send(t, f, buf, n, 0, 0, 0, 0); }

static int sk_fstat(lx_file *f, struct lx_stat64 *st)
{
	lx_vn v;
	bk_memset(&v, 0, sizeof v);
	v.mode = LX_S_IFSOCK | 0777;
	v.ino = f->ino;
	v.mtime = bk_now_real();
	lx_vn_stat(&v, st);
	return 0;
}

static int sk_ioctl(lx_thread *t, lx_file *f, lx_u32 req, lx_u32 arg)
{
	sock *s = (sock *)f->priv;
	if (req == LX_FIONREAD) {
		char tmp[1];
		int n = 0;
		if (!lx_uaccess(t, arg, 4)) return -LX_EFAULT;
		if (s->dns) { dns_slot *d = dns_poll(s); n = d ? d->alen : 0; }
		else if (s->pr) n = lx_pipe_count(s->pr);
		else if (s->bk && (bk_sock_poll(s->bk) & LX_POLLIN)) n = bk_sock_recv(s->bk, tmp, 1, 1) > 0 ? 1 : 0;
		*(lx_s32 *)arg = n;
		return 0;
	}
	return -LX_ENOTTY;
}

static lx_s64 sk_lseek(lx_file *f, lx_s64 o, int w) { (void)f; (void)o; (void)w; return -LX_ESPIPE; }

static void sock_free(sock *s);

static void sk_close(lx_file *f)
{
	sock_free((sock *)f->priv);
}

static void sock_free(sock *s)
{
	sock **l;
	for (l = &listeners; *l; l = &(*l)->lnext)
		if (*l == s) { *l = s->lnext; break; }
	while (s->nq) sock_free(s->q[--s->nq]);
	if (s->bk) bk_sock_close(s->bk);
	if (s->dq) {
		int k;
		for (k = 0; k < DNS_SLOTS; k++) dns_free(&s->dq[k]);
		bk_free(s->dq);
	}
	if (s->pr) lx_file_put(s->pr);
	if (s->pw) lx_file_put(s->pw);
	bk_free(s);
}

static const lx_fops sock_ops = { sk_read, sk_write, 0, 0, sk_lseek, sk_fstat, sk_ioctl, sk_poll, 0, sk_close, 0, 0 };

static lx_file *sock_file(sock *s, int flags)
{
	lx_file *f = lx_file_new(&sock_ops, s, LX_O_RDWR | (flags & LX_SOCK_NONBLOCK ? LX_O_NONBLOCK : 0));
	if (!f) return 0;
	f->mode = LX_S_IFSOCK | 0777;
	f->ino = sock_ino++;
	return f;
}

static sock *get_sock(lx_thread *t, int fd, lx_file **fp)
{
	lx_file *f = lx_fd_get(t->proc, fd);
	if (!f || f->ops != &sock_ops) return 0;
	if (fp) *fp = f;
	return (sock *)f->priv;
}

/* ---- the calls ---------------------------------------------------------------- */

static int get_addr(lx_thread *t, lx_u32 ua, lx_u32 len, lx_u32 *addr, lx_u16 *port)
{
	struct lx_sockaddr_in *sa = (struct lx_sockaddr_in *)ua;
	if (len < 8 || !lx_uaccess(t, ua, 8)) return -LX_EFAULT;
	if (sa->family != LX_AF_INET) return -LX_EAFNOSUPPORT;
	*addr = sa->addr;
	*port = sa->port;
	return 0;
}

static int put_addr(lx_thread *t, lx_u32 ua, lx_u32 ulen, lx_u32 addr, lx_u16 port)
{
	struct lx_sockaddr_in sa;
	lx_u32 len;
	if (!ua) return 0;
	if (!lx_uaccess(t, ulen, 4)) return -LX_EFAULT;
	len = *(lx_u32 *)ulen;
	bk_memset(&sa, 0, sizeof sa);
	sa.family = LX_AF_INET;
	sa.addr = addr;
	sa.port = port;
	if (len > sizeof sa) len = sizeof sa;
	if (!lx_uaccess(t, ua, len)) return -LX_EFAULT;
	bk_memcpy((void *)ua, &sa, len);
	*(lx_u32 *)ulen = sizeof sa;
	return 0;
}

/* ---- AF_UNIX by name ------------------------------------------------------------ */

/* sun_path from a sockaddr_un: its bytes (an abstract name starts with 0) */
static int un_name(lx_thread *t, lx_u32 ua, lx_u32 len, char *name)
{
	int n;
	if (len < 3 || len > 110 || !lx_uaccess(t, ua, len)) return len < 3 ? -LX_EINVAL : -LX_EFAULT;
	if (*(lx_u16 *)ua != LX_AF_UNIX) return -LX_EAFNOSUPPORT;
	n = (int)len - 2;
	bk_memcpy(name, (char *)ua + 2, n);
	if (name[0]) n = lx_strnlen(name, n);     /* a path: up to its NUL */
	return n;
}

static sock *un_find(const char *name, int n)
{
	sock *l;
	for (l = listeners; l; l = l->lnext)
		if (l->namelen == n && same(l->name, name, n)) return l;
	return 0;
}

static int un_put_name(lx_thread *t, lx_u32 ua, lx_u32 ulen, const char *name, int n)
{
	lx_u32 len, want = 2 + n;
	char sa[110];
	if (!ua) return 0;
	if (!lx_uaccess(t, ulen, 4)) return -LX_EFAULT;
	len = *(lx_u32 *)ulen;
	*(lx_u16 *)sa = LX_AF_UNIX;
	bk_memcpy(sa + 2, name, n);
	if (n && name[0]) { sa[2 + n] = 0; want++; }
	if (len > want) len = want;
	if (!lx_uaccess(t, ua, len)) return -LX_EFAULT;
	bk_memcpy((void *)ua, sa, len);
	*(lx_u32 *)ulen = want;
	return 0;
}

/* connect s to the listener l: two pipes, the far end queued for accept */
static int un_connect(lx_thread *t, sock *s, sock *l)
{
	lx_file *r1, *w1, *r2, *w2;
	sock *n;
	int e;
	if (l->nq >= (int)(sizeof l->q / sizeof l->q[0])) return -LX_EAGAIN;
	n = (sock *)bk_alloc(sizeof *n);
	if (!n) return -LX_ENOMEM;
	e = lx_pipe_new(&r1, &w1);
	if (e < 0) { bk_free(n); return e; }
	e = lx_pipe_new(&r2, &w2);
	if (e < 0) { lx_file_put(r1); lx_file_put(w1); bk_free(n); return e; }
	s->pr = r1; s->pw = w2;
	n->pr = r2; n->pw = w1;
	n->unix_ = 1;
	n->type = LX_SOCK_STREAM;
	n->connected = s->connected = 1;
	n->namelen = l->namelen;
	bk_memcpy(n->name, l->name, l->namelen);
	n->pid = t->proc->pid;
	s->pid = l->pid;
	l->q[l->nq++] = n;
	lx_k_event();
	return 0;
}

static int sys_socket(lx_thread *t, int domain, int type, int proto)
{
	sock *s;
	lx_file *f;
	int err = 0, base = type & 0xf;
	(void)proto;
	if (domain == LX_AF_INET6) return -LX_EAFNOSUPPORT;
	if (domain == LX_AF_UNIX) {
		if (base != LX_SOCK_STREAM) return -LX_EPROTONOSUPPORT;
		s = (sock *)bk_alloc(sizeof *s);
		if (!s) return -LX_ENOMEM;
		s->type = base;
		s->unix_ = 1;
		s->pid = t->proc->pid;
		f = sock_file(s, type);
		if (!f) { bk_free(s); return -LX_ENOMEM; }
		return lx_fd_alloc(t->proc, f, 0, type & LX_SOCK_CLOEXEC);
	}
	if (domain != LX_AF_INET) return -LX_EAFNOSUPPORT;
	if (base != LX_SOCK_STREAM && base != LX_SOCK_DGRAM) return -LX_EPROTONOSUPPORT;
	s = (sock *)bk_alloc(sizeof *s);
	if (!s) return -LX_ENOMEM;
	s->type = base;
	s->bk = bk_sock_new(base == LX_SOCK_STREAM ? BK_SOCK_TCP : BK_SOCK_UDP, &err);
	if (!s->bk && base == LX_SOCK_STREAM) { bk_free(s); return err ? err : -LX_ENETDOWN; }
	/* (a UDP socket without the network can still ask the DNS answerer) */
	f = sock_file(s, type);
	if (!f) { if (s->bk) bk_sock_close(s->bk); bk_free(s); return -LX_ENOMEM; }
	return lx_fd_alloc(t->proc, f, 0, type & LX_SOCK_CLOEXEC);
}

static int sys_socketpair(lx_thread *t, int domain, int type, int proto, lx_u32 usv)
{
	sock *a, *b;
	lx_file *r1, *w1, *r2, *w2, *fa, *fb;
	int e, da, db;
	(void)proto;
	if (domain != LX_AF_UNIX) return -LX_EAFNOSUPPORT;
	if (!lx_uaccess(t, usv, 8)) return -LX_EFAULT;
	e = lx_pipe_new(&r1, &w1);
	if (e < 0) return e;
	e = lx_pipe_new(&r2, &w2);
	if (e < 0) { lx_file_put(r1); lx_file_put(w1); return e; }
	a = (sock *)bk_alloc(sizeof *a);
	b = (sock *)bk_alloc(sizeof *b);
	if (!a || !b) { bk_free(a); bk_free(b); lx_file_put(r1); lx_file_put(w1); lx_file_put(r2); lx_file_put(w2); return -LX_ENOMEM; }
	a->type = b->type = type & 0xf;
	a->unix_ = b->unix_ = 1;
	a->pid = b->pid = t->proc->pid;
	a->pr = r1; a->pw = w2;
	b->pr = r2; b->pw = w1;
	a->connected = b->connected = 1;
	fa = sock_file(a, type);
	fb = sock_file(b, type);
	if (!fa || !fb) return -LX_ENOMEM;
	da = lx_fd_alloc(t->proc, fa, 0, type & LX_SOCK_CLOEXEC);
	if (da < 0) { lx_file_put(fb); return da; }
	db = lx_fd_alloc(t->proc, fb, 0, type & LX_SOCK_CLOEXEC);
	if (db < 0) { lx_file_put(t->proc->fd[da]); t->proc->fd[da] = 0; return db; }
	((lx_s32 *)usv)[0] = da;
	((lx_s32 *)usv)[1] = db;
	return 0;
}

static int sys_connect(lx_thread *t, int fd, lx_u32 ua, lx_u32 len)
{
	lx_file *f;
	sock *s = get_sock(t, fd, &f);
	lx_u32 addr;
	lx_u16 port;
	int r;
	if (!s) return lx_fd_get(t->proc, fd) ? -LX_ENOTSOCK : -LX_EBADF;
	if (s->unix_) {
		char name[108];
		sock *l;
		int n = un_name(t, ua, len, name);
		if (n < 0) return n;
		if (s->pr) return -LX_EISCONN;
		l = un_find(name, n);
		if (!l) return name[0] ? -LX_ENOENT : -LX_ECONNREFUSED;
		r = un_connect(t, s, l);
		if (r == -LX_EAGAIN) return (f->flags & LX_O_NONBLOCK) ? -LX_EAGAIN : LX_BLOCK;
		return r;
	}
	if (s->connecting) {
		/* a blocking connect, looked at again */
		int ev = s->bk ? bk_sock_poll(s->bk) : LX_POLLERR;
		if (lx_k.trace) bk_log("[%d] connect: poll %x", t->tid, ev);
		if (ev & LX_POLLERR) { s->connecting = 0; r = bk_sock_error(s->bk); return r ? r : -LX_ECONNREFUSED; }
		if (ev & LX_POLLOUT) { s->connecting = 0; s->connected = 1; return 0; }
		return LX_BLOCK;
	}
	r = get_addr(t, ua, len, &addr, &port);
	if (r < 0) return r;
	if (s->type == LX_SOCK_DGRAM && addr == DNS_ADDR && port == swap16(53)) { s->dns = 1; s->connected = 1; return 0; }
	if (!s->bk) return -LX_ENETUNREACH;
	if (s->connected && s->type == LX_SOCK_STREAM) return -LX_EISCONN;
	r = bk_sock_connect(s->bk, addr, port);
	if (r == 0) { s->connected = 1; return 0; }
	if (r != -LX_EINPROGRESS) return r;
	if (f->flags & LX_O_NONBLOCK) { s->connecting = 0; return -LX_EINPROGRESS; }
	s->connecting = 1;
	return LX_BLOCK;
}

static int sys_accept(lx_thread *t, int fd, lx_u32 ua, lx_u32 ulen, int flags)
{
	lx_file *f, *nf;
	sock *s = get_sock(t, fd, &f), *n;
	void *b;
	int err = 0;
	lx_u32 addr = 0;
	lx_u16 port = 0;
	if (!s) return -LX_ENOTSOCK;
	if (s->unix_) {
		int i;
		if (!s->listening) return -LX_EINVAL;
		if (!s->nq) return (f->flags & LX_O_NONBLOCK) ? -LX_EAGAIN : LX_BLOCK;
		n = s->q[0];
		for (i = 1; i < s->nq; i++) s->q[i - 1] = s->q[i];
		s->nq--;
		nf = sock_file(n, flags);
		if (!nf) { sock_free(n); return -LX_ENOMEM; }
		if (ua) un_put_name(t, ua, ulen, "", 0);    /* (the client has no name) */
		return lx_fd_alloc(t->proc, nf, 0, flags & LX_SOCK_CLOEXEC);
	}
	if (!s->bk || !s->listening) return -LX_EINVAL;
	b = bk_sock_accept(s->bk, &err, &addr, &port);
	if (!b) {
		if (err == -LX_EAGAIN && !(f->flags & LX_O_NONBLOCK)) return LX_BLOCK;
		return err ? err : -LX_EAGAIN;
	}
	n = (sock *)bk_alloc(sizeof *n);
	if (!n) { bk_sock_close(b); return -LX_ENOMEM; }
	n->bk = b;
	n->type = LX_SOCK_STREAM;
	n->connected = 1;
	nf = sock_file(n, flags);
	if (!nf) { bk_sock_close(b); bk_free(n); return -LX_ENOMEM; }
	put_addr(t, ua, ulen, addr, port);
	return lx_fd_alloc(t->proc, nf, 0, flags & LX_SOCK_CLOEXEC);
}

static int sys_sockopt(lx_thread *t, int fd, int level, int name, lx_u32 val, lx_u32 ulen, int get)
{
	sock *s = get_sock(t, fd, 0);
	lx_s32 v = 0;
	if (!s) return -LX_ENOTSOCK;
	if (!get) return 0;          /* (options are accepted and do nothing) */
	if (!lx_uaccess(t, ulen, 4) || *(lx_u32 *)ulen < 4 || !lx_uaccess(t, val, 4)) return -LX_EFAULT;
	if (level == LX_SOL_SOCKET) {
		switch (name) {
		case LX_SO_ERROR: v = s->bk ? -bk_sock_error(s->bk) : 0; break;
		case LX_SO_TYPE: v = s->type; break;
		case LX_SO_RCVBUF: case LX_SO_SNDBUF: v = 8192; break;
		case LX_SO_PEERCRED:
			/* struct ucred: the pid at the other end, uid and gid 0 */
			if (*(lx_u32 *)ulen < 12 || !lx_uaccess(t, val, 12)) return -LX_EINVAL;
			((lx_s32 *)val)[0] = s->pid;
			((lx_s32 *)val)[1] = 0;
			((lx_s32 *)val)[2] = 0;
			*(lx_u32 *)ulen = 12;
			return 0;
		default: v = 0;
		}
	}
	*(lx_s32 *)val = v;
	*(lx_u32 *)ulen = 4;
	return 0;
}

static int sys_sendmsg(lx_thread *t, int fd, lx_u32 umsg, int flags, int recv)
{
	struct lx_msghdr *m = (struct lx_msghdr *)umsg;
	struct lx_iovec *iov;
	lx_file *f;
	sock *s = get_sock(t, fd, &f);
	lx_u32 i, total = 0;
	lx_u32 addr = 0;
	lx_u16 port = 0;
	int r = 0, to = 0;
	if (!s) return -LX_ENOTSOCK;
	if (!lx_uaccess(t, umsg, sizeof *m)) return -LX_EFAULT;
	if (m->iovlen > 64 || !lx_uaccess(t, m->iov, m->iovlen * sizeof *iov)) return -LX_EFAULT;
	iov = (struct lx_iovec *)m->iov;
	if (!recv && m->name && m->namelen && !s->unix_) {
		r = get_addr(t, m->name, m->namelen, &addr, &port);
		if (r < 0) return r;
		to = 1;
	}
	for (i = 0; i < m->iovlen; i++) {
		if (!iov[i].len) continue;
		if (!lx_uaccess(t, iov[i].base, iov[i].len)) return total ? (int)total : -LX_EFAULT;
		if (recv) r = sk_recv(t, f, (char *)iov[i].base, iov[i].len, flags, &addr, &port);
		else r = sk_send(t, f, (const char *)iov[i].base, iov[i].len, flags, to, addr, port);
		if (r == LX_BLOCK) return total ? (int)total : LX_BLOCK;
		if (r < 0) return total ? (int)total : r;
		total += r;
		if ((lx_u32)r < iov[i].len || s->type == LX_SOCK_DGRAM) break;
	}
	if (recv && s->unix_) m->namelen = 0, m->controllen = 0, m->flags = 0;
	else if (recv) {
		if (m->name && m->namelen >= sizeof(struct lx_sockaddr_in) && lx_uaccess(t, m->name, sizeof(struct lx_sockaddr_in))) {
			struct lx_sockaddr_in *sa = (struct lx_sockaddr_in *)m->name;
			bk_memset(sa, 0, sizeof *sa);
			sa->family = LX_AF_INET;
			sa->addr = addr;
			sa->port = port;
			m->namelen = sizeof *sa;
		}
		m->controllen = 0;
		m->flags = 0;
	}
	return (int)total;
}

int lx_net_syscall(lx_thread *t, lx_u32 nr, lx_u32 *a)
{
	lx_file *f;
	sock *s;
	lx_u32 addr;
	lx_u16 port;
	int r;
	switch (nr) {
	case LX_NR_socket: return sys_socket(t, (int)a[0], (int)a[1], (int)a[2]);
	case LX_NR_socketpair: return sys_socketpair(t, (int)a[0], (int)a[1], (int)a[2], a[3]);
	case LX_NR_connect: return sys_connect(t, (int)a[0], a[1], a[2]);
	case LX_NR_bind:
		if (!(s = get_sock(t, (int)a[0], 0))) return -LX_ENOTSOCK;
		if (s->unix_) {
			char name[108];
			int n = un_name(t, a[1], a[2], name);
			if (n < 0) return n;
			if (s->namelen || s->pr) return -LX_EINVAL;
			if (un_find(name, n)) return -LX_EADDRINUSE;
			bk_memcpy(s->name, name, n);
			s->namelen = n;
			return 0;
		}
		if ((r = get_addr(t, a[1], a[2], &addr, &port)) < 0) return r;
		if (!s->bk) return -LX_EADDRNOTAVAIL;
		return bk_sock_bind(s->bk, addr, port);
	case LX_NR_listen:
		if (!(s = get_sock(t, (int)a[0], 0))) return -LX_ENOTSOCK;
		if (s->unix_) {
			if (!s->namelen || s->pr) return -LX_EINVAL;
			if (!s->listening) {
				if (un_find(s->name, s->namelen)) return -LX_EADDRINUSE;
				s->listening = 1;
				s->lnext = listeners;
				listeners = s;
			}
			return 0;
		}
		if (!s->bk) return -LX_EINVAL;
		r = bk_sock_listen(s->bk, (int)a[1]);
		if (r == 0) s->listening = 1;
		return r;
	case LX_NR_accept: return sys_accept(t, (int)a[0], a[1], a[2], 0);
	case LX_NR_accept4: return sys_accept(t, (int)a[0], a[1], a[2], (int)a[3]);
	case LX_NR_getsockname: case LX_NR_getpeername:
		if (!(s = get_sock(t, (int)a[0], 0))) return -LX_ENOTSOCK;
		if (s->unix_) {
			if (nr == LX_NR_getpeername && !s->pr) return -LX_ENOTCONN;
			return un_put_name(t, a[1], a[2], s->name, s->namelen);
		}
		addr = 0; port = 0;
		if (s->bk) bk_sock_name(s->bk, nr == LX_NR_getpeername, &addr, &port);
		else if (s->dns && nr == LX_NR_getpeername) { addr = DNS_ADDR; port = swap16(53); }
		else if (nr == LX_NR_getpeername && !s->pr) return -LX_ENOTCONN;
		return put_addr(t, a[1], a[2], addr, port);
	case LX_NR_send: case LX_NR_sendto:
		if (!(s = get_sock(t, (int)a[0], &f))) return lx_fd_get(t->proc, (int)a[0]) ? -LX_ENOTSOCK : -LX_EBADF;
		if (a[2] && !lx_uaccess(t, a[1], a[2])) return -LX_EFAULT;
		if (nr == LX_NR_sendto && a[4]) {
			if ((r = get_addr(t, a[4], a[5], &addr, &port)) < 0) return r;
			return sk_send(t, f, (const char *)a[1], a[2], (int)a[3], 1, addr, port);
		}
		return sk_send(t, f, (const char *)a[1], a[2], (int)a[3], 0, 0, 0);
	case LX_NR_recv: case LX_NR_recvfrom:
		if (!(s = get_sock(t, (int)a[0], &f))) return lx_fd_get(t->proc, (int)a[0]) ? -LX_ENOTSOCK : -LX_EBADF;
		if (a[2] && !lx_uaccess(t, a[1], a[2])) return -LX_EFAULT;
		addr = 0; port = 0;
		r = sk_recv(t, f, (char *)a[1], a[2], (int)a[3], &addr, &port);
		if (r >= 0 && nr == LX_NR_recvfrom && a[4]) put_addr(t, a[4], a[5], addr, port);
		return r;
	case LX_NR_sendmsg: return sys_sendmsg(t, (int)a[0], a[1], (int)a[2], 0);
	case LX_NR_recvmsg: return sys_sendmsg(t, (int)a[0], a[1], (int)a[2], 1);
	case LX_NR_shutdown:
		if (!(s = get_sock(t, (int)a[0], 0))) return -LX_ENOTSOCK;
		if (a[1] != LX_SHUT_RD) s->shut_wr = 1;
		if (s->pr) return 0;
		return s->bk ? bk_sock_shutdown(s->bk, (int)a[1]) : 0;
	case LX_NR_setsockopt: return sys_sockopt(t, (int)a[0], (int)a[1], (int)a[2], a[3], a[4], 0);
	case LX_NR_getsockopt: return sys_sockopt(t, (int)a[0], (int)a[1], (int)a[2], a[3], a[4], 1);
	case LX_NR_sendmmsg: case LX_NR_recvmmsg: case LX_NR_recvmmsg_time64: return -LX_ENOSYS;
	}
	return LX_NOSYS;
}
