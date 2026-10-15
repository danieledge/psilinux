/* lx_dbt_rt.c - Alpine mode: the runtime that translated code enters
 * (dbt_enter, lx_dbt_entry.S), and the interpreter for the instructions the
 * translator leaves to it: the v6 media and DSP instructions, clz, ldrd and
 * strd in their odd forms, loads into pc it does not do inline, and the
 * thread pointer register. VFP is lx_vfp.c.
 *
 * It runs on the Linux thread that holds the CPU, on that thread's own
 * stack (t->hostsp), with the guest's registers in t->dframe. */
#include "lx_dbt.h"

lx_u32 dbt_lookup(lx_thread *t, lx_u32 gpc);
lx_u32 dbt_gen(void);
void dbt_chain(lx_u32 site, lx_u32 host, lx_u32 gen_then);
int dbt_block_of(lx_u32 h, lx_u32 *gpc, lx_u32 *host);
int lx_vfp(lx_thread *t, lx_u32 *frame, lx_u32 pc, lx_u32 insn);

/* ---- guest memory, a byte at a time when it is not aligned ----------------- */

static lx_u32 ld32(lx_u32 a)
{
	const lx_u8 *p = (const lx_u8 *)a;
	if (!(a & 3)) return *(const lx_u32 *)a;
	return p[0] | ((lx_u32)p[1] << 8) | ((lx_u32)p[2] << 16) | ((lx_u32)p[3] << 24);
}
static lx_u32 ld16(lx_u32 a)
{
	const lx_u8 *p = (const lx_u8 *)a;
	return p[0] | ((lx_u32)p[1] << 8);
}
static void st32(lx_u32 a, lx_u32 v)
{
	lx_u8 *p = (lx_u8 *)a;
	if (!(a & 3)) { *(lx_u32 *)a = v; return; }
	p[0] = (lx_u8)v; p[1] = (lx_u8)(v >> 8); p[2] = (lx_u8)(v >> 16); p[3] = (lx_u8)(v >> 24);
}
static void st16(lx_u32 a, lx_u32 v)
{
	lx_u8 *p = (lx_u8 *)a;
	p[0] = (lx_u8)v; p[1] = (lx_u8)(v >> 8);
}

lx_u32 dbt_ld32(lx_u32 a) { return ld32(a); }
void dbt_st32(lx_u32 a, lx_u32 v) { st32(a, v); }

/* ---- flags and conditions -------------------------------------------------------- */

#define FN 0x80000000u
#define FZ 0x40000000u
#define FC 0x20000000u
#define FV 0x10000000u
#define FQ 0x08000000u

static int cond_ok(lx_u32 cpsr, lx_u32 cond)
{
	int n = (cpsr & FN) != 0, z = (cpsr & FZ) != 0, c = (cpsr & FC) != 0, v = (cpsr & FV) != 0, r;
	switch (cond >> 1) {
	case 0: r = z; break;
	case 1: r = c; break;
	case 2: r = n; break;
	case 3: r = v; break;
	case 4: r = c && !z; break;
	case 5: r = n == v; break;
	case 6: r = !z && n == v; break;
	default: return 1;
	}
	return (cond & 1) ? !r : r;
}

static lx_u32 nz(lx_u32 cpsr, lx_u32 v)
{
	cpsr &= ~(FN | FZ);
	if (v & 0x80000000u) cpsr |= FN;
	if (!v) cpsr |= FZ;
	return cpsr;
}

/* ---- the integer interpreter ------------------------------------------------------- */

typedef struct ix {
	lx_thread *t;
	lx_u32 *f;                  /* r0-r12, sp, lr, pc (the next one), cpsr */
	lx_u32 pc;                  /* this instruction's address */
} ix;

static lx_u32 rr(ix *x, int n) { return n == 15 ? x->pc + 8 : x->f[n]; }
static void wr(ix *x, int n, lx_u32 v) { x->f[n == 15 ? DF_PC : n] = v; }

/* the shifter: an immediate or a register shifted; *carry the carry out */
static lx_u32 shifter(ix *x, lx_u32 i, int *carry)
{
	lx_u32 c = (x->f[DF_CPSR] & FC) ? 1 : 0, v, amt;
	int type;
	if (i & 0x02000000u) {
		lx_u32 imm = i & 0xFF, rot = ((i >> 8) & 15) * 2;
		v = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
		*carry = rot ? (int)(v >> 31) : (int)c;
		return v;
	}
	type = (int)(i >> 5) & 3;
	if (i & 0x10) {
		/* by a register (pc reads +12) */
		v = (i & 15) == 15 ? x->pc + 12 : x->f[i & 15];
		amt = x->f[(i >> 8) & 15] & 0xFF;
		if (amt == 0) { *carry = (int)c; return v; }
		switch (type) {
		case 0: if (amt < 32) { *carry = (int)((v >> (32 - amt)) & 1); return v << amt; }
			*carry = amt == 32 ? (int)(v & 1) : 0; return 0;
		case 1: if (amt < 32) { *carry = (int)((v >> (amt - 1)) & 1); return v >> amt; }
			*carry = amt == 32 ? (int)(v >> 31) : 0; return 0;
		case 2: if (amt < 32) { *carry = (int)((v >> (amt - 1)) & 1); return (lx_u32)((lx_s32)v >> amt); }
			*carry = (int)(v >> 31); return (v & 0x80000000u) ? 0xFFFFFFFFu : 0;
		default: amt &= 31;
			if (!amt) { *carry = (int)(v >> 31); return v; }
			*carry = (int)((v >> (amt - 1)) & 1); return (v >> amt) | (v << (32 - amt));
		}
	}
	v = rr(x, (int)(i & 15));
	amt = (i >> 7) & 31;
	switch (type) {
	case 0: if (!amt) { *carry = (int)c; return v; }
		*carry = (int)((v >> (32 - amt)) & 1); return v << amt;
	case 1: if (!amt) { *carry = (int)(v >> 31); return 0; }
		*carry = (int)((v >> (amt - 1)) & 1); return v >> amt;
	case 2: if (!amt) { *carry = (int)(v >> 31); return (v & 0x80000000u) ? 0xFFFFFFFFu : 0; }
		*carry = (int)((v >> (amt - 1)) & 1); return (lx_u32)((lx_s32)v >> amt);
	default:
		if (!amt) { *carry = (int)(v & 1); return (c << 31) | (v >> 1); }   /* rrx */
		*carry = (int)((v >> (amt - 1)) & 1); return (v >> amt) | (v << (32 - amt));
	}
}

