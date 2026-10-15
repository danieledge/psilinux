/* lx_fsys.c - the system calls about files: open, read, write, stat,
 * directories, pipes, poll and select, and the rest. */
#include "lx_core.h"
#include "lx_vfs.h"

extern int lx_is_tty(lx_file *f);
extern int lx_is_pipe(lx_file *f);

static int getpath(lx_thread *t, lx_u32 up, char *out)
{
	int r = lx_ustr(t, up, out, LX_PATH_MAX);
	if (r < 0) return r;
	if (r == 0) return -LX_ENOENT;
	return 0;
}

static int sys_openat(lx_thread *t, int dirfd, lx_u32 upath, int flags, lx_u32 mode)
{
	char path[LX_PATH_MAX];
	int err = 0, r;
	lx_file *f;
	r = getpath(t, upath, path);
	if (r < 0) return r;
	f = lx_vfs_open(t->proc, dirfd, path, flags, mode, &err);
	if (!f) return err ? err : -LX_ENOENT;
	return lx_fd_alloc(t->proc, f, 0, flags & LX_O_CLOEXEC);
}

static int do_read(lx_thread *t, lx_file *f, lx_u32 buf, lx_u32 n)
{
	if ((f->flags & LX_O_ACCMODE) == LX_O_WRONLY) return -LX_EBADF;
	if (!f->ops->read) return -LX_EINVAL;
	if (n && !lx_uaccess(t, buf, n)) return -LX_EFAULT;
	return f->ops->read(t, f, (char *)buf, n);
}

static int do_write(lx_thread *t, lx_file *f, lx_u32 buf, lx_u32 n)
{
	if ((f->flags & LX_O_ACCMODE) == LX_O_RDONLY && !lx_is_tty(f)) return -LX_EBADF;
	if (!f->ops->write) return -LX_EINVAL;
	if (n && !lx_uaccess(t, buf, n)) return -LX_EFAULT;
	return f->ops->write(t, f, (const char *)buf, n);
}

/* readv/writev: as far as the first short transfer; blocks only if nothing moved */
static int do_iov(lx_thread *t, lx_file *f, lx_u32 uiov, int cnt, int wr, lx_s64 off, int use_off)
{
	struct lx_iovec *iov = (struct lx_iovec *)uiov;
	int i, total = 0, r;
	if (cnt < 0 || cnt > 1024) return -LX_EINVAL;
	if (!lx_uaccess(t, uiov, cnt * sizeof *iov)) return -LX_EFAULT;
	for (i = 0; i < cnt; i++) {
		if (!iov[i].len) continue;
		if (!lx_uaccess(t, iov[i].base, iov[i].len)) return total ? total : -LX_EFAULT;
		if (use_off) {
			if (wr) r = f->ops->pwrite ? f->ops->pwrite(t, f, (const char *)iov[i].base, iov[i].len, off + total) : -LX_ESPIPE;
			else r = f->ops->pread ? f->ops->pread(t, f, (char *)iov[i].base, iov[i].len, off + total) : -LX_ESPIPE;
		} else r = wr ? do_write(t, f, iov[i].base, iov[i].len) : do_read(t, f, iov[i].base, iov[i].len);
		if (r == LX_BLOCK) return total ? total : LX_BLOCK;
		if (r < 0) return total ? total : r;
		total += r;
		if ((lx_u32)r < iov[i].len) break;
	}
	return total;
}

static int sys_stat(lx_thread *t, int dirfd, lx_u32 upath, lx_u32 ust, int flags)
{
	char path[LX_PATH_MAX];
	struct lx_stat64 st;
	int r;
	if (!lx_uaccess(t, ust, sizeof st)) return -LX_EFAULT;
	r = lx_ustr(t, upath, path, sizeof path);
	if (r < 0) return r;
	if (r == 0) {
		lx_file *f;
		if (!(flags & LX_AT_EMPTY_PATH)) return -LX_ENOENT;
		f = lx_fd_get(t->proc, dirfd);
		if (!f) return -LX_EBADF;
		if (!f->ops->fstat) return -LX_EINVAL;
		r = f->ops->fstat(f, &st);
	} else r = lx_vfs_stat(t->proc, dirfd, path, !(flags & LX_AT_SYMLINK_NOFOLLOW), &st);
	if (r < 0) return r;
	bk_memcpy((void *)ust, &st, sizeof st);
	return 0;
}

static int sys_fstat(lx_thread *t, int fd, lx_u32 ust)
{
	lx_file *f = lx_fd_get(t->proc, fd);
	struct lx_stat64 st;
	int r;
	if (!f) return -LX_EBADF;
	if (!lx_uaccess(t, ust, sizeof st)) return -LX_EFAULT;
	if (!f->ops->fstat) {
		bk_memset(&st, 0, sizeof st);
		st.st_mode = f->mode;
		st.st_ino = st.st_ino32 = (lx_u32)f->ino;
		st.st_nlink = 1;
		st.st_blksize = 4096;
	} else {
		r = f->ops->fstat(f, &st);
		if (r < 0) return r;
	}
	bk_memcpy((void *)ust, &st, sizeof st);
	return 0;
}

