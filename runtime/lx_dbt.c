/* lx_dbt.c - Alpine mode: the translator and the code cache (lx_dbt.h).
 *
 * A block is translated from its guest pc up to an unconditional transfer
 * of control (or 48 instructions). Its host code is laid out as
 *     the instructions | exit stubs | literal pool
 * An exit to a guest address that is translated already is a direct
 * branch; otherwise it branches to a stub that asks the runtime (DBT_CHAIN)
 * to translate the target and then patches the branch to go straight
 * there ("chaining"). Indirect transfers (returns, calls through pointers,
 * jump tables) put the guest target at [sp, #-4] and branch to the
 * dispatcher, which looks it up in lx_cpu.hash.
 *
 * Everything here runs on the thread that holds the CPU (lx_dbt.h), except
 * dbt_init and dbt_invalidate (the kernel thread), which only queue. */
#include "lx_dbt.h"

lx_cpu_t lx_cpu;

#define CACHE_MAX   (1024 * 1024)       /* the code cache's address space */
#define CACHE_STEP  (64 * 1024)         /* committed this much at a time */
#define BLOCK_ROOM  (6 * 1024)          /* the most one block can take */
#define MAXBLOCKS   16384
#define MAXN        48                  /* guest instructions in a block */
#define MAXWORDS    420                 /* host words of instructions in a block (literals must stay in reach) */
#define BH_SIZE     4096                /* the runtime's own hash of every block */

typedef struct dbt_block {
	lx_u32 gpc;                 /* guest pc */
	lx_u32 host;                /* its host code (it ends where the next block starts) */
	int next;                   /* the next block in its bh chain, -1 */
} dbt_block;

static lx_region cache;
static lx_u32 cache_top, cache_committed, cache_start;
static lx_u32 hash_tab[DBT_HASH_SIZE * 2];
static dbt_block *blocks;
static int nblocks;
static int bh[BH_SIZE];
static int ready;
static lx_u32 gen;                      /* +1 at every flush */
static lx_u32 tp_off;                   /* offsetof(lx_thread, tp) */

/* ranges of guest code that went away, from the kernel thread: dealt with
 * by the thread holding the CPU before it next looks anything up */
#define NINVAL 16
static volatile lx_u32 inval_lo[NINVAL], inval_hi[NINVAL];
static volatile int inval_n, inval_all;
static lx_u8 code_mb[4096 / 8];         /* the megabytes of address space with translated code */

int dbt_available(void) { return ready; }

static void hash_clear(void)
{
	int i;
	for (i = 0; i < (int)DBT_HASH_SIZE; i++) { hash_tab[2 * i] = DBT_EMPTY; hash_tab[2 * i + 1] = 0; }
	for (i = 0; i < BH_SIZE; i++) bh[i] = -1;
}

static void flush(void)
{
	hash_clear();
	nblocks = 0;
	cache_top = cache_start;
	gen++;
	lx_cpu.flushes++;
	/* memory short: the cache gives back what it had committed */
	if (lx_mem_avail() < 2u * 1024 * 1024 && cache_committed > CACHE_STEP && bk_code_grow(&cache, CACHE_STEP) == 0)
		cache_committed = CACHE_STEP;
	if (lx_k.trace) bk_log("dbt: code cache flushed (%u)", lx_cpu.flushes);
}

int dbt_init(void)
{
	lx_u32 *p, n, i;
	lx_thread *t0 = 0;
	if (ready) return 0;
	if (!lx_reserve_check(CACHE_STEP, 0) || bk_code_region(&cache, CACHE_MAX, CACHE_STEP) < 0) { bk_log("dbt: no memory for the code cache"); return -LX_ENOMEM; }
	cache_committed = CACHE_STEP;
	blocks = (dbt_block *)bk_alloc(MAXBLOCKS * sizeof(dbt_block));
	if (!blocks) { bk_region_free(&cache); return -LX_ENOMEM; }
	tp_off = (lx_u32)((char *)&t0->tp - (char *)t0);
	if (tp_off > 4095) { bk_log("dbt: lx_thread.tp at %u: out of reach", tp_off); return -LX_EINVAL; }
	/* the cache starts with a copy of the dispatcher and a veneer to
	 * dbt_enter, both in reach of a branch from any block */
	p = (lx_u32 *)cache.base;
	n = (lx_u32)((char *)dbt_dispatch_end - (char *)dbt_dispatch_start) / 4;
	for (i = 0; i < n; i++) p[i] = dbt_dispatch_start[i];
	p[n - 2] = (lx_u32)&lx_cpu;
	p[n - 1] = (lx_u32)&dbt_enter;
	lx_cpu.dispatch = (lx_u32)p;
	p += n;
	p[0] = 0xE51FF004;                  /* ldr pc, [pc, #-4] */
	p[1] = (lx_u32)&dbt_enter;
	lx_cpu.enter = (lx_u32)p;
	p += 2;
	cache_start = cache_top = (lx_u32)p;
	lx_cpu.cache_base = cache.base;
	lx_cpu.cache_end = cache.base + CACHE_MAX;
	lx_cpu.hash = hash_tab;
	hash_clear();
	ready = 1;
	bk_log("dbt: ready, code cache %u KB at %x", CACHE_MAX / 1024, cache.base);
	return 0;
}

/* ---- finding blocks ---------------------------------------------------------- */

static int bh_index(lx_u32 gpc) { return (int)((gpc >> 2) ^ (gpc >> 14)) & (BH_SIZE - 1); }

static int block_find(lx_u32 gpc)
{
	int b = bh[bh_index(gpc)];
	while (b >= 0 && blocks[b].gpc != gpc) b = blocks[b].next;
	return b;
}

static void hash_put(lx_u32 gpc, lx_u32 host)
{
	lx_u32 k = (gpc >> 2) & (DBT_HASH_SIZE - 1);
	hash_tab[2 * k] = DBT_EMPTY;        /* (never a half-made entry) */
	hash_tab[2 * k + 1] = host;
	hash_tab[2 * k] = gpc;
}

/* the block that holds host address h (blocks are made in address order) */
int dbt_block_of(lx_u32 h, lx_u32 *gpc, lx_u32 *host)
{
	int lo = 0, hi = nblocks - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		lx_u32 end = mid + 1 < nblocks ? blocks[mid + 1].host : cache_top;
		if (h < blocks[mid].host) hi = mid - 1;
		else if (h >= end) lo = mid + 1;
		else { *gpc = blocks[mid].gpc; *host = blocks[mid].host; return mid; }
	}
	return -1;
}

static void do_invalidations(void)
{
	int i, b, k;
	if (inval_all) { inval_all = 0; inval_n = 0; flush(); return; }
	if (!inval_n) return;
	for (k = 0; k < inval_n; k++) {
		lx_u32 lo = inval_lo[k], hi = inval_hi[k];
		for (i = 0; i < (int)DBT_HASH_SIZE; i++)
			if (hash_tab[2 * i] >= lo && hash_tab[2 * i] < hi) hash_tab[2 * i] = DBT_EMPTY;
		for (b = 0; b < nblocks; b++)
			if (blocks[b].gpc >= lo && blocks[b].gpc < hi) {
				/* out of the runtime's hash; its code stays until the next
				 * flush (only blocks of the same range branch into it) */
				int *pp = &bh[bh_index(blocks[b].gpc)];
				while (*pp >= 0 && *pp != b) pp = &blocks[*pp].next;
				if (*pp == b) *pp = blocks[b].next;
				blocks[b].gpc = DBT_EMPTY;
			}
	}
	inval_n = 0;
}