static int i_dp(ix *x, lx_u32 i)
{
	int op = (int)(i >> 21) & 15, S = (int)(i >> 20) & 1, rn = (int)(i >> 16) & 15, rd = (int)(i >> 12) & 15, carry;
	lx_u32 a = rr(x, rn), b, r = 0, cpsr = x->f[DF_CPSR];
	int cout, vout = (cpsr & FV) != 0, logic = 0;
	if ((i & 0x02000010u) == 0x10 && rn == 15) a = x->pc + 12;
	b = shifter(x, i, &carry);
	cout = carry;
	switch (op) {
	case 0: r = a & b; logic = 1; break;
	case 1: r = a ^ b; logic = 1; break;
	case 2: r = a - b; cout = a >= b; vout = (int)(((a ^ b) & (a ^ r)) >> 31); break;
	case 3: r = b - a; cout = b >= a; vout = (int)(((b ^ a) & (b ^ r)) >> 31); break;
	case 4: r = a + b; cout = r < a; vout = (int)((~(a ^ b) & (a ^ r)) >> 31); break;
	case 5: { lx_u32 c = (cpsr & FC) ? 1 : 0; r = a + b + c; cout = c ? r <= a : r < a; vout = (int)((~(a ^ b) & (a ^ r)) >> 31); break; }
	case 6: { lx_u32 c = (cpsr & FC) ? 1 : 0; r = a - b - (1 - c); cout = c ? a >= b : a > b; vout = (int)(((a ^ b) & (a ^ r)) >> 31); break; }
	case 7: { lx_u32 c = (cpsr & FC) ? 1 : 0; r = b - a - (1 - c); cout = c ? b >= a : b > a; vout = (int)(((b ^ a) & (b ^ r)) >> 31); break; }
	case 8: r = a & b; logic = 1; break;
	case 9: r = a ^ b; logic = 1; break;
	case 10: r = a - b; cout = a >= b; vout = (int)(((a ^ b) & (a ^ r)) >> 31); break;
	case 11: r = a + b; cout = r < a; vout = (int)((~(a ^ b) & (a ^ r)) >> 31); break;
	case 12: r = a | b; logic = 1; break;
	case 13: r = b; logic = 1; break;
	case 14: r = a & ~b; logic = 1; break;
	default: r = ~b; logic = 1; break;
	}
	if (op < 8 || op > 11) wr(x, rd, r);
	if (S || (op >= 8 && op <= 11)) {
		if (rd == 15 && !(op >= 8 && op <= 11)) return 0;   /* (no SPSR in user mode) */
		cpsr = nz(cpsr, r);
		cpsr = cout ? cpsr | FC : cpsr & ~FC;
		if (!logic) cpsr = vout ? cpsr | FV : cpsr & ~FV;
		x->f[DF_CPSR] = cpsr;
	}
	return 0;
}

static int i_mul(ix *x, lx_u32 i)
{
	int op = (int)(i >> 21) & 7, S = (int)(i >> 20) & 1;
	int rd = (int)(i >> 16) & 15, rn = (int)(i >> 12) & 15, rs = (int)(i >> 8) & 15, rm = (int)(i & 15);
	lx_u32 a = x->f[rm], b = x->f[rs], lo, hi;
	lx_u64 p;
	switch (op) {
	case 0: lo = a * b; x->f[rd] = lo; if (S) x->f[DF_CPSR] = nz(x->f[DF_CPSR], lo); return 0;
	case 1: lo = a * b + x->f[rn]; x->f[rd] = lo; if (S) x->f[DF_CPSR] = nz(x->f[DF_CPSR], lo); return 0;
	case 2: /* umaal rdlo(rn), rdhi(rd) */
		p = (lx_u64)a * b + x->f[rn] + x->f[rd];
		x->f[rn] = (lx_u32)p; x->f[rd] = (lx_u32)(p >> 32); return 0;
	case 3: x->f[rd] = x->f[rn] - a * b; return 0;   /* mls */
	case 4: p = (lx_u64)a * b; break;
	case 5: p = (lx_u64)a * b + (((lx_u64)x->f[rd] << 32) | x->f[rn]); break;
	case 6: p = (lx_u64)((lx_s64)(lx_s32)a * (lx_s32)b); break;
	default: p = (lx_u64)((lx_s64)(lx_s32)a * (lx_s32)b) + (((lx_u64)x->f[rd] << 32) | x->f[rn]); break;
	}
	lo = (lx_u32)p; hi = (lx_u32)(p >> 32);
	x->f[rn] = lo; x->f[rd] = hi;
	if (S) {
		lx_u32 c = x->f[DF_CPSR] & ~(FN | FZ);
		if (hi & 0x80000000u) c |= FN;
		if (!lo && !hi) c |= FZ;
		x->f[DF_CPSR] = c;
	}
	return 0;
}

/* addressing mode 2 (word, byte) and 3 (halfword, dual) */
static int i_ls(ix *x, lx_u32 i)
{
	int I = (int)(i >> 25) & 1, P = (int)(i >> 24) & 1, U = (int)(i >> 23) & 1, B = (int)(i >> 22) & 1, W = (int)(i >> 21) & 1, L = (int)(i >> 20) & 1;
	int rn = (int)(i >> 16) & 15, rd = (int)(i >> 12) & 15, carry;
	lx_u32 off, base = rr(x, rn), addr, v;
	if (I) { lx_u32 ii = i & ~0x02000000u; if (ii & 0x10) return LX_SIGILL; off = shifter(x, ii, &carry); }
	else off = i & 0xFFF;
	addr = P ? (U ? base + off : base - off) : base;
	if (L) {
		v = B ? *(lx_u8 *)addr : ld32(addr);
		if (!P || W) wr(x, rn, U ? base + off : base - off);
		wr(x, rd, v);
	} else {
		v = rd == 15 ? x->pc + 8 : x->f[rd];
		if (B) *(lx_u8 *)addr = (lx_u8)v; else st32(addr, v);
		if (!P || W) wr(x, rn, U ? base + off : base - off);
	}
	return 0;
}

