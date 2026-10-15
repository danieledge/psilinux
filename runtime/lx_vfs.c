/* lx_vfs.c - PsiLinux's file system.
 *
 *   /            the root: a read-only image (ROOT.IMG: BusyBox and the other
 *                programs, /etc...) with a writable layer over it, a folder
 *                on the Psion's disk (<disk>:\Linux\root\). Changes go to the
 *                layer; deleting a file of the image leaves a "whiteout".
 *                Symbolic links, modes and whiteouts in the layer live in a
 *                small file per folder, _LXMETA (the disk is FAT).
 *   /c, /d ...   the Psion's disks, as they are (WSL's /mnt/c, shorter)
 *   /tmp         in the writable layer like the rest (<disk>:\Linux\root\tmp\)
 *   /dev, /proc  made up by the kernel (lx_dev.c)
 *
 * Paths are resolved here, component by component, following symbolic
 * links (only the root and /dev, /proc have any). */
#include "lx_core.h"
#include "lx_vfs.h"

/* ---- file descriptors ------------------------------------------------------ */

lx_file *lx_file_new(const lx_fops *ops, void *priv, int flags)
{
	lx_file *f = (lx_file *)bk_alloc(sizeof *f);
	if (!f) return 0;
	f->refs = 1;
	f->ops = ops;
	f->priv = priv;
	f->flags = flags;
	return f;
}

lx_file *lx_file_get(lx_file *f) { if (f) f->refs++; return f; }

void lx_file_put(lx_file *f)
{
	if (!f || --f->refs > 0) return;
	if (f->ops && f->ops->close) f->ops->close(f);
	if (f->path) bk_free(f->path);
	bk_free(f);
}

int lx_fd_alloc(lx_proc *p, lx_file *f, int min, int cloexec)
{
	int i;
	for (i = min; i < LX_MAXFD; i++)
		if (!p->fd[i]) {
			p->fd[i] = f;
			p->fdflags[i] = cloexec ? LX_FD_CLOEXEC : 0;
			return i;
		}
	lx_file_put(f);
	return -LX_EMFILE;
}

lx_file *lx_fd_get(lx_proc *p, int fd)
{
	if (fd < 0 || fd >= LX_MAXFD) return 0;
	return p->fd[fd];
}

/* ---- names on the Psion's disks ------------------------------------------------ */

/* Characters FAT will not have in a name are written %XX. */
static int bad_char(unsigned char c)
{
	return c < 32 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
}

