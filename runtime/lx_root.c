/* lx_root.c - the root file system: ROOT.IMG (read-only, made by
 * linux/tools/mkroot.py) under a writable layer, a folder on the Psion's
 * disk. Like Linux's overlayfs:
 *  - a name in the layer hides the same name in the image;
 *  - changing a file of the image copies it up into the layer first;
 *  - deleting one leaves a whiteout ('w') in the folder's _LXMETA;
 *  - a folder deleted and made again is opaque ('o'): the image's files in
 *    it stay hidden.
 * The layer is FAT, so symbolic links ('l') and modes other than the
 * defaults ('m': 0644 files, 0755 folders) are kept in _LXMETA too: one line
 * per name, "type<TAB>mode<TAB>name<TAB>target".
 *
 * ROOT.IMG, version 1 (little-endian):
 *   0   "LXROOT01"
 *   8   u32 entry count, u32 entries offset, u32 names offset, u32 names size
 *   24  u32 flags, u32 build time, then zeros to 64
 *   entries, 32 bytes each: parent, name (offset in names), mode, size,
 *       data (file: offset in ROOT.IMG; link: offset of the target in names),
 *       mtime (seconds), first child, child count
 *   Entry 0 is "/". A folder's children are consecutive, sorted by name. */
#include "lx_core.h"
#include "lx_vfs.h"

typedef struct img_ent {
	lx_u32 parent, name, mode, size, data, mtime, first, count;
} img_ent;

static void *img_h;                     /* ROOT.IMG, open */
static img_ent *ents;
static lx_u32 nents;
static char *names;
static lx_u32 names_size;
static char upper_root[LX_PATH_MAX];    /* the layer's folder */

void lx_root_init(void)
{
	char path[LX_PATH_MAX];
	char hdr[64];
	int err;
	lx_u32 eoff, noff;
	ents = 0; nents = 0; names = 0;
	lx_native_join(upper_root, sizeof upper_root, lx_k.root_native, "root");
	lx_native_join(path, sizeof path, lx_k.root_native, "ROOT.IMG");
	img_h = bk_file_open(path, LX_O_RDONLY, &err);
	if (!img_h) { bk_log("no root image %s (%d): the root is the layer only", path, err); return; }
	if (bk_file_read(img_h, 0, hdr, 64) != 64 || lx_strncmp(hdr, "LXROOT01", 8) != 0) {
		bk_log("%s is not a PsiLinux root image", path);
		bk_file_close(img_h); img_h = 0; return;
	}
	bk_memcpy(&nents, hdr + 8, 4);
	bk_memcpy(&eoff, hdr + 12, 4);
	bk_memcpy(&noff, hdr + 16, 4);
	bk_memcpy(&names_size, hdr + 20, 4);
	ents = (img_ent *)bk_alloc(nents * sizeof(img_ent));
	names = (char *)bk_alloc(names_size + 1);
	if (!ents || !names ||
	    bk_file_read(img_h, eoff, (char *)ents, nents * sizeof(img_ent)) != (int)(nents * sizeof(img_ent)) ||
	    bk_file_read(img_h, noff, names, names_size) != (int)names_size) {
		bk_log("cannot read the root image");
		bk_free(ents); bk_free(names); ents = 0; nents = 0;
		bk_file_close(img_h); img_h = 0; return;
	}
	names[names_size] = 0;
}

int lx_root_info(lx_u32 *entries, lx_u32 *bytes)
{
	*entries = nents;
	*bytes = nents * sizeof(img_ent) + names_size;
	return img_h ? 0 : -1;
}

/* the image's child of folder e called name (n bytes), or -1 */
static int img_child(int e, const char *name, int n)
{
	int lo, hi;
	if (e < 0 || !LX_S_ISDIR(ents[e].mode)) return -1;
	lo = ents[e].first;
	hi = lo + ents[e].count - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2, c;
		const char *s = names + ents[mid].name;
		c = lx_strncmp(s, name, n);
		if (c == 0 && s[n]) c = 1;
		if (c == 0) return mid;
		if (c < 0) lo = mid + 1; else hi = mid - 1;
	}
	return -1;
}