static int i_extra(ix *x, lx_u32 i)
{
	int P = (int)(i >> 24) & 1, U = (int)(i >> 23) & 1, I = (int)(i >> 22) & 1, W = (int)(i >> 21) & 1, L = (int)(i >> 20) & 1;
	int rn = (int)(i >> 16) & 15, rd = (int)(i >> 12) & 15, sh = (int)(i >> 5) & 3;
	lx_u32 off = I ? (((i >> 4) & 0xF0) | (i & 0xF)) : rr(x, (int)(i & 15));
	lx_u32 base = rr(x, rn), addr = P ? (U ? base + off : base - off) : base;
	lx_u32 nb = U ? base + off : base - off;
	if (!L && sh >= 2) {
		if (rd & 1 || rd == 14) return LX_SIGILL;
		if (sh == 2) { lx_u32 a = ld32(addr), b = ld32(addr + 4); if (!P || W) wr(x, rn, nb); x->f[rd] = a; x->f[rd + 1] = b; }
		else { st32(addr, x->f[rd]); st32(addr + 4, x->f[rd + 1]); if (!P || W) wr(x, rn, nb); }
		return 0;
	}
	if (L) {
		lx_u32 v = sh == 1 ? ld16(addr) : sh == 2 ? (lx_u32)(lx_s32)*(signed char *)addr : (lx_u32)(lx_s32)(short)ld16(addr);
		if (!P || W) wr(x, rn, nb);
		wr(x, rd, v);
	} else {
		st16(addr, rd == 15 ? x->pc + 8 : x->f[rd]);
		if (!P || W) wr(x, rn, nb);
	}
	return 0;
}

static int i_ldm(ix *x, lx_u32 i)
{
	int P = (int)(i >> 24) & 1, U = (int)(i >> 23) & 1, W = (int)(i >> 21) & 1, L = (int)(i >> 20) & 1;
	int rn = (int)(i >> 16) & 15, r, n = 0;
	lx_u32 list = i & 0xFFFF, base = x->f[rn], a, nb;
	lx_u32 vals[16];
	if (i & 0x00400000u) return LX_SIGILL;
	for (r = 0; r < 16; r++) if (list & (1u << r)) n++;
	a = U ? (P ? base + 4 : base) : (P ? base - 4 * n : base - 4 * n + 4);
	nb = U ? base + 4 * n : base - 4 * n;
	if (L) {
		for (r = 0; r < 16; r++) if (list & (1u << r)) { vals[r] = ld32(a); a += 4; }
		if (W) x->f[rn] = nb;
		for (r = 0; r < 16; r++) if (list & (1u << r)) wr(x, r, vals[r]);
	} else {
		for (r = 0; r < 16; r++) if (list & (1u << r)) { st32(a, r == 15 ? x->pc + 8 : x->f[r]); a += 4; }
		if (W) x->f[rn] = nb;
	}
	return 0;
}

static lx_u32 sat_s(lx_s64 v, int bits, int *q)
{
	lx_s64 hi = ((lx_s64)1 << (bits - 1)) - 1, lo = -((lx_s64)1 << (bits - 1));
	if (v > hi) { *q = 1; return (lx_u32)hi; }
	if (v < lo) { *q = 1; return (lx_u32)lo; }
	return (lx_u32)v;
}
static lx_u32 sat_u(lx_s64 v, int bits, int *q)
{
	lx_s64 hi = ((lx_s64)1 << bits) - 1;
	if (v > hi) { *q = 1; return (lx_u32)hi; }
	if (v < 0) { *q = 1; return 0; }
	return (lx_u32)v;
}

static lx_u32 ror32(lx_u32 v, int r) { r &= 31; return r ? (v >> r) | (v << (32 - r)) : v; }

/* the parallel adds and subtracts (bytes and halves), with the GE flags */
static int i_parallel(ix *x, lx_u32 i)
{
	int op1 = (int)(i >> 20) & 7, op2 = (int)(i >> 5) & 7, k, q = 0;
	lx_u32 a = x->f[(i >> 16) & 15], b = x->f[i & 15], r = 0, ge = 0;
	int sgn = op1 <= 3, kind = op1 & 3;     /* 1 plain, 2 saturating, 3 halving */
	if (kind == 0) return LX_SIGILL;
	if (op2 == 4 || op2 == 7) {
		/* 8-bit lanes */
		for (k = 0; k < 4; k++) {
			lx_s32 ea = sgn ? (signed char)(a >> (8 * k)) : (lx_s32)((a >> (8 * k)) & 0xFF);
			lx_s32 eb = sgn ? (signed char)(b >> (8 * k)) : (lx_s32)((b >> (8 * k)) & 0xFF);
			lx_s32 s = op2 == 4 ? ea + eb : ea - eb;
			lx_u32 lane;
			if (kind == 2) lane = sgn ? sat_s(s, 8, &q) : sat_u(s, 8, &q);
			else if (kind == 3) lane = (lx_u32)(s >> 1);
			else { lane = (lx_u32)s; if (sgn ? s >= 0 : (op2 == 4 ? s >= 0x100 : s >= 0)) ge |= 1u << k; }
			r |= (lane & 0xFF) << (8 * k);
		}
	} else if (op2 <= 3) {
		/* 16-bit lanes: add16, asx (add top, sub bottom), sax, sub16 */
		for (k = 0; k < 2; k++) {
			lx_s32 ea = sgn ? (short)(a >> (16 * k)) : (lx_s32)((a >> (16 * k)) & 0xFFFF);
			lx_s32 eb0 = sgn ? (short)(b >> (16 * k)) : (lx_s32)((b >> (16 * k)) & 0xFFFF);
			lx_s32 ebx = sgn ? (short)(b >> (16 * (1 - k))) : (lx_s32)((b >> (16 * (1 - k))) & 0xFFFF);
			lx_s32 s;
			int add;
			lx_u32 lane;
			if (op2 == 0) { s = ea + eb0; add = 1; }
			else if (op2 == 3) { s = ea - eb0; add = 0; }
			else if (op2 == 1) { add = k == 1; s = add ? ea + ebx : ea - ebx; }
			else { add = k == 0; s = add ? ea + ebx : ea - ebx; }
			if (kind == 2) lane = sgn ? sat_s(s, 16, &q) : sat_u(s, 16, &q);
			else if (kind == 3) lane = (lx_u32)(s >> 1);
			else { lane = (lx_u32)s; if (sgn ? s >= 0 : (add ? s >= 0x10000 : s >= 0)) ge |= 3u << (2 * k); }
			r |= (lane & 0xFFFF) << (16 * k);
		}
	} else return LX_SIGILL;
	x->f[(i >> 12) & 15] = r;
	if (kind == 1) x->t->dbt_ge = ge;
	return 0;
}

