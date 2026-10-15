/* lx_mem.c - memory for PsiLinux's programs.
 *
 * Program images and thread stacks are regions of their own (an RChunk each
 * on EPOC, so they go back to EPOC whole when a program ends). Everything
 * mmap gives out comes from one arena, a single chunk managed here in 4 KB
 * pages: first fit, and the chunk is made smaller whenever its top pages are
 * free, so memory goes back to the Psion's other programs. */
#include "lx_core.h"
#include "lx_dbt.h"

#define PAGE 4096u
#define MAXPAGES 8192                   /* 32 MB of address space at most */

static lx_u8 used[MAXPAGES / 8];
static lx_u32 npages;                   /* the arena's size in pages (address space) */
static lx_u32 committed;                /* pages committed now */
static lx_u32 base;

static int page_used(lx_u32 i) { return used[i >> 3] & (1 << (i & 7)); }
static void page_set(lx_u32 i, int v)
{
	if (v) used[i >> 3] |= (lx_u8)(1 << (i & 7));
	else used[i >> 3] &= (lx_u8)~(1 << (i & 7));
}

/* The memory reserve (Preferences: "Keep memory for other programs"): Linux
 * takes no RAM from EPOC that would leave less than the reserve free. Every
 * growth that takes RAM goes through here: images, stacks, a fork's saved
 * memory, the arena, the code cache and threads (DECISIONS.md 51). The
 * kernel's own small allocations do not, so the runner keeps working. */
int lx_reserve_ok(lx_u32 grow) { return lx_reserve_check(grow, 1); }

/* (may_drop 0: the caches are left alone, as when the translator itself
 * grows, in the middle of translating) */
int lx_reserve_check(lx_u32 grow, int may_drop)
{
	lx_u32 res = bk_mem_reserve(), total, freeb;
	if (!res || !grow) return 1;
	bk_mem_info(&total, &freeb);
	if (freeb >= grow && freeb - grow >= res) return 1;
	/* the caches give way first */
	if (may_drop && lx_mem_cached()) {
		lx_mem_reclaim();
		bk_mem_info(&total, &freeb);
		if (freeb >= grow && freeb - grow >= res) return 1;
	}
	if (!lx_k.reserve_refused)
		bk_log("memory: kept %u KB for other programs, so refused %u KB (%u KB free)", res / 1024, (grow + 1023) / 1024, freeb / 1024);
	lx_k.reserve_refused++;
	lx_k.reserve_refused_kb += (grow + 1023) / 1024;
	return 0;
}

lx_u32 lx_mem_cached(void) { return lx_exec_cache_bytes() + lx_simg_unused_bytes(); }

lx_u32 lx_mem_avail(void)
{
	lx_u32 total, freeb, res = bk_mem_reserve();
	bk_mem_info(&total, &freeb);
	return freeb > res ? freeb - res : 0;
}

int lx_region_new(lx_region *r, lx_u32 size, int kind)
{
	if (!lx_reserve_ok((size + 4095) & ~4095u)) return -LX_ENOMEM;
	return bk_region_new(r, size, kind);
}

/* commits the arena up to top pages (only a growth is checked) */
static int arena_grow(lx_u32 top)
{
	if (top > committed && !lx_reserve_ok((top - committed) * PAGE)) return -LX_ENOMEM;
	return bk_arena_grow(top * PAGE);
}

static void arena_init(void)
{
	if (npages) return;
	base = bk_arena_base();
	npages = bk_arena_max() / PAGE;
	if (npages > MAXPAGES) npages = MAXPAGES;
}

/* the arena is just big enough for its highest page in use */
static void arena_trim(void)
{
	lx_u32 top = committed;
	while (top > 0 && !page_used(top - 1)) top--;
	if (top != committed && bk_arena_grow(top * PAGE) == 0) committed = top;
}

lx_u32 lx_arena_alloc(lx_u32 len)
{
	lx_u32 n = (len + PAGE - 1) / PAGE, i, run = 0, start = 0;
	arena_init();
	if (!n || !npages) return 0;
	for (i = 0; i < npages; i++) {
		if (page_used(i)) { run = 0; continue; }
		if (!run) start = i;
		if (++run == n) break;
	}
	if (run < n) return 0;
	if (start + n > committed) {
		if (arena_grow(start + n) < 0) {
			extern void lx_exec_cache_drop(void);
			lx_mem_reclaim();                 /* (the program cache gives way first) */
			if (arena_grow(start + n) < 0) return 0;
		}
		committed = start + n;
	}
	for (i = start; i < start + n; i++) page_set(i, 1);
	bk_memset((void *)(base + start * PAGE), 0, n * PAGE);
	return base + start * PAGE;
}

