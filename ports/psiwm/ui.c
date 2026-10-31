/* ui.c - drawing in the Psion's style, and the EIKON controls psiwm's
 * programs share: the menu bar and its panes, pop-up lists, dialogs, the
 * line editor, infoprints and busy messages, the clipboard. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/utsname.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include "psiwm.h"

Drawable D;
Pixmap backbuf;
static GC gc, gst, g1, gpic;
static Pixmap tpix;                 /* text: one line of bits, 1 deep */
static Pixmap picpm[I_COUNT], picmask[I_COUNT];
static unsigned long curfg = ~0UL;

long now_ms(void)
	{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000;
	}

static void fg(int g)
	{
	if (pix[g] != curfg)
		XSetForeground(dpy, gc, curfg = pix[g]);
	}

void ui_init(void)
	{
	int i, x, y;
	backbuf = XCreatePixmap(dpy, root, SW, SH, DefaultDepth(dpy, DefaultScreen(dpy)));
	D = backbuf;
	gc = XCreateGC(dpy, root, 0, 0);
	gpic = XCreateGC(dpy, root, 0, 0);
	XSetGraphicsExposures(dpy, gc, False);
	XSetGraphicsExposures(dpy, gpic, False);
	tpix = XCreatePixmap(dpy, root, SW, 48, 1);
	g1 = XCreateGC(dpy, tpix, 0, 0);
	XSetForeground(dpy, g1, 1);
	XSetBackground(dpy, g1, 0);
	{
	XGCValues v;
	v.fill_style = FillStippled;
	v.stipple = tpix;
	v.graphics_exposures = False;
	gst = XCreateGC(dpy, root, GCFillStyle | GCStipple | GCGraphicsExposures, &v);
	}
	/* the pictures: a pixmap and a mask each */
	for (i = 0; i < I_COUNT; i++)
		{
		const Pic *p = &pics[i];
		int bpl = (p->w + 7) / 8;
		char *mb = calloc(bpl * p->h, 1);
		XImage *im;
		picpm[i] = XCreatePixmap(dpy, root, p->w, p->h, DefaultDepth(dpy, DefaultScreen(dpy)));
		im = XGetImage(dpy, picpm[i], 0, 0, p->w, p->h, AllPlanes, ZPixmap);
		for (y = 0; y < p->h; y++)
			for (x = 0; x < p->w; x++)
				{
				int v = p->px[y * p->w + x];
				XPutPixel(im, x, y, pix[v == 255 ? WHITE : v]);
				if (v != 255) mb[y * bpl + x / 8] |= 1 << (x & 7);
				}
		XPutImage(dpy, picpm[i], gc, im, 0, 0, 0, 0, p->w, p->h);
		XDestroyImage(im);
		picmask[i] = XCreateBitmapFromData(dpy, root, mb, p->w, p->h);
		free(mb);
		}
	}

void fill(int x, int y, int w, int h, int g)
	{
	if (w <= 0 || h <= 0) return;
	fg(g);
	XFillRectangle(dpy, D, gc, x, y, w, h);
	}

void box(int x, int y, int w, int h, int g)
	{
	if (w <= 0 || h <= 0) return;
	fg(g);
	XDrawRectangle(dpy, D, gc, x, y, w - 1, h - 1);
	}

void line(int x1, int y1, int x2, int y2, int g, int width)
	{
	fg(g);
	XSetLineAttributes(dpy, gc, width > 1 ? width : 0, LineSolid, width > 1 ? CapRound : CapButt, JoinRound);
	XDrawLine(dpy, D, gc, x1, y1, x2, y2);
	if (width > 1) XSetLineAttributes(dpy, gc, 0, LineSolid, CapButt, JoinMiter);
	}

void disc(int x, int y, int r, int g)
	{
	fg(g);
	XFillArc(dpy, D, gc, x - r, y - r, 2 * r, 2 * r, 0, 360 * 64);
	}

/* two greys in a 1-pixel check (EIKON's patterned bands) */
void fill2(int x, int y, int w, int h, int g1_, int g2_)
	{
	static Pixmap tiles[8]; static int keys[8], n;
	int i, k = g1_ * 16 + g2_;
	XGCValues v;
	if (w <= 0 || h <= 0) return;
	for (i = 0; i < n && keys[i] != k; i++) ;
	if (i == n)
		{
		if (n == 8) i = 7; else n++;
		if (tiles[i]) XFreePixmap(dpy, tiles[i]);
		tiles[i] = XCreatePixmap(dpy, root, 2, 2, DefaultDepth(dpy, DefaultScreen(dpy)));
		fg(g1_); XFillRectangle(dpy, tiles[i], gc, 0, 0, 2, 2);
		fg(g2_); XDrawPoint(dpy, tiles[i], gc, 1, 0); XDrawPoint(dpy, tiles[i], gc, 0, 1);
		keys[i] = k;
		}
	v.fill_style = FillTiled; v.tile = tiles[i]; v.ts_x_origin = 0; v.ts_y_origin = 0;
	XChangeGC(dpy, gc, GCFillStyle | GCTile | GCTileStipXOrigin | GCTileStipYOrigin, &v);
	XFillRectangle(dpy, D, gc, x, y, w, h);
	XSetFillStyle(dpy, gc, FillSolid);
	}