static int i_media(ix *x, lx_u32 i)
{
	int rd = (int)(i >> 12) & 15, rn = (int)(i >> 16) & 15, rm = (int)(i & 15), rs = (int)(i >> 8) & 15;
	lx_u32 b20 = (i >> 20) & 0xFF, op2 = (i >> 5) & 7, m = x->f[rm];
	int q = 0;
	if ((b20 & 0xF8) == 0x60) return i_parallel(x, i);
	if (b20 == 0x68) {
		if ((op2 & 1) == 0) {                   /* pkhbt / pkhtb */
			lx_u32 sh = (i >> 7) & 31, n = x->f[rn], v;
			if (i & 0x40) { v = sh ? (lx_u32)((lx_s32)m >> sh) : ((m & 0x80000000u) ? 0xFFFFFFFFu : 0); x->f[rd] = (n & 0xFFFF0000u) | (v & 0xFFFF); }
			else { v = m << sh; x->f[rd] = (n & 0xFFFF) | (v & 0xFFFF0000u); }
			return 0;
		}
		if (op2 == 3) {                          /* sxtab16 / sxtb16 */
			lx_u32 v = ror32(m, 8 * (int)((i >> 10) & 3)), add = rn == 15 ? 0 : x->f[rn];
			lx_u32 lo = (lx_u32)(lx_s32)(signed char)v + (add & 0xFFFF), hi = (lx_u32)(lx_s32)(signed char)(v >> 16) + (add >> 16);
			x->f[rd] = (lo & 0xFFFF) | (hi << 16);
			return 0;
		}
		if (op2 == 5) {                          /* sel */
			lx_u32 ge = x->t->dbt_ge, a = x->f[rn], r = 0;
			int k;
			for (k = 0; k < 4; k++) r |= ((ge >> k) & 1 ? a : m) & (0xFFu << (8 * k));
			x->f[rd] = r;
			return 0;
		}
		return LX_SIGILL;
	}
	if ((b20 & 0xFA) == 0x6A && (op2 & 1) == 0) {      /* ssat / usat */
		lx_u32 sh = (i >> 7) & 31, n = x->f[rm];
		int bits = (int)((i >> 16) & 31) + 1;
		lx_s64 v = (i & 0x40) ? (lx_s64)((lx_s32)n >> (sh ? sh : 31)) : (lx_s64)(lx_s32)(n << sh);
		if (i & 0x00400000u) x->f[rd] = sat_u(v, bits - 1, &q);
		else x->f[rd] = sat_s(v, bits, &q);
		if (q) x->f[DF_CPSR] |= FQ;
		return 0;
	}
	if ((b20 == 0x6A || b20 == 0x6E) && op2 == 1) {    /* ssat16 / usat16 */
		int bits = (int)((i >> 16) & 15) + 1;
		lx_u32 lo, hi;
		if (b20 == 0x6E) { lo = sat_u((short)m, bits - 1, &q); hi = sat_u((short)(m >> 16), bits - 1, &q); }
		else { lo = sat_s((short)m, bits, &q); hi = sat_s((short)(m >> 16), bits, &q); }
		x->f[rd] = (lo & 0xFFFF) | (hi << 16);
		if (q) x->f[DF_CPSR] |= FQ;
		return 0;
	}
	if ((b20 & 0xF8) == 0x68 && op2 == 3) {            /* the extends */
		lx_u32 v = ror32(m, 8 * (int)((i >> 10) & 3)), add = rn == 15 ? 0 : x->f[rn], r;
		switch (b20) {
		case 0x6A: r = (lx_u32)(lx_s32)(signed char)v; break;
		case 0x6B: r = (lx_u32)(lx_s32)(short)v; break;
		case 0x6C: { lx_u32 lo = (v & 0xFF) + (add & 0xFFFF), hi = ((v >> 16) & 0xFF) + (add >> 16); x->f[rd] = (lo & 0xFFFF) | (hi << 16); return 0; }
		case 0x6E: r = v & 0xFF; break;
		case 0x6F: r = v & 0xFFFF; break;
		default: return LX_SIGILL;
		}
		x->f[rd] = r + add;
		return 0;
	}
	if (b20 == 0x6B && op2 == 1) { x->f[rd] = (m >> 24) | ((m >> 8) & 0xFF00) | ((m << 8) & 0xFF0000) | (m << 24); return 0; }   /* rev */
	if (b20 == 0x6B && op2 == 5) { x->f[rd] = ((m >> 8) & 0x00FF00FFu) | ((m << 8) & 0xFF00FF00u); return 0; }             /* rev16 */
	if (b20 == 0x6F && op2 == 5) { x->f[rd] = (lx_u32)(lx_s32)(short)(((m >> 8) & 0xFF) | ((m & 0xFF) << 8)); return 0; }   /* revsh */
	if (b20 == 0x6F && op2 == 1) {                       /* rbit (v6T2) */
		lx_u32 r = 0; int k;
		for (k = 0; k < 32; k++) if (m & (1u << k)) r |= 1u << (31 - k);
		x->f[rd] = r;
		return 0;
	}
	if ((b20 & 0xF8) == 0x70) {
		/* signed multiplies: smlad smlsd smlald smlsld smmla smmls smmul (rd at 19:16, ra at 15:12) */
		int RD = rn, RA = rd, sw = (int)(i >> 5) & 1;
		lx_u32 n = m, mm = sw ? ror32(x->f[rs], 16) : x->f[rs];   /* (Rn is bits 3:0; Rm, bits 11:8, is the one the X forms rotate) */
		lx_s32 p1 = (lx_s32)(short)n * (short)mm, p2 = (lx_s32)(short)(n >> 16) * (short)(mm >> 16);
		int sub = (int)(i >> 6) & 1;
		if ((b20 & 7) == 0) {
			lx_s64 s = sub ? (lx_s64)p1 - p2 : (lx_s64)p1 + p2;
			if (RA != 15) s += (lx_s32)x->f[RA];
			if (s > 0x7FFFFFFF || s < -(lx_s64)0x80000000) x->f[DF_CPSR] |= FQ;
			x->f[RD] = (lx_u32)s;
			return 0;
		}
		if ((b20 & 7) == 4) {
			lx_s64 s = sub ? (lx_s64)p1 - p2 : (lx_s64)p1 + p2;
			lx_u64 acc = ((lx_u64)x->f[RD] << 32) | x->f[RA];
			acc += (lx_u64)s;
			x->f[RA] = (lx_u32)acc; x->f[RD] = (lx_u32)(acc >> 32);
			return 0;
		}
		if ((b20 & 7) == 5) {
			lx_s64 p = (lx_s64)(lx_s32)x->f[rs] * (lx_s32)m;
			int round = (int)(i >> 5) & 1;
			if ((i >> 6) & 1) p = (RA == 15 ? 0 : ((lx_s64)(lx_s32)x->f[RA] << 32)) - p;
			else if (RA != 15) p += (lx_s64)(lx_s32)x->f[RA] << 32;
			if (round) p += 0x80000000;
			x->f[RD] = (lx_u32)(p >> 32);
			return 0;
		}
		return LX_SIGILL;
	}
	if (b20 == 0x78 && op2 == 0) {                       /* usad8 / usada8 */
		lx_u32 n = x->f[rs], s = 0; int k;
		for (k = 0; k < 4; k++) { lx_s32 d = (lx_s32)((m >> (8 * k)) & 0xFF) - (lx_s32)((n >> (8 * k)) & 0xFF); s += (lx_u32)(d < 0 ? -d : d); }
		if (rd != 15) s += x->f[rd];
		x->f[rn] = s;
		return 0;
	}
	if ((b20 & 0xFE) == 0x7C && (op2 & 3) == 0) {       /* bfc / bfi (v6T2) */
		int lsb = (int)(i >> 7) & 31, msb = (int)(i >> 16) & 31;
		lx_u32 mask, src = rm == 15 ? 0 : x->f[rm];
		if (msb < lsb) return LX_SIGILL;
		mask = (msb - lsb == 31) ? 0xFFFFFFFFu : (((1u << (msb - lsb + 1)) - 1) << lsb);
		x->f[rd] = (x->f[rd] & ~mask) | ((src << lsb) & mask);
		return 0;
	}
	if (((b20 & 0xFE) == 0x7A || (b20 & 0xFE) == 0x7E) && (op2 & 3) == 2) {   /* sbfx / ubfx (v6T2) */
		int lsb = (int)(i >> 7) & 31, w = (int)((i >> 16) & 31) + 1;
		lx_u32 v = m >> lsb;
		if (lsb + w > 32) return LX_SIGILL;
		if (w < 32) v &= (1u << w) - 1;
		if ((b20 & 0xFE) == 0x7A && w < 32 && (v >> (w - 1)) & 1) v |= ~((1u << w) - 1);
		x->f[rd] = v;
		return 0;
	}
	return LX_SIGILL;
}

