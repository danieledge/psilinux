/* fbtest.c - the screen, the pen and AF_UNIX sockets (PsiLinux's kernel):
 * /dev/fb0's ioctls, mmap and palette, PSIFB_DAMAGE, /dev/input's ioctls
 * and events, and a Unix socket served and connected by name, across fork.
 *   lx-cc -static-pie -o fbtest fbtest.c
 * With "draw", it leaves a test picture on the screen and waits for the pen
 * (PSIHOST_INPUT on the host). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <linux/fb.h>
#include <linux/input.h>

struct psifb_rect { unsigned short x, y, w, h; };
#define PSIFB_DAMAGE _IOW('F', 0xa0, struct psifb_rect)

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_fb(int draw)
{
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo x;
	unsigned short r[256], g[256], b[256];
	struct fb_cmap cm = { 0, 256, r, g, b, 0 };
	struct psifb_rect d = { 0, 0, 640, 240 };
	unsigned char *p;
	int fd = open("/dev/fb0", O_RDWR), i, y;
	CHECK(fd >= 0, "open /dev/fb0: %s", strerror(errno));
	if (fd < 0) return;
	CHECK(ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && v.xres == 640 && v.yres == 240 && v.bits_per_pixel == 8, "vscreeninfo %ux%u %u", v.xres, v.yres, v.bits_per_pixel);
	CHECK(ioctl(fd, FBIOGET_FSCREENINFO, &x) == 0 && x.line_length == 640 && x.visual == FB_VISUAL_PSEUDOCOLOR && x.smem_len >= 640 * 240, "fscreeninfo");
	v.bits_per_pixel = 16;
	CHECK(ioctl(fd, FBIOPUT_VSCREENINFO, &v) < 0 && v.bits_per_pixel == 8, "16 bpp refused, and the mode given back");
	/* depth 4, the Psion's own greys: 320 bytes a row, and back to 8 */
	v.bits_per_pixel = 4;
	CHECK(ioctl(fd, FBIOPUT_VSCREENINFO, &v) == 0 && v.bits_per_pixel == 4 && v.grayscale, "depth 4");
	CHECK(ioctl(fd, FBIOGET_FSCREENINFO, &x) == 0 && x.line_length == 320 && x.smem_len == 320 * 240, "depth 4 fscreeninfo");
	v.bits_per_pixel = 8;
	CHECK(ioctl(fd, FBIOPUT_VSCREENINFO, &v) == 0 && v.bits_per_pixel == 8, "depth 8 again");
	CHECK(ioctl(fd, FBIOGET_FSCREENINFO, &x) == 0 && x.line_length == 640, "depth 8 fscreeninfo");
	p = mmap(0, x.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(p != MAP_FAILED, "mmap: %s", strerror(errno));
	if (p == MAP_FAILED) return;
	/* a grey ramp in the palette: index = grey */
	for (i = 0; i < 256; i++) r[i] = g[i] = b[i] = (unsigned short)(i * 257);
	CHECK(ioctl(fd, FBIOPUTCMAP, &cm) == 0, "putcmap");
	memset(r, 0, sizeof r);
	CHECK(ioctl(fd, FBIOGETCMAP, &cm) == 0 && r[200] == 200 * 257, "getcmap");
	/* the picture: 16 bars, a smooth ramp (dithered), a frame and a disc */
	for (y = 0; y < 240; y++)
		for (i = 0; i < 640; i++) {
			int c;
			if (y < 80) c = (i / 40) * 17;
			else if (y < 160) c = i * 255 / 639;
			else { int dx = i - 320, dy = y - 200; c = dx * dx + dy * dy * 4 < 1600 ? 0 : 255; }
			if (i == 0 || i == 639 || y == 0 || y == 239) c = 0;
			p[y * 640 + i] = (unsigned char)c;
		}
	CHECK(ioctl(fd, PSIFB_DAMAGE, &d) == 0, "damage");
	{
		unsigned char c;
		lseek(fd, 640 * 100 + 320, SEEK_SET);
		CHECK(read(fd, &c, 1) == 1 && c == p[640 * 100 + 320], "read() sees mmap's pixels");
	}
	if (draw) return;
	munmap(p, x.smem_len);
	close(fd);
}