/* (the kernel thread) */
void dbt_invalidate(lx_u32 lo, lx_u32 hi)
{
	lx_u32 mb;
	if (!ready || hi <= lo) return;
	for (mb = lo >> 20; mb <= (hi - 1) >> 20; mb++)
		if (code_mb[mb >> 3] & (1 << (mb & 7))) break;
	if (mb > (hi - 1) >> 20) return;        /* (no code was translated there) */
	if (inval_n >= NINVAL) { inval_all = 1; return; }
	inval_lo[inval_n] = lo;
	inval_hi[inval_n] = hi;
	inval_n++;
}

/* ---- emitting ARM code --------------------------------------------------------- */

#define AL 0xE0000000u
#define NLIT 48
#define NEXIT 12

typedef struct em {
	lx_u32 *p;                  /* the next word */
	lx_u32 *start;
	lx_u32 lit[NLIT]; int nlit;
	lx_u32 *lref[NLIT * 2]; int lidx[NLIT * 2]; int nref;
	lx_u32 *xsite[NEXIT]; lx_u32 xtarget[NEXIT]; int nexit;
	int full;                   /* out of literal or exit slots: end the block */
} em;

static void E(em *e, lx_u32 w) { *e->p++ = w; }

static int lit_index(em *e, lx_u32 v)
{
	int i;
	for (i = 0; i < e->nlit; i++) if (e->lit[i] == v) return i;
	if (e->nlit >= NLIT) { e->full = 1; return 0; }
	e->lit[e->nlit] = v;
	return e->nlit++;
}

/* ldr<cond> rd, =v */
static void e_ldr_lit(em *e, lx_u32 cond, int rd, lx_u32 v)
{
	int k = lit_index(e, v);
	if (e->nref >= NLIT * 2) { e->full = 1; k = 0; }
	else { e->lref[e->nref] = e->p; e->lidx[e->nref] = k; e->nref++; }
	E(e, cond | 0x059F0000u | ((lx_u32)rd << 12));
}

static lx_u32 b_to(lx_u32 cond, lx_u32 *at, lx_u32 dest)
{
	return cond | 0x0A000000u | (((dest - ((lx_u32)at + 8)) >> 2) & 0x00FFFFFFu);
}

static void e_b(em *e, lx_u32 cond, lx_u32 dest) { E(e, b_to(cond, e->p, dest)); }

/* b<cond> to guest address target (a stub or the block, at the end) */
static void e_exit(em *e, lx_u32 cond, lx_u32 target)
{
	if (e->nexit >= NEXIT) { e->full = 1; }
	else { e->xsite[e->nexit] = e->p; e->xtarget[e->nexit] = target; e->nexit++; }
	E(e, cond | 0x0A000000u);
}

/* into the runtime: str r0, [sp, #-8]; mov r0, pc; b enter; {reason, a, b} */
static void e_enter(em *e, lx_u32 reason, lx_u32 a, lx_u32 b, int words)
{
	E(e, 0xE50D0008u);
	E(e, 0xE1A0000Fu);
	e_b(e, AL, lx_cpu.enter);
	E(e, reason);
	E(e, a);
	if (words > 2) E(e, b);
}

/* the guest target is at [sp, #-4]: to the dispatcher */
static void e_dispatch(em *e) { e_b(e, AL, lx_cpu.dispatch); }

static void e_str_below(em *e, int r, int off) { E(e, 0xE50D0000u | ((lx_u32)r << 12) | (lx_u32)off); }  /* str r, [sp, #-off] */
static void e_ldr_below(em *e, int r, int off) { E(e, 0xE51D0000u | ((lx_u32)r << 12) | (lx_u32)off); }  /* ldr r, [sp, #-off] */

/* a conditional sequence: b<not cond> over it; patched by skip_end */
static lx_u32 *skip_begin(em *e, lx_u32 cond)
{
	lx_u32 *at;
	if (cond == AL) return 0;
	at = e->p;
	E(e, (cond ^ 0x10000000u) | 0x0A000000u);
	return at;
}
static void skip_end(em *e, lx_u32 *at)
{
	if (at) *at = b_to(*at & 0xF0000000u, at, (lx_u32)e->p);
}

/* a register in r0-r12 that is none of the mask's */
static int free_reg(lx_u32 used)
{
	int r;
	for (r = 0; r <= 12; r++) if (!(used & (1u << r))) return r;
	return -1;
}

/* (debugging, DBT_SLOW_FIXUP) the access through the runtime, as a site
 * patched after an alignment fault does it (dbt_patch_unaligned) */
static int via_fixup(em *e, lx_u32 i)
{
	lx_u32 *s = skip_begin(e, i & 0xF0000000u);
	e_enter(e, DBT_FIXUP, (i & 0x0FFFFFFFu) | AL, 0, 3);
	e->p[-1] = (lx_u32)e->p;
	skip_end(e, s);
	return 0;
}

/* ---- the instructions -------------------------------------------------------- */

enum { GO_ON = 0, ENDED = 1 };

/* interpreted by the runtime (lx_dbt_rt.c), then on with the next instruction */
static int helper(em *e, lx_u32 pc, lx_u32 i)
{
	lx_u32 *s = skip_begin(e, i & 0xF0000000u);
	e_enter(e, DBT_HELPER, pc, 0, 2);
	skip_end(e, s);
	return GO_ON;
}

static int undef(em *e, lx_u32 pc, lx_u32 i)
{
	(void)i;
	e_enter(e, DBT_UNDEF, pc, 0, 2);
	return ENDED;
}

/* an instruction that writes pc in a way not done inline: the runtime
 * interprets it and goes on from the pc it left (DBT_HELPER + dispatch) */
static int branch_helper(em *e, lx_u32 pc, lx_u32 i)
{
	lx_u32 cond = i & 0xF0000000u;
	lx_u32 *s = skip_begin(e, cond);
	e_enter(e, DBT_HELPER, pc, 0, 2);   /* (the runtime returns to the new pc's block) */
	skip_end(e, s);
	if (cond != AL) { e_exit(e, AL, pc + 4); }
	return ENDED;
}

/* the end of an indirect transfer: [cond] ... b dispatch, then the
 * fall-through when the condition failed */
static int indirect_end(em *e, lx_u32 pc, lx_u32 *s)
{
	e_dispatch(e);
	if (s) { skip_end(e, s); e_exit(e, AL, pc + 4); }
	return ENDED;
}