/* msr: the flags (N Z C V Q) and, with the s field, GE */
static int i_msr(ix *x, lx_u32 i, lx_u32 v)
{
	if (i & 0x00400000u) return LX_SIGILL;
	if (i & 0x00080000u) {
		x->f[DF_CPSR] = (x->f[DF_CPSR] & 0x0FFFFFFFu) | (v & 0xF0000000u);
		x->t->dbt_q = (v >> 27) & 1;
	}
	if (i & 0x00040000u) x->t->dbt_ge = (v >> 16) & 15;
	return 0;
}

static int i_misc(ix *x, lx_u32 i)
{
	int rd = (int)(i >> 12) & 15, rm = (int)(i & 15), rn = (int)(i >> 16) & 15, op = (int)(i >> 21) & 3;
	lx_u32 m = x->f[rm];
	switch ((i >> 4) & 0xF) {
	case 0x0:
		if (op & 1) return i_msr(x, i, m);
		if (i & 0x00400000u) return LX_SIGILL;
		x->f[rd] = (x->f[DF_CPSR] & 0xF0000000u) | (x->t->dbt_q << 27) | (x->t->dbt_ge << 16) | 0x10;
		return 0;
	case 0x1:
		if (op == 3) {                                   /* clz */
			lx_u32 n = 0;
			if (!m) n = 32;
			else { while (!(m & 0x80000000u)) { m <<= 1; n++; } }
			x->f[rd] = n;
			return 0;
		}
		return LX_SIGILL;
	case 0x5: {                                          /* qadd qsub qdadd qdsub */
		lx_s64 a = (lx_s32)m, b = (lx_s32)x->f[rn], r;
		int q = 0;
		if (op & 2) { b = (lx_s32)sat_s(2 * b, 32, &q); }
		r = (op & 1) ? a - b : a + b;
		x->f[rd] = sat_s(r, 32, &q);
		if (q) x->f[DF_CPSR] |= FQ;
		return 0;
	}
	default:
		if ((i & 0x90) == 0x80) {
			/* smlaxy (op 0), smlawy/smulwy (1), smlalxy (2), smulxy (3): rd 19:16, rn 15:12, rs 11:8 */
			int X = (int)(i >> 5) & 1, Y = (int)(i >> 6) & 1, RD = rn, RN = rd, RS = (int)(i >> 8) & 15;
			lx_s32 a = X ? (short)(m >> 16) : (short)m, b = Y ? (short)(x->f[RS] >> 16) : (short)x->f[RS];
			switch (op) {
			case 0: { lx_s64 r = (lx_s64)a * b + (lx_s32)x->f[RN]; if (r > 0x7FFFFFFF || r < -(lx_s64)0x80000000) x->f[DF_CPSR] |= FQ; x->f[RD] = (lx_u32)r; return 0; }
			case 1: { lx_s64 p = (lx_s64)(lx_s32)m * b; lx_s32 r = (lx_s32)(p >> 16);
				if (!X) { lx_s64 s = (lx_s64)r + (lx_s32)x->f[RN]; if (s > 0x7FFFFFFF || s < -(lx_s64)0x80000000) x->f[DF_CPSR] |= FQ; x->f[RD] = (lx_u32)s; }
				else x->f[RD] = (lx_u32)r;
				return 0; }
			case 2: { lx_u64 acc = ((lx_u64)x->f[RD] << 32) | x->f[RN]; acc += (lx_u64)((lx_s64)a * b); x->f[RN] = (lx_u32)acc; x->f[RD] = (lx_u32)(acc >> 32); return 0; }
			default: x->f[RD] = (lx_u32)(a * b); return 0;
			}
		}
		return LX_SIGILL;
	}
}