/* ---- _LXMETA, cached ---------------------------------------------------------- */

typedef struct meta_ent { char type; lx_u32 mode; char *name; char *target; } meta_ent;
typedef struct meta {
	char dir[LX_PATH_MAX];       /* the layer's folder (native) */
	int exists;                  /* the folder is there */
	int opaque;
	int n, cap;
	meta_ent *e;
	lx_u32 used;                 /* for LRU */
} meta;

#define NMETA 16
static meta mcache[NMETA];
static lx_u32 mclock;

static void meta_clear(meta *m)
{
	int i;
	for (i = 0; i < m->n; i++) { bk_free(m->e[i].name); if (m->e[i].target) bk_free(m->e[i].target); }
	bk_free(m->e);
	m->e = 0; m->n = m->cap = 0; m->opaque = 0;
}

static void meta_add(meta *m, char type, lx_u32 mode, const char *name, const char *target)
{
	if (m->n == m->cap) {
		int nc = m->cap ? m->cap * 2 : 8;
		meta_ent *ne = (meta_ent *)bk_alloc(nc * sizeof *ne);
		if (!ne) return;
		if (m->e) { bk_memcpy(ne, m->e, m->n * sizeof *ne); bk_free(m->e); }
		m->e = ne;
		m->cap = nc;
	}
	m->e[m->n].type = type;
	m->e[m->n].mode = mode;
	m->e[m->n].name = lx_strdup(name);
	m->e[m->n].target = target ? lx_strdup(target) : 0;
	m->n++;
}

static void meta_parse(meta *m, char *buf, int len)
{
	int i = 0;
	while (i < len) {
		char *f[4];
		int nf = 0, s = i;
		while (i < len && buf[i] != '\n') i++;
		buf[i] = 0;
		f[nf++] = buf + s;
		{
			char *q;
			for (q = buf + s; *q && nf < 4; q++) if (*q == '\t') { *q = 0; f[nf++] = q + 1; }
		}
		i++;
		if (nf < 3) continue;
		{
			lx_u32 mode = 0;
			const char *o = f[1];
			while (*o >= '0' && *o <= '7') mode = mode * 8 + (*o++ - '0');
			if (f[0][0] == 'o') m->opaque = 1;
			else meta_add(m, f[0][0], mode, f[2], nf > 3 ? f[3] : 0);
		}
	}
}

static meta *meta_get(const char *dir)
{
	int i, victim = 0;
	meta *m;
	char path[LX_PATH_MAX];
	bk_stat st;
	void *h;
	int err;
	for (i = 0; i < NMETA; i++)
		if (mcache[i].dir[0] && lx_strcmp(mcache[i].dir, dir) == 0) { mcache[i].used = ++mclock; return &mcache[i]; }
	for (i = 1; i < NMETA; i++) if (mcache[i].used < mcache[victim].used) victim = i;
	m = &mcache[victim];
	meta_clear(m);
	lx_strlcpy(m->dir, dir, sizeof m->dir);
	m->used = ++mclock;
	m->exists = bk_stat_path(dir, &st) == 0 && st.is_dir;
	if (!m->exists) return m;
	lx_snprintf(path, sizeof path, "%s%c%s", dir, bk_native_sep(), LX_META_NAME);
	h = bk_file_open(path, LX_O_RDONLY, &err);
	if (h) {
		lx_s64 size = 0;
		bk_file_size(h, &size);
		if (size > 0 && size < 256 * 1024) {
			char *buf = (char *)bk_alloc((lx_u32)size + 1);
			if (buf) {
				int r = bk_file_read(h, 0, buf, (lx_u32)size);
				if (r > 0) meta_parse(m, buf, r);
				bk_free(buf);
			}
		}
		bk_file_close(h);
	}
	return m;
}

static void meta_forget(const char *dir)
{
	int i;
	for (i = 0; i < NMETA; i++)
		if (mcache[i].dir[0] && lx_strcmp(mcache[i].dir, dir) == 0) { meta_clear(&mcache[i]); mcache[i].dir[0] = 0; mcache[i].used = 0; }
}