static int sys_statx(lx_thread *t, int dirfd, lx_u32 upath, int flags, lx_u32 mask, lx_u32 ubuf)
{
	struct lx_stat64 st;
	struct lx_statx *x = (struct lx_statx *)ubuf;
	char path[LX_PATH_MAX];
	int r;
	(void)mask;
	if (!lx_uaccess(t, ubuf, sizeof *x)) return -LX_EFAULT;
	r = lx_ustr(t, upath, path, sizeof path);
	if (r < 0) return r;
	if (r == 0 && (flags & LX_AT_EMPTY_PATH)) {
		lx_file *f = lx_fd_get(t->proc, dirfd);
		if (!f) return -LX_EBADF;
		r = f->ops->fstat ? f->ops->fstat(f, &st) : -LX_EINVAL;
	} else if (r == 0) return -LX_ENOENT;
	else r = lx_vfs_stat(t->proc, dirfd, path, !(flags & LX_AT_SYMLINK_NOFOLLOW), &st);
	if (r < 0) return r;
	bk_memset(x, 0, sizeof *x);
	x->stx_mask = 0x7ff;
	x->stx_blksize = st.st_blksize;
	x->stx_nlink = st.st_nlink;
	x->stx_mode = (lx_u16)st.st_mode;
	x->stx_ino = st.st_ino;
	x->stx_size = st.st_size;
	x->stx_blocks = st.st_blocks;
	x->stx_atime.tv_sec = st.st_atime_; x->stx_atime.tv_nsec = st.st_atime_nsec;
	x->stx_mtime.tv_sec = st.st_mtime_; x->stx_mtime.tv_nsec = st.st_mtime_nsec;
	x->stx_ctime.tv_sec = st.st_ctime_; x->stx_ctime.tv_nsec = st.st_ctime_nsec;
	x->stx_btime = x->stx_mtime;
	x->stx_rdev_major = (lx_u32)(st.st_rdev >> 8);
	x->stx_rdev_minor = (lx_u32)(st.st_rdev & 0xff);
	x->stx_dev_major = (lx_u32)(st.st_dev >> 8);
	x->stx_dev_minor = (lx_u32)(st.st_dev & 0xff);
	return 0;
}

static int sys_access(lx_thread *t, int dirfd, lx_u32 upath, int mode, int flags)
{
	char path[LX_PATH_MAX];
	struct lx_stat64 st;
	int r = getpath(t, upath, path);
	if (r < 0) return r;
	r = lx_vfs_stat(t->proc, dirfd, path, !(flags & LX_AT_SYMLINK_NOFOLLOW), &st);
	if (r < 0) return r;
	if ((mode & LX_X_OK) && !LX_S_ISDIR(st.st_mode) && !(st.st_mode & 0111)) return -LX_EACCES;
	return 0;
}

static int sys_getcwd(lx_thread *t, lx_u32 buf, lx_u32 size)
{
	int n = lx_strlen(t->proc->cwd) + 1;
	if ((lx_u32)n > size) return -LX_ERANGE;
	if (!lx_uaccess(t, buf, n)) return -LX_EFAULT;
	bk_memcpy((void *)buf, t->proc->cwd, n);
	return n;
}

static int set_cwd(lx_proc *p, int dirfd, const char *path)
{
	char c[LX_PATH_MAX];
	lx_vn v;
	int r = lx_vfs_resolve(p, dirfd, path, 1, c);
	if (r < 0) return r;
	r = lx_vn_lookup(p, c, &v);
	if (r < 0) return r;
	if (!LX_S_ISDIR(v.mode)) return -LX_ENOTDIR;
	lx_strlcpy(p->cwd, c, sizeof p->cwd);
	return 0;
}

static int sys_fcntl(lx_thread *t, int fd, int cmd, lx_u32 arg)
{
	lx_proc *p = t->proc;
	lx_file *f = lx_fd_get(p, fd);
	if (!f) return -LX_EBADF;
	switch (cmd) {
	case LX_F_DUPFD: case LX_F_DUPFD_CLOEXEC:
		if (arg >= LX_MAXFD) return -LX_EINVAL;
		return lx_fd_alloc(p, lx_file_get(f), (int)arg, cmd == LX_F_DUPFD_CLOEXEC);
	case LX_F_GETFD: return p->fdflags[fd] & LX_FD_CLOEXEC;
	case LX_F_SETFD: p->fdflags[fd] = (lx_u8)(arg & LX_FD_CLOEXEC); return 0;
	case LX_F_GETFL: return f->flags | LX_O_LARGEFILE;
	case LX_F_SETFL:
		f->flags = (f->flags & ~(LX_O_APPEND | LX_O_NONBLOCK | LX_O_ASYNC)) | (arg & (LX_O_APPEND | LX_O_NONBLOCK | LX_O_ASYNC));
		return 0;
	case LX_F_GETLK: case LX_F_GETLK64:
		/* no locks are held by anyone else: F_UNLCK (2) */
		if (lx_uaccess(t, arg, 2)) *(lx_u16 *)arg = 2;
		return 0;
	case LX_F_SETLK: case LX_F_SETLKW: case LX_F_SETLK64: case LX_F_SETLKW64:
		return 0;
	case LX_F_SETOWN: case LX_F_GETOWN: return 0;
	case 1031: case 1032: return 0;     /* F_SETPIPE_SZ / F_GETPIPE_SZ */
	}
	return -LX_EINVAL;
}