/* data processing, with pc as an operand or the destination */
static int xl_dp(em *e, lx_u32 pc, lx_u32 i)
{
	lx_u32 cond = i & 0xF0000000u;
	int op = (int)(i >> 21) & 15, rn = (int)(i >> 16) & 15, rd = (int)(i >> 12) & 15;
	int imm = (int)(i >> 25) & 1, regsh = !imm && ((i >> 4) & 1), rm = (int)(i & 15), rs = (int)(i >> 8) & 15;
	int test = op >= 8 && op <= 11, mov = op == 13 || op == 15;
	int usen = !mov, usem = !imm;
	int pcuse = (usen && rn == 15) || (usem && rm == 15) || (regsh && rs == 15);
	int pcdst = !test && rd == 15;
	lx_u32 pcv = pc + (regsh ? 12 : 8), used, ni;
	int T;
	if (!pcuse && !pcdst) { E(e, i); return GO_ON; }
	if (regsh && (rs == 15 || rd == 15)) return undef(e, pc, i);
	used = (usen ? 1u << rn : 0) | (usem ? 1u << rm : 0) | (regsh ? 1u << rs : 0);
	if (!pcdst) {
		if (rd == 13) return helper(e, pc, i);
		/* pc as an operand: the value in a register first */
		if (!test && !(used & (1u << rd))) {
			e_ldr_lit(e, cond, rd, pcv);
			ni = i;
			if (usen && rn == 15) ni = (ni & ~0x000F0000u) | ((lx_u32)rd << 16);
			if (usem && rm == 15) ni = (ni & ~0xFu) | (lx_u32)rd;
			E(e, ni);
			return GO_ON;
		}
		T = free_reg(used | (1u << rd) | (1u << 15));
		e_str_below(e, T, 4);
		e_ldr_lit(e, AL, T, pcv);
		ni = i;
		if (usen && rn == 15) ni = (ni & ~0x000F0000u) | ((lx_u32)T << 16);
		if (usem && rm == 15) ni = (ni & ~0xFu) | (lx_u32)T;
		E(e, ni);
		e_ldr_below(e, T, 4);
		return GO_ON;
	}
	/* pc is the destination: a branch to a computed address */
	{
		lx_u32 *s = skip_begin(e, cond);
		if (op == 13 && !imm && !regsh && ((i >> 4) & 0xFF) == 0 && rm != 15) {
			/* mov pc, rm */
			e_str_below(e, rm, 4);
			return indirect_end(e, pc, s);
		}
		T = free_reg(used | (1u << 13) | (1u << 15));
		e_str_below(e, T, 8);
		ni = (i & 0x0FEFFFFFu) | AL;       /* (no S: flags are not set by a branch) */
		ni = (ni & ~0x0000F000u) | ((lx_u32)T << 12);
		if (pcuse) {
			e_ldr_lit(e, AL, T, pcv);
			if (usen && rn == 15) ni = (ni & ~0x000F0000u) | ((lx_u32)T << 16);
			if (usem && rm == 15) ni = (ni & ~0xFu) | (lx_u32)T;
		}
		E(e, ni);
		e_str_below(e, T, 4);
		e_ldr_below(e, T, 8);
		return indirect_end(e, pc, s);
	}
}

/* ldr/str (word, byte) */
static int xl_ls(em *e, lx_u32 pc, lx_u32 i)
{
	lx_u32 cond = i & 0xF0000000u;
	int I = (int)(i >> 25) & 1, P = (int)(i >> 24) & 1, U = (int)(i >> 23) & 1, W = (int)(i >> 21) & 1, L = (int)(i >> 20) & 1;
	int rn = (int)(i >> 16) & 15, rd = (int)(i >> 12) & 15, rm = (int)(i & 15);
	if (I && rm == 15) return undef(e, pc, i);
	if (rn != 15 && rd != 15) {
		if ((lx_cpu.slow & DBT_SLOW_FIXUP) && !((i >> 22) & 1) && !(I && (i & 0x10))) { via_fixup(e, i); return GO_ON; }
		E(e, i);
		return GO_ON;
	}
	if (rd == 15 && !L) return helper(e, pc, i);           /* str pc: rare */
	if (rd == 15) {
		/* a load into pc */
		lx_u32 *s;
		if (((i >> 22) & 1)) return undef(e, pc, i);       /* ldrb pc */
		s = skip_begin(e, cond);
		if (rn == 13 && !I && !P && U && (i & 0xFFF) == 4) {
			/* ldr pc, [sp], #4: the target is then at [sp, #-4] */
			E(e, 0xE28DD004u);                             /* add sp, sp, #4 */
			return indirect_end(e, pc, s);
		}
		if (rn == 13 && (!P || W)) { if (s) skip_end(e, s); return branch_helper(e, pc, i); }
		{
			int T = free_reg((1u << rn) | (I ? 1u << rm : 0) | (1u << 13) | (1u << 15));
			lx_u32 ni = (i & 0x0FFF0FFFu) | AL | ((lx_u32)T << 12);
			e_str_below(e, T, 8);
			if (rn == 15) {
				if (!P || W) { /* (writeback to pc) */ }
				e_ldr_lit(e, AL, T, pc + 8);
				ni = (ni & ~0x000F0000u & ~0x00200000u) | ((lx_u32)T << 16);
			}
			E(e, ni);
			e_str_below(e, T, 4);
			e_ldr_below(e, T, 8);
			return indirect_end(e, pc, s);
		}
	}
	/* rn == 15: pc-relative */
	if (!P || W) return undef(e, pc, i);
	if (!L) return helper(e, pc, i);                       /* a store into the code */
	if (!I) {
		lx_u32 off = i & 0xFFF, addr = U ? pc + 8 + off : pc + 8 - off;
		e_ldr_lit(e, cond, rd, addr);
		E(e, cond | 0x05900000u | (i & 0x00400000u) | ((lx_u32)rd << 16) | ((lx_u32)rd << 12));   /* ldr{b} rd, [rd] */
		return GO_ON;
	}
	if (rd != rm) {
		e_ldr_lit(e, cond, rd, pc + 8);
		E(e, (i & ~0x000F0000u) | ((lx_u32)rd << 16));
		return GO_ON;
	}
	return helper(e, pc, i);
}

/* ldrh/strh/ldrsb/ldrsh, ldrd/strd */
static int xl_extra(em *e, lx_u32 pc, lx_u32 i)
{
	lx_u32 cond = i & 0xF0000000u;
	int P = (int)(i >> 24) & 1, U = (int)(i >> 23) & 1, I = (int)(i >> 22) & 1, W = (int)(i >> 21) & 1, L = (int)(i >> 20) & 1;
	int rn = (int)(i >> 16) & 15, rd = (int)(i >> 12) & 15, rm = (int)(i & 15), sh = (int)(i >> 5) & 3;
	lx_u32 off = ((i >> 4) & 0xF0) | (i & 0xF);
	if (!L && sh != 1) {
		/* ldrd (sh 2) / strd (sh 3): two words */
		int r2 = rd + 1;
		if (rd & 1 || rd == 14) return undef(e, pc, i);
		if (rn == 15 || (!I && rm == 15)) return helper(e, pc, i);
		if (I && P && !W) {
			lx_u32 base = (cond) | 0x05000000u | ((lx_u32)rn << 16) | (U ? 0x00800000u : 0) | 0x01000000u;
			lx_u32 o1 = off, o2 = U ? off + 4 : off - 4;
			lx_u32 *s = skip_begin(e, cond);
			base = (base & 0x0FFFFFFFu) | AL;
			if (sh == 2) {
				/* ldrd: the base last if it is one of the pair */
				if (rn == rd) {
					if (!U && off < 4) { if (s) skip_end(e, s); return helper(e, pc, i); }
					E(e, base | 0x00100000u | ((lx_u32)r2 << 12) | (U ? o2 : o2));
					E(e, base | 0x00100000u | ((lx_u32)rd << 12) | o1);
				} else {
					E(e, base | 0x00100000u | ((lx_u32)rd << 12) | o1);
					if (!U && off < 4) { /* the second word is above the base */
						E(e, (base & ~0x00800000u) | 0x00800000u | 0x00100000u | ((lx_u32)r2 << 12) | (4 - off));
					} else E(e, base | 0x00100000u | ((lx_u32)r2 << 12) | o2);
				}
			} else {
				E(e, base | ((lx_u32)rd << 12) | o1);
				if (!U && off < 4) E(e, (base & ~0x00800000u) | 0x00800000u | ((lx_u32)r2 << 12) | (4 - off));
				else E(e, base | ((lx_u32)r2 << 12) | o2);
			}
			skip_end(e, s);
			return GO_ON;
		}
		if (I && !P && !W && U && rn != rd && rn != r2 && rn != 15 && off >= 4) {
			/* post-index: ldrd rd, [rn], #off  ->  ldr rd, [rn], #4; ldr r2, [rn], #off-4 */
			lx_u32 L1 = sh == 2 ? 0x00100000u : 0;
			E(e, cond | 0x04800000u | L1 | ((lx_u32)rn << 16) | ((lx_u32)rd << 12) | 4);
			E(e, cond | 0x04800000u | L1 | ((lx_u32)rn << 16) | ((lx_u32)r2 << 12) | (off - 4));
			return GO_ON;
		}
		if (I && P && W && U && rn != rd && rn != r2 && rn != 15) {
			/* pre-index with writeback: ldr rd, [rn, #off]!; ldr r2, [rn, #4] */
			lx_u32 L1 = sh == 2 ? 0x00100000u : 0;
			E(e, cond | 0x05A00000u | L1 | ((lx_u32)rn << 16) | ((lx_u32)rd << 12) | off);
			E(e, cond | 0x05800000u | L1 | ((lx_u32)rn << 16) | ((lx_u32)r2 << 12) | 4);
			return GO_ON;
		}
		return helper(e, pc, i);
	}
	if (rd == 15) return undef(e, pc, i);
	if (rn == 15) {
		if (!P || W || !L) return helper(e, pc, i);
		if (I) {
			lx_u32 addr = U ? pc + 8 + off : pc + 8 - off;
			e_ldr_lit(e, cond, rd, addr);
			E(e, (i & 0xF0000060u) | 0x01D00090u | ((lx_u32)rd << 16) | ((lx_u32)rd << 12));   /* ldrh/sb/sh rd, [rd] */
			return GO_ON;
		}
		return helper(e, pc, i);
	}
	if (!I && rm == 15) return undef(e, pc, i);
	if ((lx_cpu.slow & DBT_SLOW_FIXUP) && (sh == 1 || (L && sh == 3))) { via_fixup(e, i); return GO_ON; }
	E(e, i);
	return GO_ON;
}

