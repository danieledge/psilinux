/* lx_rand.c - the kernel's random numbers: getrandom, /dev/random and
 * /dev/urandom, AT_RANDOM.
 *
 * A ChaCha20 generator with fast key erasure: each request is a keystream
 * under the current key, and the key is then replaced by more keystream, so
 * what was given out cannot be worked back from a later state. Entropy is
 * folded into the key before the next request:
 *  - at the first request: the clocks, the backend's own random numbers and
 *    the free memory (on EPOC these are guessable: Math::Rand and the tick);
 *  - all along: the time of every key typed and of every network or timer
 *    event (lx_rand_event);
 *  - whatever is written to /dev/urandom or /dev/random, as on Linux. The
 *    Alpine root saves 64 bytes at each start and writes them back at the
 *    next (/usr/sbin/psilinux-seed), so the pool carries over between
 *    sessions; the first session after an install is the weakest.
 * It never blocks (a 5mx has no hardware source to wait for). */
#include "lx_core.h"

static lx_u32 key[8];
static lx_u32 pool[16];         /* events and stirred bytes, folded into the key at the next request */
static lx_u32 pool_at, pending, seeded;
static lx_u32 nonce;

#define ROTL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) \
	a += b; d ^= a; d = ROTL(d, 16); c += d; b ^= c; b = ROTL(b, 12); \
	a += b; d ^= a; d = ROTL(d, 8);  c += d; b ^= c; b = ROTL(b, 7)

static void chacha(const lx_u32 *k, lx_u32 ctr, lx_u32 n0, lx_u32 n1, lx_u32 *out)
{
	lx_u32 x[16], in[16];
	int i;
	in[0] = 0x61707865; in[1] = 0x3320646e; in[2] = 0x79622d32; in[3] = 0x6b206574;
	for (i = 0; i < 8; i++) in[4 + i] = k[i];
	in[12] = ctr; in[13] = 0; in[14] = n0; in[15] = n1;
	for (i = 0; i < 16; i++) x[i] = in[i];
	for (i = 0; i < 10; i++) {
		QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]);
		QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
		QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
		QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
	}
	for (i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

/* the pool into the key: key' = ChaCha20(key ^ pool[0..7], pool[8..15]) */
static void fold(void)
{
	lx_u32 k[8], out[16];
	int i;
	for (i = 0; i < 8; i++) k[i] = key[i] ^ pool[i];
	chacha(k, pool[8] ^ pool[12], pool[9] ^ pool[13] ^ pool[10], pool[11] ^ pool[14] ^ pool[15], out);
	for (i = 0; i < 8; i++) key[i] = out[i] ^ out[8 + i];
	for (i = 0; i < 16; i++) pool[i] = 0;
	pool_at = 0;
	pending = 0;
}

void lx_rand_stir(const void *p, lx_u32 n)
{
	const lx_u8 *b = (const lx_u8 *)p;
	lx_u32 i, w = 0;
	for (i = 0; i < n; i++) {
		w = (w << 8) | b[i];
		if ((i & 3) == 3 || i == n - 1) {
			pool[pool_at] = ROTL(pool[pool_at], 7) ^ w;
			if (++pool_at == 16) fold();     /* (64 bytes in: fold them before taking more) */
			w = 0;
		}
	}
	pending = 1;
}

/* cheap: an event's time (and what it was), for the next request */
void lx_rand_event(lx_u32 what)
{
	lx_s64 t = bk_now_mono();
	lx_u32 v = (lx_u32)t ^ ((lx_u32)(t >> 32) * 0x9E3779B1u) ^ (what * 0x85EBCA6Bu);
	pool[pool_at & 15] = ROTL(pool[pool_at & 15], 5) ^ v;
	pool_at = (pool_at + 1) & 15;
	pending = 1;
}

static void seed(void)
{
	lx_u32 s[16], total = 0, freeb = 0;
	lx_s64 now = bk_now_real(), mono = bk_now_mono();
	int i;
	for (i = 0; i < 10; i++) s[i] = bk_random();
	s[10] = (lx_u32)now; s[11] = (lx_u32)(now >> 32);
	s[12] = (lx_u32)mono; s[13] = (lx_u32)(mono >> 32);
	bk_mem_info(&total, &freeb);
	s[14] = freeb; s[15] = (lx_u32)(long)&s;
	lx_rand_stir(s, sizeof s);
	seeded = 1;
}

void lx_rand_bytes(void *buf, lx_u32 n)
{
	lx_u8 *o = (lx_u8 *)buf;
	lx_u32 blk[16], ctr = 0;
	int i;
	if (!seeded) seed();
	if (pending) fold();
	nonce++;
	while (n) {
		lx_u32 k = n < 64 ? n : 64;
		chacha(key, ++ctr, nonce, 0x70736c78, blk);
		bk_memcpy(o, blk, k);
		o += k;
		n -= k;
	}
	/* fast key erasure: a new key, from a block nobody sees */
	chacha(key, 0, nonce, 0x6b657921, blk);
	for (i = 0; i < 8; i++) key[i] = blk[i];
	for (i = 0; i < 16; i++) blk[i] = 0;
}