/* Linux name -> native name */
void lx_name_out(const char *in, char *out, int max)
{
	int o = 0;
	static const char hex[] = "0123456789ABCDEF";
	for (; *in && o < max - 4; in++) {
		unsigned char c = (unsigned char)*in;
		if (bad_char(c) || (c == '%' && 0)) {
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		} else out[o++] = (char)c;
	}
	out[o] = 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

/* native name -> Linux name */
void lx_name_in(const char *in, char *out, int max)
{
	int o = 0;
	for (; *in && o < max - 1; in++) {
		if (in[0] == '%' && hexval(in[1]) >= 0 && hexval(in[2]) >= 0) {
			int c = hexval(in[1]) * 16 + hexval(in[2]);
			if (bad_char((unsigned char)c)) { out[o++] = (char)c; in += 2; continue; }
		}
		out[o++] = *in;
	}
	out[o] = 0;
}

/* native path of a Linux path under a native folder: "D:\Linux\root\" + "/etc/x" */
void lx_native_join(char *out, int max, const char *nroot, const char *rel)
{
	char sep = bk_native_sep();
	int o;
	lx_strlcpy(out, nroot, max);
	o = lx_strlen(out);
	if (o && out[o - 1] == sep) o--;
	while (*rel && o < max - 2) {
		const char *e;
		char comp[256], enc[300];
		int n;
		while (*rel == '/') rel++;
		if (!*rel) break;
		e = rel;
		while (*e && *e != '/') e++;
		n = (int)(e - rel);
		if (n > 255) n = 255;
		bk_memcpy(comp, rel, n);
		comp[n] = 0;
		lx_name_out(comp, enc, sizeof enc);
		out[o++] = sep;
		n = lx_strlen(enc);
		if (o + n >= max - 1) break;
		bk_memcpy(out + o, enc, n);
		o += n;
		rel = e;
	}
	if (o == 2 && out[1] == ':') out[o++] = sep;   /* "D:" -> "D:\" */
	if (o == 0) out[o++] = sep;
	out[o] = 0;
}

/* ---- mounts ------------------------------------------------------------------- */

typedef struct mount {
	const char *at;             /* "/c" */
	int fs;
	char native[LX_PATH_MAX];
} mount;

#define MAXMOUNT 12
static mount mounts[MAXMOUNT];
static int nmounts;

static void add_mount(const char *at, int fs, const char *native)
{
	mount *m;
	if (nmounts >= MAXMOUNT) return;
	m = &mounts[nmounts++];
	m->at = lx_strdup(at);
	m->fs = fs;
	if (native) lx_strlcpy(m->native, native, sizeof m->native);
}

/* which mount a canonical path is on, and the path inside it */
static mount *find_mount(const char *path, const char **rel)
{
	int i, best = -1, blen = 0;
	for (i = 0; i < nmounts; i++) {
		int n = lx_strlen(mounts[i].at);
		if (n == 1) { if (best < 0) { best = i; blen = 0; } continue; }
		if (lx_strncmp(path, mounts[i].at, n) == 0 && (path[n] == 0 || path[n] == '/') && n > blen) { best = i; blen = n; }
	}
	if (best < 0) return 0;
	*rel = path + blen;
	return &mounts[best];
}

int lx_vfs_init(void)
{
	char drives[32], at[4], native[8], nr[LX_PATH_MAX];
	int i, n;
	char sep = bk_native_sep();
	nmounts = 0;
	lx_root_init();
	lx_native_join(nr, sizeof nr, lx_k.root_native, "root");
	add_mount("/", LX_FS_ROOT, nr);
	add_mount("/dev", LX_FS_DEV, 0);
	add_mount("/proc", LX_FS_PROC, 0);
	/* (/tmp is part of the root's writable layer, so it has links and modes) */
	n = bk_drive_list(drives);
	for (i = 0; i < n; i++) {
		at[0] = '/'; at[1] = (char)(drives[i] | 0x20); at[2] = 0;
		native[0] = drives[i]; native[1] = ':'; native[2] = sep; native[3] = 0;
		add_mount(at, LX_FS_NATIVE, native);
	}
	/* the overlay's own folders */
	lx_native_join(nr, sizeof nr, lx_k.root_native, "root");
	bk_mkdir(nr);
	return 0;
}

/* Mounts a program asks for (Alpine's OpenRC mounts /run, /dev/pts, /sys
 * and the like at boot): the kernel's file systems are fixed, so a mount
 * is only noted, and shown in /proc/mounts, so that the programs find
 * what they expect. It changes nothing in the tree. */
#define NPSEUDO 16
static struct { char at[96]; char fs[16]; char src[24]; } pseudo[NPSEUDO];

static int pseudo_find(const char *at)
{
	int i;
	for (i = 0; i < NPSEUDO; i++) if (pseudo[i].at[0] && lx_strcmp(pseudo[i].at, at) == 0) return i;
	return -1;
}

int lx_vfs_mount(lx_proc *p, const char *src, const char *at, const char *fs, lx_u32 flags)
{
	static const char *ok[] = { "proc", "sysfs", "tmpfs", "devtmpfs", "devpts", "mqueue", "cgroup", "cgroup2",
		"securityfs", "debugfs", "configfs", "fusectl", "binfmt_misc", "ramfs", "pstore", "efivarfs", "tracefs", 0 };
	char c[LX_PATH_MAX];
	struct lx_stat64 st;
	int i, r = lx_vfs_resolve(p, LX_AT_FDCWD, at, 1, c);
	if (r < 0) return r;
	if (lx_vfs_stat(p, LX_AT_FDCWD, c, 1, &st) < 0) return -LX_ENOENT;
	if (!LX_S_ISDIR(st.st_mode)) return -LX_ENOTDIR;
	if (flags & 0x20) return 0;                       /* MS_REMOUNT */
	if (flags & 0x1000) return 0;                     /* MS_BIND: (the same files are there already) */
	if (flags & (0x40000 | 0x80000 | 0x100000 | 0x2000)) return 0;   /* MS_PRIVATE, SLAVE, SHARED, MOVE */
	for (i = 0; ok[i]; i++) if (lx_strcmp(ok[i], fs) == 0) break;
	if (!ok[i]) return -LX_ENODEV;
	i = pseudo_find(c);
	if (i < 0) for (i = 0; i < NPSEUDO && pseudo[i].at[0]; i++) ;
	if (i >= NPSEUDO) return -LX_ENOMEM;
	lx_strlcpy(pseudo[i].at, c, sizeof pseudo[i].at);
	lx_strlcpy(pseudo[i].fs, fs, sizeof pseudo[i].fs);
	lx_strlcpy(pseudo[i].src, src && src[0] ? src : fs, sizeof pseudo[i].src);
	return 0;
}

int lx_vfs_umount(lx_proc *p, const char *at)
{
	char c[LX_PATH_MAX];
	int i, r = lx_vfs_resolve(p, LX_AT_FDCWD, at, 1, c);
	if (r < 0) return r;
	i = pseudo_find(c);
	if (i < 0) return -LX_EINVAL;
	pseudo[i].at[0] = 0;
	return 0;
}

/* /proc/self/mountinfo: the same, as OpenRC's mountinfo reads it */
int lx_mount_info(char *buf, int size)
{
	int i, o = 0, id = 20;
	for (i = 0; i < nmounts && o < size; i++, id++) {
		const char *t = mounts[i].fs == LX_FS_ROOT ? "psiroot" : mounts[i].fs == LX_FS_DEV ? "devtmpfs" :
			mounts[i].fs == LX_FS_PROC ? "proc" : "vfat";
		o += lx_snprintf(buf + o, size - o, "%d %d 0:%d / %s rw - %s %s rw\n", id, i ? 20 : 1, id, mounts[i].at, t,
			mounts[i].fs == LX_FS_NATIVE ? mounts[i].native : t);
	}
	for (i = 0; i < NPSEUDO && o < size; i++, id++)
		if (pseudo[i].at[0] && lx_strcmp(pseudo[i].at, "/proc") && lx_strcmp(pseudo[i].at, "/dev"))
			o += lx_snprintf(buf + o, size - o, "%d 20 0:%d / %s rw,nosuid,nodev - %s %s rw\n", id, id, pseudo[i].at, pseudo[i].fs, pseudo[i].src);
	return o < size ? o : size;
}

int lx_mount_list(char *buf, int size)
{
	int i, o = 0;
	for (i = 0; i < nmounts; i++) {
		const char *t = mounts[i].fs == LX_FS_ROOT ? "psiroot" : mounts[i].fs == LX_FS_DEV ? "devtmpfs" :
			mounts[i].fs == LX_FS_PROC ? "proc" : "vfat";
		o += lx_snprintf(buf + o, size - o, "%s %s %s rw 0 0\n", mounts[i].fs == LX_FS_NATIVE ? mounts[i].native : t, mounts[i].at, t);
		if (o >= size) break;
	}
	for (i = 0; i < NPSEUDO && o < size; i++)
		if (pseudo[i].at[0] && lx_strcmp(pseudo[i].at, "/proc") && lx_strcmp(pseudo[i].at, "/dev"))
			o += lx_snprintf(buf + o, size - o, "%s %s %s rw,nosuid,nodev 0 0\n", pseudo[i].src, pseudo[i].at, pseudo[i].fs);
	return o < size ? o : size;
}

/* ---- looking a path up -------------------------------------------------------- */

static lx_u64 path_ino(const char *s)
{
	lx_u64 h = 1469598103934665603ull;
	while (*s) { h ^= (lx_u8)*s++; h *= 1099511628211ull; }
	return h & 0x7fffffffffffull;
}

/* Fills in v for the canonical path (no symbolic link followed at the end).
 * Returns 0, or -ENOENT etc. */
int lx_vn_lookup(lx_proc *p, const char *path, lx_vn *v)
{
	const char *rel;
	mount *m = find_mount(path, &rel);
	int r;
	bk_memset(v, 0, sizeof *v);
	lx_strlcpy(v->path, path, sizeof v->path);
	v->img = -1;
	if (!m) return -LX_ENOENT;
	v->fs = m->fs;
	switch (m->fs) {
	case LX_FS_ROOT:
		r = lx_root_lookup(m->native, rel, v);
		break;
	case LX_FS_NATIVE: {
		bk_stat st;
		lx_native_join(v->native, sizeof v->native, m->native, rel);
		r = bk_stat_path(v->native, &st);
		if (r < 0) break;
		v->upper = 1;
		v->mode = st.is_dir ? (LX_S_IFDIR | 0755) : (LX_S_IFREG | (st.readonly ? 0555 : 0755));
		v->size = st.size;
		v->mtime = st.mtime;
		break;
	}
	case LX_FS_DEV:
		r = lx_dev_lookup(p, rel, v);
		break;
	case LX_FS_PROC:
		r = lx_proc_lookup(p, rel, v);
		break;
	default:
		r = -LX_ENOENT;
	}
	if (r == 0 && !v->ino) v->ino = path_ino(path);
	return r;
}

/* the canonical directory a dirfd or the cwd stands for */
static int base_dir(lx_proc *p, int dirfd, char *out)
{
	lx_file *f;
	if (dirfd == LX_AT_FDCWD) { lx_strlcpy(out, p->cwd, LX_PATH_MAX); return 0; }
	f = lx_fd_get(p, dirfd);
	if (!f) return -LX_EBADF;
	if (!f->path || !LX_S_ISDIR(f->mode)) return -LX_ENOTDIR;
	lx_strlcpy(out, f->path, LX_PATH_MAX);
	return 0;
}

/* Resolves path (relative to dirfd) to a canonical one, following symbolic
 * links in every component, and in the last one too if follow. A missing
 * last component is fine (out names it); a missing folder on the way is
 * -ENOENT. */
int lx_vfs_resolve(lx_proc *p, int dirfd, const char *path, int follow, char *out)
{
	char cur[LX_PATH_MAX];
	char rest[LX_PATH_MAX];
	char tmp[LX_PATH_MAX];
	int links = 0, r;
	if (!path[0]) return -LX_ENOENT;
	if (path[0] == '/') lx_strlcpy(cur, "/", sizeof cur);
	else if ((r = base_dir(p, dirfd, cur)) < 0) return r;
	lx_strlcpy(rest, path, sizeof rest);
	for (;;) {
		char comp[256];
		const char *s = rest;
		int n = 0, last;
		lx_vn v;
		while (*s == '/') s++;
		if (!*s) break;
		while (s[n] && s[n] != '/') n++;
		if (n > 255) return -LX_ENAMETOOLONG;
		bk_memcpy(comp, s, n);
		comp[n] = 0;
		s += n;
		while (*s == '/') s++;
		last = *s == 0;
		/* (rest becomes what is left after this component) */
		lx_strlcpy(tmp, s, sizeof tmp);
		lx_strlcpy(rest, tmp, sizeof rest);
		if (comp[0] == '.' && comp[1] == 0) continue;
		if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
			int i = lx_strlen(cur);
			while (i > 1 && cur[i - 1] != '/') i--;
			if (i > 1) i--;
			cur[i] = 0;
			if (!cur[0]) lx_strlcpy(cur, "/", sizeof cur);
			continue;
		}
		/* cur/comp */
		{
			int cl = lx_strlen(cur);
			if (cl + 1 + n >= LX_PATH_MAX) return -LX_ENAMETOOLONG;
			if (cl > 1) cur[cl++] = '/';
			bk_memcpy(cur + cl, comp, n + 1);
		}
		if (last && !follow) break;
		r = lx_vn_lookup(p, cur, &v);
		if (r < 0) {
			if (last) break;              /* (a new name: fine) */
			return r;
		}
		if (LX_S_ISLNK(v.mode)) {
			int ll;
			if (++links > 40) return -LX_ELOOP;
			/* the target, then what was left */
			ll = lx_strlen(v.link);
			if (ll + 1 + lx_strlen(rest) >= LX_PATH_MAX) return -LX_ENAMETOOLONG;
			bk_memcpy(tmp, v.link, ll);
			tmp[ll] = '/';
			lx_strlcpy(tmp + ll + 1, rest, sizeof tmp - ll - 1);
			lx_strlcpy(rest, tmp, sizeof rest);
			/* the link's folder */
			{
				int i = lx_strlen(cur);
				while (i > 1 && cur[i - 1] != '/') i--;
				if (i > 1) i--;
				cur[i] = 0;
				if (!cur[0] || v.link[0] == '/') lx_strlcpy(cur, "/", sizeof cur);
			}
			continue;
		}
		if (!last && !LX_S_ISDIR(v.mode)) return -LX_ENOTDIR;
	}
	lx_strlcpy(out, cur, LX_PATH_MAX);
	return 0;
}