static int i_cp15(ix *x, lx_u32 i)
{
	int L = (int)(i >> 20) & 1, crn = (int)(i >> 16) & 15, rt = (int)(i >> 12) & 15, crm = (int)(i & 15);
	int op1 = (int)(i >> 21) & 7, op2 = (int)(i >> 5) & 7;
	if (op1 || crn != 13 || crm) return LX_SIGILL;
	if (op2 == 2) { if (L) x->f[rt] = x->t->tpidrurw; else x->t->tpidrurw = x->f[rt]; return 0; }
	if (op2 == 3 && L && rt != 15) { x->f[rt] = x->t->tp; return 0; }
	return LX_SIGILL;
}

/* one guest instruction; 0, or the signal it raises. A write to pc is left
 * in frame[DF_PC], which holds the next instruction's address otherwise. */
static int interp1(lx_thread *t, lx_u32 *frame, lx_u32 pc, lx_u32 i);

int dbt_interp(lx_thread *t, lx_u32 *frame, lx_u32 pc, lx_u32 i)
{
	int r = interp1(t, frame, pc, i);
	if (frame[DF_CPSR] & FQ) { t->dbt_q = 1; frame[DF_CPSR] &= ~FQ; }   /* (sticky, and kept in the thread) */
	return r;
}

static int interp1(lx_thread *t, lx_u32 *frame, lx_u32 pc, lx_u32 i)
{
	ix x;
	x.t = t; x.f = frame; x.pc = pc;
	if ((i >> 28) == 0xF) return LX_SIGILL;
	if (!cond_ok(frame[DF_CPSR], i >> 28)) return 0;
	switch ((i >> 25) & 7) {
	case 0:
		if ((i & 0x90) == 0x90) {
			if ((i & 0x0F0000F0u) == 0x00000090u) return i_mul(&x, i);
			if ((i & 0x0F0000F0u) == 0x01000090u) {
				if (!(i & 0x00800000u)) {            /* swp, swpb */
					lx_u32 a = frame[(i >> 16) & 15], v;
					if (i & 0x00400000u) { v = *(lx_u8 *)a; *(lx_u8 *)a = (lx_u8)frame[i & 15]; }
					else { v = ld32(a); st32(a, frame[i & 15]); }
					frame[(i >> 12) & 15] = v;
					return 0;
				}
				{   /* ldrexd/strexd odd register cases */
					int op = (int)(i >> 20) & 0xF, rn = (int)(i >> 16) & 15, rd = (int)(i >> 12) & 15, rt = (int)(i & 15);
					lx_u32 a = frame[rn];
					if (op == 0xB) { lx_u32 lo = ld32(a), hi = ld32(a + 4); frame[rd] = lo; frame[rd + 1] = hi; return 0; }
					if (op == 0xA) { st32(a, frame[rt]); st32(a + 4, frame[rt + 1]); frame[rd] = 0; return 0; }
					return LX_SIGILL;
				}
			}
			return i_extra(&x, i);
		}
		if ((i & 0x01900000u) == 0x01000000u) return i_misc(&x, i);
		return i_dp(&x, i);
	case 1:
		if ((i & 0x01900000u) == 0x01000000u) {
			if (i & 0x00200000u) {                   /* msr cpsr, #imm */
				lx_u32 imm = i & 0xFF, rot = ((i >> 8) & 15) * 2;
				if (!(i & 0x000F0000u)) return 0;    /* (a hint) */
				return i_msr(&x, i, rot ? (imm >> rot) | (imm << (32 - rot)) : imm);
			}
			if (!(i & 0x00200000u)) {                /* movw / movt */
				lx_u32 imm = ((i >> 4) & 0xF000) | (i & 0xFFF);
				int rd = (int)(i >> 12) & 15;
				if (i & 0x00400000u) frame[rd] = (frame[rd] & 0xFFFF) | (imm << 16);
				else frame[rd] = imm;
				return 0;
			}
			return 0;
		}
		return i_dp(&x, i);
	case 2: return i_ls(&x, i);
	case 3:
		if (i & 0x10) return i_media(&x, i);
		return i_ls(&x, i);
	case 4: return i_ldm(&x, i);
	case 6: case 7:
		if (((i >> 8) & 15) == 10 || ((i >> 8) & 15) == 11) return lx_vfp(t, frame, pc, i);
		if (((i >> 8) & 15) == 15 && (i & 0x0F000010u) == 0x0E000010u) return i_cp15(&x, i);
		return LX_SIGILL;
	}
	return LX_SIGILL;
}

