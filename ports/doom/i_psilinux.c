/* i_psilinux.c - Doom (doomgeneric) on PsiLinux: the screen, the keyboard, the
 * pen and the clock. It takes the place of doomgeneric's i_video.c and of a
 * DG_* backend (doomgeneric_xlib.c and the others are not built).
 *
 * The screen: Doom draws 320x200 in 8 bits (I_VideoBuffer). Each frame goes
 * straight into /dev/fb0 (640x240, 8 bits, pseudo-colour), every pixel
 * doubled across with 32-bit writes, and 200 rows made 240 by drawing every
 * fifth row twice (or, with -letterbox, 200 rows in the middle), by
 * psi_blit.S. PsiLinux's screen is asked for in the Psion's own 16 greys
 * (depth 4): a doubled pixel is then exactly one byte, its two greys
 * dithered, from a table made with the palette, so the app shows Doom's
 * frames without converting them (its biggest cost in a frame otherwise).
 * Only the rows that changed since the last frame are copied,
 * and one PSIFB_DAMAGE says where. Doom's palette (PLAYPAL, with its pain and
 * pick-up tints) is the frame buffer's colour map: one FBIOPUTCMAP each time
 * it changes. The Psion shows each entry as a grey, so the map handed over is
 * greys already, through a tone curve (Doom's palette is dark; 16 greys lose
 * its shadows).
 *
 * The keys: /dev/input/event0, Linux KEY_* codes, pressed and let go. The
 * pen (/dev/input/event1): a tap fires. Arrows move, Ctrl fires, Space
 * uses, Shift runs, Fn+Left/Right (Home, End) step aside, Esc, Enter, Y, N,
 * the digits and Tab are Doom's own, and Menu is F11 (lighter).
 *
 * Options (besides Doom's own):
 *   -letterbox      320x200 doubled across, 200 rows, black above and below
 *   -tone N         the grey curve: 0 Doom's own greys, 1 to 4 lighter and
 *                   stronger (2: gamma 1.5, gain 1.3)
 *   -fps            frames a second, every 5 s, on stderr
 *   -frames N       quit after N frames (for timing)
 *   -dumptic N FILE write Doom's 320x200 screen to FILE at game tic N, and
 *                   quit (a test of the drawing: tics, unlike frames, are
 *                   the same from run to run)
 *   -fbfile FILE    draw into FILE (640x240 bytes, then 256 greys) instead of
 *                   /dev/fb0, with no ioctls: for running under plain qemu-arm
 *   -8bit           /dev/fb0 in 8 bits with a colour map, as before depth 4
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include "config.h"
#include "doomtype.h"
#include "doomkeys.h"
#include "d_event.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#include "v_video.h"
#include "z_zone.h"
#include "doomgeneric.h"

/* PsiLinux's own: say where the picture changed (lx_fb.c) */
struct psifb_rect { uint16_t x, y, w, h; };
#define PSIFB_DAMAGE _IOW('F', 0xa0, struct psifb_rect)

/* /dev/input's event as the PsiLinux kernel writes it: 16 bytes (musl's
 * struct input_event has a 64-bit time, and is 24) */
struct psi_event { uint32_t sec, usec; uint16_t type, code; int32_t value; };

/* Linux's key codes (linux/input-event-codes.h, whose names Doom's
 * doomkeys.h uses for its own) */
enum {
	LK_ESC = 1, LK_1 = 2, LK_9 = 10, LK_0 = 11, LK_MINUS = 12, LK_EQUAL = 13, LK_BACKSPACE = 14,
	LK_TAB = 15, LK_Q = 16, LK_RIGHTBRACE = 27, LK_ENTER = 28, LK_LEFTCTRL = 29, LK_A = 30,
	LK_GRAVE = 41, LK_LEFTSHIFT = 42, LK_BACKSLASH = 43, LK_Z = 44, LK_SLASH = 53,
	LK_RIGHTSHIFT = 54, LK_LEFTALT = 56, LK_SPACE = 57, LK_F1 = 59, LK_F10 = 68, LK_F11 = 87,
	LK_F12 = 88, LK_RIGHTCTRL = 97, LK_RIGHTALT = 100, LK_HOME = 102, LK_UP = 103,
	LK_PAGEUP = 104, LK_LEFT = 105, LK_RIGHT = 106, LK_END = 107, LK_DOWN = 108,
	LK_PAGEDOWN = 109, LK_DELETE = 111, LK_COMPOSE = 127, LK_BTN_TOUCH = 0x14a
};
#define LX_EV_KEY 1

#define FB_W 640
#define FB_H 240

