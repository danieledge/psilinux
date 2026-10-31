/* splash.c - the "Starting PsiLinux" screen, drawn in memory (one byte a
 * pixel, the grey level 0-15) so that the same picture comes from two
 * places: psisplash (splashmain.c), which puts it straight on /dev/fb0 before
 * the X server is up, and psiwm, which shows it while the desktop starts. */
#include <string.h>
#include "font.h"
#include "icons.h"
#include "splash.h"

static unsigned char *S;

static void sfill(int x, int y, int w, int h, int g)
	{
	int j;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > SPW) w = SPW - x;
	if (y + h > SPH) h = SPH - y;
	for (j = 0; j < h; j++) memset(S + (y + j) * SPW + x, g, w > 0 ? w : 0);
	}

static void sbox(int x, int y, int w, int h, int g)
	{
	sfill(x, y, w, 1, g); sfill(x, y + h - 1, w, 1, g);
	sfill(x, y, 1, h, g); sfill(x + w - 1, y, 1, h, g);
	}

static int sglyph(const unsigned char **sp)
	{
	const unsigned char *s = *sp;
	int c = *s++;
	if (c == 0xc2 && *s == 0xa9) { s++; *sp = s; return 96; }       /* (c) */
	if (c == 0xe2 && s[0] == 0x80 && s[1] == 0xa6) { s += 2; *sp = s; return 95; }   /* ... */
	*sp = s;
	return c >= 32 && c < 127 ? c - 32 : '?' - 32;
	}

static int stw(const BFont *f, const char *s)
	{
	const unsigned char *p = (const unsigned char *)s;
	int w = 0;
	while (*p) w += f->g[sglyph(&p)].adv;
	return w;
	}

static int stext(const BFont *f, int x, int y, const char *s, int g)
	{
	const unsigned char *p = (const unsigned char *)s;
	int x0 = x;
	while (*p)
		{
		const Glyph *gl = &f->g[sglyph(&p)];
		const unsigned char *b = f->bits + gl->off;
		int r, i, gb = (gl->w + 7) / 8;
		for (r = 0; r < f->h; r++, b += gb)
			for (i = 0; i < gl->w; i++)
				if (b[i >> 3] & (1 << (i & 7)))
					{
					int X = x + gl->xo + i, Y = y + r;
					if (X >= 0 && X < SPW && Y >= 0 && Y < SPH) S[Y * SPW + X] = g;
					}
		x += gl->adv;
		}
	return x - x0;
	}

static void spic(int id, int x, int y, int k)
	{
	const Pic *p = &pics[id];
	int i, j;
	for (j = 0; j < p->h; j++)
		for (i = 0; i < p->w; i++)
			{
			int v = p->px[j * p->w + i];
			if (v != 255) sfill(x + i * k, y + j * k, k, k, v);
			}
	}

/* the screen: the penguin, the name, what is happening, how far it has got */
void splash_draw(unsigned char *buf, const char *msg, int percent, const char *version)
	{
	int x, y, w;
	char v[48];
	S = buf;
	sfill(0, 0, SPW, SPH, 15);
	/* a band top and bottom, as the Psion's own start-up screen frames its logo */
	sfill(0, 0, SPW, 6, 10);
	sfill(0, 6, SPW, 1, 0);
	sfill(0, SPH - 7, SPW, 1, 0);
	sfill(0, SPH - 6, SPW, 6, 10);
	/* the penguin and the words, centred as one */
	w = 96 + 24 + stw(&f_title, "Linux on the Psion Series 5mx");
	x = (SPW - w) / 2;
	spic(I_P_PENGUIN, x, 34, 2);
	x += 96 + 24;
	stext(&f_huge, x, 44, "PsiLinux", 0);
	stext(&f_title, x + 2, 44 + f_huge.h + 4, "Linux on the Psion Series 5mx", 5);
	strcpy(v, "Version ");
	strncat(v, version, sizeof v - 10);
	stext(&f_normal, x + 2, 44 + f_huge.h + 24, v, 5);
	/* the progress: an EIKON progress bar, sunk */
	y = 166; w = 300; x = (SPW - w) / 2;
	stext(&f_bold, x, y - f_bold.h - 4, msg, 0);
	sbox(x, y, w, 16, 0);
	sfill(x + 1, y + 1, w - 2, 14, 15);
	sfill(x + 1, y + 1, w - 2, 1, 10);
	sfill(x + 1, y + 1, 1, 14, 10);
	if (percent > 100) percent = 100;
	if (percent > 0)
		{
		int f = (w - 4) * percent / 100, k;
		sfill(x + 2, y + 2, f, 12, 5);
		/* (segments, as the Psion's progress bars have) */
		for (k = x + 2 + 12; k < x + 2 + f; k += 12) sfill(k, y + 2, 1, 12, 15);
		}
	stext(&f_small, (SPW - stw(&f_small, "© 2026 Dan Edge")) / 2, SPH - 7 - f_small.h - 6, "© 2026 Dan Edge", 8);
	}