/* forget every folder under (and at) dir: after a rename or rmdir */
static void meta_forget_tree(const char *dir)
{
	int i, n = lx_strlen(dir);
	for (i = 0; i < NMETA; i++)
		if (mcache[i].dir[0] && lx_strncmp(mcache[i].dir, dir, n) == 0) { meta_clear(&mcache[i]); mcache[i].dir[0] = 0; mcache[i].used = 0; }
}

static int meta_save(meta *m)
{
	char path[LX_PATH_MAX];
	void *h;
	int err, i, o = 0, cap = 64;
	char *buf;
	lx_snprintf(path, sizeof path, "%s%c%s", m->dir, bk_native_sep(), LX_META_NAME);
	if (!m->n && !m->opaque) { bk_unlink(path); return 0; }
	for (i = 0; i < m->n; i++) cap += 24 + lx_strlen(m->e[i].name) + (m->e[i].target ? lx_strlen(m->e[i].target) : 0);
	buf = (char *)bk_alloc(cap);
	if (!buf) return -LX_ENOMEM;
	if (m->opaque) o += lx_snprintf(buf + o, cap - o, "o\t0\t.\n");
	for (i = 0; i < m->n; i++)
		o += lx_snprintf(buf + o, cap - o, "%c\t%o\t%s%s%s\n", m->e[i].type, m->e[i].mode, m->e[i].name,
			m->e[i].target ? "\t" : "", m->e[i].target ? m->e[i].target : "");
	h = bk_file_open(path, LX_O_WRONLY | LX_O_CREAT | LX_O_TRUNC, &err);
	if (!h) { bk_free(buf); return err; }
	bk_file_truncate(h, 0);
	err = bk_file_write(h, 0, buf, o);
	bk_file_close(h);
	bk_free(buf);
	return err < 0 ? err : 0;
}

static meta_ent *meta_find(meta *m, const char *name)
{
	int i;
	for (i = 0; i < m->n; i++) if (lx_strcmp(m->e[i].name, name) == 0) return &m->e[i];
	return 0;
}

static void meta_del(meta *m, const char *name)
{
	int i;
	for (i = 0; i < m->n; i++)
		if (lx_strcmp(m->e[i].name, name) == 0) {
			bk_free(m->e[i].name);
			if (m->e[i].target) bk_free(m->e[i].target);
			m->e[i] = m->e[--m->n];
			return;
		}
}

/* ---- lookups -------------------------------------------------------------- */

/* the layer's folder for a relative Linux folder ("" = the layer's root) */
static void upper_dir(const char *reldir, char *out)
{
	lx_native_join(out, LX_PATH_MAX, upper_root, reldir);
}

static void split(const char *rel, char *dir, const char **name)
{
	int i = lx_strlen(rel);
	while (i > 0 && rel[i - 1] != '/') i--;
	*name = rel + i;
	if (i <= 1) dir[0] = 0;
	else { bk_memcpy(dir, rel, i - 1); dir[i - 1] = 0; }
}

/* Walks rel through the image, honouring the layer's whiteouts and opaque
 * folders on the way: the image's entry it names, or -1. */
static int img_visible(const char *rel)
{
	int e = 0;
	char dir[LX_PATH_MAX];
	int dl = 0;
	if (!nents) return -1;
	dir[0] = 0;
	for (;;) {
		const char *s;
		int n;
		meta *m;
		while (*rel == '/') rel++;
		if (!*rel) return e;
		s = rel;
		n = 0;
		while (s[n] && s[n] != '/') n++;
		/* the layer may hide it */
		{
			char ud[LX_PATH_MAX], nm[256];
			upper_dir(dir, ud);
			m = meta_get(ud);
			if (m->exists) {
				if (m->opaque && dl > 0) return -1;
				if (n > 255) return -1;
				bk_memcpy(nm, s, n);
				nm[n] = 0;
				{
					meta_ent *me = meta_find(m, nm);
					if (me && (me->type == 'w' || me->type == 'l')) return -1;
				}
			}
		}
		e = img_child(e, s, n);
		if (e < 0) return -1;
		if (dl + 1 + n >= LX_PATH_MAX) return -1;
		dir[dl++] = '/';
		bk_memcpy(dir + dl, s, n);
		dl += n;
		dir[dl] = 0;
		rel = s + n;
	}
}