/* ---- text: the fonts are bitmaps (font.h), set into a 1-deep line and
 * filled through it as a stipple, so text has no background ---- */

static int glyph_of(const unsigned char **sp, const unsigned char *e)
	{
	const unsigned char *s = *sp;
	int c = *s++;
	if (c >= 0x80)
		{
		int cp = 0, more = 0;
		if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; more = 1; }
		else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; more = 2; }
		else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; more = 3; }
		while (more-- > 0 && s < e && (*s & 0xc0) == 0x80) cp = cp << 6 | (*s++ & 0x3f);
		*sp = s;
		switch (cp)
			{
		case 0x2026: return 95;
		case 0xa9: return 96;
		case 0xb0: return 97;
		case 0xa3: return 98;
		case 0xd7: return 99;
		case 0xf7: return 100;
		case 0x2212: return 101;
		case 0xb1: return 102;
			}
		return '?' - 32;
		}
	*sp = s;
	if (c < 32 || c > 126) return '?' - 32;
	return c - 32;
	}

int tw(const BFont *f, const char *s, int n)
	{
	const unsigned char *p = (const unsigned char *)s, *e;
	int w = 0;
	if (n < 0) n = strlen(s);
	e = p + n;
	while (p < e) w += f->g[glyph_of(&p, e)].adv;
	return w;
	}

static unsigned char tbits[48 * (SW / 8)];

int text(const BFont *f, int x, int y, const char *s, int n, int g)
	{
	const unsigned char *p = (const unsigned char *)s, *e;
	int w, bpl, px = 0, r, i;
	XImage im;
	if (n < 0) n = strlen(s);
	w = tw(f, s, n);
	if (w <= 0) return 0;
	if (w > SW) w = SW;
	bpl = (w + 7) / 8;
	memset(tbits, 0, bpl * f->h);
	e = p + n;
	while (p < e)
		{
		const Glyph *gl = &f->g[glyph_of(&p, e)];
		const unsigned char *b = f->bits + gl->off;
		int gb = (gl->w + 7) / 8;
		if (gl->w)
			for (r = 0; r < f->h; r++, b += gb)
				for (i = 0; i < gl->w; i++)
					if (b[i >> 3] & (1 << (i & 7)))
						{
						int X = px + gl->xo + i;
						if (X < w) tbits[r * bpl + (X >> 3)] |= 1 << (X & 7);
						}
		px += gl->adv;
		}
	memset(&im, 0, sizeof im);
	im.width = w; im.height = f->h; im.format = XYBitmap; im.data = (char *)tbits;
	im.byte_order = LSBFirst; im.bitmap_unit = 8; im.bitmap_bit_order = LSBFirst;
	im.bitmap_pad = 8; im.depth = 1; im.bytes_per_line = bpl; im.bits_per_pixel = 1;
	XInitImage(&im);
	XPutImage(dpy, tpix, g1, &im, 0, 0, 0, 0, w, f->h);
	XSetForeground(dpy, gst, pix[g]);
	XSetTSOrigin(dpy, gst, x, y);
	XFillRectangle(dpy, D, gst, x, y, w, f->h);
	return w;
	}

int textfit(const BFont *f, int x, int y, const char *s, int maxw, int g)
	{
	int n = strlen(s), ew;
	if (tw(f, s, n) <= maxw) return text(f, x, y, s, n, g);
	ew = tw(f, "…", -1);
	while (n > 0 && tw(f, s, n) + ew > maxw) n--;
	while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) n--;   /* (not inside a character) */
	x += text(f, x, y, s, n, g);
	return text(f, x, y, "…", -1, g);
	}

void textc(const BFont *f, int x, int y, int w, const char *s, int g)
	{
	text(f, x + (w - tw(f, s, -1)) / 2, y, s, -1, g);
	}

int pic_right(int id)
	{
	const Pic *p = &pics[id];
	int x, y, r = 0;
	for (y = 0; y < p->h; y++)
		for (x = r; x < p->w; x++)
			if (p->px[y * p->w + x] != 255) r = x + 1;
	return r;
	}

void pic(int id, int x, int y)
	{
	XSetClipMask(dpy, gpic, picmask[id]);
	XSetClipOrigin(dpy, gpic, x, y);
	XCopyArea(dpy, picpm[id], D, gpic, 0, 0, pics[id].w, pics[id].h, x, y);
	}

/* an EIKON command button: black edge, white top-left, dark bottom-right */
void bevel(int x, int y, int w, int h, int down)
	{
	box(x, y, w, h, BLACK);
	fill(x + 1, y + 1, w - 2, h - 2, down ? MID : LIGHT);
	if (down)
		{
		fill(x + 1, y + 1, w - 2, 1, DARK);
		fill(x + 1, y + 1, 1, h - 2, DARK);
		}
	else
		{
		fill(x + 1, y + 1, w - 3, 1, WHITE);
		fill(x + 1, y + 1, 1, h - 3, WHITE);
		fill(x + 1, y + h - 3, w - 2, 2, DARK);
		fill(x + w - 3, y + 1, 2, h - 2, DARK);
		}
	}

