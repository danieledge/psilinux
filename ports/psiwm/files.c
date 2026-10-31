/* files.c - Files: the disks and folders, as the Psion's System screen
 * shows them. Open, cut, copy and paste, rename, delete, create new folder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include "psiwm.h"

enum { F_OPEN = 1, F_NEWDIR, F_RENAME, F_DELETE, F_CUT, F_COPY, F_PASTE, F_UP, F_HIDDEN, F_DISK, F_ROOT, F_C, F_D };
static const MenuItem ff[] = {
	{ "Open", "Ctrl+O", F_OPEN, MI_SEP },
	{ "Create new folder…", "Shift+Ctrl+N", F_NEWDIR, 0 },
	{ "Rename…", "Ctrl+R", F_RENAME, 0 },
	{ "Delete", "Ctrl+D", F_DELETE, MI_SEP },
	{ "Close", "Ctrl+E", C_CLOSE, 0 } };
static const MenuItem fe[] = {
	{ "Cut", "Ctrl+X", F_CUT, 0 }, { "Copy", "Ctrl+C", F_COPY, 0 }, { "Paste", "Ctrl+V", F_PASTE, 0 } };
static const MenuItem fv[] = {
	{ "Up a folder", "Ctrl+U", F_UP, MI_SEP },
	{ "Show hidden files", 0, F_HIDDEN, MI_CHECK },
	{ "Show toolbar", "Ctrl+T", C_TOOLBAR, MI_CHECK } };
static const MenuItem fd[] = {
	{ "Linux (/)", 0, F_ROOT, 0 }, { "Internal disk (C)", 0, F_C, 0 }, { "Memory disk (D)", 0, F_D, 0 } };
static const MenuItem ft[] = { { "Help", "Shift+Ctrl+H", C_HELP, 0 },
	{ "About PsiLinux…", "Shift+Ctrl+A", C_ABOUT, 0 } };
static const Menu fmenus[] = { { "File", ff, 5 }, { "Edit", fe, 3 }, { "View", fv, 3 }, { "Disk", fd, 3 }, { "Tools", ft, 2 } };
static const TbBut ftb[] = {
	{ I_T_DISK, "Disk", F_DISK }, { I_T_NEWFOLDER, "New folder", F_NEWDIR },
	{ I_T_COPY, "Copy", F_COPY }, { I_T_PASTE, "Paste", F_PASTE } };
static const Menu diskpop = { "Disk", fd, 3 };

typedef struct { char *name; long long size; time_t mt; char dir, exe, link; } Ent;
typedef struct
	{
	char path[512];
	Ent *e; int n, cap;
	int sel, top, hidden, lastx;
	char clip[512]; int cut;
	int sbdrag;
	} Files;

#define RH 17             /* a row */
#define HDR 17            /* the column headings */
#define STH 17            /* the status line */
#define SBW 16            /* the scroll bar */

static int cmp(const void *a, const void *b)
	{
	const Ent *x = a, *y = b;
	if (x->dir != y->dir) return y->dir - x->dir;
	return strcasecmp(x->name, y->name);
	}

static void freelist(Files *f)
	{
	int i;
	for (i = 0; i < f->n; i++) free(f->e[i].name);
	f->n = 0;
	}

static int at_root(Files *f) { return !strcmp(f->path, "/"); }

static void load(App *a, const char *keep)
	{
	Files *f = a->p;
	DIR *d;
	struct dirent *de;
	freelist(f);
	if (!at_root(f))
		{
		if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 64; f->e = realloc(f->e, f->cap * sizeof *f->e); }
		memset(&f->e[0], 0, sizeof f->e[0]);
		f->e[0].name = strdup("..");
		f->e[0].dir = 1;
		f->n = 1;
		}
	d = opendir(f->path);
	while (d && (de = readdir(d)))
		{
		struct stat st, lst;
		char p[1024];
		Ent *e;
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		if (de->d_name[0] == '.' && !f->hidden) continue;
		if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 64; f->e = realloc(f->e, f->cap * sizeof *f->e); }
		snprintf(p, sizeof p, "%s/%s", at_root(f) ? "" : f->path, de->d_name);
		e = &f->e[f->n++];
		memset(e, 0, sizeof *e);
		e->name = strdup(de->d_name);
		if (lstat(p, &lst) == 0)
			{
			e->link = S_ISLNK(lst.st_mode);
			if (stat(p, &st) != 0) st = lst;
			e->dir = S_ISDIR(st.st_mode);
			e->exe = !e->dir && (st.st_mode & 0111);
			e->size = st.st_size;
			e->mt = st.st_mtime;
			}
		}
	if (d) closedir(d);
	else infoprint("Folder not found - %s", strerror(errno));
	qsort(f->e + !at_root(f), f->n - !at_root(f), sizeof *f->e, cmp);
	f->sel = 0; f->top = 0;
	if (keep)
		{
		int i;
		for (i = 0; i < f->n; i++) if (!strcmp(f->e[i].name, keep)) f->sel = i;
		}
	snprintf(a->title, sizeof a->title, "%s", f->path);
	app_retitle(a);
	app_dirty(a);
	}