/* is the layer's folder for reldir (and every one above it) there? */
static int upper_exists(const char *reldir)
{
	char ud[LX_PATH_MAX];
	upper_dir(reldir, ud);
	return meta_get(ud)->exists;
}

int lx_root_lookup(const char *upper_native, const char *rel, lx_vn *v)
{
	char dir[LX_PATH_MAX], ud[LX_PATH_MAX];
	const char *name;
	meta *m;
	meta_ent *me = 0;
	int e;
	(void)upper_native;
	v->img = -1;
	if (!rel[0] || (rel[0] == '/' && !rel[1])) {
		v->mode = LX_S_IFDIR | 0755;
		v->img = nents ? 0 : -1;
		v->upper = 1;
		upper_dir("", v->native);
		v->mtime = lx_k.boot_time;
		return 0;
	}
	split(rel, dir, &name);
	upper_dir(dir, ud);
	m = meta_get(ud);
	if (m->exists) me = meta_find(m, name);
	if (me && me->type == 'l') {
		v->mode = LX_S_IFLNK | 0777;
		v->mtime = lx_k.boot_time;
		lx_strlcpy(v->link, me->target ? me->target : "", sizeof v->link);
		v->upper = 1;
		return 0;
	}
	/* the layer */
	if (m->exists) {
		bk_stat st;
		lx_native_join(v->native, sizeof v->native, upper_root, rel);
		if (bk_stat_path(v->native, &st) == 0) {
			v->upper = 1;
			v->mode = st.is_dir ? (LX_S_IFDIR | 0755) : (LX_S_IFREG | 0644);
			v->size = st.size;
			v->mtime = st.mtime;
			if (me && me->type == 'm') v->mode = (v->mode & LX_S_IFMT) | (me->mode & 07777);
			else {
				/* (a file of the image copied up keeps its mode) */
				e = img_visible(rel);
				if (e >= 0 && (ents[e].mode & LX_S_IFMT) == (v->mode & LX_S_IFMT)) v->mode = ents[e].mode;
			}
			e = img_visible(rel);
			v->img = e;
			return 0;
		}
	}
	if (me && me->type == 'w') return -LX_ENOENT;
	e = img_visible(rel);
	if (e < 0) return -LX_ENOENT;
	v->img = e;
	v->mode = ents[e].mode;
	if (me && me->type == 'm') v->mode = (v->mode & LX_S_IFMT) | (me->mode & 07777);
	v->size = LX_S_ISREG(ents[e].mode) ? ents[e].size : 0;
	v->mtime = (lx_s64)ents[e].mtime * 1000000;
	v->ino = e + 1;
	if (LX_S_ISLNK(ents[e].mode)) lx_strlcpy(v->link, names + ents[e].data, sizeof v->link);
	lx_native_join(v->native, sizeof v->native, upper_root, rel);
	return 0;
}

static const char *rel_of(const char *path)
{
	return path[0] == '/' && path[1] == 0 ? "" : path;
}

void lx_root_list(lx_vn *v, void *dl)
{
	const char *rel = rel_of(v->path);
	char ud[LX_PATH_MAX];
	meta *m;
	int i;
	upper_dir(rel, ud);
	m = meta_get(ud);
	if (m->exists) {
		for (i = 0; i < m->n; i++)
			if (m->e[i].type == 'l') {
				char full[LX_PATH_MAX];
				lx_snprintf(full, sizeof full, "%s/%s", rel, m->e[i].name);
				lx_dlist_add(dl, m->e[i].name, LX_DT_LNK, 0);
			}
		lx_list_native(dl, ud, rel);
	}
	if (v->img >= 0 && !(m->exists && m->opaque && rel[0])) {
		img_ent *d = &ents[v->img];
		for (i = 0; i < (int)d->count; i++) {
			img_ent *c = &ents[d->first + i];
			const char *nm = names + c->name;
			int t = LX_S_ISDIR(c->mode) ? LX_DT_DIR : LX_S_ISLNK(c->mode) ? LX_DT_LNK : LX_DT_REG;
			if (m->exists) {
				meta_ent *me = meta_find(m, nm);
				if (me && me->type == 'w') continue;
			}
			lx_dlist_add(dl, nm, t, d->first + i + 1);
		}
	}
}