/* ldm/stm */
static int xl_ldm(em *e, lx_u32 pc, lx_u32 i)
{
	lx_u32 cond = i & 0xF0000000u, list = i & 0xFFFF;
	int P = (int)(i >> 24) & 1, U = (int)(i >> 23) & 1, S = (int)(i >> 22) & 1, W = (int)(i >> 21) & 1, L = (int)(i >> 20) & 1;
	int rn = (int)(i >> 16) & 15, n = 0, r;
	if (rn == 15 || !list) return undef(e, pc, i);
	if (S) return undef(e, pc, i);                       /* (user-mode registers: not for a program) */
	if (!(list & 0x8000)) { E(e, i); return GO_ON; }
	if (!L) return helper(e, pc, i);                     /* stm with pc */
	for (r = 0; r < 16; r++) if (list & (1u << r)) n++;
	{
		lx_u32 *s = skip_begin(e, cond);
		lx_u32 rest = list & 0x7FFF;
		if (rn == 13 && !P && U && W) {
			/* ldmia sp!, {..., pc}: a return */
			if (rest) E(e, (i & 0x0FFF0000u) | AL | rest);
			E(e, 0xE28DD004u);                       /* add sp, sp, #4 */
			return indirect_end(e, pc, s);
		}
		if (rn == 13 || (W && (rest & (1u << rn)))) { if (s) skip_end(e, s); return branch_helper(e, pc, i); }
		{
			/* the pc word's offset from the base */
			int start = U ? (P ? 4 : 0) : (P ? -4 * n : -4 * n + 4);
			int pcoff = start + 4 * (n - 1);
			int T = free_reg(rest | (1u << rn) | (1u << 13) | (1u << 15));
			if (T < 0) { if (s) skip_end(e, s); return branch_helper(e, pc, i); }
			e_str_below(e, T, 8);
			if (pcoff >= 0) E(e, 0xE5900000u | ((lx_u32)rn << 16) | ((lx_u32)T << 12) | (lx_u32)pcoff);
			else E(e, 0xE5100000u | ((lx_u32)rn << 16) | ((lx_u32)T << 12) | (lx_u32)(-pcoff));
			e_str_below(e, T, 4);
			e_ldr_below(e, T, 8);
			if (rest) {
				/* the others, with the base moved as the whole instruction would */
				lx_u32 ni = (i & 0x0FFF0000u) | AL | rest;
				if (W) {
					/* writeback counted pc too: one more word */
					if (U && !P) { E(e, ni & ~0x00200000u); E(e, 0xE2800000u | ((lx_u32)rn << 16) | ((lx_u32)rn << 12) | (lx_u32)(4 * n)); }
					else if (U && P) { E(e, ni & ~0x00200000u); E(e, 0xE2800000u | ((lx_u32)rn << 16) | ((lx_u32)rn << 12) | (lx_u32)(4 * n)); }
					else { E(e, ni & ~0x00200000u); E(e, 0xE2400000u | ((lx_u32)rn << 16) | ((lx_u32)rn << 12) | (lx_u32)(4 * n)); }
				} else E(e, ni);
			} else if (W) {
				if (U) E(e, 0xE2800000u | ((lx_u32)rn << 16) | ((lx_u32)rn << 12) | (lx_u32)(4 * n));
				else E(e, 0xE2400000u | ((lx_u32)rn << 16) | ((lx_u32)rn << 12) | (lx_u32)(4 * n));
			}
			return indirect_end(e, pc, s);
		}
	}
}

/* clz without flags (the ARM710T has none): smear the bits below the top
 * one, invert, and count the ones that are left. One spare register:
 * (v & m) + ((v >> 2) & m) is v - 3 * ((v >> 2) & m). */
static int xl_clz(em *e, lx_u32 i, int rd, int rm)
{
	lx_u32 cond = i & 0xF0000000u, *s = skip_begin(e, cond);
	int U = free_reg((1u << rd) | (1u << rm) | (1u << 13) | (1u << 15));
	lx_u32 D = (lx_u32)rd, Ub = (lx_u32)U;
	e_str_below(e, U, 4);
	E(e, AL | 0x01800020u | ((lx_u32)rm << 16) | (D << 12) | (1u << 7) | (lx_u32)rm);   /* orr rd, rm, rm, lsr #1 */
	E(e, AL | 0x01800020u | (D << 16) | (D << 12) | (2u << 7) | D);                     /* orr rd, rd, rd, lsr #2 */
	E(e, AL | 0x01800020u | (D << 16) | (D << 12) | (4u << 7) | D);                     /* ...#4 */
	E(e, AL | 0x01800020u | (D << 16) | (D << 12) | (8u << 7) | D);                     /* ...#8 */
	E(e, AL | 0x01800020u | (D << 16) | (D << 12) | (16u << 7) | D);                    /* ...#16 */
	E(e, AL | 0x01E00000u | (D << 12) | D);                                             /* mvn rd, rd: count its ones */
	e_ldr_lit(e, AL, U, 0x55555555u);
	E(e, AL | 0x00000020u | (Ub << 16) | (Ub << 12) | (1u << 7) | D);                   /* and U, U, rd, lsr #1 */
	E(e, AL | 0x00400000u | (D << 16) | (D << 12) | Ub);                                /* sub rd, rd, U */
	e_ldr_lit(e, AL, U, 0x33333333u);
	E(e, AL | 0x00000020u | (Ub << 16) | (Ub << 12) | (2u << 7) | D);                   /* and U, U, rd, lsr #2 */
	E(e, AL | 0x00400000u | (D << 16) | (D << 12) | (2u << 7) | Ub);                    /* sub rd, rd, U, lsl #2 */
	E(e, AL | 0x00800000u | (D << 16) | (D << 12) | Ub);                                /* add rd, rd, U */
	E(e, AL | 0x00800020u | (D << 16) | (D << 12) | (4u << 7) | D);                     /* add rd, rd, rd, lsr #4 */
	e_ldr_lit(e, AL, U, 0x0F0F0F0Fu);
	E(e, AL | 0x00000000u | (D << 16) | (D << 12) | Ub);                                /* and rd, rd, U */
	E(e, AL | 0x00800000u | (D << 16) | (D << 12) | (8u << 7) | D);                     /* add rd, rd, rd, lsl #8 */
	E(e, AL | 0x00800000u | (D << 16) | (D << 12) | (16u << 7) | D);                    /* add rd, rd, rd, lsl #16 */
	E(e, AL | 0x01A00020u | (D << 12) | (24u << 7) | D);                                /* mov rd, rd, lsr #24 */
	e_ldr_below(e, U, 4);
	skip_end(e, s);
	return GO_ON;
}