int lx_vfs_stat(lx_proc *p, int dirfd, const char *path, int follow, struct lx_stat64 *st)
{
	char c[LX_PATH_MAX];
	lx_vn v;
	int r = lx_vfs_resolve(p, dirfd, path, follow, c);
	if (r < 0) return r;
	r = lx_vn_lookup(p, c, &v);
	if (r < 0) return r;
	lx_vn_stat(&v, st);
	return 0;
}

void lx_vn_stat(const lx_vn *v, struct lx_stat64 *st)
{
	bk_memset(st, 0, sizeof *st);
	st->st_dev = v->fs == LX_FS_ROOT ? 0x801 : v->fs == LX_FS_NATIVE ? 0x802 : v->fs == LX_FS_DEV ? 5 : 3;
	st->st_ino = v->ino;
	st->st_ino32 = (lx_u32)v->ino;
	st->st_mode = v->mode;
	st->st_nlink = LX_S_ISDIR(v->mode) ? 2 : 1;
	st->st_uid = 0;
	st->st_gid = 0;
	st->st_rdev = v->rdev;
	st->st_size = LX_S_ISLNK(v->mode) ? lx_strlen(v->link) : v->size;
	st->st_blksize = 4096;
	st->st_blocks = (v->size + 511) / 512;
	st->st_mtime_ = (lx_u32)(v->mtime / 1000000);
	st->st_mtime_nsec = (lx_u32)(v->mtime % 1000000) * 1000;
	st->st_atime_ = st->st_ctime_ = st->st_mtime_;
	st->st_atime_nsec = st->st_ctime_nsec = st->st_mtime_nsec;
}