/* ---- the image's files -------------------------------------------------------- */

typedef struct rfile { lx_u32 data, size; } rfile;

static int rf_pread(lx_thread *t, lx_file *f, char *buf, lx_u32 n, lx_s64 off)
{
	rfile *r = (rfile *)f->priv;
	(void)t;
	if (off >= r->size) return 0;
	if (off + n > r->size) n = (lx_u32)(r->size - off);
	return bk_file_read(img_h, r->data + off, buf, n);
}

static int rf_read(lx_thread *t, lx_file *f, char *buf, lx_u32 n)
{
	int r = rf_pread(t, f, buf, n, f->pos);
	if (r > 0) f->pos += r;
	return r;
}

static lx_s64 rf_lseek(lx_file *f, lx_s64 off, int whence)
{
	rfile *r = (rfile *)f->priv;
	lx_s64 base = whence == LX_SEEK_CUR ? f->pos : whence == LX_SEEK_END ? r->size : 0;
	if (whence > 2 || base + off < 0) return -LX_EINVAL;
	f->pos = base + off;
	return f->pos;
}

static int rf_fstat(lx_file *f, struct lx_stat64 *st)
{
	lx_vn v;
	rfile *r = (rfile *)f->priv;
	bk_memset(&v, 0, sizeof v);
	v.fs = LX_FS_ROOT;
	v.mode = f->mode;
	v.size = r->size;
	v.ino = f->ino;
	v.mtime = lx_k.boot_time;
	lx_vn_stat(&v, st);
	return 0;
}

static int rf_write(lx_thread *t, lx_file *f, const char *b, lx_u32 n) { (void)t; (void)f; (void)b; (void)n; return -LX_EBADF; }
static int rf_poll(lx_file *f) { (void)f; return LX_POLLIN | LX_POLLOUT; }
static void rf_close(lx_file *f) { bk_free(f->priv); }

static const lx_fops rfile_ops = { rf_read, rf_write, rf_pread, 0, rf_lseek, rf_fstat, 0, rf_poll, 0, rf_close, 0, 0 };

/* the layer's folders down to reldir, made if they are not there */
static int make_upper_dirs(const char *reldir)
{
	char part[LX_PATH_MAX], ud[LX_PATH_MAX];
	int i = 0, n = lx_strlen(reldir);
	while (i <= n) {
		if (i == n || (reldir[i] == '/' && i > 0)) {
			bk_memcpy(part, reldir, i);
			part[i] = 0;
			upper_dir(part, ud);
			if (!meta_get(ud)->exists) {
				int r = bk_mkdir(ud);
				meta_forget(ud);
				if (r < 0 && !meta_get(ud)->exists) return r;
			}
		}
		i++;
	}
	return 0;
}

static int copy_up(lx_vn *v)
{
	char dir[LX_PATH_MAX];
	const char *name;
	const char *rel = rel_of(v->path);
	void *h;
	int err, r = 0;
	lx_u32 off = 0;
	char *buf;
	img_ent *e;
	if (v->upper || v->img < 0) return 0;
	e = &ents[v->img];
	split(rel, dir, &name);
	r = make_upper_dirs(dir);
	if (r < 0) return r;
	if (LX_S_ISDIR(e->mode)) return bk_mkdir(v->native);
	h = bk_file_open(v->native, LX_O_WRONLY | LX_O_CREAT | LX_O_TRUNC, &err);
	if (!h) return err;
	buf = (char *)bk_alloc(4096);
	if (!buf) { bk_file_close(h); return -LX_ENOMEM; }
	while (off < e->size) {
		lx_u32 n = e->size - off > 4096 ? 4096 : e->size - off;
		r = bk_file_read(img_h, e->data + off, buf, n);
		if (r <= 0) { r = -LX_EIO; break; }
		r = bk_file_write(h, off, buf, r);
		if (r <= 0) { r = -LX_ENOSPC; break; }
		off += r;
		r = 0;
	}
	bk_free(buf);
	bk_file_close(h);
	{
		char ud[LX_PATH_MAX];
		upper_dir(dir, ud);
		meta_forget(ud);
	}
	v->upper = 1;
	return r;
}