static void test_input(int wait_pen)
{
	char name[64] = "";
	unsigned long bits[4] = { 0 };
	struct input_absinfo ai;
	struct input_event e[8];
	int k = open("/dev/input/event0", O_RDONLY | O_NONBLOCK), t = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
	CHECK(k >= 0 && t >= 0, "open /dev/input/event0, 1");
	if (k < 0 || t < 0) return;
	CHECK(ioctl(t, EVIOCGNAME(sizeof name), name) > 0 && strstr(name, "touch"), "name: %s", name);
	CHECK(ioctl(t, EVIOCGBIT(0, sizeof bits), bits) > 0 && (bits[0] & (1 << EV_ABS)), "pen has EV_ABS");
	CHECK(ioctl(t, EVIOCGABS(ABS_X), &ai) == 0 && ai.maximum == 639, "ABS_X max %d", ai.maximum);
	CHECK(ioctl(t, EVIOCGABS(ABS_Y), &ai) == 0 && ai.maximum == 239, "ABS_Y max %d", ai.maximum);
	CHECK(ioctl(k, EVIOCGBIT(EV_KEY, sizeof bits), bits) > 0 && (bits[KEY_A / 32] & (1u << (KEY_A % 32))), "keyboard has KEY_A");
	CHECK(read(t, e, sizeof e) < 0 && errno == EAGAIN, "no events yet");
	if (wait_pen) {
		struct pollfd pf = { t, POLLIN, 0 };
		int n, i, x = -1, y = -1, down = 0;
		printf("waiting for the pen...\n");
		fflush(stdout);
		CHECK(poll(&pf, 1, 20000) == 1, "poll: a pen event");
		usleep(100000);
		n = (int)read(t, e, sizeof e);
		for (i = 0; i < n / (int)sizeof e[0]; i++) {
			if (e[i].type == EV_ABS && e[i].code == ABS_X) x = e[i].value;
			if (e[i].type == EV_ABS && e[i].code == ABS_Y) y = e[i].value;
			if (e[i].type == EV_KEY && e[i].code == BTN_TOUCH) down = e[i].value;
		}
		printf("pen at %d,%d %s\n", x, y, down ? "down" : "up");
		CHECK(x >= 0 && y >= 0, "pen position");
	}
	close(k);
	close(t);
}

static void test_unix(void)
{
	struct sockaddr_un a;
	int s = socket(AF_UNIX, SOCK_STREAM, 0), c, pid, st;
	char buf[64];
	socklen_t len;
	struct ucred cr;
	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, "/tmp/.X11-unix/X9");
	CHECK(s >= 0, "socket(AF_UNIX)");
	CHECK(bind(s, (struct sockaddr *)&a, sizeof a) == 0, "bind: %s", strerror(errno));
	CHECK(listen(s, 4) == 0, "listen");
	c = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(bind(c, (struct sockaddr *)&a, sizeof a) < 0 && errno == EADDRINUSE, "bound twice");
	close(c);
	pid = fork();
	if (pid == 0) {
		int k = socket(AF_UNIX, SOCK_STREAM, 0);
		if (connect(k, (struct sockaddr *)&a, sizeof a) < 0) _exit(1);
		/* (a fork child runs while its parent waits, as vfork: it cannot
		 * wait for an answer from it) */
		if (write(k, "hello X", 7) != 7) _exit(2);
		close(k);
		_exit(0);
	}
	c = accept(s, 0, 0);
	CHECK(c >= 0, "accept: %s", strerror(errno));
	CHECK(read(c, buf, sizeof buf) == 7 && !memcmp(buf, "hello X", 7), "read from the client");
	len = sizeof cr;
	CHECK(getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0 && cr.pid == pid, "SO_PEERCRED pid %d (want %d)", cr.pid, pid);
	CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0, "client status %x", st);
	CHECK(read(c, buf, sizeof buf) == 0, "EOF after the client closed");
	close(c);
	/* abstract names, and refused when nobody listens */
	c = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	memcpy(a.sun_path, "\0nobody", 7);
	CHECK(connect(c, (struct sockaddr *)&a, 2 + 7) < 0 && errno == ECONNREFUSED, "connect to nobody");
	close(c);
	close(s);
}

int main(int argc, char **argv)
{
	int draw = argc > 1 && !strcmp(argv[1], "draw");
	if (argc > 1 && !strcmp(argv[1], "hold")) {
		/* the picture, then 15 s without asking for the pen */
		test_fb(1);
		sleep(15);
		printf("fbtest: held\n");
		return 0;
	}
	test_unix();
	test_fb(draw);
	test_input(draw);
	printf("fbtest: %d passed, %d failed\n", passes, fails);
	return fails != 0;
}
