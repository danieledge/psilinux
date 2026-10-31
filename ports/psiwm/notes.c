/* notes.c - Notes: a plain-text editor. Words wrap to the window; Shift and
 * the arrows select; cut, copy and paste go through X's clipboard. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "psiwm.h"

enum { N_NEW = 1, N_OPEN, N_SAVE, N_SAVEAS, N_CUT, N_COPY, N_PASTE, N_ALL, N_FIND, N_NEXT, N_ZIN, N_ZOUT };
static const MenuItem nf[] = {
	{ "Create new file", "Ctrl+N", N_NEW, 0 },
	{ "Open file…", "Ctrl+O", N_OPEN, 0 },
	{ "Save", "Ctrl+S", N_SAVE, 0 },
	{ "Save as…", "Shift+Ctrl+S", N_SAVEAS, MI_SEP },
	{ "Close", "Ctrl+E", C_CLOSE, 0 } };
static const MenuItem ne[] = {
	{ "Cut", "Ctrl+X", N_CUT, 0 }, { "Copy", "Ctrl+C", N_COPY, 0 }, { "Paste", "Ctrl+V", N_PASTE, MI_SEP },
	{ "Select all", "Ctrl+A", N_ALL, MI_SEP },
	{ "Find…", "Ctrl+F", N_FIND, 0 }, { "Find next", "Ctrl+J", N_NEXT, 0 } };
static const MenuItem nv[] = {
	{ "Zoom in", "Ctrl+M", N_ZIN, 0 }, { "Zoom out", "Shift+Ctrl+M", N_ZOUT, MI_SEP },
	{ "Show toolbar", "Ctrl+T", C_TOOLBAR, MI_CHECK } };
static const MenuItem nt[] = { { "Help", "Shift+Ctrl+H", C_HELP, 0 },
	{ "About PsiLinux…", "Shift+Ctrl+A", C_ABOUT, 0 } };
static const Menu nmenus[] = { { "File", nf, 5 }, { "Edit", ne, 6 }, { "View", nv, 3 }, { "Tools", nt, 2 } };
static const TbBut ntb[] = {
	{ I_T_NEW, "New file", N_NEW }, { I_T_OPEN, "Open file", N_OPEN },
	{ I_T_SAVE, "Save", N_SAVE }, { I_T_FIND, "Find", N_FIND } };

static const BFont *const zooms[3] = { &f_small, &f_normal, &f_large };

typedef struct
	{
	char *b; int len, cap;
	int cur, anchor;            /* the selection: anchor..cur (anchor -1: none) */
	int *ls, nl, lcap;          /* where each shown line starts */
	int top, goalx, zoom, changed, lw;
	char path[512], find[128];
	int closing;
	} Notes;

#define MX 6                    /* the margins */
#define SBW 16

static const BFont *fnt(Notes *n) { return zooms[n->zoom]; }

static void wrap(App *a)
	{
	Notes *n = a->p;
	const BFont *f = fnt(n);
	int w = a->w - 2 * MX - SBW, i = 0;
	n->nl = 0;
	n->lw = w;
	for (;;)
		{
		int x = 0, brk = -1, j = i;
		if (n->nl == n->lcap) { n->lcap = n->lcap ? n->lcap * 2 : 256; n->ls = realloc(n->ls, n->lcap * sizeof(int)); }
		n->ls[n->nl++] = i;
		while (j < n->len && n->b[j] != '\n')
			{
			const unsigned char *p = (const unsigned char *)n->b + j, *q = p;
			int k = j, cw;
			/* (one character, whole) */
			q++;
			while (q < (const unsigned char *)n->b + n->len && (*q & 0xc0) == 0x80) q++;
			cw = n->b[j] == '\t' ? tw(f, "    ", 4) : tw(f, (const char *)p, q - p);
			if (x + cw > w && j > i)
				{
				if (brk > i) j = brk;
				break;
				}
			x += cw;
			j = k + (q - p);
			if (n->b[k] == ' ') brk = j;
			}
		if (j >= n->len) break;
		i = n->b[j] == '\n' ? j + 1 : j;
		}
	}

static int line_of(Notes *n, int pos)
	{
	int lo = 0, hi = n->nl - 1;
	while (lo < hi)
		{
		int m = (lo + hi + 1) / 2;
		if (n->ls[m] <= pos) lo = m; else hi = m - 1;
		}
	/* (a position at a wrapped line's end belongs to the line after) */
	return lo;
	}

