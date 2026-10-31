/* lx_fb.c - the Linux screen and its input: /dev/fb0 and /dev/input/event*.
 *
 *  - /dev/fb0: 640x240, 8 bits a pixel, palette indices (pseudocolour), as
 *    any Linux framebuffer. mmap gives the pixels themselves: they are in the
 *    global chunk the app draws from (lxfb.h), so a program's drawing costs
 *    nothing to pass on. Programs say where they drew with PSIFB_DAMAGE (or
 *    FBIOPAN_DISPLAY: all of it), and the app draws just that. A program
 *    that maps the screen and never says so is looked at by the app
 *    regularly instead (poll_ms).
 *  - /dev/input/event0 (the keyboard) and event1 (the pen, absolute, in
 *    screen pixels): evdev, with the ioctls that X, SDL and libinput ask.
 *    The app's events come through the runner (lx_k_input).
 */
#include "lx_core.h"
#include "lx_vfs.h"
#include "../common/lxfb.h"

/* ---- the screen ------------------------------------------------------------- */

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP         0x4604
#define FBIOPUTCMAP         0x4605
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611
#define FBIO_WAITFORVSYNC   0x40044620
#define PSIFB_DAMAGE        0x400846a0      /* _IOW('F', 0xa0, struct { u16 x, y, w, h; }) */

typedef struct { lx_u32 offset, length, msb_right; } fb_bitfield;
typedef struct {
	lx_u32 xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, grayscale;
	fb_bitfield red, green, blue, transp;
	lx_u32 nonstd, activate, height, width, accel_flags;
	lx_u32 pixclock, left_margin, right_margin, upper_margin, lower_margin, hsync_len, vsync_len;
	lx_u32 sync, vmode, rotate, colorspace, reserved[4];
} fb_var;
typedef struct {
	char id[16];
	lx_u32 smem_start, smem_len, type, type_aux, visual;
	lx_u16 xpanstep, ypanstep, ywrapstep, pad;
	lx_u32 line_length, mmio_start, mmio_len, accel;
	lx_u16 capabilities, reserved[2], pad2;
} fb_fix;
typedef struct { lx_u32 start, len, red, green, blue, transp; } fb_cmap;

typedef struct fbfile {
	int pushes;                 /* it said where it drew (PSIFB_DAMAGE, FBIOPAN_DISPLAY) */
	int mapped;
} fbfile;

static LxFbHeader *fb;          /* (the header; the pixels are LX_FB_HDR on) */
static int fb_users;            /* open /dev/fb0 files (a mapping keeps its file open) */
static int fb_silent;           /* of those, mapped and never saying where they drew */
static lx_u16 cmap[3][256];

#define PIX ((lx_u8 *)fb + LX_FB_HDR)
#define ROW (fb->depth == 4 ? LX_FB_ROW4 : LX_FB_W)    /* a row's bytes */
#define BYTES (ROW * LX_FB_H)

static void grey_of(int i)
{
	/* (Rec. 601 luma, as the eye sees it) */
	fb->grey[i] = (lx_u8)(((lx_u32)cmap[0][i] * 77 + (lx_u32)cmap[1][i] * 150 + (lx_u32)cmap[2][i] * 29) >> 16);
}

static void cmap_default(void)
{
	int i;
	/* the 16 VGA colours, then a grey ramp: what an 8-bit console starts with */
	static const lx_u8 vga[16][3] = {
		{0,0,0},{170,0,0},{0,170,0},{170,85,0},{0,0,170},{170,0,170},{0,170,170},{170,170,170},
		{85,85,85},{255,85,85},{85,255,85},{255,255,85},{85,85,255},{255,85,255},{85,255,255},{255,255,255} };
	for (i = 0; i < 256; i++) {
		lx_u16 v = (lx_u16)(i * 257);
		if (i < 16) { cmap[0][i] = vga[i][0] * 257; cmap[1][i] = vga[i][1] * 257; cmap[2][i] = vga[i][2] * 257; }
		else cmap[0][i] = cmap[1][i] = cmap[2][i] = v;
		grey_of(i);
	}
	fb->grey_seq++;
}