static int sys_ioctl(lx_thread *t, int fd, lx_u32 req, lx_u32 arg)
{
	lx_proc *p = t->proc;
	lx_file *f = lx_fd_get(p, fd);
	if (!f) return -LX_EBADF;
	switch (req) {
	case LX_FIOCLEX: p->fdflags[fd] |= LX_FD_CLOEXEC; return 0;
	case LX_FIONCLEX: p->fdflags[fd] &= ~LX_FD_CLOEXEC; return 0;
	case LX_FIONBIO:
		if (!lx_uaccess(t, arg, 4)) return -LX_EFAULT;
		if (*(lx_u32 *)arg) f->flags |= LX_O_NONBLOCK; else f->flags &= ~LX_O_NONBLOCK;
		return 0;
	}
	if (f->ops->ioctl) return f->ops->ioctl(t, f, req, arg);
	return -LX_ENOTTY;
}

static int sys_dup3(lx_thread *t, int oldfd, int newfd, int flags)
{
	lx_proc *p = t->proc;
	lx_file *f = lx_fd_get(p, oldfd);
	if (!f) return -LX_EBADF;
	if (newfd < 0 || newfd >= LX_MAXFD) return -LX_EBADF;
	if (oldfd == newfd) return flags == -1 ? newfd : -LX_EINVAL;
	if (p->fd[newfd]) lx_file_put(p->fd[newfd]);
	p->fd[newfd] = lx_file_get(f);
	p->fdflags[newfd] = (flags > 0 && (flags & LX_O_CLOEXEC)) ? LX_FD_CLOEXEC : 0;
	return newfd;
}

static int sys_pipe2(lx_thread *t, lx_u32 ufds, int flags)
{
	lx_file *r, *w;
	int a, b, e;
	if (!lx_uaccess(t, ufds, 8)) return -LX_EFAULT;
	e = lx_pipe_new(&r, &w);
	if (e < 0) return e;
	if (flags & LX_O_NONBLOCK) { r->flags |= LX_O_NONBLOCK; w->flags |= LX_O_NONBLOCK; }
	a = lx_fd_alloc(t->proc, r, 0, flags & LX_O_CLOEXEC);
	if (a < 0) { lx_file_put(w); return a; }
	b = lx_fd_alloc(t->proc, w, 0, flags & LX_O_CLOEXEC);
	if (b < 0) { lx_file_put(t->proc->fd[a]); t->proc->fd[a] = 0; return b; }
	((lx_s32 *)ufds)[0] = a;
	((lx_s32 *)ufds)[1] = b;
	return 0;
}

static int sys_getdents64(lx_thread *t, int fd, lx_u32 buf, lx_u32 n)
{
	lx_file *f = lx_fd_get(t->proc, fd);
	if (!f) return -LX_EBADF;
	if (!f->ops->getdents) return -LX_ENOTDIR;
	if (!lx_uaccess(t, buf, n)) return -LX_EFAULT;
	return f->ops->getdents(f, (char *)buf, n);
}

static int sys_readlink(lx_thread *t, int dirfd, lx_u32 upath, lx_u32 buf, lx_u32 size)
{
	char path[LX_PATH_MAX], tmp[LX_PATH_MAX];
	int r = getpath(t, upath, path);
	if (r < 0) return r;
	if (!lx_uaccess(t, buf, size)) return -LX_EFAULT;
	r = lx_vfs_readlink(t->proc, dirfd, path, tmp, sizeof tmp);
	if (r < 0) return r;
	if ((lx_u32)r > size) r = (int)size;
	bk_memcpy((void *)buf, tmp, r);
	return r;
}

