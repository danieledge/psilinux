/* lxfb.h - the Linux screen: memory shared between psilxrun.exe (the
 * runner) and PsiLinux.app, as lxshared.h is for the terminal. Plain C: the
 * kernel (linux/runtime) uses it too.
 *
 * The runner makes the global chunk the first time a program opens
 * /dev/fb0 and sets LxShared.fb_on; the app opens it and shows the Linux
 * screen. Rings, each with one writer and one reader, so no locks:
 *   dmg: the runner writes the rectangles programs changed, the app reads
 *        them (it draws those, and only those)
 *   in:  the app writes keys and the pen, the runner reads (/dev/input)
 * The pixels are 8 bits, palette indices (Linux's FB_VISUAL_PSEUDOCOLOR).
 * The app shows each as the grey the kernel worked out from the palette
 * (grey[]), in the Psion's 16 greys, dithered. Or (depth 4, asked for with
 * FBIOPUT_VSCREENINFO) the Psion's own pixels: 16 greys, 0 black, two a
 * byte with the left one in the low 4 bits, 320 bytes a row. The app copies
 * those as they are: a program that makes its greys itself (Doom, doubling
 * its pixels anyway) saves the app's conversion. */
#ifndef LXFB_H
#define LXFB_H

#define LX_FB_NAME    "PsiLinuxFb"
#define LX_FB_MAGIC   0x42464c50u        /* 'PLFB' */
#define LX_FB_W       640
#define LX_FB_H       240
#define LX_FB_HDR     4096               /* the pixels start a page in: mmap gives them alone */
#define LX_FB_PIXELS  (LX_FB_W * LX_FB_H)
#define LX_FB_SIZE    (LX_FB_HDR + LX_FB_PIXELS)
#define LX_FB_DMG     32                 /* (a power of 2) */
#define LX_FB_IN      128                /* (a power of 2) */
#define LX_FB_ROW4    (LX_FB_W / 2)      /* a row's bytes at depth 4 */

/* /dev/input: which device an event is for */
enum { LX_IN_KBD = 0, LX_IN_PEN = 1, LX_IN_NDEV = 2 };

typedef struct
	{
	unsigned short x, y, w, h;
	} LxFbRect;

typedef struct
	{
	unsigned char dev;                 /* LX_IN_* */
	unsigned char type;                /* EV_KEY, EV_ABS, EV_SYN */
	unsigned short code;
	int value;
	} LxInEvent;

typedef struct
	{
	unsigned int magic;
	volatile unsigned int dmg_head;    /* written by the runner */
	volatile unsigned int dmg_tail;    /* written by the app */
	volatile unsigned int dmg_all;     /* the runner adds 1: draw everything (the ring was full, or a new program) */
	LxFbRect dmg[LX_FB_DMG];

	volatile unsigned int in_head;     /* written by the app */
	volatile unsigned int in_tail;     /* written by the runner */
	LxInEvent in[LX_FB_IN];

	volatile unsigned int grey_seq;    /* the runner adds 1 after changing grey[] */
	volatile int poll_ms;              /* >0: a program draws without saying where: the app looks this often */
	unsigned char grey[256];           /* each palette entry's grey, 0 (black) to 255 (white) */
	volatile unsigned int depth;       /* bits a pixel: 8, or 4 (the Psion's own) */
	} LxFbHeader;

#endif