static void damage(int x, int y, int w, int h)
{
	unsigned head;
	if (!fb) return;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > LX_FB_W) w = LX_FB_W - x;
	if (y + h > LX_FB_H) h = LX_FB_H - y;
	if (w <= 0 || h <= 0) return;
	head = fb->dmg_head;
	if (head - fb->dmg_tail >= LX_FB_DMG) fb->dmg_all++;     /* (full: the app draws it all) */
	else {
		LxFbRect *r = &fb->dmg[head & (LX_FB_DMG - 1)];
		r->x = (lx_u16)x; r->y = (lx_u16)y; r->w = (lx_u16)w; r->h = (lx_u16)h;
		fb->dmg_head = head + 1;
	}
	bk_fb_notify();
}

static void set_poll(void)
{
	if (fb) fb->poll_ms = fb_silent ? 100 : 0;
}

static int fb_pread(lx_thread *t, lx_file *f, char *buf, lx_u32 n, lx_s64 off)
{
	(void)t; (void)f;
	if (off >= BYTES) return 0;
	if (off + n > BYTES) n = (lx_u32)(BYTES - off);
	bk_memcpy(buf, PIX + off, n);
	return (int)n;
}

static int fb_pwrite(lx_thread *t, lx_file *f, const char *buf, lx_u32 n, lx_s64 off)
{
	int y0, y1;
	(void)t; (void)f;
	if (off >= BYTES) return n ? -LX_ENOSPC : 0;
	if (off + n > BYTES) n = (lx_u32)(BYTES - off);
	bk_memcpy(PIX + off, buf, n);
	y0 = (int)(off / ROW);
	y1 = (int)((off + n + ROW - 1) / ROW);
	damage(0, y0, LX_FB_W, y1 - y0);
	return (int)n;
}

static int fb_read(lx_thread *t, lx_file *f, char *buf, lx_u32 n)
{
	int r = fb_pread(t, f, buf, n, f->pos);
	if (r > 0) f->pos += r;
	return r;
}

static int fb_write(lx_thread *t, lx_file *f, const char *buf, lx_u32 n)
{
	int r = fb_pwrite(t, f, buf, n, f->pos);
	if (r > 0) f->pos += r;
	return r;
}

static lx_s64 fb_lseek(lx_file *f, lx_s64 off, int whence)
{
	lx_s64 p = whence == 0 ? off : whence == 1 ? f->pos + off : BYTES + off;
	if (p < 0) return -LX_EINVAL;
	return f->pos = p;
}

static int fb_fstat(lx_file *f, struct lx_stat64 *st)
{
	lx_vn v;
	bk_memset(&v, 0, sizeof v);
	v.mode = f->mode;
	v.ino = f->ino;
	v.rdev = 0x1d00;            /* (29, 0) */
	v.fs = LX_FS_DEV;
	lx_vn_stat(&v, st);
	return 0;
}

static void fill_var(fb_var *v)
{
	bk_memset(v, 0, sizeof *v);
	v->xres = v->xres_virtual = LX_FB_W;
	v->yres = v->yres_virtual = LX_FB_H;
	v->bits_per_pixel = fb->depth;
	v->grayscale = fb->depth == 4;
	v->red.length = v->green.length = v->blue.length = fb->depth;
	v->height = 45;             /* mm: the 5mx's screen */
	v->width = 120;
	v->pixclock = 100000;
	v->vmode = 0;
}