static int sys_utimensat(lx_thread *t, int dirfd, lx_u32 upath, lx_u32 times, int flags, int is64)
{
	char path[LX_PATH_MAX];
	lx_s64 mtime = bk_now_real();
	int r;
	if (times) {
		lx_s64 sec;
		lx_s32 nsec;
		if (is64) {
			struct lx_timespec64 *ts = (struct lx_timespec64 *)times;
			if (!lx_uaccess(t, times, 32)) return -LX_EFAULT;
			sec = ts[1].tv_sec; nsec = ts[1].tv_nsec;
		} else {
			struct lx_timespec32 *ts = (struct lx_timespec32 *)times;
			if (!lx_uaccess(t, times, 16)) return -LX_EFAULT;
			sec = ts[1].tv_sec; nsec = ts[1].tv_nsec;
		}
		if (nsec == ((1 << 30) - 2)) return 0;          /* UTIME_OMIT */
		if (nsec != ((1 << 30) - 1)) mtime = sec * 1000000 + nsec / 1000;   /* (else UTIME_NOW) */
	}
	if (!upath) {
		lx_file *f = lx_fd_get(t->proc, dirfd);
		if (!f) return -LX_EBADF;
		if (!f->path) return 0;
		return lx_vfs_utime(t->proc, LX_AT_FDCWD, f->path, mtime, 1);
	}
	r = getpath(t, upath, path);
	if (r < 0) return r;
	return lx_vfs_utime(t->proc, dirfd, path, mtime, !(flags & LX_AT_SYMLINK_NOFOLLOW));
}

/* the disk a path is on: its file's native name, or the root's */
static void statfs_native(lx_thread *t, const char *path, int fd, char *out)
{
	lx_vn v;
	char c[LX_PATH_MAX];
	lx_strlcpy(out, lx_k.root_native, LX_PATH_MAX);
	if (fd >= 0) {
		lx_file *f = lx_fd_get(t->proc, fd);
		if (!f || !f->path) return;
		path = f->path;
	}
	if (lx_vfs_resolve(t->proc, LX_AT_FDCWD, path, 1, c) < 0 || lx_vn_lookup(t->proc, c, &v) < 0) return;
	if (v.fs == LX_FS_NATIVE && v.native[0]) lx_strlcpy(out, v.native, LX_PATH_MAX);
}

static int sys_statfs(lx_thread *t, lx_u32 ubuf, lx_u32 size, const char *native)
{
	struct lx_statfs64 s;
	lx_u64 total = 0, freeb = 0;
	char d[4];
	if (size > sizeof s) size = sizeof s;
	if (!lx_uaccess(t, ubuf, size)) return -LX_EFAULT;
	d[0] = native[0]; d[1] = ':'; d[2] = bk_native_sep(); d[3] = 0;
	bk_disk_info(bk_native_sep() == '\\' ? d : native, &total, &freeb);
	bk_memset(&s, 0, sizeof s);
	s.f_type = 0x4d44;            /* MSDOS_SUPER_MAGIC */
	s.f_bsize = 4096;
	s.f_frsize = 4096;
	s.f_blocks = total / 4096;
	s.f_bfree = s.f_bavail = freeb / 4096;
	s.f_files = 65536;
	s.f_ffree = 65536;
	s.f_namelen = 255;
	bk_memcpy((void *)ubuf, &s, size);
	return 0;
}

/* ---- poll and select --------------------------------------------------------- */

static int poll_fd(lx_proc *p, int fd)
{
	lx_file *f = lx_fd_get(p, fd);
	if (!f) return LX_POLLNVAL;
	if (!f->ops->poll) return LX_POLLIN | LX_POLLOUT;
	return f->ops->poll(f);
}

static int timeout_us(lx_thread *t, lx_u32 uts, int kind, lx_s64 *us)
{
	/* kind 0: milliseconds (int), 1: timespec32, 2: timespec64, 3: timeval32 */
	if (kind == 0) { *us = (int)uts < 0 ? -1 : (lx_s64)(int)uts * 1000; return 0; }
	if (!uts) { *us = -1; return 0; }
	if (kind == 1) { struct lx_timespec32 *ts = (struct lx_timespec32 *)uts; if (!lx_uaccess(t, uts, 8)) return -LX_EFAULT; *us = (lx_s64)ts->tv_sec * 1000000 + ts->tv_nsec / 1000; }
	else if (kind == 2) { struct lx_timespec64 *ts = (struct lx_timespec64 *)uts; if (!lx_uaccess(t, uts, 16)) return -LX_EFAULT; *us = ts->tv_sec * 1000000 + ts->tv_nsec / 1000; }
	else { struct lx_timeval32 *tv = (struct lx_timeval32 *)uts; if (!lx_uaccess(t, uts, 8)) return -LX_EFAULT; *us = (lx_s64)tv->tv_sec * 1000000 + tv->tv_usec; }
	return 0;
}

/* the temporary signal mask of ppoll and pselect6 */
static int use_sigmask(lx_thread *t, lx_u32 umask)
{
	if (!umask || t->sigmask_saved) return 0;
	if (!lx_uaccess(t, umask, 8)) return -LX_EFAULT;
	t->saved_sigmask[0] = t->sigmask[0];
	t->saved_sigmask[1] = t->sigmask[1];
	t->sigmask[0] = ((lx_u32 *)umask)[0];
	t->sigmask[1] = ((lx_u32 *)umask)[1];
	t->sigmask_saved = 1;
	lx_signal_proc(t->proc, 0);
	return 0;
}