void show(Window w, int x, int y, int cw, int ch)
	{
	XCopyArea(dpy, backbuf, w, gc, x, y, cw, ch, x, y);
	}

/* a window with a shadow to its right and below (shaped) */
static void shadow_shape(Window w, int cw, int ch)
	{
	XRectangle r[2];
	r[0].x = 0; r[0].y = 0; r[0].width = cw; r[0].height = ch;
	r[1].x = SHD; r[1].y = SHD; r[1].width = cw; r[1].height = ch;
	XShapeCombineRectangles(dpy, w, ShapeBounding, 0, 0, r, 2, ShapeSet, Unsorted);
	}

static Window popup_window(void)
	{
	XSetWindowAttributes a;
	a.override_redirect = True;
	a.save_under = True;
	a.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask;
	return XCreateWindow(dpy, root, 0, 0, 10, 10, 0, CopyFromParent, InputOutput, CopyFromParent,
		CWOverrideRedirect | CWSaveUnder | CWEventMask, &a);
	}

/* ---- menus: the menu bar, its panes, and pop-up lists ---- */

int menu_up;
static Window mbar, mpane;
static App *ma;                 /* whose (0: a foreign program's) */
static const Menu *mm;
static int mn, msel, mitem, mpop, mpopx, mpopy;
static int mbx[12], mbw[12];    /* the titles' positions */
static int pw, ph, px0;          /* the pane */
#define MIH 18                  /* an item's height */
#define MSEP 5

static int item_state(App *a, int cmd)
	{
	if (cmd >= C_CLOSE) return wm_state(a, cmd);
	return a && a->k->state ? a->k->state(a, cmd) : 0;
	}

static void run_cmd(App *a, int cmd)
	{
	if (item_state(a, cmd) & MS_DIM) { infoprint("Not available at this time"); return; }
	if (cmd >= C_CLOSE) wm_cmd(a, cmd);
	else if (a && a->k->cmd) a->k->cmd(a, cmd);
	}

void do_cmd(App *a, int cmd) { run_cmd(a, cmd); }

static void key_name(const char *k, int *ctrl, int *shift, KeySym *ks)
	{
	*ctrl = strstr(k, "Ctrl+") != 0;
	*shift = strstr(k, "Shift+") != 0;
	k = strrchr(k, '+') ? strrchr(k, '+') + 1 : k;
	if (!strcmp(k, "Tab")) *ks = XK_Tab;
	else if (!strcmp(k, "Del")) *ks = XK_Delete;
	else if (!strcmp(k, "Enter")) *ks = XK_Return;
	else if (!strcmp(k, "Space")) *ks = XK_space;
	else if (!strcmp(k, "Esc")) *ks = XK_Escape;
	else *ks = (unsigned char)(k[0] >= 'A' && k[0] <= 'Z' ? k[0] + 32 : k[0]);
	}

int menu_shortcut(App *a, const Menu *menus, int n, KeySym ks, unsigned st)
	{
	int i, j, c, s;
	KeySym k;
	if (ks >= 'A' && ks <= 'Z') ks += 32;
	for (i = 0; i < n; i++)
		for (j = 0; j < menus[i].n; j++)
			{
			const MenuItem *m = &menus[i].items[j];
			if (!m->key) continue;
			key_name(m->key, &c, &s, &k);
			if (k == ks && c == !!(st & ControlMask) && s == !!(st & ShiftMask))
				{
				run_cmd(a, m->cmd);
				return 1;
				}
			}
	return 0;
	}

static int pane_height(const Menu *m)
	{
	int i, h = 6;
	for (i = 0; i < m->n; i++) h += MIH + ((m->items[i].flags & MI_SEP) && i < m->n - 1 ? MSEP : 0);
	return h;
	}

static int item_y(const Menu *m, int k)
	{
	int i, y = 3;
	for (i = 0; i < k; i++) y += MIH + ((m->items[i].flags & MI_SEP) ? MSEP : 0);
	return y;
	}