static int line_end(Notes *n, int l)
	{
	int e = l + 1 < n->nl ? n->ls[l + 1] : n->len;
	if (e > n->ls[l] && e <= n->len && e > 0 && n->b[e - 1] == '\n') e--;
	return e;
	}

static int xpos(Notes *n, int l, int pos)
	{
	const BFont *f = fnt(n);
	int i, x = 0;
	for (i = n->ls[l]; i < pos; i++)
		x += n->b[i] == '\t' ? tw(f, "    ", 4) : ((unsigned char)n->b[i] & 0xc0) == 0x80 ? 0 : tw(f, n->b + i, 1 + ((unsigned char)n->b[i] >= 0xc0) + ((unsigned char)n->b[i] >= 0xe0));
	return x;
	}

static int pos_at(Notes *n, int l, int x)
	{
	int i, e = line_end(n, l), px = 0;
	const BFont *f = fnt(n);
	for (i = n->ls[l]; i < e; )
		{
		int k = i + 1, cw;
		while (k < e && ((unsigned char)n->b[k] & 0xc0) == 0x80) k++;
		cw = n->b[i] == '\t' ? tw(f, "    ", 4) : tw(f, n->b + i, k - i);
		if (px + cw / 2 >= x) return i;
		px += cw;
		i = k;
		}
	return e;
	}

static void retitle(App *a)
	{
	Notes *n = a->p;
	const char *s = strrchr(n->path, '/');
	snprintf(a->title, sizeof a->title, "%s", n->path[0] ? (s ? s + 1 : n->path) : "Untitled");
	app_retitle(a);
	}

static void set_text(App *a, const char *s, int len)
	{
	Notes *n = a->p;
	if (len + 1 > n->cap) { n->cap = len + 4096; n->b = realloc(n->b, n->cap); }
	memcpy(n->b, s, len);
	n->len = len;
	n->cur = 0; n->anchor = -1; n->top = 0; n->changed = 0;
	wrap(a);
	app_dirty(a);
	}

static int load(App *a, const char *path)
	{
	Notes *n = a->p;
	FILE *f = fopen(path, "rb");
	char *buf;
	long sz;
	if (!f) { infoprint("File not found"); return 0; }
	fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
	if (sz > 256 * 1024) { fclose(f); infoprint("File too large for Notes"); return 0; }
	buf = malloc(sz + 1);
	sz = fread(buf, 1, sz, f);
	fclose(f);
	set_text(a, buf, sz);
	free(buf);
	snprintf(n->path, sizeof n->path, "%s", path);
	retitle(a);
	return 1;
	}

static int save(App *a)
	{
	Notes *n = a->p;
	FILE *f = fopen(n->path, "wb");
	if (!f || fwrite(n->b, 1, n->len, f) != (size_t)n->len) { if (f) fclose(f); infoprint("Could not save - %s", strerror(errno)); return 0; }
	if (fclose(f)) { infoprint("Could not save - %s", strerror(errno)); return 0; }
	n->changed = 0;
	infoprint("Saved");
	return 1;
	}

static void n_init(App *a, const char *arg)
	{
	Notes *n = calloc(1, sizeof *n);
	a->p = n;
	n->zoom = 1; n->anchor = -1; n->goalx = -1;
	n->cap = 4096; n->b = malloc(n->cap);
	wrap(a);
	if (arg) load(a, arg);
	else retitle(a);
	}

static void ensure_visible(App *a)
	{
	Notes *n = a->p;
	int rows = a->h / fnt(n)->h, l = line_of(n, n->cur);
	if (l < n->top) n->top = l;
	if (l >= n->top + rows) n->top = l - rows + 1;
	}