void lx_arena_free(lx_u32 addr, lx_u32 len)
{
	lx_u32 i, first, n = (len + PAGE - 1) / PAGE;
	if (!npages || addr < base) return;
	first = (addr - base) / PAGE;
	for (i = first; i < first + n && i < npages; i++) page_set(i, 0);
	arena_trim();
}

/* can the mapping at addr grow to newlen where it is? */
static int arena_extend(lx_u32 addr, lx_u32 oldlen, lx_u32 newlen)
{
	lx_u32 i, first = (addr - base) / PAGE, o = (oldlen + PAGE - 1) / PAGE, n = (newlen + PAGE - 1) / PAGE;
	if (first + n > npages) return 0;
	for (i = first + o; i < first + n; i++) if (page_used(i)) return 0;
	if (first + n > committed) {
		if (arena_grow(first + n) < 0) return 0;
		committed = first + n;
	}
	for (i = first + o; i < first + n; i++) page_set(i, 1);
	bk_memset((void *)(base + (first + o) * PAGE), 0, (n - o) * PAGE);
	return 1;
}

lx_mm *lx_mm_new(void)
{
	lx_mm *mm = (lx_mm *)bk_alloc(sizeof *mm);
	if (mm) mm->refs = 1;
	return mm;
}

static void writeback(lx_map *m, lx_u32 addr, lx_u32 len)
{
	if (!m->file || !(m->prot & LX_PROT_WRITE) || !(m->flags & LX_MAP_SHARED)) return;
	if (m->file->ops->pwrite)
		m->file->ops->pwrite(0, m->file, (const char *)addr, len, m->off + (addr - m->addr));
}

void lx_mm_put(lx_mm *mm)
{
	lx_map *m, *n;
	if (!mm || --mm->refs > 0) return;
	if (lx_k.memlog) bk_log("memory: %s used %u KB of mappings at most (image %u KB, own %u KB)", mm->exe, mm->peak / 1024,
		(mm->image.size + mm->interp.size) / 1024, mm->img_s ? 0 : (mm->image.size + mm->interp.size) / 1024);
	for (m = mm->maps; m; m = n) {
		n = m->next;
		if (m->file) lx_file_put(m->file);
		if (m->simg) { bk_free(m); continue; }          /* (a shared library: lx_mm_unshare_all) */
		writeback(m, m->addr, m->len);
		if (mm->dbt) dbt_invalidate(m->addr, m->addr + m->len);
		lx_arena_free(m->addr, m->len);
		bk_free(m);
	}
	if (mm->dbt) {
		/* its code's translations go with it (lx_dbt.c), unless it was a
		 * shared image (those stay, with their translations) */
		if (mm->image.size && !mm->img_s) dbt_invalidate(mm->image.base, mm->image.base + mm->image.size);
		if (mm->interp.size && !mm->interp_s) dbt_invalidate(mm->interp.base, mm->interp.base + mm->interp.size);
	}
	if (mm->image.size && !mm->img_s) bk_region_free(&mm->image);
	if (mm->interp.size && !mm->interp_s) bk_region_free(&mm->interp);
	lx_mm_unshare_all(mm);
	bk_free(mm);
}

static lx_map *map_add(lx_mm *mm, lx_u32 addr, lx_u32 len)
{
	lx_map *m = (lx_map *)bk_alloc(sizeof *m);
	if (!m) return 0;
	m->addr = addr;
	m->len = len;
	m->next = mm->maps;
	mm->maps = m;
	mm->mapped += len;
	if (mm->mapped > mm->peak) mm->peak = mm->mapped;
	return m;
}

/* Alpine mode: is f an ELF shared object (ld-musl mapping a library)?
 * Fills the program headers. */
