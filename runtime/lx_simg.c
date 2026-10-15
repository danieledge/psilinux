/* lx_simg.c - Alpine mode: program images that processes share.
 *
 * Without an MMU, every process used to load its own copy of each program
 * (BusyBox 1 MB, musl 650 KB) at an address of its own, and the translator
 * translated each copy afresh. A shared image is loaded once, at one
 * address, and every process that runs or maps the same file (the same
 * path, size, inode and time) uses it:
 *  - its code and read-only data are there once, and their translations
 *    stay in the code cache for every process, even after the last one
 *    ends (unused images are kept while there is memory to spare);
 *  - its writable part (data, bss, and the rest of that page, which musl's
 *    dynamic linker gives to malloc) is each process's own: one process's
 *    contents are in place, the others' are kept in a store each, and
 *    swapped in when their process gets the CPU (lx_mm_resident). Only one
 *    translated thread runs guest code at a time (lx_dbt.h), so that is
 *    safe; the kernel swaps only when no translated thread is in guest code.
 * Code reaches its data by fixed offsets from itself, so an image's data
 * must be at the same place for everyone: hence the swapping.
 *
 * Everything here runs on the kernel thread. */
#include "lx_core.h"
#include "lx_dbt.h"

#define NSIMG 32
#define KEEP_UNUSED 8                   /* unused images kept, at most */
static lx_simg simgs[NSIMG];
static lx_u32 simg_clock;

lx_simg *lx_simg_find(const char *path, lx_s64 size, lx_u64 ino, lx_s64 mtime, int lib)
{
	int i;
	for (i = 0; i < NSIMG; i++) {
		lx_simg *s = &simgs[i];
		if (!s->len) continue;
		if (s->size == size && s->ino == ino && s->mtime == mtime && s->is_lib == lib && lx_strcmp(s->path, path) == 0) return s;
	}
	return 0;
}

static void simg_free(lx_simg *s)
{
	if (lx_k.trace) bk_log("simg: %s goes (%u KB)", s->path, s->len / 1024);
	dbt_invalidate(s->base, s->base + s->len);
	bk_region_free(&s->reg);
	bk_free(s->pristine);
	bk_memset(s, 0, sizeof *s);
}

/* the memory unused images hold (it can be had back) */
lx_u32 lx_simg_unused_bytes(void)
{
	int i;
	lx_u32 n = 0;
	for (i = 0; i < NSIMG; i++) if (simgs[i].len && !simgs[i].users) n += simgs[i].len;
	return n;
}

/* unused images go: all of them (memory is short), or the oldest beyond
 * KEEP_UNUSED */
void lx_simg_trim(int all)
{
	int i, n;
	for (;;) {
		lx_simg *old = 0;
		n = 0;
		for (i = 0; i < NSIMG; i++) {
			lx_simg *s = &simgs[i];
			if (!s->len || s->users) continue;
			n++;
			if (!old || s->used < old->used) old = s;
		}
		if (!old || (!all && n <= KEEP_UNUSED)) return;
		simg_free(old);
	}
}

/* Registers a newly loaded image (its region is then the image's, not the
 * caller's). rw_lo..rw_hi is its writable part, already as loaded. */
lx_simg *lx_simg_add(const char *path, lx_s64 size, lx_u64 ino, lx_s64 mtime, int lib, lx_region *reg, lx_u32 base, lx_u32 len, lx_u32 rw_lo, lx_u32 rw_hi)
{
	int i;
	lx_simg *s = 0;
	for (i = 0; i < NSIMG; i++) if (!simgs[i].len) { s = &simgs[i]; break; }
	if (!s) {
		lx_simg_trim(1);
		for (i = 0; i < NSIMG; i++) if (!simgs[i].len) { s = &simgs[i]; break; }
		if (!s) return 0;
	}
	bk_memset(s, 0, sizeof *s);
	if (rw_hi > rw_lo) {
		s->pristine = (char *)bk_alloc(rw_hi - rw_lo);
		if (!s->pristine) return 0;
		bk_memcpy(s->pristine, (const void *)rw_lo, rw_hi - rw_lo);
	}
	lx_strlcpy(s->path, path, sizeof s->path);
	s->size = size;
	s->ino = ino;
	s->mtime = mtime;
	s->is_lib = lib;
	s->reg = *reg;
	s->base = base;
	s->len = len;
	s->rw_lo = rw_lo;
	s->rw_hi = rw_hi;
	s->used = ++simg_clock;
	if (lx_k.trace) bk_log("simg: %s, %u KB at %x, writable %x-%x", path, len / 1024, base, rw_lo, rw_hi);
	return s;
}