static void n_draw(App *a)
	{
	Notes *n = a->p;
	const BFont *f = fnt(n);
	int rows = a->h / f->h, l, y, s0 = -1, s1 = -1;
	if (n->lw != a->w - 2 * MX - SBW) wrap(a);
	ensure_visible(a);
	if (n->anchor >= 0 && n->anchor != n->cur)
		{ s0 = n->anchor < n->cur ? n->anchor : n->cur; s1 = n->anchor < n->cur ? n->cur : n->anchor; }
	fill(0, 0, a->w - SBW, a->h, WHITE);
	for (l = n->top, y = 0; l < n->nl && l < n->top + rows; l++, y += f->h)
		{
		int st = n->ls[l], e = line_end(n, l), i, x = MX;
		/* in runs: plain, selected, plain; tabs as spaces */
		for (i = st; i < e; )
			{
			int j = i, sel = s0 >= 0 && i >= s0 && i < s1;
			while (j < e && n->b[j] != '\t' && (s0 < 0 || (j >= s0 && j < s1) == sel)) j++;
			if (j == i)
				{
				/* a tab */
				int w = tw(f, "    ", 4);
				if (sel) fill(x, y, w, f->h, BLACK);
				x += w; i++;
				continue;
				}
			if (sel) { int w = tw(f, n->b + i, j - i); fill(x, y, w, f->h, BLACK); x += text(f, x, y, n->b + i, j - i, WHITE); }
			else x += text(f, x, y, n->b + i, j - i, BLACK);
			i = j;
			}
		if (s0 >= 0 && e < n->len && n->b[e] == '\n' && e >= s0 && e < s1) fill(x, y, 4, f->h, BLACK);
		}
	/* the cursor */
	if (n->anchor < 0 || n->anchor == n->cur)
		{
		l = line_of(n, n->cur);
		if (l >= n->top && l < n->top + rows)
			fill(MX + xpos(n, l, n->cur) - 1, (l - n->top) * f->h, 2, f->h, BLACK);
		}
	if (!n->len)
		textc(&f_normal, 0, a->h / 2 - 8, a->w - SBW, "Type here, or tap Open file", MID);
	/* the scroll bar */
	{
	int sx = a->w - SBW, th, ty;
	fill(sx, 0, SBW, a->h, WHITE);
	if (n->nl > rows)
		{
		fill2(sx, 0, SBW, a->h, LIGHT, WHITE);
		fill(sx, 0, 1, a->h, BLACK);
		th = a->h * rows / n->nl; if (th < 14) th = 14; ty = (a->h - th) * n->top / (n->nl - rows);
		bevel(sx + 1, ty, SBW - 1, th, 0);
		}
	}
	}

static void del_sel(Notes *n)
	{
	int s0, s1;
	if (n->anchor < 0 || n->anchor == n->cur) { n->anchor = -1; return; }
	s0 = n->anchor < n->cur ? n->anchor : n->cur;
	s1 = n->anchor < n->cur ? n->cur : n->anchor;
	memmove(n->b + s0, n->b + s1, n->len - s1);
	n->len -= s1 - s0;
	n->cur = s0;
	n->anchor = -1;
	n->changed = 1;
	}

static void insert(App *a, const char *s, int len)
	{
	Notes *n = a->p;
	del_sel(n);
	if (n->len + len + 1 > n->cap) { n->cap = n->len + len + 4096; n->b = realloc(n->b, n->cap); }
	memmove(n->b + n->cur + len, n->b + n->cur, n->len - n->cur);
	memcpy(n->b + n->cur, s, len);
	n->len += len;
	n->cur += len;
	n->changed = 1;
	wrap(a);
	}

static void pasted(App *a, const char *s, int len)
	{
	if (!s || len <= 0) { infoprint("Nothing to paste"); return; }
	insert(a, s, len);
	app_dirty(a);
	}

static void copy_sel(Notes *n)
	{
	int s0 = n->anchor < n->cur ? n->anchor : n->cur, s1 = n->anchor < n->cur ? n->cur : n->anchor;
	clip_set(n->b + s0, s1 - s0);
	}

static int find_next(App *a, int from)
	{
	Notes *n = a->p;
	int k = strlen(n->find), i;
	if (!k) return 0;
	for (i = 0; i < n->len; i++)
		{
		int p = (from + i) % n->len;
		if (p + k <= n->len && !strncasecmp(n->b + p, n->find, k))
			{
			n->anchor = p; n->cur = p + k;
			app_dirty(a);
			return 1;
			}
		}
	infoprint("Not found");
	return 0;
	}

static void found(App *a, int ok, char *const *ed)
	{
	Notes *n = a->p;
	if (!ok) return;
	if (!ed[0][0]) { infoprint("No text entered"); return; }
	snprintf(n->find, sizeof n->find, "%s", ed[0]);
	find_next(a, n->cur);
	}

static void n_cmd(App *a, int cmd);

static void opened(App *a, int ok, char *const *ed)
	{
	if (!ok) return;
	if (!ed[0][0]) { infoprint("No filename entered"); return; }
	load(a, ed[0]);
	}

static void saved_as(App *a, int ok, char *const *ed)
	{
	Notes *n = a->p;
	struct stat st;
	if (!ok) { n->closing = 0; return; }
	if (!ed[0][0]) { infoprint("No filename entered"); n->closing = 0; return; }
	if (stat(ed[0], &st) == 0 && S_ISDIR(st.st_mode)) { infoprint("That is a folder"); n->closing = 0; return; }
	snprintf(n->path, sizeof n->path, "%s", ed[0]);
	retitle(a);
	if (save(a) && n->closing) app_close(a);
	}