static int fb_ioctl(lx_thread *t, lx_file *f, lx_u32 req, lx_u32 arg)
{
	fbfile *ff = (fbfile *)f->priv;
	switch (req) {
	case FBIOGET_VSCREENINFO: case FBIOPUT_VSCREENINFO: {
		/* (640x240 in 8 bits or 4: a request for anything else gets the mode
		 * back, and says so) */
		fb_var want;
		lx_u32 b;
		if (!lx_uaccess(t, arg, sizeof(fb_var))) return -LX_EFAULT;
		bk_memcpy(&want, (void *)arg, sizeof want);
		b = want.bits_per_pixel;
		if (req == FBIOPUT_VSCREENINFO && !((b == 0 || b == 8 || b == 4) &&
		    (!want.xres || want.xres == LX_FB_W) && (!want.yres || want.yres == LX_FB_H))) {
			fill_var((fb_var *)arg);
			return -LX_EINVAL;
		}
		if (req == FBIOPUT_VSCREENINFO) {
			if (b && b != fb->depth) {
				fb->depth = b;
				bk_memset(PIX, 0, LX_FB_PIXELS);     /* (black, in either) */
			}
			fb->dmg_all++, bk_fb_notify();
		}
		fill_var((fb_var *)arg);
		return 0;
	}
	case FBIOGET_FSCREENINFO: {
		fb_fix *x = (fb_fix *)arg;
		if (!lx_uaccess(t, arg, sizeof(fb_fix))) return -LX_EFAULT;
		bk_memset(x, 0, sizeof *x);
		lx_strlcpy(x->id, "Psion 5mx", sizeof x->id);
		x->smem_start = (lx_u32)PIX;
		x->smem_len = BYTES;
		x->visual = fb->depth == 4 ? 4 : 3;     /* FB_VISUAL_STATIC_PSEUDOCOLOR, PSEUDOCOLOR (type 0: packed pixels) */
		x->line_length = ROW;
		return 0;
	}
	case FBIOGETCMAP: case FBIOPUTCMAP: {
		fb_cmap *c = (fb_cmap *)arg;
		lx_u32 i, n;
		int k;
		if (!lx_uaccess(t, arg, sizeof(fb_cmap))) return -LX_EFAULT;
		if (c->start >= 256 || c->len > 256 - c->start) return -LX_EINVAL;
		n = c->len;
		for (k = 0; k < 3; k++) {
			lx_u32 p = k == 0 ? c->red : k == 1 ? c->green : c->blue;
			if (!lx_uaccess(t, p, n * 2)) return -LX_EFAULT;
			if (req == FBIOGETCMAP) bk_memcpy((void *)p, &cmap[k][c->start], n * 2);
			else bk_memcpy(&cmap[k][c->start], (void *)p, n * 2);
		}
		if (req == FBIOGETCMAP) {
			if (c->transp && lx_uaccess(t, c->transp, n * 2)) bk_memset((void *)c->transp, 0, n * 2);
			return 0;
		}
		for (i = c->start; i < c->start + n; i++) grey_of((int)i);
		fb->grey_seq++;
		fb->dmg_all++;          /* (every pixel may look different now) */
		bk_fb_notify();
		return 0;
	}
	case FBIOPAN_DISPLAY:
		if (!ff->pushes) { ff->pushes = 1; if (ff->mapped) { fb_silent--; set_poll(); } }
		damage(0, 0, LX_FB_W, LX_FB_H);
		return 0;
	case PSIFB_DAMAGE: {
		lx_u16 *r = (lx_u16 *)arg;
		if (!lx_uaccess(t, arg, 8)) return -LX_EFAULT;
		if (!ff->pushes) { ff->pushes = 1; if (ff->mapped) { fb_silent--; set_poll(); } }
		damage(r[0], r[1], r[2], r[3]);
		return 0;
	}
	case FBIOBLANK: case FBIO_WAITFORVSYNC: return 0;
	}
	return -LX_ENOTTY;
}

static int fb_mmap(lx_thread *t, lx_file *f, lx_u32 len, lx_s64 off, lx_u32 *addr)
{
	fbfile *ff = (fbfile *)f->priv;
	(void)t;
	if (off < 0 || off + len > ((LX_FB_PIXELS + 4095) & ~4095)) return -LX_EINVAL;
	if (!ff->mapped) {
		ff->mapped = 1;
		if (!ff->pushes) { fb_silent++; set_poll(); }
	}
	*addr = (lx_u32)PIX + (lx_u32)off;
	return 0;
}

static int fb_poll(lx_file *f) { (void)f; return LX_POLLIN | LX_POLLOUT; }

static void fb_close(lx_file *f)
{
	fbfile *ff = (fbfile *)f->priv;
	if (ff->mapped && !ff->pushes) fb_silent--;
	bk_free(ff);
	if (--fb_users == 0) {
		fb_silent = 0;
		bk_fb_release();
		fb = 0;
	} else set_poll();
}

static const lx_fops fb_ops = { fb_read, fb_write, fb_pread, fb_pwrite, fb_lseek, fb_fstat, fb_ioctl, fb_poll, 0, fb_close, 0, 0, fb_mmap };

lx_file *lx_fb_open(int flags, lx_u64 ino, int *err)
{
	fbfile *ff;
	lx_file *f;
	int fresh = !fb;
	if (!fb) {
		fb = (LxFbHeader *)bk_fb_map();
		if (!fb) { *err = -LX_ENOMEM; return 0; }
	}
	ff = (fbfile *)bk_alloc(sizeof *ff);
	f = ff ? lx_file_new(&fb_ops, ff, flags) : 0;
	if (!f) {
		bk_free(ff);
		if (fresh) { bk_fb_release(); fb = 0; }
		*err = -LX_ENOMEM;
		return 0;
	}
	f->mode = LX_S_IFCHR | 0660;
	f->ino = ino;
	if (fresh) {
		/* (a new screen: black, the default palette) */
		bk_memset(PIX, 0, LX_FB_PIXELS);
		fb->depth = 8;
		cmap_default();
		fb->dmg_all++;
	}
	fb_users++;
	set_poll();
	return f;
}