static void go(App *a, const char *path, const char *keep)
	{
	Files *f = a->p;
	snprintf(f->path, sizeof f->path, "%s", path);
	load(a, keep);
	}

static void f_init(App *a, const char *arg)
	{
	Files *f = calloc(1, sizeof *f);
	struct stat st;
	a->p = f;
	f->lastx = -1;
	snprintf(f->path, sizeof f->path, "%s", arg ? arg : stat("/c", &st) == 0 ? "/c" : getenv("HOME") ? getenv("HOME") : "/");
	load(a, 0);
	}

static int rows(App *a) { return (a->h - HDR - STH) / RH; }

static void sizestr(char *b, long long s)
	{
	if (s < 1024) sprintf(b, "%lld bytes", s);
	else if (s < 1024 * 1024) sprintf(b, "%lld KB", (s + 1023) / 1024);
	else sprintf(b, "%lld.%lld MB", s / (1024 * 1024), s % (1024 * 1024) * 10 / (1024 * 1024));
	}

static void f_draw(App *a)
	{
	Files *f = a->p;
	int i, y, nr = rows(a), lw = a->w - SBW;
	int cs = lw - 226, cd = lw - 108;      /* the size and date columns */
	char b[64];
	fill(0, 0, a->w, a->h, WHITE);
	/* the headings */
	fill(0, 0, lw, HDR, PALE);
	fill(0, HDR - 1, lw, 1, MID);
	text(&f_bold, 26, 1, "Name", -1, BLACK);
	text(&f_bold, cs + 90 - tw(&f_bold, "Size", -1), 1, "Size", -1, BLACK);
	text(&f_bold, cd, 1, "Modified", -1, BLACK);
	if (f->sel < f->top) f->top = f->sel;
	if (f->sel >= f->top + nr) f->top = f->sel - nr + 1;
	for (i = f->top, y = HDR; i < f->n && i < f->top + nr; i++, y += RH)
		{
		Ent *e = &f->e[i];
		int g = BLACK, isup = !strcmp(e->name, "..");
		if (i == f->sel)
			{
			fill(0, y, lw, RH, BLACK);
			g = WHITE;
			}
		pic(isup ? I_S_UP : e->dir ? I_S_FOLDER : e->exe ? I_S_PROG : I_S_FILE, 5, y + 1);
		if (i == f->sel)
			{
			/* (the picture keeps a white patch, as a tag) */
			fill(3, y + 1, 19, RH - 2, WHITE);
			pic(isup ? I_S_UP : e->dir ? I_S_FOLDER : e->exe ? I_S_PROG : I_S_FILE, 5, y + 1);
			}
		textfit(&f_normal, 26, y + 1, isup ? "Up a folder" : e->name, cs - 34, g);
		if (!e->dir)
			{
			sizestr(b, e->size);
			text(&f_normal, cs + 90 - tw(&f_normal, b, -1), y + 1, b, -1, g);
			}
		if (!isup && e->mt)
			{
			struct tm *tm = localtime(&e->mt);
			time_t now = time(0);
			strftime(b, sizeof b, now - e->mt < 180 * 86400L && now >= e->mt ? "%e %b %H:%M" : "%e %b %Y", tm);
			text(&f_normal, cd, y + 1, b[0] == ' ' ? b + 1 : b, -1, g);
			}
		}
	/* the scroll bar: EIKON's, slimmer */
	{
	int sx = lw, sh = a->h - STH, th, ty;
	if (f->n > nr)
		{
		fill2(sx, 0, SBW, sh, LIGHT, WHITE);
		fill(sx, 0, 1, sh, BLACK);
		th = sh * nr / f->n; if (th < 14) th = 14;
		ty = (sh - th) * f->top / (f->n - nr);
		bevel(sx + 1, ty, SBW - 1, th, 0);
		}
	else th = 0, ty = 0;
	if (th > 20) { fill(sx + 5, ty + th / 2 - 3, SBW - 10, 1, DARK); fill(sx + 5, ty + th / 2, SBW - 10, 1, DARK); fill(sx + 5, ty + th / 2 + 3, SBW - 10, 1, DARK); }
	}
	/* the status line: the disk, and how many */
	{
	int nd = 0, nf = 0, x;
	y = a->h - STH;
	for (i = 0; i < f->n; i++) { if (!strcmp(f->e[i].name, "..")) continue; if (f->e[i].dir) nd++; else nf++; }
	fill(0, y, a->w, STH, LIGHT);
	fill(0, y, a->w, 1, BLACK);
	pic(I_S_DISK, 4, y + 2);
	x = 24;
	x += text(&f_bold, x, y + 1, !strncmp(f->path, "/c", 2) && (f->path[2] == 0 || f->path[2] == '/') ? "Internal disk"
		: !strncmp(f->path, "/d", 2) && (f->path[2] == 0 || f->path[2] == '/') ? "Memory disk" : "Linux", -1, BLACK) + 14;
	snprintf(b, sizeof b, "%d folder%s, %d file%s", nd, nd == 1 ? "" : "s", nf, nf == 1 ? "" : "s");
	text(&f_normal, x, y + 1, b, -1, BLACK);
	if (f->clip[0])
		{
		const char *s = strrchr(f->clip, '/');
		snprintf(b, sizeof b, "%s: %s", f->cut ? "Cut" : "Copied", s ? s + 1 : f->clip);
		textfit(&f_normal, a->w - 190, y + 1, b, 184, DARK);
		}
	}
	}