typedef struct { lx_u32 type, offset, vaddr, paddr, filesz, memsz, flags, align; } m_phdr;
static int elf_so(lx_thread *t, lx_file *f, m_phdr *ph, int *phnum)
{
	lx_u8 eh[52];
	lx_u32 phoff;
	int n;
	if (f->ops->pread(t, f, (char *)eh, 52, 0) != 52) return 0;
	if (eh[0] != 0x7f || eh[1] != 'E' || eh[2] != 'L' || eh[3] != 'F' || eh[4] != 1 || eh[5] != 1) return 0;
	if ((eh[16] | (eh[17] << 8)) != 3 || (eh[18] | (eh[19] << 8)) != 40) return 0;    /* ET_DYN, EM_ARM */
	phoff = eh[28] | (eh[29] << 8) | (eh[30] << 16) | ((lx_u32)eh[31] << 24);
	n = eh[44] | (eh[45] << 8);
	if ((eh[42] | (eh[43] << 8)) != 32 || n < 1 || n > 16) return 0;
	if (f->ops->pread(t, f, (char *)ph, n * 32, phoff) != n * 32) return 0;
	*phnum = n;
	return 1;
}

/* A library's image, shared by every process that maps the same file (the
 * dynamic linker's first mapping of it: offset 0, the whole span). Its
 * writable part is each process's own (lx_simg.c). Returns the address, 0
 * if f is no such thing, or -errno. */
static int map_shared_lib(lx_thread *t, lx_mm *mm, lx_file *f, lx_u32 rlen, lx_u32 len, int prot, int flags)
{
	m_phdr ph[16];
	int n, i;
	struct lx_stat64 st;
	lx_simg *s;
	lx_map *m;
	lx_s64 mt;
	if (!f->ops->fstat || f->ops->fstat(f, &st) < 0 || !f->path) return 0;
	if (!elf_so(t, f, ph, &n)) return 0;
	mt = (lx_s64)st.st_mtime_ * 1000000 + st.st_mtime_nsec / 1000;
	s = lx_simg_find(f->path, st.st_size, st.st_ino, mt, 1);
	if (s && s->len != rlen) s = 0;
	if (!s) {
		lx_region reg;
		lx_u32 lo = 0, hi = 0, done = 0;
		if (lx_region_new(&reg, rlen, LX_REG_IMAGE) < 0) {
			extern void lx_exec_cache_drop(void);
			lx_mem_reclaim();
			if (lx_region_new(&reg, rlen, LX_REG_IMAGE) < 0) return -LX_ENOMEM;
		}
		bk_memset((void *)reg.base, 0, rlen);
		while (done < len) {
			int r = f->ops->pread(t, f, (char *)reg.base + done, len - done, done);
			if (r <= 0) break;
			done += r;
		}
		/* the writable part (as the dynamic linker will lay it out, to the
		 * end of its page), and its contents as the file has them */
		for (i = 0; i < n; i++) {
			lx_u32 a, b;
			if (ph[i].type != 1 || !(ph[i].flags & 2)) continue;
			a = ph[i].vaddr & ~3u;
			b = (ph[i].vaddr + ph[i].memsz + 4095) & ~4095u;
			if (b > rlen || ph[i].filesz > ph[i].memsz) { bk_region_free(&reg); return 0; }
			if (!hi || a < lo) lo = a;
			if (b > hi) hi = b;
			bk_memset((void *)(reg.base + (ph[i].vaddr & ~4095u)), 0, b - (ph[i].vaddr & ~4095u));
			f->ops->pread(t, f, (char *)(reg.base + ph[i].vaddr), ph[i].filesz, ph[i].offset);
		}
		s = lx_simg_add(f->path, st.st_size, st.st_ino, mt, 1, &reg, reg.base, rlen, hi ? reg.base + lo : 0, hi ? reg.base + hi : 0);
		if (!s) { bk_region_free(&reg); return 0; }
	}
	m = map_add(mm, s->base, rlen);
	if (!m) return -LX_ENOMEM;
	n = lx_mm_share(mm, s);
	if (n < 0) {
		mm->maps = m->next; mm->mapped -= rlen; bk_free(m);
		if (!s->users) lx_simg_trim(0);
		/* a process with very many libraries: this one is its own copy */
		return n == -LX_ENOSPC ? 0 : -LX_ENOMEM;
	}
	m->simg = s;
	m->file = lx_file_get(f);
	m->prot = prot;
	m->flags = flags;
	lx_mm_resident(mm);                 /* (the caller is running: its own contents in place) */
	return (int)s->base;
}