/* ---- what i_video.c gave the rest of Doom ------------------------------- */

byte *I_VideoBuffer = NULL;
boolean screensaver_mode = false;
boolean screenvisible = true;
float mouse_acceleration = 2.0;     /* (no mouse: never used in arithmetic) */
int mouse_threshold = 10;
int usemouse = 0;
int usegamma = 0;

/* ---- the frame buffer --------------------------------------------------- */

static int fb_fd = -1;
static uint8_t *fb;                 /* 640x240 */
static int fb4;                     /* the screen is the Psion's 16 greys (depth 4): 320 bytes a row */
static uint8_t grey_lut[2][256];    /* depth 4: a palette entry -> a doubled pixel's byte, by row parity */
static int redraw;                  /* every row next frame (the palette changed, at depth 4) */
static int fb_isfile;
static int letterbox;
static uint32_t *prev;              /* the last frame copied (320x200), to see what changed */
static int16_t row_dst[SCREENHEIGHT];   /* each Doom row's first screen row */
static uint8_t row_dup[SCREENHEIGHT];   /* and whether it is drawn twice */
static uint8_t tone[256];           /* grey -> the grey handed to the screen */
static int tone_level = 2;
static int frames, frames_max, show_fps, dump_tic = -1;
static const char *dump_file;
extern int gametic;
static uint32_t fps_t0;
static int fps_n;

/* the tone curve: out = gain * in^(1/g), g = 1 + level/4 and gain 1 to 1.5
 * (level 0 to 4), made once with integer arithmetic: the largest o with
 * (o/255)^(4+level) <= (in/255)^4, in 16.16 fixed point, times the gain.
 * Doom's palette is dark, and its scenes sit low in it: level 2 (gamma 1.5,
 * gain 1.3, the brightest highlights clipped to white) spreads a typical
 * room over the Psion's 16 greys instead of the darkest 6 */
static uint32_t fpow(uint32_t x, int n)
{
	uint32_t p = 65536;
	while (n--) p = (uint32_t)(((uint64_t)p * x) >> 16);
	return p;
}

static void make_tone(int level)
{
	static const int gain[5] = { 256, 294, 333, 358, 384 };   /* (1, 1.15, 1.3, 1.4, 1.5) */
	int i, o = 0, t;
	for (i = 0; i < 256; i++) {
		uint32_t want = fpow((uint32_t)i * 65536u / 255u, 4);
		while (o < 255 && fpow((uint32_t)(o + 1) * 65536u / 255u, 4 + level) <= want) o++;
		t = o * gain[level] >> 8;
		tone[i] = (uint8_t)(t > 255 ? 255 : t);
	}
}

static void fb_open(void)
{
	int p = M_CheckParmWithArgs("-fbfile", 1);
	if (p > 0) {
		fb_fd = open(myargv[p + 1], O_RDWR | O_CREAT | O_TRUNC, 0644);
		if (fb_fd < 0 || ftruncate(fb_fd, FB_W * FB_H + 256) < 0)
			I_Error("I_InitGraphics: cannot make %s: %s", myargv[p + 1], strerror(errno));
		fb_isfile = 1;
	} else {
		struct fb_var_screeninfo v;
		fb_fd = open("/dev/fb0", O_RDWR);
		if (fb_fd < 0)
			I_Error("I_InitGraphics: cannot open /dev/fb0: %s", strerror(errno));
		if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) < 0 || v.xres != FB_W || v.yres != FB_H)
			I_Error("I_InitGraphics: /dev/fb0 is not 640x240");
		if (M_CheckParm("-8bit") <= 0) {
			v.bits_per_pixel = 4;
			v.grayscale = 1;
			fb4 = ioctl(fb_fd, FBIOPUT_VSCREENINFO, &v) == 0 && v.bits_per_pixel == 4;
		}
		if (!fb4) {
			v.bits_per_pixel = 8;
			v.grayscale = 0;
			if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &v) < 0 || v.bits_per_pixel != 8)
				I_Error("I_InitGraphics: /dev/fb0 is not 640x240 in 8 bits");
		}
	}
	fb = mmap(0, FB_W * FB_H, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb == MAP_FAILED)
		I_Error("I_InitGraphics: cannot map the screen: %s", strerror(errno));
}

static void fb_damage(int x, int y, int w, int h)
{
	struct psifb_rect r;
	if (fb_isfile) return;
	r.x = (uint16_t)x; r.y = (uint16_t)y; r.w = (uint16_t)w; r.h = (uint16_t)h;
	ioctl(fb_fd, PSIFB_DAMAGE, &r);
}

static void input_open(void);