/* smla<x><y> rd, rn, rm, ra / smul<x><y> rd, rn, rm: the chosen halves
 * sign-extended into two spare registers, then ARMv4's mla or mul. (The Q
 * flag is not set when smla's addition overflows: DECISIONS.md 39.) */
static int xl_smla(em *e, lx_u32 pc, lx_u32 i, int mul)
{
	lx_u32 cond = i & 0xF0000000u, *s;
	int rd = (int)(i >> 16) & 15, ra = (int)(i >> 12) & 15, rm = (int)(i >> 8) & 15, rn = (int)(i & 15);
	int x = (int)(i >> 5) & 1, y = (int)(i >> 6) & 1, T, U;
	lx_u32 used = (1u << rd) | (1u << rn) | (1u << rm) | (mul ? 0 : 1u << ra) | (1u << 13) | (1u << 15);
	if (rd == 15 || rn == 15 || rm == 15 || (!mul && ra == 15)) return undef(e, pc, i);
	T = free_reg(used);
	U = free_reg(used | (1u << T));
	s = skip_begin(e, cond);
	e_str_below(e, T, 4);
	e_str_below(e, U, 8);
	if (x) E(e, AL | 0x01A00040u | ((lx_u32)T << 12) | (16u << 7) | (lx_u32)rn);           /* mov T, rn, asr #16 */
	else {
		E(e, AL | 0x01A00000u | ((lx_u32)T << 12) | (16u << 7) | (lx_u32)rn);              /* mov T, rn, lsl #16 */
		E(e, AL | 0x01A00040u | ((lx_u32)T << 12) | (16u << 7) | (lx_u32)T);               /* mov T, T, asr #16 */
	}
	if (y) E(e, AL | 0x01A00040u | ((lx_u32)U << 12) | (16u << 7) | (lx_u32)rm);
	else {
		E(e, AL | 0x01A00000u | ((lx_u32)U << 12) | (16u << 7) | (lx_u32)rm);
		E(e, AL | 0x01A00040u | ((lx_u32)U << 12) | (16u << 7) | (lx_u32)U);
	}
	if (mul) E(e, AL | 0x00000090u | ((lx_u32)rd << 16) | ((lx_u32)U << 8) | (lx_u32)T);   /* mul rd, T, U */
	else E(e, AL | 0x00200090u | ((lx_u32)rd << 16) | ((lx_u32)ra << 12) | ((lx_u32)U << 8) | (lx_u32)T);   /* mla rd, T, U, ra */
	e_ldr_below(e, U, 8);
	e_ldr_below(e, T, 4);
	skip_end(e, s);
	return GO_ON;
}

/* the "miscellaneous" space: mrs/msr, bx, blx, clz, bkpt, the DSP multiplies */
static int xl_misc(em *e, lx_u32 pc, lx_u32 i)
{
	lx_u32 cond = i & 0xF0000000u;
	int op = (int)(i >> 21) & 3, rm = (int)(i & 15);
	switch ((i >> 4) & 0xF) {
	case 0x0:
		/* mrs and msr: the Q and GE flags are kept in the thread (an
		 * ARMv4 CPSR has neither), so the runtime does them */
		if (i & 0x00400000u) return undef(e, pc, i);   /* the spsr */
		return helper(e, pc, i);
	case 0x1:
		if (op == 1 || op == 1) {
			if (op == 1) {                       /* bx rm */
				lx_u32 *s = skip_begin(e, cond);
				e_str_below(e, rm, 4);
				return indirect_end(e, pc, s);
			}
		}
		if (op == 3) {                               /* clz rd, rm */
			int rd = (int)(i >> 12) & 15;
			if (rd == 15 || rm == 15) return undef(e, pc, i);
			return xl_clz(e, i, rd, rm);
		}
		return undef(e, pc, i);
	case 0x2:
		if (op == 1) {                               /* bxj: as bx */
			lx_u32 *s = skip_begin(e, cond);
			e_str_below(e, rm, 4);
			return indirect_end(e, pc, s);
		}
		return undef(e, pc, i);
	case 0x3:
		if (op == 1) {                               /* blx rm */
			lx_u32 *s = skip_begin(e, cond);
			if (rm == 15) return undef(e, pc, i);
			e_str_below(e, rm, 4);
			e_ldr_lit(e, AL, 14, pc + 4);
			return indirect_end(e, pc, s);
		}
		return undef(e, pc, i);
	case 0x5:
		return helper(e, pc, i);                     /* qadd, qsub, qdadd, qdsub */
	case 0x7:
		if (op == 1) { e_enter(e, DBT_UNDEF, pc, 0, 2); return ENDED; }   /* bkpt: SIGTRAP in the runtime */
		return undef(e, pc, i);
	default:
		if ((i & 0x90) == 0x80) {
			/* smlaxy (op 0) and smulxy (op 3) inline (apk's hashing does
			 * millions); smlaw, smulw, smlalxy to the interpreter */
			if (op == 0 || op == 3) return xl_smla(e, pc, i, op == 3);
			return helper(e, pc, i);
		}
		return undef(e, pc, i);
	}
}