static void pane_draw(void)
	{
	const Menu *m = &mm[msel];
	int i, x2 = pw;
	fill(0, 0, pw + SHD, ph + SHD, DARK);       /* (the shadow shows through the shape) */
	fill(0, 0, pw, ph, PALER);
	box(0, 0, pw, ph, BLACK);
	for (i = 0; i < m->n; i++)
		{
		const MenuItem *it = &m->items[i];
		int y = item_y(m, i), st = item_state(ma, it->cmd);
		int g = st & MS_DIM ? MID : BLACK;
		if (i == mitem)
			{
			fill(3, y, pw - 6, MIH, BLACK);
			g = st & MS_DIM ? MID : WHITE;
			}
		if (it->flags & MI_CHECK)
			{
			/* a tick box: ticked or empty */
			box(8, y + 4, 10, 10, g);
			if (st & MS_TICK)
				{
				line(10, y + 9, 12, y + 11, g, 1); line(12, y + 11, 16, y + 6, g, 1);
				line(10, y + 8, 12, y + 10, g, 1); line(12, y + 10, 16, y + 5, g, 1);
				}
			}
		text(&f_normal, 24, y + 1, it->label, -1, g);
		if (it->key)
			text(&f_normal, x2 - 10 - tw(&f_normal, it->key, -1), y + 1, it->key, -1, g);
		if ((it->flags & MI_SEP) && i < m->n - 1)
			fill(6, y + MIH + MSEP / 2, pw - 12, 1, DARK);
		}
	show(mpane, 0, 0, pw + SHD, ph + SHD);
	}

static void bar_draw(void)
	{
	int i, w = mbx[mn - 1] + mbw[mn - 1] + 6;
	fill(0, 0, w + SHD, 22 + SHD, DARK);
	fill(0, 0, w, 22, PALER);
	box(0, 0, w, 22, BLACK);
	for (i = 0; i < mn; i++)
		{
		if (i == msel)
			{
			fill(mbx[i], 3, mbw[i], 16, BLACK);
			text(&f_bold, mbx[i] + 7, 4, mm[i].title, -1, WHITE);
			}
		else
			text(&f_bold, mbx[i] + 7, 4, mm[i].title, -1, BLACK);
		}
	show(mbar, 0, 0, w + SHD, 22 + SHD);
	}

static void pane_place(void)
	{
	const Menu *m = &mm[msel];
	int i, lw = 0, kw = 0, x, y;
	for (i = 0; i < m->n; i++)
		{
		int a = tw(&f_normal, m->items[i].label, -1), b = m->items[i].key ? tw(&f_normal, m->items[i].key, -1) : 0;
		if (a > lw) lw = a;
		if (b > kw) kw = b;
		}
	pw = 24 + lw + (kw ? 22 + kw : 0) + 12;
	ph = pane_height(m);
	if (mpop) { x = mpopx - pw; y = mpopy; if (x < 0) x = 0; }
	else { x = mbx[msel]; y = 21; }
	if (x + pw + SHD > SW) x = SW - pw - SHD;
	if (y + ph + SHD > SH) y = SH - ph - SHD;
	px0 = x;
	XMoveResizeWindow(dpy, mpane, x, y, pw + SHD, ph + SHD);
	shadow_shape(mpane, pw, ph);
	XMapRaised(dpy, mpane);
	}