static int sys_poll(lx_thread *t, lx_u32 ufds, lx_u32 nfds, lx_u32 uts, int kind, lx_u32 umask)
{
	struct lx_pollfd *pf = (struct lx_pollfd *)ufds;
	lx_u32 i;
	int n = 0, r;
	lx_s64 us;
	if (nfds > LX_MAXFD * 2) return -LX_EINVAL;
	if (nfds && !lx_uaccess(t, ufds, nfds * sizeof *pf)) return -LX_EFAULT;
	r = timeout_us(t, uts, kind, &us);
	if (r < 0) return r;
	r = use_sigmask(t, umask);
	if (r < 0) return r;
	for (i = 0; i < nfds; i++) {
		int ev;
		if (pf[i].fd < 0) { pf[i].revents = 0; continue; }
		ev = poll_fd(t->proc, pf[i].fd);
		pf[i].revents = (short)(ev & (pf[i].events | LX_POLLERR | LX_POLLHUP | LX_POLLNVAL));
		if (pf[i].revents) n++;
	}
	if (n || us == 0) return n;
	if (t->timed_out) { t->timed_out = 0; t->deadline = 0; return 0; }
	if (us > 0 && !t->deadline) t->deadline = lx_now() + us;
	return LX_BLOCK;
}

static int sys_select(lx_thread *t, int nfds, lx_u32 rd, lx_u32 wr, lx_u32 ex, lx_u32 uts, int kind, lx_u32 umask)
{
	lx_u32 rin[LX_MAXFD / 32], win[LX_MAXFD / 32], rout[LX_MAXFD / 32], wout[LX_MAXFD / 32];
	int i, n = 0, words, r;
	lx_s64 us;
	if (nfds < 0) return -LX_EINVAL;
	if (nfds > LX_MAXFD) nfds = LX_MAXFD;
	words = (nfds + 31) / 32;
	if ((rd && !lx_uaccess(t, rd, words * 4)) || (wr && !lx_uaccess(t, wr, words * 4)) || (ex && !lx_uaccess(t, ex, words * 4))) return -LX_EFAULT;
	r = timeout_us(t, uts, kind, &us);
	if (r < 0) return r;
	r = use_sigmask(t, umask);
	if (r < 0) return r;
	for (i = 0; i < words; i++) {
		rin[i] = rd ? ((lx_u32 *)rd)[i] : 0;
		win[i] = wr ? ((lx_u32 *)wr)[i] : 0;
		rout[i] = wout[i] = 0;
	}
	for (i = 0; i < nfds; i++) {
		lx_u32 bit = 1u << (i & 31);
		int ev;
		if (!((rin[i >> 5] | win[i >> 5]) & bit)) continue;
		ev = poll_fd(t->proc, i);
		if (ev & LX_POLLNVAL) return -LX_EBADF;
		if ((rin[i >> 5] & bit) && (ev & (LX_POLLIN | LX_POLLHUP | LX_POLLERR))) { rout[i >> 5] |= bit; n++; }
		if ((win[i >> 5] & bit) && (ev & (LX_POLLOUT | LX_POLLERR))) { wout[i >> 5] |= bit; n++; }
	}
	if (!n && us != 0) {
		if (t->timed_out) { t->timed_out = 0; t->deadline = 0; }
		else {
			if (us > 0 && !t->deadline) t->deadline = lx_now() + us;
			return LX_BLOCK;
		}
	}
	for (i = 0; i < words; i++) {
		if (rd) ((lx_u32 *)rd)[i] = rout[i];
		if (wr) ((lx_u32 *)wr)[i] = wout[i];
		if (ex) ((lx_u32 *)ex)[i] = 0;
	}
	return n;
}

/* sendfile(out, in, offset, count): a copy through the kernel */
static int sys_sendfile(lx_thread *t, int out, int in, lx_u32 uoff, lx_u32 count, int is64)
{
	lx_file *fi = lx_fd_get(t->proc, in), *fo = lx_fd_get(t->proc, out);
	char buf[2048];
	lx_s64 off = 0;
	int n, w;
	if (!fi || !fo) return -LX_EBADF;
	if (uoff) {
		if (!lx_uaccess(t, uoff, is64 ? 8 : 4)) return -LX_EFAULT;
		off = is64 ? *(lx_s64 *)uoff : *(lx_s32 *)uoff;
	}
	if (count > sizeof buf) count = sizeof buf;
	if (uoff) n = fi->ops->pread ? fi->ops->pread(t, fi, buf, count, off) : -LX_ESPIPE;
	else n = fi->ops->read ? fi->ops->read(t, fi, buf, count) : -LX_EINVAL;
	if (n <= 0) return n;
	w = fo->ops->write ? fo->ops->write(t, fo, buf, n) : -LX_EINVAL;
	if (w == LX_BLOCK) w = 0;
	if (w < 0) return w;
	if (uoff) { if (is64) *(lx_s64 *)uoff = off + w; else *(lx_s32 *)uoff = (lx_s32)(off + w); }
	else if (w < n && fi->ops->lseek) fi->ops->lseek(fi, w - n, LX_SEEK_CUR);
	return w;
}