/* ---- /dev/input ------------------------------------------------------------- */

#define EV_SYN 0
#define EV_KEY 1
#define EV_ABS 3
#define EV_MSC 4
#define EV_REP 0x14
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_TOUCH 0x14a
#define KEY_BYTES 96            /* KEY_MAX 0x2ff */
#define EVQ 64

typedef struct { lx_u32 sec, usec; lx_u16 type, code; lx_s32 value; } input_event;

typedef struct evq {
	int dev;
	int head, count;
	int grab;
	input_event ev[EVQ];
	struct evq *next;
} evq;

static evq *evqs;
static lx_u8 keys[LX_IN_NDEV][KEY_BYTES];
static lx_s32 absv[2];
static evq *grabber[LX_IN_NDEV];

static const char *const ev_name[LX_IN_NDEV] = { "Psion 5mx keyboard", "Psion 5mx touch screen" };

void lx_k_input(int dev, int type, int code, int value)
{
	lx_s64 now = bk_now_real();
	input_event e;
	evq *q;
	if (dev < 0 || dev >= LX_IN_NDEV) return;
	if (type == EV_KEY && code < KEY_BYTES * 8) {
		if (value) keys[dev][code >> 3] |= (lx_u8)(1 << (code & 7));
		else keys[dev][code >> 3] &= (lx_u8)~(1 << (code & 7));
	}
	if (type == EV_ABS && code < 2) absv[code] = value;
	e.sec = (lx_u32)(now / 1000000);
	e.usec = (lx_u32)(now % 1000000);
	e.type = (lx_u16)type;
	e.code = (lx_u16)code;
	e.value = value;
	for (q = evqs; q; q = q->next) {
		if (q->dev != dev || (grabber[dev] && grabber[dev] != q)) continue;
		if (q->count == EVQ) continue;          /* (full: a program not reading) */
		q->ev[(q->head + q->count) % EVQ] = e;
		q->count++;
	}
	if (type == EV_SYN) lx_k_event();
}

static int ev_read(lx_thread *t, lx_file *f, char *buf, lx_u32 n)
{
	evq *q = (evq *)f->priv;
	lx_u32 done = 0;
	(void)t;
	if (n < sizeof(input_event)) return -LX_EINVAL;
	if (!q->count) return (f->flags & LX_O_NONBLOCK) ? -LX_EAGAIN : LX_BLOCK;
	while (q->count && done + sizeof(input_event) <= n) {
		bk_memcpy(buf + done, &q->ev[q->head], sizeof(input_event));
		q->head = (q->head + 1) % EVQ;
		q->count--;
		done += sizeof(input_event);
	}
	return (int)done;
}

static int ev_write(lx_thread *t, lx_file *f, const char *buf, lx_u32 n) { (void)t; (void)f; (void)buf; return (int)n; }
static int ev_poll(lx_file *f) { return ((evq *)f->priv)->count ? LX_POLLIN : 0; }
static lx_s64 ev_lseek(lx_file *f, lx_s64 o, int w) { (void)f; (void)o; (void)w; return -LX_ESPIPE; }

static void put_bits(lx_u8 *d, int len, int bit)
{
	if (bit >> 3 < len) d[bit >> 3] |= (lx_u8)(1 << (bit & 7));
}