/* sets (or clears, for the default) the mode note of name in its folder */
static int set_mode_note(const char *rel, lx_u32 mode, int isdir)
{
	char dir[LX_PATH_MAX], ud[LX_PATH_MAX];
	const char *name;
	meta *m;
	lx_u32 dflt = isdir ? 0755 : 0644;
	split(rel, dir, &name);
	upper_dir(dir, ud);
	m = meta_get(ud);
	if (!m->exists) {
		int r = make_upper_dirs(dir);
		if (r < 0) return r;
		meta_forget(ud);
		m = meta_get(ud);
	}
	meta_del(m, name);
	if ((mode & 07777) != dflt) meta_add(m, 'm', mode & 07777, name, 0);
	return meta_save(m);
}

lx_file *lx_root_open(lx_proc *p, lx_vn *v, int exists, int flags, lx_u32 mode, int *err)
{
	int acc = flags & LX_O_ACCMODE;
	const char *rel = rel_of(v->path);
	(void)p;
	if (exists && !v->upper && v->img >= 0 && acc == LX_O_RDONLY && !(flags & LX_O_TRUNC)) {
		rfile *r = (rfile *)bk_alloc(sizeof *r);
		lx_file *f;
		if (!r) { *err = -LX_ENOMEM; return 0; }
		r->data = ents[v->img].data;
		r->size = ents[v->img].size;
		f = lx_file_new(&rfile_ops, r, flags);
		if (!f) { bk_free(r); *err = -LX_ENOMEM; return 0; }
		f->mode = v->mode;
		f->ino = v->ino;
		return f;
	}
	if (exists && !v->upper) {
		/* writing a file of the image: copy it up */
		int r;
		if (flags & LX_O_TRUNC) {
			char dir[LX_PATH_MAX];
			const char *name;
			split(rel, dir, &name);
			r = make_upper_dirs(dir);
			if (r >= 0) { void *h = bk_file_open(v->native, LX_O_WRONLY | LX_O_CREAT | LX_O_TRUNC, &r); if (h) { bk_file_close(h); r = 0; } }
			{ char ud[LX_PATH_MAX]; upper_dir(dir, ud); meta_forget(ud); }
		} else r = copy_up(v);
		if (r < 0) { *err = r; return 0; }
	}
	if (!exists) {
		char dir[LX_PATH_MAX], ud[LX_PATH_MAX];
		const char *name;
		meta *m;
		int r;
		lx_native_join(v->native, sizeof v->native, upper_root, rel);
		split(rel, dir, &name);
		if (!upper_exists(dir)) {
			/* the folder must be there (in the image or the layer) */
			lx_vn pv;
			char pp[LX_PATH_MAX];
			lx_strlcpy(pp, dir[0] ? dir : "/", sizeof pp);
			if (lx_vn_lookup(p, pp, &pv) < 0 || !LX_S_ISDIR(pv.mode)) { *err = -LX_ENOENT; return 0; }
			r = make_upper_dirs(dir);
			if (r < 0) { *err = r; return 0; }
		}
		upper_dir(dir, ud);
		m = meta_get(ud);
		if (meta_find(m, name)) { meta_del(m, name); meta_save(m); }
		v->mode = LX_S_IFREG | (mode & 07777);
		if ((mode & 07777) != 0644) set_mode_note(rel, mode, 0);
	}
	{
		/* the layer's file */
		void *h = bk_file_open(v->native, flags, err);
		lx_file *f;
		extern lx_file *lx_native_file(void *h, const char *native, lx_u32 mode, lx_u64 ino, int flags);
		if (!h) return 0;
		f = lx_native_file(h, v->native, v->mode ? v->mode : (LX_S_IFREG | 0644), v->ino, flags);
		if (!f) { bk_file_close(h); *err = -LX_ENOMEM; }
		return f;
	}
}