int lx_fs_syscall(lx_thread *t, lx_u32 nr, lx_u32 *a)
{
	lx_proc *p = t->proc;
	lx_file *f;
	char path[LX_PATH_MAX], path2[LX_PATH_MAX];
	int r;
	switch (nr) {
	case LX_NR_read:
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		return do_read(t, f, a[1], a[2]);
	case LX_NR_write:
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		return do_write(t, f, a[1], a[2]);
	case LX_NR_readv: case LX_NR_writev:
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		return do_iov(t, f, a[1], (int)a[2], nr == LX_NR_writev, 0, 0);
	case LX_NR_preadv: case LX_NR_pwritev:
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		return do_iov(t, f, a[1], (int)a[2], nr == LX_NR_pwritev, (lx_s64)a[3] | ((lx_s64)a[4] << 32), 1);
	case LX_NR_preadv2: case LX_NR_pwritev2: {
		/* (fd, iov, count, offset low, offset high, flags): offset -1 = where the file is */
		lx_s64 off = (lx_s64)a[3] | ((lx_s64)a[4] << 32);
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		return do_iov(t, f, a[1], (int)a[2], nr == LX_NR_pwritev2, off, off != -1);
	}
	case LX_NR_pread64: case LX_NR_pwrite64: {
		lx_s64 off = (lx_s64)a[4] | ((lx_s64)a[5] << 32);    /* (EABI: the 64-bit offset in r4:r5) */
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		if (a[2] && !lx_uaccess(t, a[1], a[2])) return -LX_EFAULT;
		if (nr == LX_NR_pread64) return f->ops->pread ? f->ops->pread(t, f, (char *)a[1], a[2], off) : -LX_ESPIPE;
		return f->ops->pwrite ? f->ops->pwrite(t, f, (const char *)a[1], a[2], off) : -LX_ESPIPE;
	}
	case LX_NR_open: return sys_openat(t, LX_AT_FDCWD, a[0], (int)a[1], a[2]);
	case LX_NR_openat: return sys_openat(t, (int)a[0], a[1], (int)a[2], a[3]);
	case LX_NR_creat: return sys_openat(t, LX_AT_FDCWD, a[0], LX_O_CREAT | LX_O_WRONLY | LX_O_TRUNC, a[1]);
	case LX_NR_close:
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		p->fd[a[0]] = 0;
		p->fdflags[a[0]] = 0;
		lx_file_put(f);
		return 0;
	case LX_NR_lseek: {
		lx_s64 r64;
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		if (!f->ops->lseek) return -LX_ESPIPE;
		r64 = f->ops->lseek(f, (lx_s32)a[1], (int)a[2]);
		if (r64 > 0x7fffffff) return -LX_EINVAL;
		return (int)r64;
	}
	case LX_NR__llseek: {
		lx_s64 r64, off = ((lx_s64)a[1] << 32) | a[2];
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		if (!f->ops->lseek) return -LX_ESPIPE;
		if (!lx_uaccess(t, a[3], 8)) return -LX_EFAULT;
		r64 = f->ops->lseek(f, off, (int)a[4]);
		if (r64 < 0) return (int)r64;
		bk_memcpy((void *)a[3], &r64, 8);
		return 0;
	}
	case LX_NR_dup:
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		return lx_fd_alloc(p, lx_file_get(f), 0, 0);
	case LX_NR_dup2: return sys_dup3(t, (int)a[0], (int)a[1], -1);
	case LX_NR_dup3: return sys_dup3(t, (int)a[0], (int)a[1], (int)a[2] | 0x10000000);
	case LX_NR_fcntl: case LX_NR_fcntl64: return sys_fcntl(t, (int)a[0], (int)a[1], a[2]);
	case LX_NR_ioctl: return sys_ioctl(t, (int)a[0], a[1], a[2]);
	case LX_NR_pipe: return sys_pipe2(t, a[0], 0);
	case LX_NR_pipe2: return sys_pipe2(t, a[0], (int)a[1]);
	case LX_NR_stat64: return sys_stat(t, LX_AT_FDCWD, a[0], a[1], 0);
	case LX_NR_lstat64: return sys_stat(t, LX_AT_FDCWD, a[0], a[1], LX_AT_SYMLINK_NOFOLLOW);
	case LX_NR_fstat64: return sys_fstat(t, (int)a[0], a[1]);
	case LX_NR_fstatat64: return sys_stat(t, (int)a[0], a[1], a[2], (int)a[3]);
	case LX_NR_statx: return sys_statx(t, (int)a[0], a[1], (int)a[2], a[3], a[4]);
	case LX_NR_access: return sys_access(t, LX_AT_FDCWD, a[0], (int)a[1], 0);
	case LX_NR_faccessat: return sys_access(t, (int)a[0], a[1], (int)a[2], 0);
	case LX_NR_faccessat2: return sys_access(t, (int)a[0], a[1], (int)a[2], (int)a[3]);
	case LX_NR_readlink: return sys_readlink(t, LX_AT_FDCWD, a[0], a[1], a[2]);
	case LX_NR_readlinkat: return sys_readlink(t, (int)a[0], a[1], a[2], a[3]);
	case LX_NR_getdents64: return sys_getdents64(t, (int)a[0], a[1], a[2]);
	case LX_NR_getcwd: return sys_getcwd(t, a[0], a[1]);
	case LX_NR_chdir:
		if ((r = getpath(t, a[0], path)) < 0) return r;
		return set_cwd(p, LX_AT_FDCWD, path);
	case LX_NR_fchdir:
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		if (!f->path) return -LX_ENOTDIR;
		return set_cwd(p, LX_AT_FDCWD, f->path);
	case LX_NR_chroot: return -LX_EPERM;
	case LX_NR_mkdir:
		if ((r = getpath(t, a[0], path)) < 0) return r;
		return lx_vfs_mkdir(p, LX_AT_FDCWD, path, a[1]);
	case LX_NR_mkdirat:
		if ((r = getpath(t, a[1], path)) < 0) return r;
		return lx_vfs_mkdir(p, (int)a[0], path, a[2]);
	case LX_NR_rmdir:
		if ((r = getpath(t, a[0], path)) < 0) return r;
		return lx_vfs_unlink(p, LX_AT_FDCWD, path, 1);
	case LX_NR_unlink:
		if ((r = getpath(t, a[0], path)) < 0) return r;
		return lx_vfs_unlink(p, LX_AT_FDCWD, path, 0);
	case LX_NR_unlinkat:
		if ((r = getpath(t, a[1], path)) < 0) return r;
		return lx_vfs_unlink(p, (int)a[0], path, (a[2] & LX_AT_REMOVEDIR) ? 1 : 0);
	case LX_NR_rename:
		if ((r = getpath(t, a[0], path)) < 0 || (r = getpath(t, a[1], path2)) < 0) return r;
		return lx_vfs_rename(p, LX_AT_FDCWD, path, LX_AT_FDCWD, path2);
	case LX_NR_renameat: case LX_NR_renameat2:
		if ((r = getpath(t, a[1], path)) < 0 || (r = getpath(t, a[3], path2)) < 0) return r;
		if (nr == LX_NR_renameat2 && a[4] & 1) {            /* RENAME_NOREPLACE */
			struct lx_stat64 st;
			if (lx_vfs_stat(p, (int)a[2], path2, 0, &st) == 0) return -LX_EEXIST;
		}
		return lx_vfs_rename(p, (int)a[0], path, (int)a[2], path2);
	case LX_NR_symlink:
		if ((r = getpath(t, a[0], path)) < 0 || (r = getpath(t, a[1], path2)) < 0) return r;
		return lx_vfs_symlink(p, path, LX_AT_FDCWD, path2);
	case LX_NR_symlinkat:
		if ((r = getpath(t, a[0], path)) < 0 || (r = getpath(t, a[2], path2)) < 0) return r;
		return lx_vfs_symlink(p, path, (int)a[1], path2);
	case LX_NR_link: case LX_NR_linkat:
		return -LX_EPERM;        /* FAT has no hard links */
	case LX_NR_chmod:
		if ((r = getpath(t, a[0], path)) < 0) return r;
		return lx_vfs_chmod(p, LX_AT_FDCWD, path, a[1], 1);
	case LX_NR_fchmodat:
		if ((r = getpath(t, a[1], path)) < 0) return r;
		return lx_vfs_chmod(p, (int)a[0], path, a[2], 1);
	case LX_NR_fchmod:
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		return f->path ? lx_vfs_chmod(p, LX_AT_FDCWD, f->path, a[1], 1) : 0;
	case LX_NR_chown32: case LX_NR_lchown32: case LX_NR_fchown32: case LX_NR_fchownat:
	case LX_NR_chown: case LX_NR_lchown: case LX_NR_fchown:
		return 0;               /* one user */
	case LX_NR_utimensat: return sys_utimensat(t, (int)a[0], a[1], a[2], (int)a[3], 0);
	case LX_NR_utimensat_time64: return sys_utimensat(t, (int)a[0], a[1], a[2], (int)a[3], 1);
	case LX_NR_utimes: {
		if ((r = getpath(t, a[0], path)) < 0) return r;
		if (a[1]) {
			struct lx_timeval32 *tv = (struct lx_timeval32 *)a[1];
			if (!lx_uaccess(t, a[1], 16)) return -LX_EFAULT;
			return lx_vfs_utime(p, LX_AT_FDCWD, path, (lx_s64)tv[1].tv_sec * 1000000 + tv[1].tv_usec, 1);
		}
		return lx_vfs_utime(p, LX_AT_FDCWD, path, bk_now_real(), 1);
	}
	case LX_NR_truncate64: {
		lx_s64 len = (lx_s64)a[2] | ((lx_s64)a[3] << 32);
		int err = 0;
		if ((r = getpath(t, a[0], path)) < 0) return r;
		f = lx_vfs_open(p, LX_AT_FDCWD, path, LX_O_WRONLY, 0, &err);
		if (!f) return err;
		r = f->ops->truncate ? f->ops->truncate(f, len) : -LX_EINVAL;
		lx_file_put(f);
		return r;
	}
	case LX_NR_ftruncate64: {
		lx_s64 len = (lx_s64)a[2] | ((lx_s64)a[3] << 32);
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		return f->ops->truncate ? f->ops->truncate(f, len) : -LX_EINVAL;
	}
	case LX_NR_fsync: case LX_NR_fdatasync:
		if (!(f = lx_fd_get(p, (int)a[0]))) return -LX_EBADF;
		return f->ops->sync ? f->ops->sync(f) : 0;
	case LX_NR_sync: case LX_NR_syncfs: return 0;
	case LX_NR_statfs64: case LX_NR_statfs:
		if ((r = getpath(t, a[0], path)) < 0) return r;
		statfs_native(t, path, -1, path2);
		return sys_statfs(t, nr == LX_NR_statfs ? a[1] : a[2], nr == LX_NR_statfs ? 64 : a[1], path2);
	case LX_NR_fstatfs64: case LX_NR_fstatfs:
		statfs_native(t, 0, (int)a[0], path2);
		return sys_statfs(t, nr == LX_NR_fstatfs ? a[1] : a[2], nr == LX_NR_fstatfs ? 64 : a[1], path2);
	case LX_NR_poll: return sys_poll(t, a[0], a[1], a[2], 0, 0);
	case LX_NR_ppoll: return sys_poll(t, a[0], a[1], a[2], 1, a[3]);
	case LX_NR_ppoll_time64: return sys_poll(t, a[0], a[1], a[2], 2, a[3]);
	case LX_NR__newselect: return sys_select(t, (int)a[0], a[1], a[2], a[3], a[4], 3, 0);
	case LX_NR_pselect6: case LX_NR_pselect6_time64: {
		lx_u32 mask = 0;
		if (a[5]) { if (!lx_uaccess(t, a[5], 8)) return -LX_EFAULT; mask = ((lx_u32 *)a[5])[0]; }
		return sys_select(t, (int)a[0], a[1], a[2], a[3], a[4], nr == LX_NR_pselect6 ? 1 : 2, mask);
	}
	case LX_NR_flock: return 0;
	case LX_NR_fadvise64_64: return 0;
	case LX_NR_fallocate: return -LX_EOPNOTSUPP;
	case LX_NR_mknod: case LX_NR_mknodat: return -LX_EPERM;
	case LX_NR_mount: {
		char fs[32], src[64];
		if ((r = lx_ustr(t, a[1], path, sizeof path)) < 0) return r;
		fs[0] = src[0] = 0;
		if (a[2] && (r = lx_ustr(t, a[2], fs, sizeof fs)) < 0) return r;
		if (a[0] && lx_ustr(t, a[0], src, sizeof src) < 0) src[0] = 0;
		return lx_vfs_mount(p, src, path, fs, a[3]);
	}
	case LX_NR_umount2:
		if ((r = lx_ustr(t, a[0], path, sizeof path)) < 0) return r;
		return lx_vfs_umount(p, path);
	case LX_NR_sendfile: return sys_sendfile(t, (int)a[0], (int)a[1], a[2], a[3], 0);
	case LX_NR_sendfile64: return sys_sendfile(t, (int)a[0], (int)a[1], a[2], a[3], 1);
	case LX_NR_copy_file_range: case LX_NR_splice: case LX_NR_tee: return -LX_ENOSYS;
	case LX_NR_eventfd2: case LX_NR_eventfd:
		f = lx_eventfd_new(a[0], nr == LX_NR_eventfd2 ? (int)a[1] : 0);
		if (!f) return -LX_ENOMEM;
		return lx_fd_alloc(p, f, 0, nr == LX_NR_eventfd2 && (a[1] & LX_O_CLOEXEC));
	case LX_NR_inotify_init: case LX_NR_inotify_init1: case LX_NR_epoll_create: case LX_NR_epoll_create1:
	case LX_NR_signalfd: case LX_NR_signalfd4: case LX_NR_timerfd_create: case LX_NR_memfd_create:
		return -LX_ENOSYS;
	case LX_NR_getxattr: case LX_NR_lgetxattr: case LX_NR_fgetxattr:
	case LX_NR_listxattr: case LX_NR_llistxattr: case LX_NR_flistxattr:
		return -LX_EOPNOTSUPP;
	case LX_NR_setxattr: case LX_NR_lsetxattr: case LX_NR_fsetxattr:
		return -LX_EOPNOTSUPP;
	case LX_NR_umask: return 0;
	}
	return LX_NOSYS;
}