int lx_vfs_readlink(lx_proc *p, int dirfd, const char *path, char *buf, int size)
{
	char c[LX_PATH_MAX];
	lx_vn v;
	int r = lx_vfs_resolve(p, dirfd, path, 0, c), n;
	if (r < 0) return r;
	r = lx_vn_lookup(p, c, &v);
	if (r < 0) return r;
	if (!LX_S_ISLNK(v.mode)) return -LX_EINVAL;
	n = lx_strlen(v.link);
	if (n > size) n = size;
	bk_memcpy(buf, v.link, n);
	return n;
}

/* ---- native files ----------------------------------------------------------- */

typedef struct nfile {
	void *h;
	char native[LX_PATH_MAX];
} nfile;

static int nf_pread(lx_thread *t, lx_file *f, char *buf, lx_u32 n, lx_s64 off)
{
	(void)t;
	return bk_file_read(((nfile *)f->priv)->h, off, buf, n);
}

static int nf_read(lx_thread *t, lx_file *f, char *buf, lx_u32 n)
{
	int r = nf_pread(t, f, buf, n, f->pos);
	if (r > 0) f->pos += r;
	return r;
}

static int nf_pwrite(lx_thread *t, lx_file *f, const char *buf, lx_u32 n, lx_s64 off)
{
	(void)t;
	return bk_file_write(((nfile *)f->priv)->h, off, buf, n);
}

