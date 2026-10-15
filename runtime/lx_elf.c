/* lx_elf.c - loads a program for execve: a static-pie ELF for ARM EABI
 * (built with the PsiLinux toolchain), or a "#!" script.
 *
 * The image is loaded whole into a region of its own at any address; the
 * program relocates itself (musl's rcrt1, _dlstart_c) using AT_PHDR, so the
 * loader does no relocation. See linux/docs/DESIGN.md for why. */
#include "lx_core.h"
#include "lx_dbt.h"

#define EI_NIDENT 16
typedef struct {
	lx_u8 ident[EI_NIDENT];
	lx_u16 type, machine;
	lx_u32 version, entry, phoff, shoff, flags;
	lx_u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_ehdr;
typedef struct {
	lx_u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
} elf_phdr;
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_GNU_STACK 0x6474e551
#define PF_X 1
#define ET_EXEC 2
#define ET_DYN 3
#define EM_ARM 40
#define EF_ARM_VFP_FLOAT 0x400

/* ---- the program cache --------------------------------------------------
 * Every exec loads the whole program, and BusyBox is 1.3 MB: from the card
 * that is the slow part of starting any command. The bytes of the last few
 * programs run are kept here, so running one again is a copy in memory.
 * Keyed by the program's path, size and time; at most LX_EXEC_CACHE bytes,
 * and emptied when the Psion runs short of memory. */
#define LX_EXEC_CACHE (2 * 1024 * 1024)
#define LX_EXEC_SLOTS 4
typedef struct xcache { char path[LX_PATH_MAX]; lx_s64 size; lx_u64 ino; char *data; lx_u32 used; lx_region reg; } xcache;
static xcache cache[LX_EXEC_SLOTS];
static lx_u32 cache_clock;

static lx_u32 cache_total(void)
{
	lx_u32 n = 0;
	int i;
	for (i = 0; i < LX_EXEC_SLOTS; i++) if (cache[i].data) n += (lx_u32)cache[i].size;
	return n;
}

/* (each cached program is a chunk of its own, not the kernel's heap: F32
 * reading a whole program into one big heap cell took the emulated 5mx's
 * kernel down, and a chunk goes back to EPOC whole when it is dropped) */
static void slot_free(xcache *c)
{
	if (!c->data) return;
	bk_region_free(&c->reg);
	c->data = 0;
}

static const char *cur_cache;
static lx_s64 cur_cache_size;

/* memory is short: every cached program goes, except one being loaded now */
void lx_exec_cache_drop(void)
{
	int i;
	for (i = 0; i < LX_EXEC_SLOTS; i++)
		if (cache[i].data && cache[i].data != cur_cache) slot_free(&cache[i]);
}

/* (while a program loads, its images are registered before any process
 * uses them: lx_mem_reclaim must not take them, so it leaves the shared
 * images alone until the load is over) */
static int exec_loading;

/* memory is needed: the program cache goes, and the shared images that no
 * process uses (with their translations) */
void lx_mem_reclaim(void)
{
	lx_exec_cache_drop();
	if (!exec_loading) lx_simg_trim(1);
}

/* the program cache's size (memory that can be had back) */
lx_u32 lx_exec_cache_bytes(void) { return cache_total(); }

/* the cached bytes of the program f (opened as path), or 0 */
static const char *cache_get(lx_thread *t, lx_file *f, const char *path)
{
	struct lx_stat64 st;
	int i, victim = 0;
	char *d;
	lx_s64 done = 0;
	if (!f->ops->fstat || f->ops->fstat(f, &st) < 0) return 0;
	for (i = 0; i < LX_EXEC_SLOTS; i++)
		if (cache[i].data && cache[i].size == st.st_size && cache[i].ino == st.st_ino && lx_strcmp(cache[i].path, path) == 0) {
			cache[i].used = ++cache_clock;
			return cache[i].data;
		}
	if (st.st_size <= 0 || st.st_size > LX_EXEC_CACHE / 2) return 0;
	if (lx_k.no_cache) return 0;
	if (lx_mem_avail() < 3 * 1024 * 1024) { lx_exec_cache_drop(); return 0; }     /* (memory is short) */
	while (cache_total() + st.st_size > LX_EXEC_CACHE) {
		int old = -1;
		for (i = 0; i < LX_EXEC_SLOTS; i++) if (cache[i].data && (old < 0 || cache[i].used < cache[old].used)) old = i;
		if (old < 0) break;
		slot_free(&cache[old]);
	}
	for (i = 0; i < LX_EXEC_SLOTS; i++) {
		if (!cache[i].data) { victim = i; break; }
		if (cache[i].used < cache[victim].used) victim = i;
	}
	slot_free(&cache[victim]);
	if (lx_k.trace) bk_log("cache: %s, %u bytes", path, (lx_u32)st.st_size);
	if (bk_region_new(&cache[victim].reg, (lx_u32)st.st_size, LX_REG_IMAGE) < 0) return 0;   /* (within the reserve: above) */
	d = (char *)cache[victim].reg.base;
	while (done < st.st_size) {
		lx_u32 piece = st.st_size - done > 32768 ? 32768 : (lx_u32)(st.st_size - done);
		int r = f->ops->pread(t, f, d + done, piece, done);
		if (r <= 0) { bk_region_free(&cache[victim].reg); return 0; }
		done += r;
	}
	lx_strlcpy(cache[victim].path, path, LX_PATH_MAX);
	cache[victim].size = st.st_size;
	cache[victim].ino = st.st_ino;
	cache[victim].data = d;
	cache[victim].used = ++cache_clock;
	return d;
}

static int rd(lx_thread *t, lx_file *f, void *buf, lx_u32 n, lx_s64 off)
{
	if (cur_cache) {
		if (off >= cur_cache_size) return 0;
		if (off + n > cur_cache_size) n = (lx_u32)(cur_cache_size - off);
		bk_memcpy(buf, cur_cache + off, n);
		return (int)n;
	}
	{
	lx_u32 done = 0;
	while (done < n) {
		/* (in pieces of 32 KB: one big read of a whole program held the
		 * file server, and wedged the emulator's card) */
		lx_u32 piece = n - done > 32768 ? 32768 : n - done;
		int r = f->ops->pread(t, f, (char *)buf + done, piece, off + done);
		if (r < 0) return r;
		if (r == 0) break;
		done += r;
	}
	return (int)done;
	}
}

/* Has the image the PsiLinux note (it was built with our musl)? */
static int has_note(lx_u32 base, const elf_phdr *ph, int phnum)
{
	int i;
	for (i = 0; i < phnum; i++) {
		lx_u32 p, end;
		if (ph[i].type != PT_NOTE) continue;
		p = base + ph[i].vaddr;
		end = p + ph[i].memsz;
		while (p + 12 <= end) {
			lx_u32 namesz = ((lx_u32 *)p)[0], descsz = ((lx_u32 *)p)[1], type = ((lx_u32 *)p)[2];
			const char *name = (const char *)(p + 12);
			if (namesz == 9 && type == 1 && lx_strncmp(name, "PsiLinux", 9) == 0) return 1;
			p += 12 + ((namesz + 3) & ~3u) + ((descsz + 3) & ~3u);
		}
	}
	return 0;
}

/* How many svc (swi) instructions are in its executable segments? EPOC
 * must never run one: svc 0 is an executive call there, and restarts a 5mx. */
static int count_svc(lx_u32 base, const elf_phdr *ph, int phnum)
{
	int i, n = 0;
	for (i = 0; i < phnum; i++) {
		lx_u32 *w, *end;
		if (ph[i].type != PT_LOAD || !(ph[i].flags & PF_X)) continue;
		w = (lx_u32 *)(base + ph[i].vaddr);
		end = (lx_u32 *)(base + ph[i].vaddr + (ph[i].filesz & ~3u));
		for (; w < end; w++)
			if ((*w & 0x0f000000) == 0x0f000000 && (*w >> 28) != 0xf) n++;
	}
	return n;
}

/* the file's identity, for the shared images (lx_simg.c) */
static int file_id(lx_file *f, lx_s64 *size, lx_u64 *ino, lx_s64 *mtime)
{
	struct lx_stat64 st;
	if (!f->ops->fstat || f->ops->fstat(f, &st) < 0) return -1;
	*size = st.st_size;
	*ino = st.st_ino;
	*mtime = (lx_s64)st.st_mtime_ * 1000000 + st.st_mtime_nsec / 1000;
	return 0;
}

/* an image's writable part, to the end of its page (musl's dynamic linker
 * gives the rest of the page to malloc: data too) */
static void rw_range(lx_u32 base, lx_u32 size, const elf_phdr *ph, int phnum, lx_u32 *lo, lx_u32 *hi)
{
	int i;
	*lo = *hi = 0;
	for (i = 0; i < phnum; i++)
		if (ph[i].type == PT_LOAD && (ph[i].flags & 2)) {
			lx_u32 a = base + ph[i].vaddr, b = a + ph[i].memsz;
			if (!*lo || a < *lo) *lo = a & ~3u;
			b = (b + 4095) & ~4095u;
			if (b > base + size) b = base + size;
			if (b > *hi) *hi = b;
		}
}

static lx_u32 push_str(lx_u32 *sp, const char *s)
{
	int n = lx_strlen(s) + 1;
	*sp -= n;
	bk_memcpy((void *)*sp, s, n);
	return *sp;
}

static int load_elf2(lx_thread *t, lx_proc *p, lx_file *f, const char *path,
	char **argv, int argc, char **envp, int envc, lx_thread **ntp);

/* Alpine mode: the dynamic linker a program names (PT_INTERP), loaded
 * whole into a region of its own, as the kernel does on Linux. It then
 * maps the program's libraries itself, with mmap. */
static int load_interp(lx_thread *t, lx_proc *p, const char *path, lx_region *reg, lx_u32 *entry, lx_u32 *wlo, lx_u32 *whi, lx_simg **sp)
{
	const char *saved = cur_cache;
	lx_s64 saved_size = cur_cache_size;
	elf_ehdr eh;
	elf_phdr ph[16];
	lx_file *f;
	int r, i;
	lx_u32 size = 0;
	struct lx_stat64 st;
	lx_s64 fsize = 0, fmtime = 0;
	lx_u64 fino = 0;
	*sp = 0;
	f = lx_vfs_open(p, LX_AT_FDCWD, path, LX_O_RDONLY, 0, &r);
	if (!f) { bk_log("cannot open the dynamic linker %s (%d)", path, r); return r; }
	if (file_id(f, &fsize, &fino, &fmtime) == 0) {
		lx_simg *s = lx_simg_find(path, fsize, fino, fmtime, 0);
		if (s) {
			/* (shared: loaded already, by this process or another) */
			*reg = s->reg;
			*entry = s->entry;
			*wlo = s->rw_lo;
			*whi = s->rw_hi;
			*sp = s;
			lx_file_put(f);
			return 0;
		}
	}
	cur_cache = cache_get(t, f, path);
	cur_cache_size = cur_cache && f->ops->fstat && f->ops->fstat(f, &st) == 0 ? st.st_size : 0;
	if (!cur_cache_size) cur_cache = 0;
	r = -LX_ENOEXEC;
	if (rd(t, f, &eh, sizeof eh, 0) != (int)sizeof eh || eh.machine != EM_ARM || eh.type != ET_DYN ||
	    eh.phentsize != sizeof(elf_phdr) || eh.phnum > 16) goto out;
	if (rd(t, f, ph, eh.phnum * sizeof(elf_phdr), eh.phoff) != (int)(eh.phnum * sizeof(elf_phdr))) goto out;
	for (i = 0; i < eh.phnum; i++) {
		if (ph[i].type == PT_INTERP) goto out;
		if (ph[i].type == PT_LOAD && ph[i].vaddr + ph[i].memsz > size) size = ph[i].vaddr + ph[i].memsz;
	}
	if (!size || size > 16 * 1024 * 1024) goto out;
	size = (size + 4095) & ~4095u;
	if (lx_region_new(reg, size, LX_REG_IMAGE) < 0) {
		lx_mem_reclaim();
		if (lx_region_new(reg, size, LX_REG_IMAGE) < 0) { r = -LX_ENOMEM; goto out; }
	}
	*wlo = *whi = 0;
	for (i = 0; i < eh.phnum; i++) {
		if (ph[i].type != PT_LOAD) continue;
		if (ph[i].filesz > ph[i].memsz || rd(t, f, (void *)(reg->base + ph[i].vaddr), ph[i].filesz, ph[i].offset) != (int)ph[i].filesz) {
			bk_region_free(reg);
			r = -LX_EIO;
			goto out;
		}
		bk_memset((void *)(reg->base + ph[i].vaddr + ph[i].filesz), 0, ph[i].memsz - ph[i].filesz);
		if (ph[i].flags & 2) {
			lx_u32 lo = reg->base + ph[i].vaddr, hi = lo + ph[i].memsz;
			if (!*wlo || lo < *wlo) *wlo = lo & ~3u;
			if (hi > *whi) *whi = (hi + 4095) & ~4095u;     /* (see load_elf2) */
			if (*whi > reg->base + size) *whi = reg->base + size;
		}
	}
	*entry = eh.entry;
	r = 0;
	rw_range(reg->base, size, ph, eh.phnum, wlo, whi);
	if (lx_k.trace) bk_log("load: %s, %u KB at %x", path, size / 1024, reg->base);
	if (fsize) {
		*sp = lx_simg_add(path, fsize, fino, fmtime, 0, reg, reg->base, size, *wlo, *whi);
		if (*sp) (*sp)->entry = eh.entry;
	}
out:
	lx_file_put(f);
	cur_cache = saved;
	cur_cache_size = saved_size;
	return r;
}

static int load_elf(lx_thread *t, lx_proc *p, lx_file *f, const char *path,
	char **argv, int argc, char **envp, int envc, lx_thread **ntp)
{
	int r;
	{
		struct lx_stat64 st;
		cur_cache = cache_get(t, f, path);
		cur_cache_size = cur_cache && f->ops->fstat && f->ops->fstat(f, &st) == 0 ? st.st_size : 0;
		if (!cur_cache_size) cur_cache = 0;
	}
	r = load_elf2(t, p, f, path, argv, argc, envp, envc, ntp);
	cur_cache = 0;
	return r;
}

static int load_elf2(lx_thread *t, lx_proc *p, lx_file *f, const char *path,
	char **argv, int argc, char **envp, int envc, lx_thread **ntp)
{
	elf_ehdr eh;
	elf_phdr ph[16];
	int i, r, note;
	lx_u32 size = 0, stack_size = LX_DEFAULT_STACK, sp, *v, auxn, *uargv, *uenvp, need;
	lx_region img, stk, ireg;
	lx_mm *mm;
	lx_thread *nt;
	lx_u32 execfn, platform, rnd, ientry = 0, ilo = 0, ihi = 0;
	char interp[LX_PATH_MAX];
	int dbt;
	lx_simg *simg = 0, *isimg = 0;
	lx_s64 fsize = 0, fmtime = 0;
	lx_u64 fino = 0;
	/* (the images go if loading fails, unless they are shared) */
#define FREE_IMAGES() do { if (!simg) bk_region_free(&img); if (ireg.size && !isimg) bk_region_free(&ireg); } while (0)
	if (rd(t, f, &eh, sizeof eh, 0) != (int)sizeof eh) return -LX_ENOEXEC;
	if (eh.ident[4] != 1 || eh.ident[5] != 1 || eh.machine != EM_ARM) {
		bk_log("%s: not a 32-bit ARM program", path);
		return -LX_ENOEXEC;
	}
	if (eh.type == ET_EXEC) {
		bk_log("%s: not position independent: build it with lx-cc (-static-pie)", path);
		return -LX_ENOEXEC;
	}
	if (eh.type != ET_DYN || eh.phentsize != sizeof(elf_phdr) || eh.phnum > 16) return -LX_ENOEXEC;
	if (rd(t, f, ph, eh.phnum * sizeof(elf_phdr), eh.phoff) != (int)(eh.phnum * sizeof(elf_phdr))) return -LX_ENOEXEC;
	interp[0] = 0;
	for (i = 0; i < eh.phnum; i++) {
		if (ph[i].type == PT_INTERP) {
			if (ph[i].filesz < 2 || ph[i].filesz >= sizeof interp || rd(t, f, interp, ph[i].filesz, ph[i].offset) != (int)ph[i].filesz)
				return -LX_ENOEXEC;
			interp[ph[i].filesz] = 0;
		}
		if (ph[i].type == PT_LOAD && ph[i].vaddr + ph[i].memsz > size) size = ph[i].vaddr + ph[i].memsz;
		if (ph[i].type == PT_GNU_STACK && ph[i].memsz) stack_size = ph[i].memsz;
	}
	ireg.size = 0;
	ireg.base = 0;
	ireg.bk = 0;
	if (!size || size > 32 * 1024 * 1024) return -LX_ENOEXEC;
	if (stack_size < 32 * 1024) stack_size = 32 * 1024;
	if (stack_size > 2 * 1024 * 1024) stack_size = 2 * 1024 * 1024;
	size = (size + 4095) & ~4095u;
	/* the image: a shared one (Alpine mode), or loaded now */
	simg = isimg = 0;
	if (file_id(f, &fsize, &fino, &fmtime) == 0) simg = lx_simg_find(path, fsize, fino, fmtime, 0);
	if (simg) {
		img = simg->reg;
		note = 0;
		dbt = 1;
		if (dbt_init() < 0) return -LX_ENOEXEC;
		goto loaded;
	}
	if (lx_k.trace) bk_log("load: image %u bytes", size);
	if (lx_region_new(&img, size, LX_REG_IMAGE) < 0) {
		lx_mem_reclaim();                     /* (the cache gives way first) */
		if (lx_region_new(&img, size, LX_REG_IMAGE) < 0) return -LX_ENOMEM;
	}
	for (i = 0; i < eh.phnum; i++) {
		if (ph[i].type != PT_LOAD) continue;
		if (ph[i].filesz > ph[i].memsz) { bk_region_free(&img); return -LX_ENOEXEC; }
		r = rd(t, f, (void *)(img.base + ph[i].vaddr), ph[i].filesz, ph[i].offset);
		if (r != (int)ph[i].filesz) { bk_region_free(&img); return r < 0 ? r : -LX_EIO; }
		bk_memset((void *)(img.base + ph[i].vaddr + ph[i].filesz), 0, ph[i].memsz - ph[i].filesz);
	}
	note = has_note(img.base, ph, eh.phnum);
	dbt = 0;
	if (note && interp[0]) {
		bk_log("%s: a PsiLinux program with a dynamic linker (%s): build it static-pie", path, interp);
		bk_region_free(&img);
		return -LX_ENOEXEC;
	}
	if (!note) {
		/* not built for PsiLinux: an ordinary ARM Linux program (Alpine's):
		 * it runs through the translator (Alpine mode) */
		if (dbt_init() < 0) {
			int n = count_svc(img.base, ph, eh.phnum);
			if (n || interp[0] || (eh.flags & EF_ARM_VFP_FLOAT)) {
				bk_log("%s: not built for PsiLinux, and the translator is not there", path);
				bk_region_free(&img);
				return -LX_ENOEXEC;
			}
		} else dbt = 1;
	}
	if (dbt && fsize) {
		/* a translated program: its image is shared from now on */
		lx_u32 lo, hi;
		rw_range(img.base, size, ph, eh.phnum, &lo, &hi);
		simg = lx_simg_add(path, fsize, fino, fmtime, 0, &img, img.base, size, lo, hi);
	}
loaded:
	if (dbt) {
		if (stack_size < LX_DBT_STACK) stack_size = LX_DBT_STACK;
		if (interp[0]) {
			r = load_interp(t, p, interp, &ireg, &ientry, &ilo, &ihi, &isimg);
			if (r < 0) { if (!simg) bk_region_free(&img); return r; }
		}
	}
	/* the stack, with the arguments, environment and auxiliary vector */
	need = 64 + 16;
	for (i = 0; i < argc; i++) need += lx_strlen(argv[i]) + 1 + 4;
	for (i = 0; i < envc; i++) need += lx_strlen(envp[i]) + 1 + 4;
	need += lx_strlen(path) + 1 + 40 * 4;
	if (need > stack_size / 2) { FREE_IMAGES(); return -LX_E2BIG; }
	if (lx_region_new(&stk, stack_size, LX_REG_STACK) < 0) {
		lx_mem_reclaim();
		if (lx_region_new(&stk, stack_size, LX_REG_STACK) < 0) { FREE_IMAGES(); return -LX_ENOMEM; }
	}
	sp = stk.base + stk.size;
	execfn = push_str(&sp, path);
	platform = push_str(&sp, dbt ? "v6l" : "v4l");
	sp -= 16;
	rnd = sp;
	for (i = 0; i < 16; i++) ((lx_u8 *)rnd)[i] = (lx_u8)(bk_random() >> 16);
	uargv = (lx_u32 *)bk_alloc((argc + 1) * 4);
	uenvp = (lx_u32 *)bk_alloc((envc + 1) * 4);
	if (!uargv || !uenvp) { bk_free(uargv); bk_free(uenvp); bk_region_free(&stk); FREE_IMAGES(); return -LX_ENOMEM; }
	for (i = envc - 1; i >= 0; i--) uenvp[i] = push_str(&sp, envp[i]);
	for (i = argc - 1; i >= 0; i--) uargv[i] = push_str(&sp, argv[i]);
	sp &= ~15u;
	auxn = 2 * 18;                 /* 17 entries and AT_NULL */
	sp -= (1 + argc + 1 + envc + 1 + auxn) * 4;
	sp &= ~15u;
	v = (lx_u32 *)sp;
	*v++ = argc;
	for (i = 0; i < argc; i++) *v++ = uargv[i];
	*v++ = 0;
	for (i = 0; i < envc; i++) *v++ = uenvp[i];
	*v++ = 0;
	bk_free(uargv);
	bk_free(uenvp);
#define AUX(k, val) do { *v++ = (k); *v++ = (val); } while (0)
	AUX(LX_AT_PHDR, img.base + eh.phoff);
	AUX(LX_AT_PHENT, sizeof(elf_phdr));
	AUX(LX_AT_PHNUM, eh.phnum);
	AUX(LX_AT_PAGESZ, 4096);
	AUX(LX_AT_BASE, ireg.size ? ireg.base : 0);
	AUX(LX_AT_FLAGS, 0);
	AUX(LX_AT_ENTRY, img.base + eh.entry);
	AUX(LX_AT_UID, p->uid);
	AUX(LX_AT_EUID, p->uid);
	AUX(LX_AT_GID, p->gid);
	AUX(LX_AT_EGID, p->gid);
	if (dbt) AUX(LX_AT_HWCAP, 0x02 | 0x10 | 0x40 | 0x80 | 0x8000);   /* HALF, FAST_MULT, VFP, EDSP, TLS: what the translator gives */
	else AUX(LX_AT_HWCAP, 0x02 | 0x04 | 0x10);   /* HALF, THUMB, FAST_MULT (no TLS register) */
	AUX(LX_AT_CLKTCK, 100);
	AUX(LX_AT_PLATFORM, platform);
	AUX(LX_AT_RANDOM, rnd);
	AUX(LX_AT_EXECFN, execfn);
	if (!dbt) AUX(LX_AT_SYSINFO, (lx_u32)lx_k.vdso);
	else AUX(LX_AT_SECURE, 0);
	*v++ = LX_AT_NULL;
	*v++ = 0;
#undef AUX
	/* the address space and the thread */
	mm = lx_mm_new();
	if (!mm) { bk_region_free(&stk); FREE_IMAGES(); return -LX_ENOMEM; }
	mm->image = img;
	mm->entry = img.base + eh.entry;
	mm->dbt = dbt;
	if (ireg.size) { mm->interp = ireg; mm->ilo = ilo; mm->ihi = ihi; }
	mm->img_s = simg;
	mm->interp_s = isimg;
	if ((simg && lx_mm_share(mm, simg) < 0) || (isimg && lx_mm_share(mm, isimg) < 0)) {
		lx_mm_put(mm);
		bk_region_free(&stk);
		return -LX_ENOMEM;
	}
	mm->phdr = img.base + eh.phoff;
	mm->phnum = eh.phnum;
	mm->brk = img.base + size;
	if (simg) { mm->wlo = simg->rw_lo; mm->whi = simg->rw_hi; }
	else for (i = 0; i < eh.phnum; i++)
		if (ph[i].type == PT_LOAD && (ph[i].flags & 2)) {     /* PF_W: data and bss */
			lx_u32 lo = img.base + ph[i].vaddr, hi = lo + ph[i].memsz;
			if (!mm->wlo || lo < mm->wlo) mm->wlo = lo & ~3u;
			/* (to the end of the page: musl's dynamic linker gives the rest
			 * of the last page to malloc, so it is data too) */
			if (hi > mm->whi) mm->whi = (hi + 4095) & ~4095u;
			if (mm->whi > img.base + size) mm->whi = img.base + size;
		}
	lx_strlcpy(mm->exe, path, sizeof mm->exe);
	if (lx_k.trace) bk_log("load: a thread");
	nt = lx_thread_for_exec(p);
	if (!nt) { lx_mm_put(mm); bk_region_free(&stk); return -LX_EAGAIN; }
	nt->stack = stk;
	nt->stack_lo = stk.base;
	nt->stack_hi = stk.base + stk.size;
	for (i = 0; i < 16; i++) nt->ctx[i] = 0;
	nt->ctx[13] = sp;
	nt->ctx[15] = ireg.size ? ireg.base + ientry : mm->entry;
	nt->dbt = dbt;
	nt->ctx_cpsr = 0x10;
	if (p->mm) lx_mm_put(p->mm);
	p->mm = mm;
	*ntp = nt;
	if (lx_k.trace) bk_log("exec %s: image %u KB at %x, stack %u KB at %x, sp %x argv0 at %x", path, size / 1024, img.base, stack_size / 1024, stk.base, sp, ((lx_u32 *)sp)[1]);
	return 0;
}

static int exec_load(lx_thread *t, lx_proc *p, const char *path, char **argv, int argc, char **envp, int envc, lx_thread **nt);

int lx_exec_load(lx_thread *t, lx_proc *p, const char *path, char **argv, int argc, char **envp, int envc, lx_thread **nt)
{
	int r;
	exec_loading++;
	r = exec_load(t, p, path, argv, argc, envp, envc, nt);
	exec_loading--;
	return r;
}

static int exec_load(lx_thread *t, lx_proc *p, const char *path, char **argv, int argc, char **envp, int envc, lx_thread **nt)
{
	char head[256];
	char interp[LX_PATH_MAX];
	char resolved[LX_PATH_MAX];
	int depth, r, n, i;
	char *nargv[64 + 3];
	char *arg = 0;
	lx_file *f;
	struct lx_stat64 st;
	const char *cur = path;
	char curbuf[LX_PATH_MAX];
	char **av = argv;
	int ac = argc;
	char *owned[16];
	int nowned = 0;
	for (depth = 0; depth < 4; depth++) {
		f = lx_vfs_open(p, LX_AT_FDCWD, cur, LX_O_RDONLY, 0, &r);
		if (!f) goto out;
		if (f->ops->fstat) f->ops->fstat(f, &st);
		else st.st_mode = LX_S_IFREG | 0755;
		if (!LX_S_ISREG(st.st_mode)) { lx_file_put(f); r = -LX_EACCES; goto out; }
		if (!f->ops->pread) { lx_file_put(f); r = -LX_EACCES; goto out; }
		n = rd(t, f, head, sizeof head - 1, 0);
		if (n < 4) { lx_file_put(f); r = -LX_ENOEXEC; goto out; }
		head[n] = 0;
		if (head[0] == 0x7f && head[1] == 'E' && head[2] == 'L' && head[3] == 'F') {
			if (lx_vfs_resolve(p, LX_AT_FDCWD, cur, 1, resolved) < 0) lx_strlcpy(resolved, cur, sizeof resolved);
			r = load_elf(t, p, f, resolved, av, ac, envp, envc, nt);
			lx_file_put(f);
			goto out;
		}
		lx_file_put(f);
		if (head[0] != '#' || head[1] != '!') { r = -LX_ENOEXEC; goto out; }
		/* "#!interpreter [one argument]" */
		i = 2;
		while (head[i] == ' ' || head[i] == '\t') i++;
		n = 0;
		while (head[i] && head[i] != '\n' && head[i] != ' ' && head[i] != '\t' && n < (int)sizeof interp - 1) interp[n++] = head[i++];
		interp[n] = 0;
		while (head[i] == ' ' || head[i] == '\t') i++;
		arg = 0;
		if (head[i] && head[i] != '\n') {
			int s = i;
			while (head[i] && head[i] != '\n') i++;
			while (i > s && (head[i - 1] == ' ' || head[i - 1] == '\t' || head[i - 1] == '\r')) i--;
			head[i] = 0;
			arg = head + s;
		}
		if (!n) { r = -LX_ENOEXEC; goto out; }
		if (ac > 64) { r = -LX_E2BIG; goto out; }
		/* new argv: interpreter [arg] script args... */
		{
			int k = 0, j;
			char *script = lx_strdup(cur);
			char *ip = lx_strdup(interp);
			char *ap = arg ? lx_strdup(arg) : 0;
			char *tmpv[64 + 3];
			if (nowned + 3 > 16) { r = -LX_ELOOP; goto out; }
			owned[nowned++] = script;
			owned[nowned++] = ip;
			if (ap) owned[nowned++] = ap;
			tmpv[k++] = ip;
			if (ap) tmpv[k++] = ap;
			tmpv[k++] = script;
			for (j = 1; j < ac && k < 64 + 2; j++) tmpv[k++] = av[j];
			tmpv[k] = 0;
			for (j = 0; j <= k; j++) nargv[j] = tmpv[j];
			av = nargv;
			ac = k;
		}
		lx_strlcpy(curbuf, interp, sizeof curbuf);
		cur = curbuf;
	}
	r = -LX_ELOOP;
out:
	while (nowned) bk_free(owned[--nowned]);
	return r;
}