void I_InitGraphics(void)
{
	int y;
	letterbox = M_CheckParm("-letterbox") > 0;
	show_fps = M_CheckParm("-fps") > 0;
	y = M_CheckParmWithArgs("-tone", 1);
	if (y > 0) tone_level = atoi(myargv[y + 1]);
	if (tone_level < 0) tone_level = 0;
	if (tone_level > 4) tone_level = 4;
	make_tone(tone_level);
	y = M_CheckParmWithArgs("-frames", 1);
	if (y > 0) frames_max = atoi(myargv[y + 1]);
	y = M_CheckParmWithArgs("-dumptic", 2);
	if (y > 0) { dump_tic = atoi(myargv[y + 1]); dump_file = myargv[y + 2]; }

	fb_open();
	memset(fb, 0, FB_W * FB_H);     /* (palette entry 0 is Doom's black) */
	fb_damage(0, 0, FB_W, FB_H);

	/* the rows: 200 -> 240 draws rows 2, 7, 12... twice (5 rows -> 6) */
	for (y = 0; y < SCREENHEIGHT; y++) {
		if (letterbox) { row_dst[y] = (int16_t)(y + (FB_H - SCREENHEIGHT) / 2); row_dup[y] = 0; }
		else { row_dst[y] = (int16_t)(y + y / 5 + (y % 5 > 2)); row_dup[y] = y % 5 == 2; }
	}

	I_VideoBuffer = Z_Malloc(SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);
	prev = Z_Malloc(SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);
	memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT);
	memset(prev, 0, SCREENWIDTH * SCREENHEIGHT);
	screenvisible = true;
	input_open();
	printf("I_InitGraphics: 320x200 -> 640x%d on /dev/fb0 in %d bits, tone %d\n", letterbox ? 200 : 240, fb4 ? 4 : 8, tone_level);
}

void I_ShutdownGraphics(void)
{
	if (fb4) {
		/* (8 bits again, for whatever opens the screen next) */
		struct fb_var_screeninfo v;
		if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) == 0) {
			v.bits_per_pixel = 8;
			v.grayscale = 0;
			ioctl(fb_fd, FBIOPUT_VSCREENINFO, &v);
		}
		fb4 = 0;
	}
	if (fb && fb != MAP_FAILED) munmap(fb, FB_W * FB_H);
	if (fb_fd >= 0) close(fb_fd);
	fb = 0; fb_fd = -1;
}

/* n words of s (4 pixels each; n even) doubled across into d, and into d2
 * too unless it is 0: pixels a b c d -> a a b b, c c d d (psi_blit.S) */
void psi_dbl_row(uint32_t *d, uint32_t *d2, const uint32_t *s, int n);
/* n words of s -> n words of d at depth 4: each pixel one byte, lut[pixel] */
void psi_grey_row(uint32_t *d, const uint32_t *s, const uint8_t *lut, int n);

static uint32_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)t.tv_sec * 1000u + (uint32_t)(t.tv_nsec / 1000000);
}

void I_FinishUpdate(void)
{
	const int W = SCREENWIDTH / 4;          /* words a row */
	const uint32_t *s = (const uint32_t *)I_VideoBuffer;
	uint32_t *o = prev;
	int y, x0 = W, x1 = -1, y0 = -1, y1 = -1;

	for (y = 0; y < SCREENHEIGHT; y++, s += W, o += W) {
		int a = 0, b = W - 1, dy;
		uint32_t *d;
		/* what changed in this row: from the left, then from the right */
		if (!redraw) {
			while (a < W && s[a] == o[a]) a++;
			if (a == W) continue;
			while (s[b] == o[b]) b--;
			a &= ~1; b |= 1;                /* (two words at a time) */
		}
		memcpy(o + a, s + a, (size_t)(b + 1 - a) * 4);
		dy = row_dst[y];
		if (fb4) {
			d = (uint32_t *)(fb + dy * (FB_W / 2)) + a;
			psi_grey_row(d, s + a, grey_lut[dy & 1], b + 1 - a);
			if (row_dup[y])
				psi_grey_row(d + FB_W / 8, s + a, grey_lut[(dy + 1) & 1], b + 1 - a);
		} else {
			d = (uint32_t *)(fb + dy * FB_W) + 2 * a;
			psi_dbl_row(d, row_dup[y] ? d + FB_W / 4 : 0, s + a, b + 1 - a);
		}
		if (a < x0) x0 = a;
		if (b > x1) x1 = b;
		if (y0 < 0) y0 = dy;
		y1 = dy + row_dup[y];
	}
	if (y0 >= 0)
		fb_damage(x0 * 8, y0, (x1 + 1 - x0) * 8, y1 + 1 - y0);
	redraw = 0;

	frames++;
	if (dump_tic >= 0 && gametic >= dump_tic) {
		FILE *f = fopen(dump_file, "wb");
		if (f) { fwrite(I_VideoBuffer, 1, SCREENWIDTH * SCREENHEIGHT, f); fclose(f); }
		printf("doom: tic %d drawn into %s\n", gametic, dump_file);
		I_Quit();
	}
	if (show_fps) {
		uint32_t t = now_ms();
		if (!fps_t0) fps_t0 = t;
		fps_n++;
		if (t - fps_t0 >= 5000) {
			fprintf(stderr, "doom: %d.%d frames a second\n", fps_n * 1000 / (int)(t - fps_t0),
				fps_n * 10000 / (int)(t - fps_t0) % 10);
			fps_t0 = t; fps_n = 0;
		}
	}
	if (frames_max && frames >= frames_max) {
		printf("doom: %d frames\n", frames);
		I_Quit();
	}
}