static void default_name(char *b, int max)
	{
	struct stat st;
	const char *dir = stat("/c/Documents", &st) == 0 ? "/c/Documents" : stat("/c", &st) == 0 ? "/c" : getenv("HOME") ? getenv("HOME") : "/tmp";
	snprintf(b, max, "%s/Notes.txt", dir);
	}

static void n_cmd(App *a, int cmd)
	{
	Notes *n = a->p;
	char b[512];
	switch (cmd)
		{
	case N_NEW:
		if (n->changed && n->path[0]) save(a);
		n->path[0] = 0;
		set_text(a, "", 0);
		retitle(a);
		break;
	case N_OPEN:
		{
		const char *ed[] = { "Filename", 0, 0 };
		if (n->changed && n->path[0]) save(a);
		if (n->path[0]) ed[1] = n->path; else { default_name(b, sizeof b); ed[1] = b; }
		dialog(a, "Open file", 0, ed, DB_OKCANCEL, opened);
		break;
		}
	case N_SAVE:
		if (n->path[0]) { save(a); break; }
		/* fall through: no name yet */
	case N_SAVEAS:
		{
		const char *ed[] = { "Filename", 0, 0 };
		if (n->path[0]) ed[1] = n->path; else { default_name(b, sizeof b); ed[1] = b; }
		dialog(a, "Save as", 0, ed, DB_OKCANCEL, saved_as);
		break;
		}
	case N_CUT: case N_COPY:
		if (n->anchor < 0 || n->anchor == n->cur) { infoprint("Nothing selected"); break; }
		copy_sel(n);
		if (cmd == N_CUT) { del_sel(n); wrap(a); }
		else infoprint("Copied");
		break;
	case N_PASTE: clip_get(a, pasted); break;
	case N_ALL: n->anchor = 0; n->cur = n->len; break;
	case N_FIND:
		{
		const char *ed[] = { "Find", n->find, 0 };
		dialog(a, "Find", 0, ed, DB_OKCANCEL, found);
		break;
		}
	case N_NEXT:
		if (!n->find[0]) n_cmd(a, N_FIND);
		else find_next(a, n->cur);
		break;
	case N_ZIN: n->zoom = (n->zoom + 1) % 3; wrap(a); infoprint(n->zoom == 0 ? "Small" : n->zoom == 1 ? "Medium" : "Large"); break;
	case N_ZOUT: n->zoom = (n->zoom + 2) % 3; wrap(a); infoprint(n->zoom == 0 ? "Small" : n->zoom == 1 ? "Medium" : "Large"); break;
	case C_CLOSE: break;
		}
	app_dirty(a);
	}

static int n_state(App *a, int cmd)
	{
	Notes *n = a->p;
	int has = n->anchor >= 0 && n->anchor != n->cur;
	switch (cmd)
		{
	case N_CUT: case N_COPY: return has ? 0 : MS_DIM;
		}
	return 0;
	}