int lx_sys_mmap(lx_thread *t, lx_u32 *a)
{
	lx_u32 addr = a[0], len = a[1], prot = a[2], flags = a[3];
	int fd = (int)a[4];
	lx_s64 off = t->nr == LX_NR_mmap2 ? (lx_s64)a[5] * PAGE : (lx_s64)a[5];
	lx_mm *mm = t->proc->mm;
	lx_file *f = 0;
	lx_map *m;
	lx_u32 p, rlen;
	if (!mm) return -LX_ENOMEM;
	if (!len) return -LX_EINVAL;
	rlen = (len + PAGE - 1) & ~(PAGE - 1);
	if (!(flags & LX_MAP_ANONYMOUS)) {
		f = lx_fd_get(t->proc, fd);
		if (!f) return -LX_EBADF;
		if (!f->ops->pread) return -LX_ENODEV;
	}
	if (flags & LX_MAP_FIXED) {
		/* only over a mapping we gave out already (no MMU to move pages) */
		for (m = mm->maps; m; m = m->next)
			if (addr >= m->addr && addr + rlen <= m->addr + m->len) break;
		if (!m) return -LX_EINVAL;
		p = addr;
		if (m->simg) {
			/* a shared library's segment: only its writable part is this
			 * process's to set up; the rest is the same for everyone */
			lx_simg *s = m->simg;
			lx_u32 lo = p < s->rw_lo ? s->rw_lo : p, hi = p + rlen > s->rw_hi ? s->rw_hi : p + rlen;
			if (hi <= lo) return (int)p;
			lx_mm_resident(mm);
			bk_memset((void *)lo, 0, hi - lo);
			if (f) {
				lx_u32 skip = lo - p, want = len > skip ? len - skip : 0, done = 0;
				if (want > hi - lo) want = hi - lo;
				while (done < want) {
					int r = f->ops->pread(t, f, (char *)lo + done, want - done, off + skip + done);
					if (r <= 0) break;
					done += r;
				}
			}
			return (int)p;
		}
		if (mm->dbt) dbt_invalidate(p, p + rlen);
		bk_memset((void *)p, 0, rlen);
	} else {
		if (f && mm->dbt && off == 0 && (prot & LX_PROT_EXEC) && !(flags & LX_MAP_SHARED)) {
			int r = map_shared_lib(t, mm, f, rlen, len, (int)prot, (int)flags);
			if (r != 0) return r;
		}
		p = lx_arena_alloc(rlen);
		if (!p) return -LX_ENOMEM;
		m = map_add(mm, p, rlen);
		if (!m) { lx_arena_free(p, rlen); return -LX_ENOMEM; }
	}
	if (f) {
		/* a copy of the file (MAP_SHARED + PROT_WRITE is written back at munmap and msync) */
		lx_u32 done = 0;
		while (done < len) {
			int r = f->ops->pread(t, f, (char *)p + done, len - done, off + done);
			if (r <= 0) break;
			done += r;
		}
		if (m->addr == p) {
			m->file = lx_file_get(f);
			m->off = off;
			m->prot = prot;
			m->flags = flags;
		}
	}
	return (int)p;
}

/* takes [addr, addr+len) out of the mappings, splitting them where needed */
static int unmap_range(lx_mm *mm, lx_u32 addr, lx_u32 len)
{
	lx_map **pm = &mm->maps, *m;
	lx_u32 end = addr + ((len + PAGE - 1) & ~(PAGE - 1));
	while ((m = *pm) != 0) {
		lx_u32 ms = m->addr, me = m->addr + m->len;
		if (end <= ms || addr >= me) { pm = &m->next; continue; }
		if (m->simg) {
			/* a shared library: it goes when all of it is unmapped */
			if (addr <= ms && end >= me) {
				*pm = m->next;
				mm->mapped -= m->len;
				if (m->file) lx_file_put(m->file);
				lx_mm_unshare(mm, m->simg);
				bk_free(m);
				continue;
			}
			pm = &m->next;
			continue;
		}
		if (addr <= ms && end >= me) {
			/* all of it */
			writeback(m, ms, m->len);
			*pm = m->next;
			mm->mapped -= m->len;
			lx_arena_free(ms, m->len);
			if (m->file) lx_file_put(m->file);
			bk_free(m);
			continue;
		}
		if (addr <= ms) {
			/* its start */
			writeback(m, ms, end - ms);
			lx_arena_free(ms, end - ms);
			m->off += end - ms;
			m->len = me - end;
			m->addr = end;
			mm->mapped -= end - ms;
		} else if (end >= me) {
			/* its end */
			writeback(m, addr, me - addr);
			lx_arena_free(addr, me - addr);
			m->len = addr - ms;
			mm->mapped -= me - addr;
		} else {
			/* the middle: two pieces */
			lx_map *n = (lx_map *)bk_alloc(sizeof *n);
			if (!n) return -LX_ENOMEM;
			writeback(m, addr, end - addr);
			*n = *m;
			n->addr = end;
			n->len = me - end;
			n->off = m->off + (end - ms);
			if (n->file) lx_file_get(n->file);
			m->len = addr - ms;
			m->next = n;
			lx_arena_free(addr, end - addr);
			mm->mapped -= end - addr;
		}
		pm = &m->next;
	}
	return 0;
}