void I_ReadScreen(byte *scr)
{
	memcpy(scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
}

/* PLAYPAL (already through Doom's gamma table) -> greys -> the colour map */
void I_SetPalette(byte *palette)
{
	static uint16_t g[256];
	static uint8_t last[768];
	static int have;
	struct fb_cmap cm;
	int i;
	if (have && !memcmp(last, palette, sizeof last)) return;
	have = 1;
	memcpy(last, palette, sizeof last);
	for (i = 0; i < 256; i++) {
		const byte *c = palette + 3 * i;
		/* Rec. 601 luma, as the kernel would take it */
		int l = (c[0] * 77 + c[1] * 150 + c[2] * 29) >> 8;
		g[i] = (uint16_t)(tone[l] * 257);
	}
	if (fb_isfile) {
		if (lseek(fb_fd, FB_W * FB_H, SEEK_SET) >= 0) {
			uint8_t gb[256];
			for (i = 0; i < 256; i++) gb[i] = (uint8_t)(g[i] >> 8);
			if (write(fb_fd, gb, 256) != 256) {}
		}
		return;
	}
	if (fb4) {
		/* each entry's doubled pixel: its grey as the Psion's 16, the left
		 * and right halves of a 2x2 ordered dither (as the app's own
		 * conversion, lxfbconv.h) */
		static const int t[2][2] = { { 2, 10 }, { 14, 6 } };
		int y, x;
		for (y = 0; y < 2; y++)
			for (i = 0; i < 256; i++) {
				int gr = g[i] >> 8, b = 0;
				for (x = 0; x < 2; x++) {
					int l = ((gr * 15 * 16 + 127) / 255 + t[y][x]) >> 4;
					if (gr == 255 || l > 15) l = 15;
					b |= l << (4 * x);
				}
				grey_lut[y][i] = (uint8_t)b;
			}
		redraw = 1;
		return;
	}
	cm.start = 0; cm.len = 256; cm.red = g; cm.green = g; cm.blue = g; cm.transp = 0;
	ioctl(fb_fd, FBIOPUTCMAP, &cm);
}

int I_GetPaletteIndex(int r, int gr, int b)
{
	/* (only the Hexen/Strife menus and the disk icon ask; Doom does not) */
	(void)r; (void)gr; (void)b;
	return 0;
}

void I_GetEvent(void);   /* (i_input.c) */

void I_StartFrame(void) {}
void I_StartTic(void) { I_GetEvent(); }
void I_UpdateNoBlit(void) {}
void I_BeginRead(void) {}
void I_EndRead(void) {}
void I_SetWindowTitle(char *title) { (void)title; }
void I_GraphicsCheckCommandLine(void) {}
void I_SetGrabMouseCallback(grabmouse_callback_t func) { (void)func; }
void I_EnableLoadingDisk(void) {}
void I_BindVideoVariables(void) {}
void I_DisplayFPSDots(boolean dots_on) { (void)dots_on; }
void I_CheckIsScreensaver(void) {}

/* ---- the keys and the pen ----------------------------------------------- */

static int kbd_fd = -1, pen_fd = -1;

static void input_open(void)
{
	kbd_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	pen_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
}

/* a Linux key -> Doom's */
static int doom_key(int k)
{
	static const char row1[] = "qwertyuiop[]", row2[] = "asdfghjkl;'`", row3[] = "zxcvbnm,./";
	if (k >= LK_1 && k <= LK_9) return '1' + k - LK_1;
	if (k >= LK_Q && k <= LK_RIGHTBRACE) return row1[k - LK_Q];
	if (k >= LK_A && k <= LK_GRAVE) return row2[k - LK_A];
	if (k >= LK_Z && k <= LK_SLASH) return row3[k - LK_Z];
	if (k >= LK_F1 && k <= LK_F10) return KEY_F1 + k - LK_F1;
	switch (k) {
	case LK_0: return '0';
	case LK_UP: return KEY_UPARROW;
	case LK_DOWN: return KEY_DOWNARROW;
	case LK_LEFT: return KEY_LEFTARROW;
	case LK_RIGHT: return KEY_RIGHTARROW;
	case LK_LEFTCTRL: case LK_RIGHTCTRL: return KEY_FIRE;
	case LK_SPACE: return KEY_USE;
	case LK_LEFTSHIFT: case LK_RIGHTSHIFT: return KEY_RSHIFT;    /* run */
	case LK_LEFTALT: case LK_RIGHTALT: return KEY_RALT;          /* strafe */
	case LK_ENTER: return KEY_ENTER;
	case LK_ESC: return KEY_ESCAPE;
	case LK_BACKSPACE: case LK_DELETE: return KEY_BACKSPACE;
	case LK_TAB: return KEY_TAB;                                   /* the map */
	case LK_MINUS: return KEY_MINUS;
	case LK_EQUAL: return KEY_EQUALS;
	case LK_BACKSLASH: return '\\';
	case LK_HOME: return ',';            /* Fn+Left, Fn+Right: step aside */
	case LK_END: return '.';
	case LK_PAGEUP: return KEY_PGUP;
	case LK_PAGEDOWN: return KEY_PGDN;
	case LK_COMPOSE: return KEY_F11;     /* Menu: lighter (Doom's gamma) */
	case LK_F11: return KEY_F11;
	case LK_F12: return KEY_F12;
	default: return 0;
	}
}

static struct { uint8_t pressed, key; } q[64];
static int qh, qt;

static void push(int pressed, int key)
{
	if (!key || ((qh + 1) & 63) == qt) return;
	q[qh].pressed = (uint8_t)pressed;
	q[qh].key = (uint8_t)key;
	qh = (qh + 1) & 63;
}

static void poll_input(void)
{
	struct psi_event e[16];
	ssize_t n;
	int i;
	if (kbd_fd >= 0)
		while ((n = read(kbd_fd, e, sizeof e)) > 0)
			for (i = 0; i < n / (int)sizeof e[0]; i++)
				if (e[i].type == LX_EV_KEY && e[i].value != 2)   /* (not the repeats) */
					push(e[i].value != 0, doom_key(e[i].code));
	if (pen_fd >= 0)
		while ((n = read(pen_fd, e, sizeof e)) > 0)
			for (i = 0; i < n / (int)sizeof e[0]; i++)
				if (e[i].type == LX_EV_KEY && e[i].code == LK_BTN_TOUCH)
					push(e[i].value != 0, KEY_FIRE);       /* a tap fires */
}

/* Doom asks for the keys once a tic, until there are none (or a key was let
 * go). A key pressed and let go within one tic (a tap of the pen, or keys
 * that waited out the screen's melt) would be gone before the tic looked:
 * so a key let go in the same round as it was pressed is held back until
 * the next. */
static uint8_t down_now[32];        /* pressed in this round (a bit a key) */

int DG_GetKey(int *pressed, unsigned char *key)
{
	int k;
	if (qh == qt) poll_input();
	if (qh == qt) { memset(down_now, 0, sizeof down_now); return 0; }
	k = q[qt].key;
	if (!q[qt].pressed && (down_now[k >> 3] & (1 << (k & 7)))) {
		memset(down_now, 0, sizeof down_now);   /* (the next tic lets it go) */
		return 0;
	}
	if (q[qt].pressed) down_now[k >> 3] |= (uint8_t)(1 << (k & 7));
	*pressed = q[qt].pressed;
	*key = (unsigned char)k;
	qt = (qt + 1) & 63;
	return 1;
}

/* ---- the clock ---------------------------------------------------------- */

void DG_Init(void) {}
void DG_DrawFrame(void) {}
void DG_SetWindowTitle(const char *title) { (void)title; }

uint32_t DG_GetTicksMs(void)
{
	return now_ms();
}

void DG_SleepMs(uint32_t ms)
{
	struct timespec t;
	t.tv_sec = ms / 1000;
	t.tv_nsec = (long)(ms % 1000) * 1000000L;
	while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
}

int main(int argc, char **argv)
{
	doomgeneric_Create(argc, argv);
	for (;;)
		doomgeneric_Tick();
	return 0;
}