static int ev_ioctl(lx_thread *t, lx_file *f, lx_u32 req, lx_u32 arg)
{
	evq *q = (evq *)f->priv;
	int len = (int)((req >> 16) & 0x3fff), nr = (int)(req & 0xff), i;
	lx_u8 *d = (lx_u8 *)arg;
	if (((req >> 8) & 0xff) != 'E') return -LX_ENOTTY;
	if (len && !lx_uaccess(t, arg, (lx_u32)len)) return -LX_EFAULT;
	switch (nr) {
	case 0x01: *(lx_s32 *)arg = 0x010001; return 0;          /* EVIOCGVERSION */
	case 0x02: {                                             /* EVIOCGID */
		lx_u16 *id = (lx_u16 *)arg;
		id[0] = 0x19; id[1] = 0x5053; id[2] = (lx_u16)(q->dev + 1); id[3] = 1;
		return 0;
	}
	case 0x03: ((lx_s32 *)arg)[0] = 500; ((lx_s32 *)arg)[1] = 33; return 0;   /* EVIOCGREP (the Psion repeats keys itself) */
	case 0x06: case 0x07: case 0x08: {                       /* EVIOCGNAME, PHYS, UNIQ */
		const char *s = nr == 0x06 ? ev_name[q->dev] : nr == 0x07 ? (q->dev ? "psion/input1" : "psion/input0") : "";
		int l = lx_strlen(s) + 1;
		if (l > len) l = len;
		bk_memcpy(d, s, l);
		if (l) d[l - 1] = 0;
		return l;
	}
	case 0x09:                                               /* EVIOCGPROP */
		bk_memset(d, 0, len);
		if (q->dev == LX_IN_PEN) put_bits(d, len, 1);       /* INPUT_PROP_DIRECT */
		return len;
	case 0x18:                                               /* EVIOCGKEY */
		i = len < KEY_BYTES ? len : KEY_BYTES;
		bk_memset(d, 0, len);
		bk_memcpy(d, keys[q->dev], i);
		return len;
	case 0x19: case 0x1a: case 0x1b: bk_memset(d, 0, len); return len;   /* LED, SND, SW */
	case 0x90:                                               /* EVIOCGRAB */
		if (arg) { if (grabber[q->dev] && grabber[q->dev] != q) return -LX_EBUSY; grabber[q->dev] = q; }
		else if (grabber[q->dev] == q) grabber[q->dev] = 0;
		return 0;
	case 0xa0: return 0;                                     /* EVIOCSCLOCKID */
	case 0x91: return 0;                                     /* EVIOCREVOKE */
	}
	if (nr >= 0x40 && nr < 0x40 + 0x40) {                    /* EVIOCGABS(axis) */
		lx_s32 *a = (lx_s32 *)arg;
		int axis = nr - 0x40;
		if (q->dev != LX_IN_PEN || axis > 1 || len < 24) return -LX_EINVAL;
		bk_memset(a, 0, 24);
		a[0] = absv[axis];
		a[2] = axis ? LX_FB_H - 1 : LX_FB_W - 1;
		a[5] = 5;               /* resolution: pixels a mm (about) */
		return 0;
	}
	if (nr >= 0x20 && nr < 0x40) {                           /* EVIOCGBIT(type) */
		int type = nr - 0x20;
		bk_memset(d, 0, len);
		if (type == 0) {
			put_bits(d, len, EV_SYN);
			put_bits(d, len, EV_KEY);
			if (q->dev == LX_IN_PEN) put_bits(d, len, EV_ABS);
		} else if (type == EV_KEY) {
			if (q->dev == LX_IN_KBD) for (i = 1; i < 0xf8; i++) put_bits(d, len, i);
			else { put_bits(d, len, BTN_LEFT); put_bits(d, len, BTN_RIGHT); put_bits(d, len, BTN_TOUCH); }
		} else if (type == EV_ABS && q->dev == LX_IN_PEN) {
			put_bits(d, len, 0);
			put_bits(d, len, 1);
		}
		return len;
	}
	return -LX_EINVAL;
}

static int ev_fstat(lx_file *f, struct lx_stat64 *st)
{
	lx_vn v;
	bk_memset(&v, 0, sizeof v);
	v.mode = f->mode;
	v.ino = f->ino;
	v.rdev = 0x0d40 + ((evq *)f->priv)->dev;   /* (13, 64+n) */
	v.fs = LX_FS_DEV;
	lx_vn_stat(&v, st);
	return 0;
}

static void ev_close(lx_file *f)
{
	evq *q = (evq *)f->priv, **l;
	for (l = &evqs; *l; l = &(*l)->next)
		if (*l == q) { *l = q->next; break; }
	if (grabber[q->dev] == q) grabber[q->dev] = 0;
	bk_free(q);
}

static const lx_fops ev_ops = { ev_read, ev_write, 0, 0, ev_lseek, ev_fstat, ev_ioctl, ev_poll, 0, ev_close, 0, 0, 0 };

lx_file *lx_evdev_open(int dev, int flags, lx_u64 ino, int *err)
{
	evq *q = (evq *)bk_alloc(sizeof *q);
	lx_file *f = q ? lx_file_new(&ev_ops, q, flags) : 0;
	if (!f) { bk_free(q); *err = -LX_ENOMEM; return 0; }
	q->dev = dev;
	q->next = evqs;
	evqs = q;
	f->mode = LX_S_IFCHR | 0660;
	f->ino = ino;
	return f;
}