int lx_mm_unmap(lx_mm *mm, lx_u32 addr, lx_u32 len) { return unmap_range(mm, addr, len); }

int lx_sys_munmap(lx_thread *t, lx_u32 *a)
{
	lx_mm *mm = t->proc->mm;
	if (!mm) return -LX_EINVAL;
	if (a[0] & (PAGE - 1)) return -LX_EINVAL;
	/* (a fork child freeing its parent's memory: it stays, to be put back) */
	if (lx_fork_holds(mm, a[0], a[1])) return 0;
	{
		lx_map *m;
		int shared = 0;
		for (m = mm->maps; m; m = m->next) if (m->simg && a[0] < m->addr + m->len && a[0] + a[1] > m->addr) shared = 1;
		if (mm->dbt && !shared) dbt_invalidate(a[0], a[0] + a[1]);
	}
	return unmap_range(mm, a[0], a[1]);
}

int lx_sys_mremap(lx_thread *t, lx_u32 *a)
{
	lx_u32 old = a[0], oldlen = (a[1] + PAGE - 1) & ~(PAGE - 1), newlen = (a[2] + PAGE - 1) & ~(PAGE - 1), flags = a[3];
	lx_mm *mm = t->proc->mm;
	lx_map *m;
	lx_u32 p;
	if (!mm || !newlen) return -LX_EINVAL;
	for (m = mm->maps; m; m = m->next)
		if (m->addr == old) break;
	if (!m || m->len != oldlen) return -LX_EFAULT;
	if (m->simg) return -LX_EINVAL;
	if (newlen == oldlen) return (int)old;
	if (newlen < oldlen) {
		if (!lx_fork_holds(mm, old, oldlen)) unmap_range(mm, old + newlen, oldlen - newlen);
		return (int)old;
	}
	if (!lx_fork_holds(mm, old, oldlen) && arena_extend(old, oldlen, newlen)) {
		m->len = newlen;
		mm->mapped += newlen - oldlen;
		if (mm->mapped > mm->peak) mm->peak = mm->mapped;
		return (int)old;
	}
	if (!(flags & LX_MREMAP_MAYMOVE)) return -LX_ENOMEM;
	p = lx_arena_alloc(newlen);
	if (!p) return -LX_ENOMEM;
	bk_memcpy((void *)p, (const void *)old, oldlen);
	if (lx_fork_holds(mm, old, oldlen)) {
		/* (a fork child: the parent's mapping stays; the child gets a copy) */
		lx_map *n = map_add(mm, p, newlen);
		if (!n) { lx_arena_free(p, newlen); return -LX_ENOMEM; }
		return (int)p;
	}
	if (mm->dbt) dbt_invalidate(old, old + oldlen);
	lx_arena_free(old, oldlen);
	m->addr = p;
	m->len = newlen;
	mm->mapped += newlen - oldlen;
	return (int)p;
}

int lx_sys_msync(lx_thread *t, lx_u32 *a)
{
	lx_mm *mm = t->proc->mm;
	lx_map *m;
	if (!mm) return 0;
	for (m = mm->maps; m; m = m->next)
		if (a[0] < m->addr + m->len && a[0] + a[1] > m->addr) writeback(m, m->addr, m->len);
	return 0;
}