static void fullpath(Files *f, const char *name, char *out, int max)
	{
	snprintf(out, max, "%s/%s", at_root(f) ? "" : f->path, name);
	}

static int is_text(const char *p)
	{
	char b[512];
	int n, i;
	FILE *fp = fopen(p, "rb");
	if (!fp) return 0;
	n = fread(b, 1, sizeof b, fp);
	fclose(fp);
	for (i = 0; i < n; i++) if (b[i] == 0) return 0;
	return 1;
	}

static void up(App *a)
	{
	Files *f = a->p;
	char *s, old[256];
	if (at_root(f)) { infoprint("Already at the top"); return; }
	s = strrchr(f->path, '/');
	snprintf(old, sizeof old, "%s", s + 1);
	if (s == f->path) s[1] = 0; else *s = 0;
	load(a, old);
	}

static void open_sel(App *a)
	{
	Files *f = a->p;
	Ent *e;
	char p[1024];
	if (!f->n) return;
	e = &f->e[f->sel];
	if (!strcmp(e->name, "..")) { up(a); return; }
	fullpath(f, e->name, p, sizeof p);
	if (e->dir) { go(a, p, 0); return; }
	if (e->exe && !is_text(p)) { const char *v[2] = { p, 0 }; spawn(v, e->name); return; }
	if (e->size > 200000) { infoprint("File too large for Notes"); return; }
	if (is_text(p)) { notes_open_file(p); return; }
	infoprint("No program for this file");
	}

/* ---- copy, move, delete (folders too) ---- */

static int copy_file(const char *from, const char *to)
	{
	char b[4096];
	int in, out, n, ok = 1;
	struct stat st;
	if (stat(from, &st)) return 0;
	if (S_ISDIR(st.st_mode))
		{
		DIR *d;
		struct dirent *de;
		if (mkdir(to, st.st_mode & 0777) && errno != EEXIST) return 0;
		if (!(d = opendir(from))) return 0;
		while ((de = readdir(d)))
			{
			char a[1024], c[1024];
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
			snprintf(a, sizeof a, "%s/%s", from, de->d_name);
			snprintf(c, sizeof c, "%s/%s", to, de->d_name);
			if (!copy_file(a, c)) ok = 0;
			}
		closedir(d);
		return ok;
		}
	if ((in = open(from, O_RDONLY)) < 0) return 0;
	if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777)) < 0) { close(in); return 0; }
	while ((n = read(in, b, sizeof b)) > 0)
		if (write(out, b, n) != n) { ok = 0; break; }
	if (n < 0) ok = 0;
	close(in);
	if (close(out)) ok = 0;
	return ok;
	}

static int remove_all(const char *p)
	{
	struct stat st;
	if (lstat(p, &st)) return 0;
	if (S_ISDIR(st.st_mode))
		{
		DIR *d = opendir(p);
		struct dirent *de;
		int ok = 1;
		if (!d) return 0;
		while ((de = readdir(d)))
			{
			char c[1024];
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
			snprintf(c, sizeof c, "%s/%s", p, de->d_name);
			if (!remove_all(c)) ok = 0;
			}
		closedir(d);
		return rmdir(p) == 0 && ok;
		}
	return unlink(p) == 0;
	}