static int nf_write(lx_thread *t, lx_file *f, const char *buf, lx_u32 n)
{
	int r;
	if (f->flags & LX_O_APPEND) {
		lx_s64 size;
		if (bk_file_size(((nfile *)f->priv)->h, &size) == 0) f->pos = size;
	}
	r = nf_pwrite(t, f, buf, n, f->pos);
	if (r > 0) f->pos += r;
	return r;
}

static lx_s64 nf_lseek(lx_file *f, lx_s64 off, int whence)
{
	lx_s64 base = 0;
	if (whence == LX_SEEK_CUR) base = f->pos;
	else if (whence == LX_SEEK_END) { if (bk_file_size(((nfile *)f->priv)->h, &base) < 0) return -LX_EIO; }
	else if (whence != LX_SEEK_SET) return -LX_EINVAL;
	if (base + off < 0) return -LX_EINVAL;
	f->pos = base + off;
	return f->pos;
}

static int nf_fstat(lx_file *f, struct lx_stat64 *st)
{
	lx_vn v;
	lx_s64 size = 0;
	bk_memset(&v, 0, sizeof v);
	v.mode = f->mode;
	v.ino = f->ino;
	v.fs = LX_FS_NATIVE;
	bk_file_size(((nfile *)f->priv)->h, &size);
	v.size = size;
	{
		bk_stat bs;
		if (bk_stat_path(((nfile *)f->priv)->native, &bs) == 0) v.mtime = bs.mtime;
	}
	lx_vn_stat(&v, st);
	return 0;
}

static int nf_poll(lx_file *f) { (void)f; return LX_POLLIN | LX_POLLOUT; }
static void nf_close(lx_file *f) { nfile *n = (nfile *)f->priv; bk_file_close(n->h); bk_free(n); }
static int nf_truncate(lx_file *f, lx_s64 len) { return bk_file_truncate(((nfile *)f->priv)->h, len); }
static int nf_sync(lx_file *f) { return bk_file_sync(((nfile *)f->priv)->h); }

static const lx_fops nfile_ops = {
	nf_read, nf_write, nf_pread, nf_pwrite, nf_lseek, nf_fstat, 0, nf_poll, 0, nf_close, nf_truncate, nf_sync
};

static lx_file *open_native(const char *native, int flags, lx_vn *v, int *err)
{
	nfile *n;
	lx_file *f;
	void *h = bk_file_open(native, flags, err);
	if (!h) return 0;
	n = (nfile *)bk_alloc(sizeof *n);
	f = n ? lx_file_new(&nfile_ops, n, flags) : 0;
	if (!f) { bk_file_close(h); bk_free(n); *err = -LX_ENOMEM; return 0; }
	n->h = h;
	lx_strlcpy(n->native, native, sizeof n->native);
	f->mode = v->mode ? v->mode : (LX_S_IFREG | 0755);
	f->ino = v->ino;
	return f;
}

/* a file of the root's writable layer, already open (lx_root.c) */
lx_file *lx_native_file(void *h, const char *native, lx_u32 mode, lx_u64 ino, int flags)
{
	nfile *n = (nfile *)bk_alloc(sizeof *n);
	lx_file *f = n ? lx_file_new(&nfile_ops, n, flags) : 0;
	if (!f) { bk_free(n); return 0; }
	n->h = h;
	lx_strlcpy(n->native, native, sizeof n->native);
	f->mode = mode;
	f->ino = ino;
	return f;
}

/* ---- directories: a listing made at open, read out by getdents64 ------------------ */

typedef struct dent { lx_u64 ino; lx_u8 type; char name[1]; } dent;
typedef struct dlist {
	char *buf;                  /* packed: ino (8), type (1), name, NUL */
	int len, cap;
	int count;
} dlist;

