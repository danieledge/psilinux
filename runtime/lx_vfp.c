/* lx_vfp.c - Alpine mode: VFPv2 for the ARM710T, which has no FPU.
 *
 * Alpine's armhf programs pass floating-point values in VFP registers and
 * compute with VFP instructions. The translator hands every VFP instruction
 * to this file (DBT_HELPER), which keeps the register file in the thread
 * (t->vfp: s0-s31, which are d0-d15; t->fpscr) and does the arithmetic with
 * Berkeley SoftFloat 3e, specialised for VFPv2 (softfloat/). The FPSCR's
 * rounding mode and cumulative exception flags are honoured; traps, flush
 * to zero, default NaN and short vectors are not (Linux armhf does not use
 * them). VFPv3 and later instructions raise SIGILL, as on a VFPv2 CPU. */
#include "lx_dbt.h"
#include <stdint.h>
#include <stdbool.h>
#include "softfloat/softfloat.h"

lx_u32 dbt_ld32(lx_u32 a);
void dbt_st32(lx_u32 a, lx_u32 v);

#define FPSCR_RMODE(f) (((f) >> 22) & 3)

static float32_t S(lx_thread *t, int n) { float32_t r; r.v = t->vfp[n]; return r; }
static void setS(lx_thread *t, int n, float32_t v) { t->vfp[n] = v.v; }
static float64_t D(lx_thread *t, int n) { float64_t r; r.v = ((uint64_t)t->vfp[2 * n + 1] << 32) | t->vfp[2 * n]; return r; }
static void setD(lx_thread *t, int n, float64_t v) { t->vfp[2 * n] = (lx_u32)v.v; t->vfp[2 * n + 1] = (lx_u32)(v.v >> 32); }

static const uint_fast8_t rmodes[4] = { softfloat_round_near_even, softfloat_round_max, softfloat_round_min, softfloat_round_minMag };

static void begin(lx_thread *t)
{
	softfloat_roundingMode = rmodes[FPSCR_RMODE(t->fpscr)];
	softfloat_exceptionFlags = 0;
}

static void end(lx_thread *t)
{
	uint_fast8_t x = softfloat_exceptionFlags;
	lx_u32 f = t->fpscr;
	if (x & softfloat_flag_invalid) f |= 1u << 0;
	if (x & softfloat_flag_infinite) f |= 1u << 1;
	if (x & softfloat_flag_overflow) f |= 1u << 2;
	if (x & softfloat_flag_underflow) f |= 1u << 3;
	if (x & softfloat_flag_inexact) f |= 1u << 4;
	t->fpscr = f;
}