/* v6 media instructions (011x xxxx ... 1 ....) */
static int xl_media(em *e, lx_u32 pc, lx_u32 i)
{
	lx_u32 cond = i & 0xF0000000u;
	int rd = (int)(i >> 12) & 15, rm = (int)(i & 15), rn = (int)(i >> 16) & 15;
	int rot = (int)(i >> 10) & 3;
	if (i & 0x01000000u) return helper(e, pc, i);     /* 0111: the signed multiplies, usad8, the bit fields */
	if (rd == 15 || rm == 15) return undef(e, pc, i);
	if ((i & 0x0F8000F0u) == 0x06800070u && ((i >> 20) & 7) != 0 && ((i >> 20) & 7) != 4) {
		/* sxtb/sxth/uxtb/uxth (rn = 15) and the add forms */
		int op = (int)(i >> 20) & 7;            /* 2 sxtb 3 sxth 6 uxtb 7 uxth */
		lx_u32 RD = (lx_u32)rd << 12, RM = (lx_u32)rm;
		if (rn == 15) {
			lx_u32 *s = skip_begin(e, cond);
			switch (op) {
			case 6:     /* uxtb */
				if (rot == 0) E(e, AL | 0x02000000u | (0u << 21) | ((lx_u32)rm << 16) | RD | 0xFF);   /* and rd, rm, #255 */
				else if (rot == 3) E(e, AL | 0x01A00020u | RD | (24u << 7) | RM);    /* mov rd, rm, lsr #24 */
				else { E(e, AL | 0x01A00020u | RD | ((lx_u32)(rot * 8) << 7) | RM); E(e, AL | 0x02000000u | ((lx_u32)rd << 16) | RD | 0xFF); }
				break;
			case 7:     /* uxth */
				if (rot == 2) E(e, AL | 0x01A00020u | RD | (16u << 7) | RM);         /* mov rd, rm, lsr #16 */
				else if (rot == 0) { E(e, AL | 0x01A00000u | RD | (16u << 7) | RM); E(e, AL | 0x01A00020u | RD | (16u << 7) | (lx_u32)rd); }
				else if (rot == 1) { E(e, AL | 0x01A00000u | RD | (8u << 7) | RM); E(e, AL | 0x01A00020u | RD | (16u << 7) | (lx_u32)rd); }
				else { if (s) skip_end(e, s); return helper(e, pc, i); }
				break;
			case 2:     /* sxtb */
				if (rot == 3) E(e, AL | 0x01A00040u | RD | (24u << 7) | RM);         /* mov rd, rm, asr #24 */
				else { E(e, AL | 0x01A00000u | RD | ((lx_u32)(24 - rot * 8) << 7) | RM); E(e, AL | 0x01A00040u | RD | (24u << 7) | (lx_u32)rd); }
				break;
			case 3:     /* sxth */
				if (rot == 2) E(e, AL | 0x01A00040u | RD | (16u << 7) | RM);         /* mov rd, rm, asr #16 */
				else if (rot == 0 || rot == 1) { E(e, AL | 0x01A00000u | RD | ((lx_u32)(16 - rot * 8) << 7) | RM); E(e, AL | 0x01A00040u | RD | (16u << 7) | (lx_u32)rd); }
				else { if (s) skip_end(e, s); return helper(e, pc, i); }
				break;
			default:
				if (s) skip_end(e, s);
				return helper(e, pc, i);
			}
			skip_end(e, s);
			return GO_ON;
		}
		if ((op == 6 || op == 7) && rot == 0 && rn != 15) {
			/* uxtab/uxtah rd, rn, rm: the extension into a register that is not rn */
			int T = rd != rn ? rd : free_reg((1u << rn) | (1u << rm) | (1u << rd) | (1u << 13) | (1u << 15));
			lx_u32 *s = skip_begin(e, cond), RT = (lx_u32)T << 12;
			if (T != rd) e_str_below(e, T, 4);
			if (op == 6) E(e, AL | 0x02000000u | ((lx_u32)rm << 16) | RT | 0xFF);
			else { E(e, AL | 0x01A00000u | RT | (16u << 7) | RM); E(e, AL | 0x01A00020u | RT | (16u << 7) | (lx_u32)T); }
			E(e, AL | 0x00800000u | ((lx_u32)rn << 16) | RD | (lx_u32)T);                 /* add rd, rn, T */
			if (T != rd) e_ldr_below(e, T, 4);
			skip_end(e, s);
			return GO_ON;
		}
		return helper(e, pc, i);
	}
	if ((i & 0x0FFF0FF0u) == 0x06BF0F30u) {
		/* rev rd, rm */
		int T = free_reg((1u << rd) | (1u << rm) | (1u << 13) | (1u << 15));
		lx_u32 *s = skip_begin(e, cond), RT = (lx_u32)T, RD = (lx_u32)rd << 12;
		e_str_below(e, T, 4);
		E(e, AL | 0x00200000u | ((lx_u32)rm << 16) | (RT << 12) | (16u << 7) | 0x60u | (lx_u32)rm);   /* eor T, rm, rm, ror #16 */
		E(e, AL | 0x03C00800u | (RT << 16) | (RT << 12) | 0xFFu);                                       /* bic T, T, #0x00ff0000 */
		E(e, AL | 0x01A00060u | RD | (8u << 7) | (lx_u32)rm);                                          /* mov rd, rm, ror #8 */
		E(e, AL | 0x00200020u | ((lx_u32)rd << 16) | RD | (8u << 7) | RT);                              /* eor rd, rd, T, lsr #8 */
		e_ldr_below(e, T, 4);
		skip_end(e, s);
		return GO_ON;
	}
	return helper(e, pc, i);
}

/* coprocessor space */
static int xl_cop(em *e, lx_u32 pc, lx_u32 i)
{
	int cp = (int)(i >> 8) & 15;
	lx_u32 cond = i & 0xF0000000u;
	if (cp == 10 || cp == 11) return helper(e, pc, i);   /* VFP */
	if (cp == 15 && (i & 0x0F000010u) == 0x0E000010u) {
		int L = (int)(i >> 20) & 1, crn = (int)(i >> 16) & 15, rt = (int)(i >> 12) & 15, crm = (int)(i & 15);
		int op1 = (int)(i >> 21) & 7, op2 = (int)(i >> 5) & 7;
		if (op1 == 0 && crn == 13 && crm == 0 && L && op2 == 3 && rt != 15 && rt != 13) {
			/* mrc p15, 0, rt, c13, c0, 3: the thread pointer */
			e_ldr_lit(e, cond, rt, (lx_u32)&lx_cpu.cur);
			E(e, cond | 0x05900000u | ((lx_u32)rt << 16) | ((lx_u32)rt << 12));          /* ldr rt, [rt] */
			E(e, cond | 0x05900000u | ((lx_u32)rt << 16) | ((lx_u32)rt << 12) | tp_off);  /* ldr rt, [rt, #tp] */
			return GO_ON;
		}
		if (op1 == 0 && crn == 7 && !L && ((crm == 10 && (op2 == 4 || op2 == 5)) || (crm == 5 && op2 == 4)))
			return GO_ON;                                 /* dsb, dmb, isb (cp15): nothing on one CPU */
		if (op1 == 0 && crn == 13 && crm == 0 && op2 == 2) return helper(e, pc, i);   /* tpidrurw */
	}
	return undef(e, pc, i);
}

/* the unconditional space (cond 1111) */
static int xl_uncond(em *e, lx_u32 pc, lx_u32 i)
{
	if ((i & 0xFD70F000u) == 0xF550F000u) return GO_ON;          /* pld */
	if (i == 0xF57FF01Fu) return GO_ON;                          /* clrex */
	if ((i & 0xFFFFFFF0u) == 0xF57FF040u || (i & 0xFFFFFFF0u) == 0xF57FF050u || (i & 0xFFFFFFF0u) == 0xF57FF060u)
		return GO_ON;                                            /* dsb, dmb, isb */
	if ((i & 0xFFFFFDFFu) == 0xF1010000u) {                      /* setend */
		if (i & 0x200) return undef(e, pc, i);
		return GO_ON;
	}
	return undef(e, pc, i);                                      /* blx (Thumb), cps, srs, rfe... */
}

/* ldrex/strex and friends: one translated thread runs at a time, and the
 * kernel moves the CPU only at a system call or a dispatcher yield, never
 * between the two: so a plain load, and a store that always succeeds */