void lx_dlist_add(void *dl, const char *name, int type, lx_u64 ino)
{
	dlist *d = (dlist *)dl;
	int n = lx_strlen(name), need = 9 + n + 1;
	char *p;
	/* (union folders: a name seen already is not added again) */
	{
		int o = 0;
		while (o < d->len) {
			if (lx_strcmp(d->buf + o + 9, name) == 0) return;
			o += 9 + lx_strlen(d->buf + o + 9) + 1;
		}
	}
	if (d->len + need > d->cap) {
		int nc = d->cap ? d->cap * 2 : 1024;
		char *nb;
		while (nc < d->len + need) nc *= 2;
		nb = (char *)bk_alloc(nc);
		if (!nb) return;
		if (d->buf) { bk_memcpy(nb, d->buf, d->len); bk_free(d->buf); }
		d->buf = nb;
		d->cap = nc;
	}
	p = d->buf + d->len;
	bk_memcpy(p, &ino, 8);
	p[8] = (char)type;
	bk_memcpy(p + 9, name, n + 1);
	d->len += need;
	d->count++;
}

static int dir_getdents(lx_file *f, char *buf, lx_u32 n)
{
	dlist *d = (dlist *)f->priv;
	lx_u32 o = 0;
	int pos = (int)f->pos;
	while (pos < d->len) {
		const char *e = d->buf + pos;
		int nl = lx_strlen(e + 9);
		lx_u32 rec = (19 + nl + 1 + 7) & ~7u;
		lx_u64 ino;
		int next = pos + 9 + nl + 1;
		if (o + rec > n) break;
		bk_memcpy(&ino, e, 8);
		bk_memset(buf + o, 0, rec);
		bk_memcpy(buf + o, &ino, 8);
		{ lx_s64 off = next; bk_memcpy(buf + o + 8, &off, 8); }
		buf[o + 16] = (char)(rec & 0xff);
		buf[o + 17] = (char)(rec >> 8);
		buf[o + 18] = e[8];
		bk_memcpy(buf + o + 19, e + 9, nl + 1);
		o += rec;
		pos = next;
	}
	if (o == 0 && pos < d->len) return -LX_EINVAL;
	f->pos = pos;
	return (int)o;
}

static lx_s64 dir_lseek(lx_file *f, lx_s64 off, int whence)
{
	if (whence != LX_SEEK_SET || off < 0) return -LX_EINVAL;
	f->pos = off;
	return off;
}

static int dir_fstat(lx_file *f, struct lx_stat64 *st)
{
	lx_vn v;
	bk_memset(&v, 0, sizeof v);
	v.mode = f->mode;
	v.ino = f->ino;
	lx_vn_stat(&v, st);
	return 0;
}

static int dir_read(lx_thread *t, lx_file *f, char *b, lx_u32 n) { (void)t; (void)f; (void)b; (void)n; return -LX_EISDIR; }
static void dir_close(lx_file *f) { dlist *d = (dlist *)f->priv; if (d->buf) bk_free(d->buf); bk_free(d); }

static const lx_fops dir_ops = { dir_read, 0, 0, 0, dir_lseek, dir_fstat, 0, 0, dir_getdents, dir_close, 0, 0 };

static int dtype(lx_u32 mode)
{
	switch (mode & LX_S_IFMT) {
	case LX_S_IFDIR: return LX_DT_DIR;
	case LX_S_IFLNK: return LX_DT_LNK;
	case LX_S_IFCHR: return LX_DT_CHR;
	case LX_S_IFIFO: return LX_DT_FIFO;
	case LX_S_IFSOCK: return LX_DT_SOCK;
	default: return LX_DT_REG;
	}
}

/* a native folder's names (the overlay's own _LXMETA left out) */
void lx_list_native(void *dl, const char *native, const char *lpath)
{
	void *d;
	int err;
	const char *name;
	bk_stat st;
	char lname[256], full[LX_PATH_MAX];
	d = bk_dir_open(native, &err);
	if (!d) return;
	while ((name = bk_dir_next(d, &st)) != 0) {
		if (lx_strcmp(name, LX_META_NAME) == 0) continue;
		lx_name_in(name, lname, sizeof lname);
		lx_snprintf(full, sizeof full, "%s/%s", lpath, lname);
		lx_dlist_add(dl, lname, st.is_dir ? LX_DT_DIR : LX_DT_REG, path_ino(full));
	}
	bk_dir_close(d);
}

