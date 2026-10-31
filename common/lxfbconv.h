/* lxfbconv.h - the Linux screen's 8-bit pixels to the Psion's 16 greys: the
 * app's drawing, and the host harness's pictures of it (so a test sees what
 * the Psion shows). Plain C, inline.
 *
 * Each palette entry's grey (0-255) becomes one of 16, with a 2x2 ordered
 * dither for the greys between: a grey that is one of the 16 exactly comes
 * out flat. EPOC's 16-grey bitmaps (EGray16) hold two pixels a byte, the
 * left one in the low four bits; 0 is black. The tables are built when the
 * palette changes (lx_fbconv_tables); a row is then a table look-up a pixel. */
#ifndef LXFBCONV_H
#define LXFBCONV_H

typedef struct
	{
	unsigned char lo[2][256];          /* row parity: the left pixel's grey */
	unsigned char hi[2][256];          /* the right pixel's, already in the high 4 bits */
	} LxFbConv;

static inline void lx_fbconv_tables(LxFbConv *c, const unsigned char *grey)
	{
	/* Bayer 2x2 thresholds, in 16ths of a level: row 0 (1, 9), row 1 (13, 5) */
	static const int t[2][2] = { { 2, 10 }, { 14, 6 } };
	int i, y, x;
	for (y = 0; y < 2; y++)
		for (x = 0; x < 2; x++)
			for (i = 0; i < 256; i++)
				{
				int v = (grey[i] * 15 * 16 + 127) / 255 + t[y][x];   /* (16ths of a level) */
				int l = v >> 4;
				if (grey[i] == 255) l = 15;
				if (l > 15) l = 15;
				if (x) c->hi[y][i] = (unsigned char)(l << 4);
				else c->lo[y][i] = (unsigned char)l;
				}
	}

/* one row's pixels [x0, x1) (x0 and x1 even) into the 16-grey row d (d is the
 * whole row: byte x/2 is pixels x and x+1) */
static inline void lx_fbconv_row(const LxFbConv *c, int y, const unsigned char *s, unsigned char *d, int x0, int x1)
	{
	const unsigned char *lo = c->lo[y & 1], *hi = c->hi[y & 1];
	const unsigned char *e = s + x1;
	s += x0;
	d += x0 >> 1;
	while (s + 8 <= e)
		{
		d[0] = (unsigned char)(lo[s[0]] | hi[s[1]]);
		d[1] = (unsigned char)(lo[s[2]] | hi[s[3]]);
		d[2] = (unsigned char)(lo[s[4]] | hi[s[5]]);
		d[3] = (unsigned char)(lo[s[6]] | hi[s[7]]);
		s += 8; d += 4;
		}
	while (s < e)
		{
		*d++ = (unsigned char)(lo[s[0]] | hi[s[1]]);
		s += 2;
		}
	}

#endif