static void menu_show(App *a, const Menu *menus, int n, int which, int pop)
	{
	int i, x = 3;
	if (!mbar) { mbar = popup_window(); mpane = popup_window(); }
	ma = a; mm = menus; mn = n; msel = which < n ? which : 0; mitem = 0; mpop = pop;
	menu_up = 1;
	if (!pop)
		{
		for (i = 0; i < n; i++) { mbx[i] = x; mbw[i] = tw(&f_bold, menus[i].title, -1) + 14; x += mbw[i]; }
		XMoveResizeWindow(dpy, mbar, 0, 0, x + 3 + SHD, 22 + SHD);
		shadow_shape(mbar, x + 3, 22);
		XMapRaised(dpy, mbar);
		}
	pane_place();
	XGrabPointer(dpy, pop ? mpane : mbar, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
		GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
	XGrabKeyboard(dpy, pop ? mpane : mbar, True, GrabModeAsync, GrabModeAsync, CurrentTime);
	}

extern const Menu wm_menus[];
extern const int wm_nmenus;

void menu_open(App *a, int which)
	{
	if (menu_up || modal) return;
	if (a && a->k->nmenus) menu_show(a, a->k->menus, a->k->nmenus, which, 0);
	else menu_show(a, wm_menus, wm_nmenus, which, 0);
	}

void popup_open(App *a, const Menu *m, int x, int y)
	{
	if (menu_up) return;
	mpopx = x; mpopy = y;
	menu_show(a, m, 1, 0, 1);
	}

static void menu_close(void)
	{
	XUngrabPointer(dpy, CurrentTime);
	XUngrabKeyboard(dpy, CurrentTime);
	XUnmapWindow(dpy, mpane);
	if (!mpop) XUnmapWindow(dpy, mbar);
	menu_up = 0;
	}

static void menu_choose(void)
	{
	App *a = ma;
	int cmd = mm[msel].items[mitem].cmd;
	menu_close();
	run_cmd(a, cmd);
	}

static int pane_hit(int y)
	{
	const Menu *m = &mm[msel];
	int i;
	for (i = 0; i < m->n; i++)
		{
		int t = item_y(m, i);
		if (y >= t && y < t + MIH) return i;
		}
	return -1;
	}

int menu_event(XEvent *e)
	{
	int i;
	if (!menu_up) return 0;
	switch (e->type)
		{
	case Expose:
		if (e->xexpose.count) return 1;
		if (e->xexpose.window == mbar) bar_draw();
		else if (e->xexpose.window == mpane) pane_draw();
		else return 0;
		return 1;
	case ButtonPress: case ButtonRelease: case MotionNotify:
		{
		Window w = e->xbutton.window;
		int x = e->xbutton.x, y = e->xbutton.y, type = e->type;
		if (type == MotionNotify && !(e->xmotion.state & (Button1Mask | Button3Mask))) return 1;
		if (w == mpane && x >= 0 && x < pw && y >= 0 && y < ph)
			{
			i = pane_hit(y);
			if (i >= 0 && i != mitem) { mitem = i; pane_draw(); }
			if (type == ButtonRelease && i >= 0) menu_choose();
			return 1;
			}
		/* (other windows' events come relative to the grab's window) */
		if (w != mbar && w != mpane) return 1;
		if (w == mpane) { x += px0; y += mpop ? mpopy : 21; }
		if (!mpop && y >= 0 && y < 22)
			{
			for (i = 0; i < mn; i++)
				if (x >= mbx[i] && x < mbx[i] + mbw[i] && i != msel)
					{
					msel = i; mitem = 0;
					bar_draw(); pane_place(); pane_draw();
					}
			return 1;
			}
		if (type == ButtonPress) menu_close();
		return 1;
		}
	case KeyPress:
		{
		char s[8];
		KeySym ks;
		int n = XLookupString(&e->xkey, s, sizeof s, &ks, 0);
		const Menu *m = &mm[msel];
		(void)n;
		switch (ks)
			{
		case XK_Escape: menu_close(); return 1;
		case XK_Up: mitem = (mitem + m->n - 1) % m->n; pane_draw(); return 1;
		case XK_Down: mitem = (mitem + 1) % m->n; pane_draw(); return 1;
		case XK_Home: mitem = 0; pane_draw(); return 1;
		case XK_End: mitem = m->n - 1; pane_draw(); return 1;
		case XK_Left: case XK_Right:
			if (!mpop)
				{
				msel = (msel + (ks == XK_Left ? mn - 1 : 1)) % mn;
				mitem = 0;
				bar_draw(); pane_place(); pane_draw();
				}
			return 1;
		case XK_Return: case XK_KP_Enter: menu_choose(); return 1;
			}
		if (e->xkey.state & ControlMask)
			{
			App *a = ma;
			const Menu *menus = mm;
			int k = mn, pop = mpop;
			KeySym k0 = XLookupKeysym(&e->xkey, 0);
			if (k0 == XK_Shift_L || k0 == XK_Shift_R || k0 == XK_Control_L || k0 == XK_Control_R) return 1;
			menu_close();
			if (!pop) menu_shortcut(a, menus, k, k0, e->xkey.state);
			}
		return 1;
		}
		}
	return 0;
	}

/* ---- infoprints (bottom right) and busy messages (bottom left) ---- */

static Window ipw, bsw;
static long ip_until;
static char ipbuf[128], bsbuf[96];

static void msg_draw(Window w, const char *s, int *wp, int *hp)
	{
	int cw = tw(&f_bold, s, -1) + 14, ch = f_bold.h + 6;
	fill(0, 0, cw, ch, BLACK);
	text(&f_bold, 7, 3, s, -1, WHITE);
	show(w, 0, 0, cw, ch);
	*wp = cw; *hp = ch;
	}

static void msg_place(Window w, const char *s, int right)
	{
	int cw = tw(&f_bold, s, -1) + 14, ch = f_bold.h + 6;
	XMoveResizeWindow(dpy, w, right ? ww - cw - 3 : 3, SH - ch - 3, cw, ch);
	XMapRaised(dpy, w);
	}

void infoprint(const char *fmt, ...)
	{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(ipbuf, sizeof ipbuf, fmt, ap);
	va_end(ap);
	if (!ipw) ipw = popup_window();
	msg_place(ipw, ipbuf, 1);
	ip_until = now_ms() + 2500;
	}

void busy(const char *s)
	{
	if (!bsw) bsw = popup_window();
	if (!s) { bsbuf[0] = 0; XUnmapWindow(dpy, bsw); return; }
	snprintf(bsbuf, sizeof bsbuf, "%s", s);
	msg_place(bsw, bsbuf, 0);
	{
	int w, h;
	msg_draw(bsw, bsbuf, &w, &h);
	}
	XFlush(dpy);
	}

void msgs_raise(void)
	{
	if (ip_until && ipw) XRaiseWindow(dpy, ipw);
	if (bsbuf[0] && bsw) XRaiseWindow(dpy, bsw);
	}

void msg_tick(void)
	{
	if (ip_until && now_ms() >= ip_until)
		{
		ip_until = 0;
		XUnmapWindow(dpy, ipw);
		}
	}

long msg_next(void) { return ip_until; }

static int msg_expose(XEvent *e)
	{
	int w, h;
	if (e->xexpose.window == ipw && ipw) { msg_draw(ipw, ipbuf, &w, &h); return 1; }
	if (e->xexpose.window == bsw && bsw) { msg_draw(bsw, bsbuf, &w, &h); return 1; }
	return 0;
	}

/* ---- the line editor ---- */

int line_key(Line *l, KeySym ks, unsigned st, const char *s, int n)
	{
	switch (ks)
		{
	case XK_Left: if (l->all) { l->cur = 0; l->all = 0; } else if (l->cur > 0) l->cur--; return 1;
	case XK_Right: if (l->all) { l->cur = l->n; l->all = 0; } else if (l->cur < l->n) l->cur++; return 1;
	case XK_Home: l->cur = 0; l->all = 0; return 1;
	case XK_End: l->cur = l->n; l->all = 0; return 1;
	case XK_BackSpace:
		if (l->all) { l->n = l->cur = 0; l->all = 0; }
		else if (l->cur > 0) { memmove(l->s + l->cur - 1, l->s + l->cur, l->n - l->cur); l->n--; l->cur--; }
		l->s[l->n] = 0;
		return 1;
	case XK_Delete:
		if (l->all) { l->n = l->cur = 0; l->all = 0; }
		else if (l->cur < l->n) { memmove(l->s + l->cur, l->s + l->cur + 1, l->n - l->cur - 1); l->n--; }
		l->s[l->n] = 0;
		return 1;
		}
	if (st & ControlMask) return 0;
	if (n == 1 && (unsigned char)s[0] >= 32 && s[0] != 127)
		{
		if (l->all) { l->n = l->cur = 0; l->all = 0; }
		if (l->n >= (int)sizeof l->s - 1) return 1;
		memmove(l->s + l->cur + 1, l->s + l->cur, l->n - l->cur);
		l->s[l->cur++] = s[0];
		l->s[++l->n] = 0;
		return 1;
		}
	return 0;
	}

/* an edit box: white, sunk; the text scrolls to keep the cursor in view */
void line_draw(Line *l, const BFont *f, int x, int y, int w, int focused)
	{
	int h = f->h + 6, off = 0, cx;
	fill(x, y, w, h, WHITE);
	box(x, y, w, h, BLACK);
	fill(x + 1, y + 1, w - 2, 1, MID);
	fill(x + 1, y + 1, 1, h - 2, MID);
	cx = tw(f, l->s, l->cur);
	if (cx > w - 12) off = cx - (w - 12);
	if (l->all && focused && l->n)
		{
		int t = tw(f, l->s, l->n) - off;
		fill(x + 4, y + 3, t > w - 8 ? w - 8 : t, f->h, BLACK);
		text(f, x + 4 - off, y + 3, l->s, l->n, WHITE);
		}
	else
		text(f, x + 4 - off, y + 3, l->s, l->n, BLACK);
	fill(x + w - 3, y + 1, 2, h - 2, WHITE);     /* (keep long text inside) */
	if (focused && !l->all)
		fill(x + 4 + cx - off, y + 2, 2, f->h + 2, BLACK);
	}

/* ---- dialogs ---- */

typedef struct
	{
	App *owner;
	char title[80];
	char lines[6][160]; int nlines;
	char labels[3][40]; Line ed[3]; int ned, fed;
	int buttons, pic;
	DlgDone done;
	int press;                  /* the button held down, or -1 */
	int bx[2], bw, by;
	} Dlg;

static Dlg pend;
static int dlg_pic = -1;

static const char *btn_label(int b, int i)
	{
	static const char *const L[3][2] = { { "Cancel", "OK" }, { "No", "Yes" }, { "Continue", 0 } };
	return L[b][i];
	}

static void dlg_init(App *a, const char *arg)
	{
	Dlg *d = malloc(sizeof *d);
	int i, w = tw(&f_bold, pend.title, -1) + 40, h;
	(void)arg;
	*d = pend;
	a->p = d;
	for (i = 0; i < d->nlines; i++) { int t = tw(&f_bold, d->lines[i], -1) + 40; if (t > w) w = t; }
	if (d->pic >= 0) w += 56;
	if (d->ned && w < 380) w = 380;
	if (w < 260) w = 260;
	if (w > ww - 2 * FB - SHD - 8) w = ww - 2 * FB - SHD - 8;
	h = 10 + d->nlines * 18 + d->ned * 26 + 8 + 22 + (d->buttons == DB_YESNO ? 12 : 0) + 8;
	a->w = w; a->h = h;
	snprintf(a->title, sizeof a->title, "%s", d->title);
	d->press = -1;
	}

static void dlg_draw(App *a)
	{
	Dlg *d = a->p;
	int i, y = 10, lw = 0;
	int px = d->pic >= 0 ? 56 : 0;
	fill(0, 0, a->w, a->h, LIGHT);
	if (d->pic >= 0) pic(d->pic, 10, 8);
	for (i = 0; i < d->nlines; i++, y += 18)
		textc(i ? &f_normal : &f_bold, px, y, a->w - px, d->lines[i], BLACK);
	for (i = 0; i < d->ned; i++) { int t = tw(&f_bold, d->labels[i], -1); if (t > lw) lw = t; }
	for (i = 0; i < d->ned; i++, y += 26)
		{
		text(&f_bold, 14, y + 4, d->labels[i], -1, BLACK);
		line_draw(&d->ed[i], &f_normal, 14 + lw + 12, y, a->w - lw - 26 - 14, i == d->fed);
		}
	/* the buttons: bottom right, OK (or Yes) on the far right */
	d->bw = 76;
	d->by = a->h - 8 - 22 - (d->buttons == DB_YESNO ? 12 : 0);
	for (i = 0; i < 2; i++)
		{
		const char *l = btn_label(d->buttons, i);
		int x;
		if (!l) continue;
		x = d->buttons == DB_CONTINUE ? (a->w - d->bw) / 2 : a->w - 10 - (2 - i) * (d->bw + 8) + 8;
		d->bx[i] = x;
		bevel(x, d->by, d->bw, 22, d->press == i);
		textc(&f_bold, x + (d->press == i), d->by + 4 + (d->press == i), d->bw, l, BLACK);
		if (d->buttons == DB_YESNO)
			textc(&f_small, x, d->by + 23, d->bw, i ? "Y" : "N", DARK);
		}
	}

static void dlg_end(App *a, int ok)
	{
	Dlg *d = a->p;
	char *e[3];
	int i;
	App *o = d->owner;
	DlgDone done = d->done;
	Dlg copy = *d;
	for (i = 0; i < 3; i++) e[i] = copy.ed[i].s;
	app_close(a);
	if (done) done(o, ok, e);
	}

static int dlg_key(App *a, KeySym ks, unsigned st, const char *s, int n)
	{
	Dlg *d = a->p;
	if (ks == XK_Escape) { dlg_end(a, d->buttons == DB_CONTINUE ? DR_OK : DR_CANCEL); return 1; }
	if (d->buttons == DB_YESNO)
		{
		if (ks == XK_y || ks == XK_Y) { dlg_end(a, DR_OK); return 1; }
		if (ks == XK_n || ks == XK_N) { dlg_end(a, DR_CANCEL); return 1; }
		return 1;          /* (Enter is not Yes) */
		}
	if (ks == XK_Return || ks == XK_KP_Enter) { dlg_end(a, DR_OK); return 1; }
	if (d->ned)
		{
		if (ks == XK_Down || ks == XK_Tab) { d->fed = (d->fed + 1) % d->ned; d->ed[d->fed].all = 1; app_dirty(a); return 1; }
		if (ks == XK_Up) { d->fed = (d->fed + d->ned - 1) % d->ned; d->ed[d->fed].all = 1; app_dirty(a); return 1; }
		if (line_key(&d->ed[d->fed], ks, st, s, n)) { app_dirty(a); return 1; }
		}
	return 1;
	}

static int dlg_hit(Dlg *d, int x, int y)
	{
	int i;
	for (i = 0; i < 2; i++)
		if (btn_label(d->buttons, i) && x >= d->bx[i] && x < d->bx[i] + d->bw && y >= d->by && y < d->by + 22)
			return i;
	return -1;
	}

static void dlg_pen(App *a, int type, int x, int y)
	{
	Dlg *d = a->p;
	int i = dlg_hit(d, x, y), was = d->press;
	if (type == PEN_DOWN)
		{
		d->press = i;
		if (i < 0 && d->ned)
			{
			int k = (y - 10 - d->nlines * 18) / 26;
			if (k >= 0 && k < d->ned) { d->fed = k; d->ed[k].all = 0; d->ed[k].cur = d->ed[k].n; }
			}
		}
	else if (type == PEN_MOVE)
		{
		if (d->press >= 0 && i != d->press) d->press = -1;
		}
	else
		{
		d->press = -1;
		if (i >= 0 && was == i)
			{
			dlg_end(a, d->buttons == DB_CONTINUE ? DR_OK : i);
			return;
			}
		}
	app_dirty(a);
	}

static void dlg_fini(App *a) { free(a->p); }

const AppClass dialog_class = { "Dialog", -1, 0, 0, 0, 0, 0, 0, dlg_init, dlg_draw, dlg_key, dlg_pen, 0, 0, 0, dlg_fini };

void dialog(App *owner, const char *title, const char *const *lines, const char *const *edits, int buttons, DlgDone done)
	{
	int i;
	memset(&pend, 0, sizeof pend);
	pend.owner = owner;
	snprintf(pend.title, sizeof pend.title, "%s", title);
	for (i = 0; lines && lines[i] && i < 6; i++) snprintf(pend.lines[i], sizeof pend.lines[i], "%s", lines[i]);
	pend.nlines = i;
	for (i = 0; edits && edits[2 * i] && i < 3; i++)
		{
		snprintf(pend.labels[i], sizeof pend.labels[i], "%s", edits[2 * i]);
		snprintf(pend.ed[i].s, sizeof pend.ed[i].s, "%s", edits[2 * i + 1]);
		pend.ed[i].n = pend.ed[i].cur = strlen(pend.ed[i].s);
		pend.ed[i].all = 1;
		}
	pend.ned = i;
	pend.buttons = buttons;
	pend.done = done;
	pend.pic = dlg_pic;
	dlg_pic = -1;
	modal = app_open(&dialog_class, 0);
	}

void about_dialog(App *owner)
	{
	static char ver[64], sys[160];
	static const char *L[] = { ver,
		"Linux programs on the Psion Series 5mx - © 2026 Dan Edge",
		sys,
		"The desktop: psiwm " PSIWM_VERSION ", with Files, Notes, Calc, Clock & System",
		"Licences in /usr/share/licenses", 0 };
	char b[256], path[300], *p, kb1[32], kb2[32];
	struct utsname u;
	long tot = -1, fre = -1;
	snprintf(path, sizeof path, "%s/version", procroot);
	strcpy(ver, "PsiLinux");
	if (readfile(path, b, sizeof b) > 0 && (p = strstr(b, "(PsiLinux ")))
		snprintf(ver, sizeof ver, "PsiLinux %.*s", (int)strcspn(p + 10, ",)"), p + 10);
	snprintf(path, sizeof path, "%s/meminfo", procroot);
	if (readfile(path, b, sizeof b) > 0)
		{
		if ((p = strstr(b, "MemTotal:"))) tot = atol(p + 9);
		if ((p = strstr(b, "MemAvailable:"))) fre = atol(p + 13);
		}
	if (uname(&u)) strcpy(u.release, "?"), strcpy(u.machine, "arm");
	if (tot > 0 && fre >= 0)
		{
		fmt_kb(kb1, fre); fmt_kb(kb2, tot);
		snprintf(sys, sizeof sys, "Linux %s (%s) - %s free of %s", u.release, u.machine, kb1, kb2);
		}
	else snprintf(sys, sizeof sys, "Linux %s (%s)", u.release, u.machine);
	dlg_pic = I_P_PENGUIN;
	dialog(owner, "About PsiLinux", L, 0, DB_CONTINUE, 0);
	}

/* ---- the clipboard: ours is X's CLIPBOARD; pasting asks its owner ---- */

static char *clip_data;
static int clip_len;
static App *clip_app;
static ClipDone clip_done;

void clip_set(const char *s, int n)
	{
	free(clip_data);
	clip_data = malloc(n + 1);
	memcpy(clip_data, s, n);
	clip_data[n] = 0;
	clip_len = n;
	XSetSelectionOwner(dpy, A_CLIPBOARD, desk->win, CurrentTime);
	}

void clip_get(App *a, ClipDone done)
	{
	Atom sel = A_CLIPBOARD;
	Window o = XGetSelectionOwner(dpy, A_CLIPBOARD);
	if (o == desk->win) { done(a, clip_data, clip_len); return; }
	if (o == None) sel = XA_PRIMARY;
	if (XGetSelectionOwner(dpy, sel) == None) { done(a, 0, 0); return; }
	clip_app = a; clip_done = done;
	XConvertSelection(dpy, sel, A_UTF8, XInternAtom(dpy, "PSIWM_SEL", False), a->win, CurrentTime);
	}

int clip_event(XEvent *e)
	{
	if (e->type == SelectionRequest)
		{
		XSelectionRequestEvent *r = &e->xselectionrequest;
		XEvent n;
		memset(&n, 0, sizeof n);
		n.xselection.type = SelectionNotify;
		n.xselection.requestor = r->requestor;
		n.xselection.selection = r->selection;
		n.xselection.target = r->target;
		n.xselection.time = r->time;
		n.xselection.property = None;
		if (r->property == None) r->property = r->target;
		if (r->target == A_TARGETS)
			{
			Atom t[3] = { A_TARGETS, A_UTF8, XA_STRING };
			XChangeProperty(dpy, r->requestor, r->property, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, 3);
			n.xselection.property = r->property;
			}
		else if ((r->target == A_UTF8 || r->target == XA_STRING) && clip_data)
			{
			XChangeProperty(dpy, r->requestor, r->property, r->target, 8, PropModeReplace, (unsigned char *)clip_data, clip_len);
			n.xselection.property = r->property;
			}
		XSendEvent(dpy, r->requestor, False, 0, &n);
		return 1;
		}
	if (e->type == SelectionClear) { free(clip_data); clip_data = 0; clip_len = 0; return 1; }
	if (e->type == SelectionNotify && clip_done)
		{
		ClipDone done = clip_done;
		App *a = clip_app;
		clip_done = 0;
		if (e->xselection.property == None) { done(a, 0, 0); return 1; }
		{
		Atom type; int fmt; unsigned long n, after; unsigned char *data = 0;
		XGetWindowProperty(dpy, e->xselection.requestor, e->xselection.property, 0, 65536, True, AnyPropertyType,
			&type, &fmt, &n, &after, &data);
		done(a, (char *)data, data ? (int)n : 0);
		if (data) XFree(data);
		}
		return 1;
		}
	return 0;
	}

int ui_expose(XEvent *e) { return msg_expose(e); }