static lx_file *open_dir(lx_proc *p, lx_vn *v, int flags, int *err)
{
	dlist *d = (dlist *)bk_alloc(sizeof *d);
	lx_file *f;
	char up[LX_PATH_MAX];
	if (!d) { *err = -LX_ENOMEM; return 0; }
	lx_dlist_add(d, ".", LX_DT_DIR, v->ino);
	{
		int i = lx_strlen(v->path);
		lx_strlcpy(up, v->path, sizeof up);
		while (i > 1 && up[i - 1] != '/') i--;
		if (i > 1) i--;
		up[i] = 0;
		if (!up[0]) lx_strlcpy(up, "/", sizeof up);
		lx_dlist_add(d, "..", LX_DT_DIR, path_ino(up));
	}
	switch (v->fs) {
	case LX_FS_ROOT: lx_root_list(v, d); break;
	case LX_FS_NATIVE: lx_list_native(d, v->native, lx_strcmp(v->path, "/") ? v->path : ""); break;
	case LX_FS_DEV: lx_dev_list(p, v, d); break;
	case LX_FS_PROC: lx_proc_list(p, v, d); break;
	}
	/* the mount points in this folder */
	{
		int i, pl = lx_strlen(v->path);
		for (i = 0; i < nmounts; i++) {
			const char *at = mounts[i].at;
			int al = lx_strlen(at);
			if (al <= 1) continue;
			if (pl == 1) { if (at[0] == '/' && !lx_strchr(at + 1, '/')) lx_dlist_add(d, at + 1, LX_DT_DIR, path_ino(at)); }
			else if (lx_strncmp(at, v->path, pl) == 0 && at[pl] == '/' && !lx_strchr(at + pl + 1, '/'))
				lx_dlist_add(d, at + pl + 1, LX_DT_DIR, path_ino(at));
		}
	}
	f = lx_file_new(&dir_ops, d, flags);
	if (!f) { dir_close(0); *err = -LX_ENOMEM; return 0; }
	f->mode = v->mode;
	f->ino = v->ino;
	return f;
}

/* (the runner) a small file of the root, read whole: its length, or -errno */
int lx_read_file(const char *path, char *buf, int size)
{
	int err = 0, r;
	lx_proc fake;
	lx_file *f;
	bk_memset(&fake, 0, sizeof fake);
	lx_strlcpy(fake.cwd, "/", sizeof fake.cwd);
	f = lx_vfs_open(&fake, LX_AT_FDCWD, path, LX_O_RDONLY, 0, &err);
	if (!f) return err;
	r = f->ops->pread ? f->ops->pread(0, f, buf, size, 0) : -LX_EIO;
	lx_file_put(f);
	return r;
}

const char *lx_strchr(const char *s, int c)
{
	for (; *s; s++) if (*s == (char)c) return s;
	return 0;
}

/* ---- open --------------------------------------------------------------------- */

lx_file *lx_vfs_open(lx_proc *p, int dirfd, const char *path, int flags, lx_u32 mode, int *err)
{
	char c[LX_PATH_MAX];
	lx_vn v;
	lx_file *f = 0;
	int r, exists, acc = flags & LX_O_ACCMODE;
	/* /dev/fd/N of a pipe: that pipe (lx_proc_fd_file) */
	if (!(flags & LX_O_NOFOLLOW) && lx_vfs_resolve(p, dirfd, path, 0, c) == 0 && lx_vn_lookup(p, c, &v) == 0
	    && (f = lx_proc_fd_file(&v)) != 0)
		return f;
	r = lx_vfs_resolve(p, dirfd, path, !(flags & LX_O_NOFOLLOW), c);
	if (r < 0) { *err = r; return 0; }
	r = lx_vn_lookup(p, c, &v);
	exists = r == 0;
	if (!exists && r != -LX_ENOENT) { *err = r; return 0; }
	if (exists && (flags & LX_O_CREAT) && (flags & LX_O_EXCL)) { *err = -LX_EEXIST; return 0; }
	if (!exists && !(flags & LX_O_CREAT)) { *err = -LX_ENOENT; return 0; }
	if (exists && LX_S_ISLNK(v.mode)) { *err = -LX_ELOOP; return 0; }
	if (exists && LX_S_ISDIR(v.mode)) {
		if (acc != LX_O_RDONLY) { *err = -LX_EISDIR; return 0; }
		f = open_dir(p, &v, flags, err);
	} else if ((flags & LX_O_DIRECTORY) && exists) {
		*err = -LX_ENOTDIR;
		return 0;
	} else {
		switch (v.fs) {
		case LX_FS_NATIVE:
			if (!exists) {
				/* a new file: the folder must be there */
				v.mode = LX_S_IFREG | 0755;
			}
			f = open_native(v.native, flags, &v, err);
			break;
		case LX_FS_ROOT:
			f = lx_root_open(p, &v, exists, flags, mode & ~p->umask, err);
			break;
		case LX_FS_DEV:
			if (!exists) { *err = -LX_EACCES; return 0; }
			f = lx_dev_open(p, &v, flags, err);
			break;
		case LX_FS_PROC:
			if (!exists) { *err = -LX_EACCES; return 0; }
			f = lx_proc_open(p, &v, flags, err);
			break;
		}
	}
	if (!f) return 0;
	if (!f->path) f->path = lx_strdup(c);
	if (!f->ino) f->ino = v.ino ? v.ino : path_ino(c);
	f->flags = flags & ~(LX_O_CREAT | LX_O_EXCL | LX_O_TRUNC | LX_O_CLOEXEC);
	if ((flags & LX_O_TRUNC) && acc != LX_O_RDONLY && f->ops->truncate && LX_S_ISREG(f->mode)) f->ops->truncate(f, 0);
	return f;
}

/* ---- changing the tree --------------------------------------------------------- */