static int xl_excl(em *e, lx_u32 pc, lx_u32 i)
{
	lx_u32 cond = i & 0xF0000000u;
	int op = (int)(i >> 20) & 0xF, rn = (int)(i >> 16) & 15, rd = (int)(i >> 12) & 15, rt = (int)(i & 15);
	lx_u32 RN = (lx_u32)rn << 16;
	if (rn == 15 || rd == 15) return undef(e, pc, i);
	switch (op) {
	case 0x9: E(e, cond | 0x05900000u | RN | ((lx_u32)rd << 12)); return GO_ON;           /* ldrex -> ldr rd, [rn] */
	case 0xD: E(e, cond | 0x05D00000u | RN | ((lx_u32)rd << 12)); return GO_ON;           /* ldrexb -> ldrb */
	case 0xF: E(e, cond | 0x01D000B0u | RN | ((lx_u32)rd << 12)); return GO_ON;           /* ldrexh -> ldrh */
	case 0xB:                                                                              /* ldrexd */
		if (rd & 1 || rd == 14 || rn == rd || rn == rd + 1) return helper(e, pc, i);
		E(e, cond | 0x05900000u | RN | ((lx_u32)rd << 12));
		E(e, cond | 0x05900004u | RN | ((lx_u32)(rd + 1) << 12));
		return GO_ON;
	case 0x8: case 0xC: case 0xE: case 0xA:
		if (rt == 15 || rd == rn || rd == rt) return undef(e, pc, i);
		if (op == 0x8) E(e, cond | 0x05800000u | RN | ((lx_u32)rt << 12));                /* strex -> str rt, [rn] */
		else if (op == 0xC) E(e, cond | 0x05C00000u | RN | ((lx_u32)rt << 12));           /* strexb */
		else if (op == 0xE) E(e, cond | 0x01C000B0u | RN | ((lx_u32)rt << 12));           /* strexh */
		else {
			if (rt & 1 || rt == 14) return undef(e, pc, i);
			E(e, cond | 0x05800000u | RN | ((lx_u32)rt << 12));
			E(e, cond | 0x05800004u | RN | ((lx_u32)(rt + 1) << 12));
		}
		E(e, cond | 0x03A00000u | ((lx_u32)rd << 12));                                    /* mov rd, #0: it worked */
		return GO_ON;
	}
	return undef(e, pc, i);
}

/* (debugging) the instruction's class, for lx_cpu.slow */
static int slow_class(lx_u32 i)
{
	switch ((i >> 25) & 7) {
	case 0:
		if ((i & 0x90) == 0x90) {
			if ((i & 0x0F0000F0u) == 0x00000090u) return DBT_SLOW_MUL;
			if ((i & 0x0F0000F0u) == 0x01000090u) return 0;
			return DBT_SLOW_EXTRA;
		}
		if ((i & 0x01900000u) == 0x01000000u) return DBT_SLOW_MISC;
		return DBT_SLOW_DP;
	case 1: return (i & 0x01900000u) == 0x01000000u ? 0 : DBT_SLOW_DP;
	case 2: return DBT_SLOW_LS;
	case 3: return (i & 0x10) ? DBT_SLOW_MEDIA : DBT_SLOW_LS;
	case 4: return DBT_SLOW_LDM;
	}
	return 0;
}

static int writes_pc(lx_u32 i)
{
	switch ((i >> 25) & 7) {
	case 0: case 1:
		if ((i & 0x0E000090u) == 0x00000090u) return 0;
		if ((i & 0x01900000u) == 0x01000000u) return 1;      /* (bx, blx: treat as a branch) */
		return ((i >> 12) & 15) == 15 && !(((i >> 21) & 15) >= 8 && ((i >> 21) & 15) <= 11);
	case 2: case 3: return ((i >> 20) & 1) && ((i >> 12) & 15) == 15;
	case 4: return ((i >> 20) & 1) && (i & 0x8000);
	}
	return 0;
}

static int xlate_insn(em *e, lx_u32 pc, lx_u32 i)
{
	lx_u32 cond = i & 0xF0000000u;
	if (cond == 0xF0000000u) return xl_uncond(e, pc, i);
	if (lx_cpu.slow && (lx_cpu.slow & slow_class(i))) {
		if (writes_pc(i)) {
			if (slow_class(i) == DBT_SLOW_MISC) return xl_misc(e, pc, i);
			return branch_helper(e, pc, i);
		}
		return helper(e, pc, i);
	}
	switch ((i >> 25) & 7) {
	case 0:
		if ((i & 0x90) == 0x90) {
			if ((i & 0x0F0000F0u) == 0x00000090u) {
				/* multiplies: mul mla umull umlal smull smlal (pc never an operand) */
				int op = (int)(i >> 21) & 7;
				if (op == 2 || op == 3) return helper(e, pc, i);   /* umaal, mls */
				if (((i >> 16) & 15) == 15 || ((i >> 12) & 15) == 15 || ((i >> 8) & 15) == 15 || (i & 15) == 15)
					return undef(e, pc, i);
				E(e, i);
				return GO_ON;
			}
			if ((i & 0x0F0000F0u) == 0x01000090u) {
				if (i & 0x00800000u) return xl_excl(e, pc, i);
				if ((i & 0x00300F00u) == 0 && ((i >> 16) & 15) != 15) { E(e, i); return GO_ON; }   /* swp, swpb */
				return undef(e, pc, i);
			}
			return xl_extra(e, pc, i);
		}
		if ((i & 0x01900000u) == 0x01000000u) return xl_misc(e, pc, i);
		return xl_dp(e, pc, i);
	case 1:
		if ((i & 0x01900000u) == 0x01000000u) {
			if (i & 0x00200000u) {
				/* msr cpsr_f, #imm; the hints (nop, yield, wfe, wfi, sev) */
				if ((i & 0x000F0000u) == 0) return GO_ON;
				if (i & 0x00400000u) return undef(e, pc, i);
				return helper(e, pc, i);
			}
			return helper(e, pc, i);                     /* movw/movt (v6T2) */
		}
		return xl_dp(e, pc, i);
	case 2:
		return xl_ls(e, pc, i);
	case 3:
		if (i & 0x10) return xl_media(e, pc, i);
		return xl_ls(e, pc, i);
	case 4:
		return xl_ldm(e, pc, i);
	case 5: {
		lx_u32 target = pc + 8 + (lx_u32)(((lx_s32)(i << 8)) >> 6);
		if (i & 0x01000000u) e_ldr_lit(e, cond, 14, pc + 4);    /* bl: lr is the guest's return address */
		e_exit(e, cond, target);
		return cond == AL ? ENDED : GO_ON;
	}
	case 6:
		return xl_cop(e, pc, i);
	default:
		if (i & 0x01000000u) {                              /* svc */
			lx_u32 *s = skip_begin(e, cond);
			e_enter(e, DBT_SVC, pc, 0, 2);
			if (s) { skip_end(e, s); e_exit(e, AL, pc + 4); }
			return ENDED;
		}
		return xl_cop(e, pc, i);
	}
}

/* ---- blocks ------------------------------------------------------------------------ */

static int fetch_ok(lx_thread *t, lx_u32 pc)
{
	lx_mm *mm = t->proc ? t->proc->mm : 0;
	lx_map *m;
	if (pc & 3) return 0;
	if (!mm) return 0;
	if (pc >= mm->image.base && pc + 4 <= mm->image.base + mm->image.size) return 1;
	if (mm->interp.size && pc >= mm->interp.base && pc + 4 <= mm->interp.base + mm->interp.size) return 1;
	for (m = mm->maps; m; m = m->next)
		if (pc >= m->addr && pc + 4 <= m->addr + m->len) return 1;
	return 0;
}

static int grow_cache(lx_u32 need_top)
{
	lx_u32 want = need_top - cache.base;
	if (want <= cache_committed) return 0;
	want = (want + CACHE_STEP - 1) & ~(CACHE_STEP - 1);
	if (want > CACHE_MAX) return -1;
	if (!lx_reserve_check(want - cache_committed, 0) || bk_code_grow(&cache, want) < 0) return -1;
	cache_committed = want;
	return 0;
}