/* ---- unaligned accesses ----------------------------------------------------------
 * ARMv6 code may load and store words and halfwords at any address; the
 * ARM710T raises an alignment exception instead (M0, DESIGN.md). The first
 * time a translated access faults, the EPOC exception handler (bk_epoc.cpp)
 * does it here a byte at a time, and the site is patched (dbt_patch_unaligned)
 * so that from then on it comes here through the runtime (DBT_FIXUP) instead
 * of through an exception.
 *
 * r: r0-r15; i: the host instruction (an ARMv4 ldr, str, ldrh, strh or
 * ldrsh). after: the instruction faulted, and the ARM710T's abort model has
 * already updated a written-back base; otherwise it has not run at all. */
static int fixup(lx_u32 *r, lx_u32 i, int after)
{
	int P = (int)(i >> 24) & 1, U = (int)(i >> 23) & 1, W = (int)(i >> 21) & 1, L = (int)(i >> 20) & 1;
	int rn = (int)(i >> 16) & 15, rd = (int)(i >> 12) & 15;
	lx_u32 off, base, addr;
	int wb;
	if (rn == 15 || rd == 15) return -1;
	if ((i & 0x0C000000u) == 0x04000000u) {
		int I = (int)(i >> 25) & 1, B = (int)(i >> 22) & 1, carry, k;
		if (B) return -1;
		if (I) {
			ix x;
			lx_u32 f[DF_WORDS];
			if (i & 0x10) return -1;
			for (k = 0; k < 15; k++) f[k] = r[k];
			f[DF_CPSR] = 0;
			x.t = 0; x.f = f; x.pc = 0;
			off = shifter(&x, i & ~0x02000000u, &carry);
		} else off = i & 0xFFF;
		wb = !P || W;
		base = r[rn];
		if (wb && after) base = U ? base - off : base + off;      /* (undone: it was written back) */
		addr = P ? (U ? base + off : base - off) : base;
		if (wb && !after) r[rn] = U ? base + off : base - off;
		if (L) r[rd] = ld32(addr); else st32(addr, r[rd]);
	} else if ((i & 0x0E000090u) == 0x00000090u && ((i >> 5) & 3)) {
		int I = (int)(i >> 22) & 1, sh = (int)(i >> 5) & 3;
		if (!L && sh != 1) return -1;
		off = I ? (((i >> 4) & 0xF0) | (i & 0xF)) : r[i & 15];
		wb = !P || W;
		base = r[rn];
		if (wb && after) base = U ? base - off : base + off;
		addr = P ? (U ? base + off : base - off) : base;
		if (wb && !after) r[rn] = U ? base + off : base - off;
		if (!L) st16(addr, r[rd]);
		else if (sh == 1) r[rd] = ld16(addr);
		else if (sh == 3) r[rd] = (lx_u32)(lx_s32)(short)ld16(addr);
		else return -1;
	} else return -1;
	return 0;
}

/* from the exception handler: regs r0-r12, sp, lr as the faulting
 * instruction left them (sp the guest's, apart) */
int dbt_fixup(lx_u32 *regs, lx_u32 sp, lx_u32 i)
{
	lx_u32 r[16];
	int k;
	for (k = 0; k < 15; k++) r[k] = regs[k];
	r[13] = sp;
	r[15] = 0;
	if (fixup(r, i, 1) < 0) return -1;
	for (k = 0; k < 15; k++) if (k != 13) regs[k] = r[k];
	lx_cpu.fixups++;
	return 0;
}

/* (debugging) which instructions the interpreter does most */
#define NPROF 64
static lx_u32 prof_key[NPROF], prof_n[NPROF], prof_eg[NPROF];
void dbt_profile(lx_u32 insn)
{
	lx_u32 k = insn & 0x0FF000F0u;
	int i, low = 0;
	for (i = 0; i < NPROF; i++) {
		if (prof_n[i] && prof_key[i] == k) { prof_n[i]++; return; }
		if (prof_n[i] < prof_n[low]) low = i;
	}
	prof_key[low] = k; prof_n[low] = 1; prof_eg[low] = insn;
}
void dbt_profile_log(void)
{
	int i, j;
	for (j = 0; j < 12; j++) {
		int best = -1;
		for (i = 0; i < NPROF; i++) if (prof_n[i] && (best < 0 || prof_n[i] > prof_n[best])) best = i;
		if (best < 0) break;
		bk_log("  helper %08x (e.g. %08x): %u", prof_key[best], prof_eg[best], prof_n[best]);
		prof_n[best] = 0;
	}
}

/* ---- the runtime ------------------------------------------------------------------------ */

static lx_u32 continue_at(lx_thread *t, lx_u32 *frame, lx_u32 gpc);

/* a call into the kernel: the thread waits for its answer (and the CPU) */
static void kernel_call(lx_thread *t, lx_u32 *frame, int kcall)
{
	t->frame = frame;
	t->dbt_kcall = kcall;
	lx_cpu.critical = 0;
	if (kcall != LX_NR_dbt_yield) lx_cpu.syscalls++;
	bk_thread_call(t);              /* (the kernel gave us the CPU back before it answered) */
	if (t->reply == LX_R_EXIT) bk_thread_exit_self(t);   /* (without the CPU: touch nothing) */
	lx_cpu.critical = 1;
	t->dbt_kcall = 0;
	if (t->reply == LX_R_RESUME) {
		int k;
		for (k = 0; k < 16; k++) frame[k] = t->ctx[k];
		frame[DF_CPSR] = t->ctx_cpsr;
		if (t->sigframe_len) {
			/* a signal frame or a fork's stack, onto the guest's stack */
			bk_memcpy((void *)t->sigframe_at, (const void *)t->sigframe_src, t->sigframe_len);
			t->sigframe_len = 0;
		}
	}
}

/* raises sig in the guest at frame[DF_PC]: the kernel delivers it */
static lx_u32 raise_sig(lx_thread *t, lx_u32 *frame, int sig)
{
	t->dbt_sig = sig;
	kernel_call(t, frame, LX_NR_dbt_raise);
	return continue_at(t, frame, frame[DF_PC]);
}

