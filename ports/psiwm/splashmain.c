/* psisplash - the "Starting PsiLinux" screen on /dev/fb0, before the X
 * server is up (no X: a few KB of its own and the fonts). Run it from the
 * script that starts X, as often as there is news:
 *     psisplash "Starting the X server…" 30
 * psiwm then shows the same screen, and goes on from where this left off.
 *     psisplash -w SECONDS MSG PERCENT  keeps the screen open that long (or until
 *                                     killed), so it stays up while X starts
 *     psisplash -o FILE MSG PERCENT   writes a test frame buffer file instead
 *                                     (640x240x8 and its colour map, as Xfbdev -fb) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "splash.h"

#ifndef PSILINUX_VERSION
#define PSILINUX_VERSION "0.10"
#endif
#define FBIOPUTCMAP 0x4605
#define PSIFB_DAMAGE 0x400846a0
struct cmap { unsigned start, len; unsigned short *r, *g, *b, *t; };

static unsigned char px[SPW * SPH];

int main(int argc, char **argv)
	{
	const char *out = 0, *msg = "Starting PsiLinux…";
	char ver[32] = PSILINUX_VERSION, b[128];
	int pc = 10, fd, i, hold = 0;
	FILE *f;
	if (argc > 2 && !strcmp(argv[1], "-w")) { hold = atoi(argv[2]); argv += 2; argc -= 2; }
	if (argc > 2 && !strcmp(argv[1], "-o")) { out = argv[2]; argv += 2; argc -= 2; }
	if (argc > 1) msg = argv[1];
	if (argc > 2) pc = atoi(argv[2]);
	/* the version: the kernel's (/proc/version says "(PsiLinux 0.nn, ...") */
	if ((f = fopen("/proc/version", "r")))
		{
		char *p;
		if (fgets(b, sizeof b, f) && (p = strstr(b, "(PsiLinux ")))
			snprintf(ver, sizeof ver, "%.*s", (int)strcspn(p + 10, ",)"), p + 10);
		fclose(f);
		}
	splash_draw(px, msg, pc, ver);
	if (out)
		{
		unsigned char cm[768];
		memset(cm, 0, sizeof cm);
		for (i = 0; i < 16; i++) cm[3 * i] = cm[3 * i + 1] = cm[3 * i + 2] = i * 17;
		if (!(f = fopen(out, "wb"))) { perror(out); return 1; }
		fwrite(px, 1, sizeof px, f);
		fwrite(cm, 1, sizeof cm, f);
		fclose(f);
		return 0;
		}
	if ((fd = open("/dev/fb0", O_RDWR)) < 0) { perror("/dev/fb0"); return 1; }
	{
	/* palette entries 0-15: the 16 greys, so a pixel's value is its grey */
	unsigned short g[16];
	struct cmap c = { 0, 16, g, g, g, 0 };
	for (i = 0; i < 16; i++) g[i] = i * 17 * 257;
	ioctl(fd, FBIOPUTCMAP, &c);
	}
	if (pwrite(fd, px, sizeof px, 0) != (ssize_t)sizeof px) { perror("write"); return 1; }
	{
	unsigned short r[4] = { 0, 0, SPW, SPH };
	ioctl(fd, PSIFB_DAMAGE, r);
	}
	/* (while it holds the screen, PsiLinux keeps showing it: X opening it
	 * meanwhile takes over without a flash of the terminal) */
	if (hold > 0)
		sleep(hold);
	close(fd);
	return 0;
	}