int lx_root_mkdir(lx_proc *p, const char *path, lx_u32 mode)
{
	char dir[LX_PATH_MAX], ud[LX_PATH_MAX], native[LX_PATH_MAX];
	const char *name;
	const char *rel = rel_of(path);
	meta *m;
	int r, was_whiteout = 0;
	lx_vn pv;
	split(rel, dir, &name);
	lx_strlcpy(native, dir[0] ? dir : "/", sizeof native);
	if (lx_vn_lookup(p, native, &pv) < 0 || !LX_S_ISDIR(pv.mode)) return -LX_ENOENT;
	r = make_upper_dirs(dir);
	if (r < 0) return r;
	upper_dir(dir, ud);
	m = meta_get(ud);
	{
		meta_ent *me = meta_find(m, name);
		if (me && me->type == 'w') { was_whiteout = 1; meta_del(m, name); meta_save(m); }
	}
	lx_native_join(native, sizeof native, upper_root, rel);
	r = bk_mkdir(native);
	if (r < 0) return r;
	meta_forget(native);
	if (was_whiteout) {
		/* the image's folder of that name stays hidden */
		meta *nm = meta_get(native);
		nm->opaque = 1;
		meta_save(nm);
	}
	if ((mode & 07777) != 0755) set_mode_note(rel, mode, 1);
	return 0;
}

static int add_whiteout(const char *rel)
{
	char dir[LX_PATH_MAX], ud[LX_PATH_MAX];
	const char *name;
	meta *m;
	int r;
	split(rel, dir, &name);
	r = make_upper_dirs(dir);
	if (r < 0) return r;
	upper_dir(dir, ud);
	meta_forget(ud);
	m = meta_get(ud);
	meta_del(m, name);
	meta_add(m, 'w', 0, name, 0);
	return meta_save(m);
}

int lx_root_unlink(lx_proc *p, lx_vn *v, int dir)
{
	const char *rel = rel_of(v->path);
	char pdir[LX_PATH_MAX], ud[LX_PATH_MAX];
	const char *name;
	meta *m;
	int r;
	(void)p;
	split(rel, pdir, &name);
	upper_dir(pdir, ud);
	if (dir) {
		/* empty? (in the union) */
		lx_vn vv = *v;
		struct { char *buf; int len, cap, count; } dl;
		bk_memset(&dl, 0, sizeof dl);
		lx_root_list(&vv, &dl);
		if (dl.buf) bk_free(dl.buf);
		if (dl.count > 0) return -LX_ENOTEMPTY;
	}
	m = meta_get(ud);
	if (LX_S_ISLNK(v->mode) && m->exists && meta_find(m, name)) {
		meta_del(m, name);
		r = meta_save(m);
		if (r < 0) return r;
		if (v->img >= 0) return add_whiteout(rel);
		return 0;
	}
	if (v->upper) {
		if (dir) {
			char mp[LX_PATH_MAX];
			lx_snprintf(mp, sizeof mp, "%s%c%s", v->native, bk_native_sep(), LX_META_NAME);
			bk_unlink(mp);
			meta_forget_tree(v->native);
			r = bk_rmdir(v->native);
		} else r = bk_unlink(v->native);
		if (r < 0) return r;
		if (m->exists && meta_find(m, name)) { meta_del(m, name); meta_save(m); }
	}
	if (v->img >= 0) return add_whiteout(rel);
	return 0;
}