static int split_parent(const char *c, char *parent, const char **name)
{
	int i = lx_strlen(c);
	while (i > 0 && c[i - 1] != '/') i--;
	*name = c + i;
	if (i <= 1) lx_strlcpy(parent, "/", LX_PATH_MAX);
	else { bk_memcpy(parent, c, i - 1); parent[i - 1] = 0; }
	return 0;
}

int lx_vfs_mkdir(lx_proc *p, int dirfd, const char *path, lx_u32 mode)
{
	char c[LX_PATH_MAX];
	lx_vn v;
	int r = lx_vfs_resolve(p, dirfd, path, 0, c);
	if (r < 0) return r;
	if (lx_vn_lookup(p, c, &v) == 0) return -LX_EEXIST;
	switch (v.fs) {
	case LX_FS_NATIVE: return bk_mkdir(v.native);
	case LX_FS_ROOT: return lx_root_mkdir(p, c, mode & ~p->umask);
	default: return -LX_EACCES;
	}
}

int lx_vfs_unlink(lx_proc *p, int dirfd, const char *path, int dir)
{
	char c[LX_PATH_MAX];
	lx_vn v;
	int r = lx_vfs_resolve(p, dirfd, path, 0, c);
	if (r < 0) return r;
	r = lx_vn_lookup(p, c, &v);
	if (r < 0) return r;
	if (dir && !LX_S_ISDIR(v.mode)) return -LX_ENOTDIR;
	if (!dir && LX_S_ISDIR(v.mode)) return -LX_EISDIR;
	switch (v.fs) {
	case LX_FS_NATIVE: return dir ? bk_rmdir(v.native) : bk_unlink(v.native);
	case LX_FS_ROOT: return lx_root_unlink(p, &v, dir);
	default: return -LX_EACCES;
	}
}

int lx_vfs_rename(lx_proc *p, int olddir, const char *from, int newdir, const char *to)
{
	char a[LX_PATH_MAX], b[LX_PATH_MAX];
	lx_vn va, vb;
	int r, rb;
	r = lx_vfs_resolve(p, olddir, from, 0, a);
	if (r < 0) return r;
	r = lx_vfs_resolve(p, newdir, to, 0, b);
	if (r < 0) return r;
	r = lx_vn_lookup(p, a, &va);
	if (r < 0) return r;
	if (lx_strcmp(a, b) == 0) return 0;
	rb = lx_vn_lookup(p, b, &vb);
	if (va.fs != vb.fs) return -LX_EXDEV;
	if (rb == 0) {
		if (LX_S_ISDIR(vb.mode) && !LX_S_ISDIR(va.mode)) return -LX_EISDIR;
		if (!LX_S_ISDIR(vb.mode) && LX_S_ISDIR(va.mode)) return -LX_ENOTDIR;
	}
	if (va.fs == LX_FS_NATIVE) {
		{ const char *ra, *rbp; if (find_mount(a, &ra) != find_mount(b, &rbp)) return -LX_EXDEV; }
		if (rb == 0) {
			r = LX_S_ISDIR(vb.mode) ? bk_rmdir(vb.native) : bk_unlink(vb.native);
			if (r < 0) return r;
		}
		return bk_rename(va.native, vb.native);
	}
	if (va.fs == LX_FS_ROOT) return lx_root_rename(p, &va, &vb, rb == 0);
	return -LX_EACCES;
}

int lx_vfs_symlink(lx_proc *p, const char *target, int dirfd, const char *path)
{
	char c[LX_PATH_MAX];
	lx_vn v;
	int r = lx_vfs_resolve(p, dirfd, path, 0, c);
	if (r < 0) return r;
	if (lx_vn_lookup(p, c, &v) == 0) return -LX_EEXIST;
	if (v.fs != LX_FS_ROOT) return -LX_EPERM;   /* FAT has no links: only the root has them (_LXMETA) */
	return lx_root_symlink(p, c, target);
}

int lx_vfs_chmod(lx_proc *p, int dirfd, const char *path, lx_u32 mode, int follow)
{
	char c[LX_PATH_MAX];
	lx_vn v;
	int r = lx_vfs_resolve(p, dirfd, path, follow, c);
	if (r < 0) return r;
	r = lx_vn_lookup(p, c, &v);
	if (r < 0) return r;
	if (v.fs == LX_FS_ROOT) return lx_root_chmod(p, &v, mode & 07777);
	return 0;      /* (FAT: modes are made up; chmod quietly does nothing) */
}

int lx_vfs_utime(lx_proc *p, int dirfd, const char *path, lx_s64 mtime, int follow)
{
	char c[LX_PATH_MAX];
	lx_vn v;
	int r = lx_vfs_resolve(p, dirfd, path, follow, c);
	if (r < 0) return r;
	r = lx_vn_lookup(p, c, &v);
	if (r < 0) return r;
	if (v.fs == LX_FS_NATIVE) return bk_set_mtime(v.native, mtime);
	if (v.fs == LX_FS_ROOT) return lx_root_utime(p, &v, mtime);
	return 0;
}