static int n_key(App *a, KeySym ks, unsigned st, const char *s, int len)
	{
	Notes *n = a->p;
	int shift = st & ShiftMask, l = line_of(n, n->cur), rows = a->h / fnt(n)->h, old = n->cur, vert = 0;
	switch (ks)
		{
	case XK_Left: if (!shift && n->anchor >= 0 && n->anchor != n->cur) { n->cur = n->cur < n->anchor ? n->cur : n->anchor; n->anchor = -1; goto moved_noanchor; }
		while (n->cur > 0 && (((unsigned char)n->b[--n->cur]) & 0xc0) == 0x80) ; break;
	case XK_Right: if (!shift && n->anchor >= 0 && n->anchor != n->cur) { n->cur = n->cur > n->anchor ? n->cur : n->anchor; n->anchor = -1; goto moved_noanchor; }
		if (n->cur < n->len) { n->cur++; while (n->cur < n->len && ((unsigned char)n->b[n->cur] & 0xc0) == 0x80) n->cur++; } break;
	case XK_Up: case XK_Down: case XK_Page_Up: case XK_Page_Down:
		{
		int d = ks == XK_Up ? -1 : ks == XK_Down ? 1 : ks == XK_Page_Up ? -(rows - 1) : rows - 1;
		int t = l + d;
		if (n->goalx < 0) n->goalx = xpos(n, l, n->cur);
		if (t < 0) { n->cur = 0; }
		else if (t >= n->nl) { n->cur = n->len; }
		else n->cur = pos_at(n, t, n->goalx);
		if (ks == XK_Page_Up || ks == XK_Page_Down) { n->top += d; if (n->top < 0) n->top = 0; if (n->top > n->nl - 1) n->top = n->nl - 1; }
		vert = 1;
		break;
		}
	case XK_Home: n->cur = (st & ControlMask) ? 0 : n->ls[l]; break;
	case XK_End: n->cur = (st & ControlMask) ? n->len : line_end(n, l); break;
	case XK_BackSpace:
		if (n->anchor >= 0 && n->anchor != n->cur) { del_sel(n); wrap(a); }
		else if (n->cur > 0)
			{
			int p = n->cur;
			while (p > 0 && (((unsigned char)n->b[--p]) & 0xc0) == 0x80) ;
			memmove(n->b + p, n->b + n->cur, n->len - n->cur);
			n->len -= n->cur - p; n->cur = p; n->changed = 1; n->anchor = -1;
			wrap(a);
			}
		n->goalx = -1;
		app_dirty(a);
		return 1;
	case XK_Delete:
		if (n->anchor >= 0 && n->anchor != n->cur) { del_sel(n); wrap(a); }
		else if (n->cur < n->len)
			{
			int p = n->cur + 1;
			while (p < n->len && ((unsigned char)n->b[p] & 0xc0) == 0x80) p++;
			memmove(n->b + n->cur, n->b + p, n->len - p);
			n->len -= p - n->cur; n->changed = 1;
			wrap(a);
			}
		n->goalx = -1;
		app_dirty(a);
		return 1;
	case XK_Return: case XK_KP_Enter: insert(a, "\n", 1); n->goalx = -1; app_dirty(a); return 1;
	case XK_Tab: insert(a, "\t", 1); n->goalx = -1; app_dirty(a); return 1;
	case XK_Escape: if (n->anchor >= 0) { n->anchor = -1; app_dirty(a); } return 1;
	default:
		if (!(st & ControlMask) && len >= 1 && (unsigned char)s[0] >= 32 && s[0] != 127)
			{
			insert(a, s, len);
			n->goalx = -1;
			app_dirty(a);
			return 1;
			}
		return 0;
		}
	/* a move: Shift selects */
	if (shift) { if (n->anchor < 0) n->anchor = old; }
	else n->anchor = -1;
moved_noanchor:
	if (!vert) n->goalx = -1;
	app_dirty(a);
	return 1;
	}

static void n_pen(App *a, int type, int x, int y)
	{
	Notes *n = a->p;
	const BFont *f = fnt(n);
	int rows = a->h / f->h, l, p;
	if (x >= a->w - SBW)
		{
		if (n->nl > rows && type != PEN_UP)
			{
			n->top = y * (n->nl - rows) / a->h;
			if (n->top < 0) n->top = 0;
			if (n->top > n->nl - rows) n->top = n->nl - rows;
			l = line_of(n, n->cur);
			if (l < n->top) n->cur = n->ls[n->top];
			if (l >= n->top + rows) n->cur = n->ls[n->top + rows - 1];
			app_dirty(a);
			}
		return;
		}
	l = n->top + y / f->h;
	if (l >= n->nl) l = n->nl - 1;
	if (l < 0) l = 0;
	p = pos_at(n, l, x - MX);
	if (type == PEN_DOWN) { n->anchor = p; n->cur = p; }
	else { n->cur = p; }
	if (type == PEN_UP && n->anchor == n->cur) n->anchor = -1;
	n->goalx = -1;
	app_dirty(a);
	}

static void n_fini(App *a)
	{
	Notes *n = a->p;
	if (n->changed && n->len)
		{
		/* (the Psion's way: closing saves; new text gets a name of its own) */
		if (!n->path[0])
			{
			struct stat st;
			int k;
			default_name(n->path, sizeof n->path);
			for (k = 2; stat(n->path, &st) == 0 && k < 100; k++)
				{
				default_name(n->path, sizeof n->path - 8);
				sprintf(strrchr(n->path, '.'), " %d.txt", k);
				}
			}
		save(a);
		}
	free(n->b); free(n->ls); free(n);
	}

const AppClass notes_class = { "Notes", I_P_NOTES, 0, 0, nmenus, 4, ntb, 4,
	n_init, n_draw, n_key, n_pen, n_cmd, n_state, 0, n_fini };

void notes_open_file(const char *path)
	{
	App *a = app_find(&notes_class);
	if (!a) { app_open(&notes_class, path); return; }
	{
	Notes *n = a->p;
	if (n->changed && n->path[0]) save(a);
	}
	load(a, path);
	focus(a->cl);
	}