/* translates the block at gpc; returns its host address (0: no room even
 * after a flush, or no memory) */
static lx_u32 translate(lx_thread *t, lx_u32 gpc)
{
	em e;
	lx_u32 pc = gpc, *q, host;
	int n = 0, k, b;
	if (cache_top + BLOCK_ROOM > cache.base + CACHE_MAX || nblocks >= MAXBLOCKS) flush();
	if (grow_cache(cache_top + BLOCK_ROOM) < 0) {
		flush();
		if (grow_cache(cache_top + BLOCK_ROOM) < 0) return 0;
	}
	bk_memset(&e, 0, sizeof e);
	e.p = e.start = (lx_u32 *)cache_top;
	host = cache_top;
	if (!fetch_ok(t, pc)) {
		/* nothing to run there: SIGSEGV (a block of its own, not kept) */
		e_enter(&e, DBT_SEGV, pc, 0, 2);
		cache_top = (lx_u32)e.p;
		return host;
	}
	for (;;) {
		lx_u32 insn = *(lx_u32 *)pc;
		int r;
		if (n && (e.nlit > NLIT - 6 || e.nref > 2 * NLIT - 6 || e.nexit > NEXIT - 3)) {
			/* (room for the worst an instruction needs, or end the block here) */
			e_exit(&e, AL, pc);
			break;
		}
		r = xlate_insn(&e, pc, insn);
		n++;
		pc += 4;
		if (r == ENDED) break;
		if (n >= MAXN || e.full || e.p - e.start >= MAXWORDS || !fetch_ok(t, pc) || lx_cpu.trace || (lx_cpu.maxn && n >= (int)lx_cpu.maxn)) {
			e_exit(&e, AL, pc);
			break;
		}
	}
	lx_cpu.guest_insns += n;
	code_mb[gpc >> 23] |= (lx_u8)(1 << ((gpc >> 20) & 7));
	code_mb[(pc - 4) >> 23] |= (lx_u8)(1 << (((pc - 4) >> 20) & 7));
	/* the exits: straight to blocks we have, or to a stub */
	for (k = 0; k < e.nexit; k++) {
		lx_u32 tg = e.xtarget[k];
		lx_u32 *site = e.xsite[k], c = *site & 0xF0000000u;
		int tb = (lx_cpu.trace || lx_cpu.nochain) ? -1 : tg == gpc ? -2 : block_find(tg);
		if (tb == -2) { *site = b_to(c, site, host); continue; }
		if (tb >= 0) { *site = b_to(c, site, blocks[tb].host); continue; }
		*site = b_to(c, site, (lx_u32)e.p);
		e_enter(&e, DBT_CHAIN, tg, (lx_u32)site, 3);
	}
	/* the literal pool */
	q = e.p;
	for (k = 0; k < e.nlit; k++) E(&e, e.lit[k]);
	for (k = 0; k < e.nref; k++) {
		lx_u32 off = (lx_u32)(q + e.lidx[k]) - ((lx_u32)e.lref[k] + 8);
		*e.lref[k] |= off;          /* (U is set; always ahead and in reach) */
	}
	lx_cpu.host_words += (lx_u32)(e.p - e.start);
	cache_top = (lx_u32)e.p;
	/* keep it */
	b = nblocks++;
	blocks[b].gpc = gpc;
	blocks[b].host = host;
	k = bh_index(gpc);
	blocks[b].next = bh[k];
	bh[k] = b;
	if (!lx_cpu.trace && lx_cpu.nochain < 2) hash_put(gpc, host);
	lx_cpu.translations++;
	return host;
}

/* the host code for guest pc (translated now if need be), or 0 */
lx_u32 dbt_lookup(lx_thread *t, lx_u32 gpc)
{
	int b;
	do_invalidations();
	if (gpc & 3) return 0;                  /* (Thumb, or no instruction) */
	if (lx_cpu.trace) lx_cpu.trace(t, gpc);
	b = block_find(gpc);
	if (b >= 0) { if (!lx_cpu.trace && lx_cpu.nochain < 2) hash_put(gpc, blocks[b].host); return blocks[b].host; }
	return translate(t, gpc);
}

lx_u32 dbt_gen(void) { return gen; }

/* a chained exit: the branch at site goes straight to host from now on */
void dbt_chain(lx_u32 site, lx_u32 host, lx_u32 gen_then)
{
	lx_u32 *s = (lx_u32 *)site;
	if (gen_then != gen || lx_cpu.trace || lx_cpu.nochain) return;   /* (the cache was flushed meanwhile) */
	if (site < cache_start || site >= cache_top) return;
	*s = b_to(*s & 0xF0000000u, s, host);
}

/* An access at host address h (translated code) faulted for alignment and
 * was done bytewise (dbt_fixup). Unaligned accesses come in places: musl's
 * memset alone faulted some 36,000 times while OpenRC started, and an
 * exception costs the kernel a great deal (and the ER5 kernel did not
 * survive that many: the machine restarted). So the instruction becomes a
 * branch to a stub that does the access through the runtime (DBT_FIXUP),
 * a few hundred cycles instead of an exception, each time it runs:
 *     b<not cond> 1f; str r0, [sp, #-8]; mov r0, pc; b enter;
 *     .word FIXUP, insn, h + 4;  1: b h + 4
 * Called from the exception handler, while the thread runs guest code (so
 * not while the translator is busy). 0, or -1 (no room: it simply faults
 * again next time). */
int dbt_patch_unaligned(lx_u32 h)
{
	em e;
	lx_u32 insn, cond, *s, stub;
	if (!ready || h < cache_start || h >= cache_top || (h & 3)) return -1;
	if (cache_top + 16 * 4 > cache.base + CACHE_MAX || grow_cache(cache_top + 16 * 4) < 0) return -1;
	insn = *(lx_u32 *)h;
	cond = insn & 0xF0000000u;
	if (cond == 0xF0000000u) return -1;
	bk_memset(&e, 0, sizeof e);
	e.p = e.start = (lx_u32 *)cache_top;
	stub = cache_top;
	s = skip_begin(&e, cond);
	e_enter(&e, DBT_FIXUP, (insn & 0x0FFFFFFFu) | AL, h + 4, 3);
	skip_end(&e, s);
	e_b(&e, AL, h + 4);
	cache_top = (lx_u32)e.p;
	*(lx_u32 *)h = b_to(AL, (lx_u32 *)h, stub);
	lx_cpu.patched++;
	return 0;
}

lx_u32 dbt_cache_used(void) { return cache_top - cache.base; }

void dbt_stats(char *buf, int size)
{
	lx_snprintf(buf, size,
		"code cache   %u KB used, %u KB committed, %u KB at most\n"
		"blocks       %d (translated %u, flushes %u)\n"
		"guest insns  %u translated into %u host words\n"
		"runtime      %u misses, %u helpers, %u system calls\n"
		"unaligned    %u faults, %u sites patched, %u accesses through them\n",
		dbt_cache_used() / 1024, cache_committed / 1024, CACHE_MAX / 1024,
		nblocks, lx_cpu.translations, lx_cpu.flushes,
		lx_cpu.guest_insns, lx_cpu.host_words,
		lx_cpu.misses, lx_cpu.helpers, lx_cpu.syscalls,
		lx_cpu.fixups, lx_cpu.patched, lx_cpu.fixed_calls);
}