static lx_u32 continue_at(lx_thread *t, lx_u32 *frame, lx_u32 gpc)
{
	lx_u32 host = dbt_lookup(t, gpc);
	if (host) return host;
	if (gpc & 1) bk_log("pid %d (%s): a jump into Thumb code at %x: not supported", t->proc ? t->proc->pid : 0, t->proc ? t->proc->comm : "", gpc & ~1u);
	else bk_log("pid %d (%s): cannot run code at %x", t->proc ? t->proc->pid : 0, t->proc ? t->proc->comm : "", gpc);
	frame[DF_PC] = gpc;
	return raise_sig(t, frame, (gpc & 1) ? LX_SIGILL : LX_SIGSEGV);
}

static lx_u32 end_critical(lx_thread *t, lx_u32 *frame, lx_u32 host)
{
	while (lx_cpu.kill_req) {
		/* the kernel wanted this thread gone while we were busy */
		lx_cpu.kill_req = 0;
		kernel_call(t, frame, LX_NR_dbt_yield);
		host = continue_at(t, frame, frame[DF_PC]);
	}
	lx_cpu.critical = 0;
	return host;
}

static const char *insn_name(lx_u32 i)
{
	lx_u32 cp = (i >> 8) & 15;
	if ((i >> 28) == 0xF) return (i & 0x0E000000u) == 0x0A000000u ? "blx (into Thumb)" : (i & 0x0F000000u) == 0x02000000u || (i & 0x0F000000u) == 0x03000000u ? "a NEON instruction" : "an ARMv7 instruction";
	if ((i & 0x0E000000u) == 0x0C000000u || (i & 0x0F000000u) == 0x0E000000u) {
		if (cp == 10 || cp == 11) return "a VFP instruction";
		if (cp == 15) return "a coprocessor 15 operation";
		return "a coprocessor instruction";
	}
	return "an instruction";
}

lx_u32 dbt_runtime(lx_u32 *desc, lx_u32 *frame)
{
	lx_thread *t = lx_cpu.cur;
	lx_u32 host = 0, pc;
	int sig;
	lx_cpu.critical = 1;
	switch (desc[0]) {
	case DBT_HELPER: {
		lx_u32 insn;
		pc = desc[1];
		insn = *(lx_u32 *)pc;
		lx_cpu.helpers++;
		if (lx_k.trace) dbt_profile(insn);
		frame[DF_PC] = pc + 4;
		sig = dbt_interp(t, frame, pc, insn);
		if (sig) {
			frame[DF_PC] = pc;
			bk_log("pid %d (%s): %s it cannot do: %08x at %x", t->proc->pid, t->proc->comm, insn_name(insn), insn, pc);
			host = raise_sig(t, frame, sig);
		} else if (frame[DF_PC] != pc + 4) host = continue_at(t, frame, frame[DF_PC]);   /* it branched */
		else host = (lx_u32)(desc + 2);
		break;
	}
	case DBT_FIXUP: {
		/* a patched site (dbt_patch_unaligned): { FIXUP, host insn, back to } */
		lx_u32 sv = frame[DF_PC];
		lx_cpu.fixed_calls++;
		frame[DF_PC] = 0;
		if (fixup(frame, desc[1], 0) < 0) {
			lx_u32 g = 0, h = 0;
			frame[DF_PC] = dbt_block_of(desc[2] - 4, &g, &h) >= 0 ? g : sv;   /* (the block's start: near enough) */
			bk_log("pid %d (%s): an unaligned access it cannot do: %08x", t->proc->pid, t->proc->comm, desc[1]);
			host = raise_sig(t, frame, LX_SIGBUS);
			break;
		}
		frame[DF_PC] = sv;
		host = desc[2];
		break;
	}
	case DBT_CHAIN: {
		lx_u32 g = dbt_gen();
		frame[DF_PC] = desc[1];
		host = continue_at(t, frame, desc[1]);
		if (host && dbt_gen() == g && frame[DF_PC] == desc[1]) dbt_chain(desc[2], host, g);
		break;
	}
	case DBT_MISS:
		lx_cpu.misses++;
		frame[DF_PC] = lx_cpu.target;
		if (lx_cpu.yield) kernel_call(t, frame, LX_NR_dbt_yield);
		host = continue_at(t, frame, frame[DF_PC]);
		break;
	case DBT_SVC:
		frame[DF_PC] = desc[1] + 4;
		kernel_call(t, frame, 0);
		host = continue_at(t, frame, frame[DF_PC]);
		break;
	case DBT_UNDEF: {
		lx_u32 insn = *(lx_u32 *)desc[1];
		frame[DF_PC] = desc[1];
		if ((insn & 0x0FF000F0u) == 0x01200070u) host = raise_sig(t, frame, LX_SIGTRAP);   /* bkpt */
		else {
			bk_log("pid %d (%s): illegal instruction %08x at %x (%s)", t->proc->pid, t->proc->comm, insn, desc[1], insn_name(insn));
			host = raise_sig(t, frame, LX_SIGILL);
		}
		break;
	}
	default:   /* DBT_SEGV */
		frame[DF_PC] = desc[1];
		bk_log("pid %d (%s): jumped to %x, where nothing is mapped", t->proc->pid, t->proc->comm, desc[1]);
		host = raise_sig(t, frame, LX_SIGSEGV);
		break;
	}
	return end_critical(t, frame, host);
}

/* a new translated thread starts here (from lx_thread_start), holding the CPU */
void dbt_thread_start(lx_thread *t)
{
	lx_u32 sp, host, *frame = t->dframe;
	int k;
	__asm__ __volatile__("mov %0, sp" : "=r"(sp));
	t->hostsp = (sp - 64) & ~7u;
	for (k = 0; k < 16; k++) frame[k] = t->ctx[k];
	frame[DF_CPSR] = t->ctx_cpsr;
	lx_cpu.critical = 1;
	if (t->sigframe_len) {
		bk_memcpy((void *)t->sigframe_at, (const void *)t->sigframe_src, t->sigframe_len);
		t->sigframe_len = 0;
	}
	host = continue_at(t, frame, frame[DF_PC]);
	host = end_critical(t, frame, host);
	dbt_continue(host);
}