/* ---- each process's writable parts --------------------------------------- */

static int entry_of(lx_mm *mm, lx_simg *s)
{
	int i;
	for (i = 0; i < mm->nsw; i++) if (mm->sw[i].s == s) return i;
	return -1;
}

/* mm uses s: its own writable part starts as the file has it */
int lx_mm_share(lx_mm *mm, lx_simg *s)
{
	lx_u32 n = s->rw_hi - s->rw_lo;
	int k = entry_of(mm, s);
	if (k >= 0) { mm->sw[k].refs++; return 0; }
	if (mm->nsw >= LX_MM_SIMGS) return -LX_ENOSPC;    /* (the caller copies it privately) */
	k = mm->nsw;
	mm->sw[k].s = s;
	mm->sw[k].refs = 1;
	mm->sw[k].store = 0;
	if (n) {
		mm->sw[k].store = (char *)bk_alloc(n);
		if (!mm->sw[k].store) return -LX_ENOMEM;
		bk_memcpy(mm->sw[k].store, s->pristine, n);
	}
	mm->nsw++;
	s->users++;
	s->used = ++simg_clock;
	return 0;
}

static void entry_drop(lx_mm *mm, int k)
{
	lx_simg *s = mm->sw[k].s;
	if (s->owner == mm) s->owner = 0;
	bk_free(mm->sw[k].store);
	mm->sw[k] = mm->sw[mm->nsw - 1];
	mm->nsw--;
	if (--s->users <= 0) {
		s->users = 0;
		s->used = ++simg_clock;
		lx_simg_trim(0);
	}
}

/* mm no longer uses s (a library unmapped) */
void lx_mm_unshare(lx_mm *mm, lx_simg *s)
{
	int k = entry_of(mm, s);
	if (k < 0) return;
	if (--mm->sw[k].refs > 0) return;
	entry_drop(mm, k);
}

void lx_mm_unshare_all(lx_mm *mm)
{
	while (mm->nsw) entry_drop(mm, mm->nsw - 1);
}

int lx_mm_is_resident(lx_mm *mm)
{
	int i;
	if (!mm) return 1;
	for (i = 0; i < mm->nsw; i++) if (mm->sw[i].s->owner != mm) return 0;
	return 1;
}

/* mm's writable parts in place (another's go to its store first) */
void lx_mm_resident(lx_mm *mm)
{
	int i;
	if (!mm) return;
	for (i = 0; i < mm->nsw; i++) {
		lx_simg *s = mm->sw[i].s;
		lx_u32 n = s->rw_hi - s->rw_lo;
		if (s->owner == mm) continue;
		if (s->owner) {
			int k = entry_of(s->owner, s);
			if (k >= 0 && s->owner->sw[k].store) bk_memcpy(s->owner->sw[k].store, (const void *)s->rw_lo, n);
		}
		if (n) bk_memcpy((void *)s->rw_lo, mm->sw[i].store, n);
		s->owner = mm;
		lx_cpu.swaps++;
	}
}

/* Writes into mm's memory, even when mm's writable parts are not in place
 * (a fork's snapshot put back while another process holds the CPU) */
void lx_mm_write(lx_mm *mm, lx_u32 addr, const void *src, lx_u32 len)
{
	int i;
	if (mm) for (i = 0; i < mm->nsw; i++) {
		lx_simg *s = mm->sw[i].s;
		if (s->owner != mm && addr >= s->rw_lo && addr + len <= s->rw_hi) {
			bk_memcpy(mm->sw[i].store + (addr - s->rw_lo), src, len);
			return;
		}
	}
	bk_memcpy((void *)addr, src, len);
}

void lx_simg_stats(char *buf, int size)
{
	int i, o = 0;
	for (i = 0; i < NSIMG && o < size; i++) {
		lx_simg *s = &simgs[i];
		if (!s->len) continue;
		o += lx_snprintf(buf + o, size - o, "%-40s %5u KB at %08x, writable %u bytes, %d users\n",
			s->path, s->len / 1024, s->base, s->rw_hi - s->rw_lo, s->users);
	}
}