static int nan32(lx_u32 v) { return (v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu); }
static int nan64(uint64_t v) { return (v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (v & 0x000FFFFFFFFFFFFFull); }
static int snan32(lx_u32 v) { return nan32(v) && !(v & 0x00400000u); }
static int snan64(uint64_t v) { return nan64(v) && !(v & 0x0008000000000000ull); }

/* vcmp: FPSCR's N Z C V */
static void compare(lx_thread *t, int dbl, uint64_t a, uint64_t b, int e)
{
	lx_u32 fl;
	int un = dbl ? (nan64(a) || nan64(b)) : (nan32((lx_u32)a) || nan32((lx_u32)b));
	if (un) {
		if (e || (dbl ? (snan64(a) || snan64(b)) : (snan32((lx_u32)a) || snan32((lx_u32)b)))) t->fpscr |= 1u;   /* IOC */
		fl = 0x30000000u;
	} else {
		int eq, lt;
		if (dbl) { float64_t x, y; x.v = a; y.v = b; eq = f64_eq(x, y); lt = f64_lt_quiet(x, y); }
		else { float32_t x, y; x.v = (lx_u32)a; y.v = (lx_u32)b; eq = f32_eq(x, y); lt = f32_lt_quiet(x, y); }
		fl = eq ? 0x60000000u : lt ? 0x80000000u : 0x20000000u;
	}
	t->fpscr = (t->fpscr & 0x0FFFFFFFu) | fl;
}

/* the data-processing instructions (CDP form) */
static int vfp_dp(lx_thread *t, lx_u32 i)
{
	int dbl = (int)(i >> 8) & 1, op = (int)(i >> 6) & 1;
	int p = (int)(i >> 23) & 1, q = (int)(i >> 21) & 1, r = (int)(i >> 20) & 1;
	int Vd = (int)(i >> 12) & 15, Vn = (int)(i >> 16) & 15, Vm = (int)(i & 15);
	int Db = (int)(i >> 22) & 1, Nb = (int)(i >> 7) & 1, Mb = (int)(i >> 5) & 1;
	int sd = (Vd << 1) | Db, sn = (Vn << 1) | Nb, sm = (Vm << 1) | Mb;
	int dd = Vd | (Db << 4), dn = Vn | (Nb << 4), dm = Vm | (Mb << 4);
	if (dbl && (Db || Nb || Mb) && !(p && q && r)) return LX_SIGILL;      /* d16-d31: not on VFPv2 */
	begin(t);
	if (!(p && q && r)) {
		int kind = (p << 2) | (q << 1) | r;     /* 0 mla/mls, 1 nmls/nmla, 2 mul/nmul, 3 add/sub, 4 div */
		if (kind > 4) return LX_SIGILL;
		if (kind == 4 && op) return LX_SIGILL;
		if (dbl) {
			float64_t a = D(t, dn), b = D(t, dm), res;
			switch (kind) {
			case 3: res = op ? f64_sub(a, b) : f64_add(a, b); break;
			case 2: res = f64_mul(a, b); if (op) res.v ^= 0x8000000000000000ull; break;
			case 4: res = f64_div(a, b); break;
			default: {
				float64_t pr = f64_mul(a, b), acc = D(t, dd);
				if (kind == 0) res = op ? f64_sub(acc, pr) : f64_add(acc, pr);        /* vmla, vmls */
				else { acc.v ^= 0x8000000000000000ull; res = op ? f64_sub(acc, pr) : f64_add(acc, pr); }   /* vnmls, vnmla */
			}
			}
			setD(t, dd, res);
		} else {
			float32_t a = S(t, sn), b = S(t, sm), res;
			switch (kind) {
			case 3: res = op ? f32_sub(a, b) : f32_add(a, b); break;
			case 2: res = f32_mul(a, b); if (op) res.v ^= 0x80000000u; break;
			case 4: res = f32_div(a, b); break;
			default: {
				float32_t pr = f32_mul(a, b), acc = S(t, sd);
				if (kind == 0) res = op ? f32_sub(acc, pr) : f32_add(acc, pr);
				else { acc.v ^= 0x80000000u; res = op ? f32_sub(acc, pr) : f32_add(acc, pr); }
			}
			}
			setS(t, sd, res);
		}
		end(t);
		return 0;
	}
	/* the "other" instructions: opc2 in Vn, opc3 = bits 7:6 */
	{
		int opc3 = (int)(i >> 6) & 3;
		switch (Vn) {
		case 0:
			if (opc3 == 1) { if (dbl) { if (Db || Mb) return LX_SIGILL; setD(t, dd, D(t, dm)); } else t->vfp[sd] = t->vfp[sm]; return 0; }   /* vmov */
			if (opc3 == 3) { if (dbl) { if (Db || Mb) return LX_SIGILL; t->vfp[2 * dd] = t->vfp[2 * dm]; t->vfp[2 * dd + 1] = t->vfp[2 * dm + 1] & 0x7FFFFFFFu; } else t->vfp[sd] = t->vfp[sm] & 0x7FFFFFFFu; return 0; }   /* vabs */
			return LX_SIGILL;
		case 1:
			if (opc3 == 1) { if (dbl) { if (Db || Mb) return LX_SIGILL; t->vfp[2 * dd] = t->vfp[2 * dm]; t->vfp[2 * dd + 1] = t->vfp[2 * dm + 1] ^ 0x80000000u; } else t->vfp[sd] = t->vfp[sm] ^ 0x80000000u; return 0; }   /* vneg */
			if (opc3 == 3) {                    /* vsqrt */
				if (dbl) { if (Db || Mb) return LX_SIGILL; setD(t, dd, f64_sqrt(D(t, dm))); }
				else setS(t, sd, f32_sqrt(S(t, sm)));
				end(t);
				return 0;
			}
			return LX_SIGILL;
		case 4: case 5: {                       /* vcmp, vcmpe (with zero: 5) */
			int e = (int)(i >> 7) & 1;
			if (dbl) {
				if (Db || (Vn == 4 && Mb)) return LX_SIGILL;
				compare(t, 1, D(t, dd).v, Vn == 5 ? 0 : D(t, dm).v, e);
			} else compare(t, 0, t->vfp[sd], Vn == 5 ? 0 : t->vfp[sm], e);
			return 0;
		}
		case 7:                                 /* vcvt between double and single */
			if (opc3 != 3) return LX_SIGILL;
			if (dbl) { if (Mb) return LX_SIGILL; setS(t, sd, f64_to_f32(D(t, dm))); }
			else { if (Db) return LX_SIGILL; setD(t, dd, f32_to_f64(S(t, sm))); }
			end(t);
			return 0;
		case 8: {                               /* vcvt from a 32-bit integer (in Sm) */
			int sgn = (int)(i >> 7) & 1;
			lx_u32 v = t->vfp[sm];
			if (!(opc3 & 1)) return LX_SIGILL;
			if (dbl) { if (Db) return LX_SIGILL; setD(t, dd, sgn ? i32_to_f64((int32_t)v) : ui32_to_f64(v)); }
			else setS(t, sd, sgn ? i32_to_f32((int32_t)v) : ui32_to_f32(v));
			end(t);
			return 0;
		}
		case 12: case 13: {                     /* vcvt(r) to a 32-bit integer (into Sd) */
			int sgn = Vn == 13, rz = (int)(i >> 7) & 1;
			lx_u32 v;
			if (!(opc3 & 1)) return LX_SIGILL;
			if (dbl) {
				float64_t a;
				if (Mb) return LX_SIGILL;
				a = D(t, dm);
				if (sgn) v = (lx_u32)(rz ? f64_to_i32_r_minMag(a, true) : f64_to_i32(a, softfloat_roundingMode, true));
				else v = rz ? f64_to_ui32_r_minMag(a, true) : f64_to_ui32(a, softfloat_roundingMode, true);
			} else {
				float32_t a = S(t, sm);
				if (sgn) v = (lx_u32)(rz ? f32_to_i32_r_minMag(a, true) : f32_to_i32(a, softfloat_roundingMode, true));
				else v = rz ? f32_to_ui32_r_minMag(a, true) : f32_to_ui32(a, softfloat_roundingMode, true);
			}
			t->vfp[sd] = v;
			end(t);
			return 0;
		}
		}
	}
	return LX_SIGILL;
}

int lx_vfp(lx_thread *t, lx_u32 *f, lx_u32 pc, lx_u32 i)
{
	int dbl = (int)(i >> 8) & 1;
	if ((i & 0x0E000000u) == 0x0C000000u) {
		int P = (int)(i >> 24) & 1, U = (int)(i >> 23) & 1, Db = (int)(i >> 22) & 1, W = (int)(i >> 21) & 1, L = (int)(i >> 20) & 1;
		int rn = (int)(i >> 16) & 15, Vd = (int)(i >> 12) & 15;
		lx_u32 imm = i & 0xFF, base, a;
		int first, words, k;
		if ((i & 0x0FE00000u) == 0x0C400000u) {
			/* vmov between two core registers and a double, or two singles */
			int rt = Vd, rt2 = rn, vm = (int)(i & 15), mb = (int)(i >> 5) & 1, s0;
			if (rt == 15 || rt2 == 15) return LX_SIGILL;
			if (dbl) { if (mb) return LX_SIGILL; s0 = 2 * vm; }
			else { s0 = (vm << 1) | mb; if (s0 == 31) return LX_SIGILL; }
			if (L) { f[rt] = t->vfp[s0]; f[rt2] = t->vfp[s0 + 1]; }
			else { t->vfp[s0] = f[rt]; t->vfp[s0 + 1] = f[rt2]; }
			return 0;
		}
		base = rn == 15 ? (pc & ~3u) + 8 : f[rn];
		if (P && !W) {
			/* vldr / vstr */
			a = U ? base + imm * 4 : base - imm * 4;
			if (dbl) {
				if (Db) return LX_SIGILL;
				if (L) { t->vfp[2 * Vd] = dbt_ld32(a); t->vfp[2 * Vd + 1] = dbt_ld32(a + 4); }
				else { dbt_st32(a, t->vfp[2 * Vd]); dbt_st32(a + 4, t->vfp[2 * Vd + 1]); }
			} else {
				int sd = (Vd << 1) | Db;
				if (L) t->vfp[sd] = dbt_ld32(a); else dbt_st32(a, t->vfp[sd]);
			}
			return 0;
		}
		/* vldm / vstm (and vpush, vpop) */
		if (P == U || rn == 15) return LX_SIGILL;
		if (dbl) { if (Db) return LX_SIGILL; first = 2 * Vd; words = (int)(imm & ~1u); }
		else { first = (Vd << 1) | Db; words = (int)imm; }
		if (!words || first + words > 32) return LX_SIGILL;
		a = U ? base : base - imm * 4;
		for (k = 0; k < words; k++, a += 4) {
			if (L) t->vfp[first + k] = dbt_ld32(a); else dbt_st32(a, t->vfp[first + k]);
		}
		if (W) f[rn] = U ? base + imm * 4 : base - imm * 4;
		return 0;
	}
	if ((i & 0x0F000010u) == 0x0E000000u) return vfp_dp(t, i);
	if ((i & 0x0F000010u) == 0x0E000010u) {
		int L = (int)(i >> 20) & 1, op1 = (int)(i >> 21) & 7, rt = (int)(i >> 12) & 15, vn = (int)(i >> 16) & 15, nb = (int)(i >> 7) & 1;
		if (!dbl && op1 == 0) {                 /* vmov sn, rt / rt, sn */
			int sn = (vn << 1) | nb;
			if (rt == 15) return LX_SIGILL;
			if (L) f[rt] = t->vfp[sn]; else t->vfp[sn] = f[rt];
			return 0;
		}
		if (dbl && (op1 & 6) == 0) {            /* vmov.32 dn[x], rt (fmdlr, fmdhr, fmrdl, fmrdh) */
			int w = 2 * vn + (op1 & 1);
			if (nb || rt == 15) return LX_SIGILL;
			if (L) f[rt] = t->vfp[w]; else t->vfp[w] = f[rt];
			return 0;
		}
		if (!dbl && op1 == 7) {                 /* vmrs / vmsr */
			lx_u32 v;
			switch (vn) {
			case 1: v = t->fpscr; break;
			case 0: v = 0x410120B4u; break;     /* FPSID: a VFP11, VFPv2 (ARM1176's) */
			case 8: v = 0x40000000u; break;     /* FPEXC: enabled */
			default: return LX_SIGILL;
			}
			if (L) {
				if (rt == 15) f[DF_CPSR] = (f[DF_CPSR] & 0x0FFFFFFFu) | (t->fpscr & 0xF0000000u);
				else f[rt] = v;
			} else {
				if (rt == 15) return LX_SIGILL;
				if (vn == 1) t->fpscr = f[rt] & 0xFFC0009Fu;   /* (no length, stride or traps) */
			}
			return 0;
		}
	}
	return LX_SIGILL;
}