int lx_root_rename(lx_proc *p, lx_vn *from, lx_vn *to, int to_exists)
{
	const char *rf = rel_of(from->path), *rt = rel_of(to->path);
	char fdir[LX_PATH_MAX], tdir[LX_PATH_MAX], fud[LX_PATH_MAX], tud[LX_PATH_MAX];
	const char *fname, *tname;
	meta *fm, *tm;
	meta_ent *fe;
	lx_u32 fmode = 0;
	char *ftarget = 0;
	int r, fnote = 0;
	split(rf, fdir, &fname);
	split(rt, tdir, &tname);
	if (LX_S_ISDIR(from->mode) && (from->img >= 0 || (to_exists && to->img >= 0))) return -LX_EXDEV;   /* mv copies it */
	r = make_upper_dirs(tdir);
	if (r < 0) return r;
	upper_dir(fdir, fud);
	upper_dir(tdir, tud);
	/* what the old name's note said */
	fm = meta_get(fud);
	fe = fm->exists ? meta_find(fm, fname) : 0;
	if (fe && (fe->type == 'l' || fe->type == 'm')) {
		fnote = fe->type;
		fmode = fe->mode;
		if (fe->target) ftarget = lx_strdup(fe->target);
	} else if (from->img >= 0 && !LX_S_ISDIR(from->mode)) {
		fnote = 'm';
		fmode = ents[from->img].mode & 07777;
		if (LX_S_ISLNK(ents[from->img].mode)) { fnote = 'l'; ftarget = lx_strdup(names + ents[from->img].data); }
	}
	/* the target goes */
	if (to_exists) {
		r = lx_root_unlink(p, to, LX_S_ISDIR(to->mode));
		if (r < 0 && r != -LX_ENOTEMPTY) { if (ftarget) bk_free(ftarget); return r; }
		if (r == -LX_ENOTEMPTY) { if (ftarget) bk_free(ftarget); return r; }
	}
	/* the data moves (or is copied up to the new name) */
	if (fnote != 'l') {
		if (from->upper) {
			lx_native_join(to->native, sizeof to->native, upper_root, rt);
			r = bk_rename(from->native, to->native);
			meta_forget_tree(from->native);
		} else {
			lx_vn tmp = *from;
			lx_native_join(tmp.native, sizeof tmp.native, upper_root, rt);
			bk_memcpy(tmp.path, to->path, sizeof tmp.path);
			r = copy_up(&tmp);
		}
		if (r < 0) { if (ftarget) bk_free(ftarget); return r; }
	}
	/* the old name's notes and whiteout */
	meta_forget(fud);
	fm = meta_get(fud);
	if (fm->exists) { meta_del(fm, fname); meta_save(fm); }
	if (from->img >= 0) add_whiteout(rf);
	/* the new name's notes */
	meta_forget(tud);
	tm = meta_get(tud);
	meta_del(tm, tname);
	if (fnote == 'l') meta_add(tm, 'l', 0777, tname, ftarget ? ftarget : "");
	else if (fnote == 'm' && fmode != (LX_S_ISDIR(from->mode) ? 0755u : 0644u)) meta_add(tm, 'm', fmode, tname, 0);
	r = meta_save(tm);
	if (ftarget) bk_free(ftarget);
	return r;
}

int lx_root_symlink(lx_proc *p, const char *path, const char *target)
{
	char dir[LX_PATH_MAX], ud[LX_PATH_MAX];
	const char *name;
	const char *rel = rel_of(path);
	meta *m;
	lx_vn pv;
	int r;
	split(rel, dir, &name);
	lx_strlcpy(ud, dir[0] ? dir : "/", sizeof ud);
	if (lx_vn_lookup(p, ud, &pv) < 0 || !LX_S_ISDIR(pv.mode)) return -LX_ENOENT;
	r = make_upper_dirs(dir);
	if (r < 0) return r;
	upper_dir(dir, ud);
	meta_forget(ud);
	m = meta_get(ud);
	meta_del(m, name);
	meta_add(m, 'l', 0777, name, target);
	return meta_save(m);
}

int lx_root_chmod(lx_proc *p, lx_vn *v, lx_u32 mode)
{
	(void)p;
	if (LX_S_ISLNK(v->mode)) return 0;
	return set_mode_note(rel_of(v->path), mode, LX_S_ISDIR(v->mode));
}

int lx_root_utime(lx_proc *p, lx_vn *v, lx_s64 mtime)
{
	int r;
	(void)p;
	if (!v->upper) {
		if (LX_S_ISDIR(v->mode)) return 0;
		r = copy_up(v);
		if (r < 0) return r;
	}
	return bk_set_mtime(v->native, mtime);
}