static void paste(App *a)
	{
	Files *f = a->p;
	char to[1024], name[256];
	const char *base;
	struct stat st;
	int k;
	if (!f->clip[0]) { infoprint("Nothing to paste"); return; }
	base = strrchr(f->clip, '/') ? strrchr(f->clip, '/') + 1 : f->clip;
	snprintf(name, sizeof name, "%s", base);
	fullpath(f, name, to, sizeof to);
	if (!strcmp(to, f->clip) && f->cut) { infoprint("Already in this folder"); return; }
	if (!strncmp(to, f->clip, strlen(f->clip)) && to[strlen(f->clip)] == '/') { infoprint("Cannot paste a folder into itself"); return; }
	for (k = 1; stat(to, &st) == 0 && k < 100; k++)
		{
		if (k == 1) snprintf(name, sizeof name, "Copy of %s", base);
		else snprintf(name, sizeof name, "Copy (%d) of %s", k, base);
		fullpath(f, name, to, sizeof to);
		}
	if (f->cut && rename(f->clip, to) == 0) { f->clip[0] = 0; load(a, name); infoprint("Moved"); return; }
	busy(f->cut ? "Moving…" : "Copying…");
	if (!copy_file(f->clip, to)) { busy(0); infoprint("Could not copy - %s", strerror(errno)); load(a, name); return; }
	if (f->cut) { remove_all(f->clip); f->clip[0] = 0; }
	busy(0);
	load(a, name);
	infoprint(f->cut ? "Moved" : "Copied");
	}

static void made_dir(App *a, int ok, char *const *ed)
	{
	char p[1024];
	if (!ok) return;
	if (!ed[0][0]) { infoprint("No name entered"); return; }
	if (strchr(ed[0], '/')) { infoprint("A name cannot have / in it"); return; }
	fullpath(a->p, ed[0], p, sizeof p);
	if (mkdir(p, 0755)) { infoprint("Could not create folder - %s", strerror(errno)); return; }
	load(a, ed[0]);
	}

static void renamed(App *a, int ok, char *const *ed)
	{
	Files *f = a->p;
	char from[1024], to[1024];
	if (!ok || !f->n) return;
	if (!ed[0][0]) { infoprint("No name entered"); return; }
	if (strchr(ed[0], '/')) { infoprint("A name cannot have / in it"); return; }
	fullpath(f, f->e[f->sel].name, from, sizeof from);
	fullpath(f, ed[0], to, sizeof to);
	if (rename(from, to)) { infoprint("Could not rename - %s", strerror(errno)); return; }
	load(a, ed[0]);
	}

static void deleted(App *a, int ok, char *const *ed)
	{
	Files *f = a->p;
	char p[1024];
	int keep = f->sel;
	(void)ed;
	if (!ok) return;
	fullpath(f, f->e[f->sel].name, p, sizeof p);
	busy("Deleting…");
	if (!remove_all(p)) infoprint("Could not delete - %s", strerror(errno));
	else infoprint("Deleted");
	busy(0);
	load(a, 0);
	f->sel = keep < f->n ? keep : f->n - 1;
	}

static int real_sel(Files *f) { return f->n && strcmp(f->e[f->sel].name, ".."); }

static void f_cmd(App *a, int cmd)
	{
	Files *f = a->p;
	char b[300];
	switch (cmd)
		{
	case F_OPEN: open_sel(a); break;
	case F_UP: up(a); break;
	case F_HIDDEN: f->hidden = !f->hidden; load(a, real_sel(f) ? f->e[f->sel].name : 0); break;
	case F_ROOT: go(a, "/", 0); break;
	case F_C: go(a, "/c", 0); break;
	case F_D: go(a, "/d", 0); break;
	case F_DISK:
		{
		/* the Disk button: a list beside it */
		popup_open(a, &diskpop, SW - TBW, 28);
		break;
		}
	case F_NEWDIR:
		{
		const char *ed[] = { "Name", "New folder", 0 };
		dialog(a, "Create new folder", 0, ed, DB_OKCANCEL, made_dir);
		break;
		}
	case F_RENAME:
		{
		const char *ed[] = { "New name", 0, 0 };
		if (!real_sel(f)) { infoprint("No file selected"); break; }
		ed[1] = f->e[f->sel].name;
		dialog(a, "Rename", 0, ed, DB_OKCANCEL, renamed);
		break;
		}
	case F_DELETE:
		{
		const char *l[3] = { 0, 0, 0 };
		static char q[300];
		if (!real_sel(f)) { infoprint("Nothing to delete"); break; }
		l[0] = f->e[f->sel].dir ? "The folder and everything in it will be deleted" : "This action cannot be undone";
		snprintf(q, sizeof q, "Delete \"%s\"?", f->e[f->sel].name);
		dialog(a, q, l, 0, DB_YESNO, deleted);
		break;
		}
	case F_CUT: case F_COPY:
		if (!real_sel(f)) { infoprint("Nothing to %s", cmd == F_CUT ? "cut" : "copy"); break; }
		fullpath(f, f->e[f->sel].name, f->clip, sizeof f->clip);
		f->cut = cmd == F_CUT;
		clip_set(f->clip, strlen(f->clip));
		snprintf(b, sizeof b, cmd == F_CUT ? "Cut" : "Copied");
		infoprint("%s", b);
		app_dirty(a);
		break;
	case F_PASTE: paste(a); break;
		}
	}

static int f_state(App *a, int cmd)
	{
	Files *f = a->p;
	switch (cmd)
		{
	case F_HIDDEN: return f->hidden ? MS_TICK : 0;
	case F_PASTE: return f->clip[0] ? 0 : MS_DIM;
	case F_UP: return at_root(f) ? MS_DIM : 0;
	case F_RENAME: case F_DELETE: case F_CUT: case F_COPY: return real_sel(f) ? 0 : MS_DIM;
		}
	return 0;
	}

static int f_key(App *a, KeySym ks, unsigned st, const char *s, int n)
	{
	Files *f = a->p;
	int nr = rows(a);
	(void)st;
	switch (ks)
		{
	case XK_Up: if (f->sel > 0) f->sel--; break;
	case XK_Down: if (f->sel < f->n - 1) f->sel++; break;
	case XK_Page_Up: f->sel -= nr - 1; if (f->sel < 0) f->sel = 0; break;
	case XK_Page_Down: f->sel += nr - 1; if (f->sel >= f->n) f->sel = f->n - 1; break;
	case XK_Home: f->sel = 0; break;
	case XK_End: f->sel = f->n - 1; break;
	case XK_Return: case XK_KP_Enter: case XK_Right: open_sel(a); return 1;
	case XK_BackSpace: case XK_Left: up(a); return 1;
	case XK_Delete: f_cmd(a, F_DELETE); return 1;
	case XK_Escape: return 1;
	default:
		if (n == 1 && (unsigned char)s[0] > 32)
			{
			/* a letter: the next name it starts */
			int i;
			for (i = 1; i <= f->n; i++)
				{
				int k = (f->sel + i) % f->n;
				if ((f->e[k].name[0] | 32) == (s[0] | 32)) { f->sel = k; break; }
				}
			}
		else return 0;
		}
	app_dirty(a);
	return 1;
	}

static void f_pen(App *a, int type, int x, int y)
	{
	Files *f = a->p;
	int nr = rows(a), lw = a->w - SBW;
	if (x >= lw && y < a->h - STH)
		{
		/* the scroll bar: page up or down, or drag */
		if (f->n <= nr) return;
		if (type == PEN_DOWN || f->sbdrag)
			{
			f->sbdrag = type != PEN_UP;
			f->top = (y * (f->n - nr) + (a->h - STH) / 2) / (a->h - STH);
			if (f->top < 0) f->top = 0;
			if (f->top > f->n - nr) f->top = f->n - nr;
			if (f->sel < f->top) f->sel = f->top;
			if (f->sel >= f->top + nr) f->sel = f->top + nr - 1;
			app_dirty(a);
			}
		return;
		}
	if (type == PEN_UP) { f->sbdrag = 0; return; }
	if (type != PEN_DOWN) return;
	if (y >= HDR && y < HDR + nr * RH)
		{
		int i = f->top + (y - HDR) / RH;
		if (i >= f->n) return;
		/* one tap selects, a second opens */
		if (i == f->sel) open_sel(a);
		else { f->sel = i; app_dirty(a); }
		}
	else if (y >= a->h - STH) popup_open(a, &diskpop, SW - TBW, 28);
	}

static void f_fini(App *a) { Files *f = a->p; freelist(f); free(f->e); free(f); }

const AppClass files_class = { "Files", I_P_FILES, 0, 0, fmenus, 5, ftb, 4,
	f_init, f_draw, f_key, f_pen, f_cmd, f_state, 0, f_fini };
