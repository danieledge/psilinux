/* desk.c - the desktop (the "PsiLinux" screen: the programs' icons, as the
 * Psion's Extras bar has them) and the toolbar on the right: the program's
 * name, its four buttons, the clock. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include "psiwm.h"

App *desk;
Window tbwin;
extern void do_cmd(App *a, int cmd);

/* ---- the programs ---- */
static const struct { const char *label; int icon; const AppClass *k; const char *exe; } progs[] = {
	{ "Files", I_P_FILES, &files_class, 0 },
	{ "Notes", I_P_NOTES, &notes_class, 0 },
	{ "Calc", I_P_CALC, &calc_class, 0 },
	{ "Clock", I_P_CLOCK, &clock_class, 0 },
	{ "System", I_P_SYSTEM, &sysmon_class, 0 },
	{ "Terminal", I_P_TERM, 0, "xterm" },
	{ "Web", I_P_WEB, 0, "links" },
	{ "Doom", I_P_DOOM, 0, "doom" },
	{ "Eyes", I_P_EYES, 0, "xeyes" },
	{ "About", I_P_PENGUIN, 0, 0 },
};
#define NPROG 10

/* a program on PATH (or a path): its full name, or 0 */
static const char *find_exe(const char *name)
	{
	static char b[512];
	const char *p = getenv("PATH");
	if (strchr(name, '/')) return access(name, X_OK) == 0 ? name : 0;
	if (!p) p = "/usr/local/bin:/usr/bin:/bin";
	while (*p)
		{
		int n = strcspn(p, ":");
		snprintf(b, sizeof b, "%.*s/%s", n, p, name);
		if (access(b, X_OK) == 0) return b;
		p += n + (p[n] == ':');
		}
	return 0;
	}

/* Doom draws on /dev/fb0 and reads the keys itself, so it cannot share them
 * with X: psiwm closes its programs (Notes saves), hands over to
 * psiwm-alone, which stops X, runs Doom, then starts X and psiwm again. */
static char doom_path[512];

static void doom_go(App *a, int ok, char *const *ed)
	{
	const char *v[3] = { "psiwm-alone", doom_path, 0 };
	App *x, *nx;
	(void)a; (void)ed;
	if (!ok) return;
	if (!find_exe("psiwm-alone")) { infoprint("Could not start Doom - psiwm-alone not installed"); return; }
	for (x = apps; x; x = nx) { nx = x->next; if (x != desk) app_close(x); }
	busy("Starting Doom…");
	spawn(v, "Doom");
	XSync(dpy, False);
	exit(0);
	}

static void doom(void)
	{
	static const char *const L[] = {
		"Doom uses the whole screen and the keys itself",
		"The desktop closes while it runs and comes back after - other programs end",
		0 };
	const char *p = access("/d/Linux/Doom/doom", X_OK) == 0 ? "/d/Linux/Doom/doom"
		: access("/c/Linux/Doom/doom", X_OK) == 0 ? "/c/Linux/Doom/doom" : find_exe("doom");
	if (!p) { infoprint("Doom not found - put it in /d/Linux/Doom"); return; }
	snprintf(doom_path, sizeof doom_path, "%s", p);
	dialog(desk, "Play Doom?", L, 0, DB_YESNO, doom_go);
	}

void desk_run(int i)
	{
	if (i < 0 || i >= NPROG) return;
	if (progs[i].k) app_open(progs[i].k, 0);
	else if (progs[i].exe && !strcmp(progs[i].exe, "doom")) doom();
	else if (progs[i].exe && !strcmp(progs[i].exe, "links"))
		{
		static const char *const v[] = { "links", "-g", 0 };
		if (!find_exe("links")) { infoprint("Could not start Web - Links not installed"); return; }
		spawn(v, "Web");
		}
	else if (progs[i].exe)
		{
		const char *v[2] = { progs[i].exe, 0 };
		spawn(v, progs[i].label);
		}
	else about_dialog(desk);
	}

int client_icon(Client *c)
	{
	char n[32];
	int i;
	if (!c) return I_P_PENGUIN;
	if (c->app) return c->app->k->icon >= 0 ? c->app->k->icon : I_P_PROG;
	for (i = 0; c->name[i] && i < 31; i++) n[i] = c->name[i] | 32;
	n[i] = 0;
	if (strstr(n, "term")) return I_P_TERM;
	if (strstr(n, "eyes")) return I_P_EYES;
	if (strstr(n, "links") || strstr(n, "web")) return I_P_WEB;
	if (strstr(n, "doom")) return I_P_DOOM;
	if (strstr(n, "clock")) return I_P_CLOCK;
	if (strstr(n, "calc")) return I_P_CALC;
	return I_P_PROG;
	}

/* ---- the desktop ---- */
enum { D_OPEN = 1 };
static const MenuItem df[] = {
	{ "Open", "Ctrl+O", D_OPEN, MI_SEP },
	{ "Switch program", "Ctrl+Tab", C_SWITCH, 0 } };
static const MenuItem dp[] = {
	{ "Files", 0, C_RUN + 0, 0 }, { "Notes", 0, C_RUN + 1, 0 }, { "Calc", 0, C_RUN + 2, 0 },
	{ "Clock", 0, C_RUN + 3, 0 }, { "System", 0, C_RUN + 4, MI_SEP }, { "Terminal", 0, C_RUN + 5, 0 },
	{ "Web", 0, C_RUN + 6, 0 }, { "Doom", 0, C_RUN + 7, 0 } };
static const MenuItem dv[] = { { "Show toolbar", "Ctrl+T", C_TOOLBAR, MI_CHECK } };
static const MenuItem dt[] = { { "Help", "Shift+Ctrl+H", C_HELP, 0 },
	{ "About PsiLinux…", "Shift+Ctrl+A", C_ABOUT, 0 } };
static const Menu dmenus[] = { { "File", df, 2 }, { "View", dv, 1 }, { "Programs", dp, 8 }, { "Tools", dt, 2 } };
static const TbBut dtb[] = {
	{ I_T_TERM, "Terminal", C_RUN + 5 }, { I_T_OPEN, "Files", C_RUN + 0 },
	{ I_T_NOTES, "Notes", C_RUN + 1 }, { I_T_SWITCH, "Switch", C_SWITCH } };

static int dsel = -1;
#define COLS 5
#define CELLH 92
#define GRIDY 26

static void cell(int i, int *x, int *y, int *w)
	{
	int cw = (desk->w - 16) / COLS;
	*x = 8 + (i % COLS) * cw;
	*y = GRIDY + (i / COLS) * CELLH;
	*w = cw;
	}

static void minute_tick(App *a)
	{
	time_t t = time(0);
	a->tick_at = now_ms() + (60 - localtime(&t)->tm_sec) * 1000L;
	}

static void desk_init(App *a, const char *arg)
	{
	(void)arg;
	minute_tick(a);
	}

static long mem_free_kb(void)
	{
	char b[1024], path[300], *p;
	snprintf(path, sizeof path, "%s/meminfo", procroot);
	if (readfile(path, b, sizeof b) <= 0) return -1;
	p = strstr(b, "MemAvailable:");
	if (!p) p = strstr(b, "MemFree:");
	return p ? atol(strchr(p, ':') + 1) : -1;
	}

static void desk_draw(App *a)
	{
	int i, x, y, w, n = nclients();
	char b[128], kb[32];
	time_t t = time(0);
	struct tm *tm = localtime(&t);
	fill(0, 0, a->w, a->h, WHITE);
	/* the title band */
	fill2(0, 0, a->w, TH - 1, LIGHT, PALER);
	fill(0, TH - 1, a->w, 1, BLACK);
	fill(4, 1, tw(&f_bold, "PsiLinux", -1) + 6, TH - 3, PALE);
	text(&f_bold, 7, 2, "PsiLinux", -1, BLACK);
	strftime(b, sizeof b, "%A %e %B", tm);
	{
	char *s = b; while (*s == ' ') s++;
	{
	int bw = tw(&f_bold, s, -1);
	fill(a->w - bw - 10, 1, bw + 6, TH - 3, PALE);
	text(&f_bold, a->w - bw - 7, 2, s, -1, BLACK);
	}
	}
	/* the icons */
	for (i = 0; i < NPROG; i++)
		{
		int lw;
		cell(i, &x, &y, &w);
		if (i == dsel)
			{
			box(x + w / 2 - 25, y + 1, 50, 48, BLACK);
			box(x + w / 2 - 24, y + 2, 48, 46, MID);
			}
		if (progs[i].icon == I_P_PENGUIN) pic(I_P_PENGUIN, x + (w - 48) / 2, y + 3);
		else pic(progs[i].icon, x + (w - 40) / 2, y + 5);
		lw = tw(&f_bold, progs[i].label, -1);
		if (i == dsel)
			{
			fill(x + (w - lw) / 2 - 4, y + 52, lw + 8, f_bold.h + 2, BLACK);
			text(&f_bold, x + (w - lw) / 2, y + 53, progs[i].label, -1, WHITE);
			}
		else text(&f_bold, x + (w - lw) / 2, y + 53, progs[i].label, -1, BLACK);
		}
	/* the status line (as PsiLinux's own) */
	y = a->h - 18;
	fill(0, y, a->w, 18, DARK);
	fill(0, y, a->w, 1, BLACK);
	{
	long f = mem_free_kb();
	int xx = 8;
	const char *hint = "Tap an icon twice to open it";
	disc(xx + 2, y + 9, 2, WHITE);
	xx += 10;
	snprintf(b, sizeof b, n == 1 ? "1 program open" : n ? "%d programs open" : "No programs open", n);
	xx += text(&f_normal, xx, y + 2, b, -1, WHITE) + 18;
	if (f >= 0)
		{
		/* (the free memory at the right, the hint between if there is room) */
		int fw;
		fmt_kb(kb, f);
		snprintf(b, sizeof b, "%s free", kb);
		fw = tw(&f_normal, b, -1);
		text(&f_normal, a->w - 8 - fw, y + 2, b, -1, WHITE);
		if (xx + tw(&f_normal, hint, -1) + 18 <= a->w - 8 - fw) text(&f_normal, xx, y + 2, hint, -1, PALE);
		}
	else textfit(&f_normal, xx, y + 2, hint, a->w - xx - 8, PALE);
	}
	}

static void desk_open_sel(void)
	{
	if (dsel < 0) { infoprint("No program selected"); return; }
	desk_run(dsel);
	}

static int desk_key(App *a, KeySym ks, unsigned st, const char *s, int n)
	{
	(void)st; (void)s; (void)n;
	switch (ks)
		{
	case XK_Left: dsel = dsel <= 0 ? NPROG - 1 : dsel - 1; break;
	case XK_Right: dsel = (dsel + 1) % NPROG; break;
	case XK_Up: dsel = dsel < COLS ? (dsel < 0 ? 0 : dsel + COLS) % NPROG : dsel - COLS; break;
	case XK_Down: dsel = dsel < 0 ? 0 : (dsel + COLS) % NPROG; break;
	case XK_Return: case XK_KP_Enter: desk_open_sel(); return 1;
	default:
		/* a letter: the program it starts */
		if (n == 1)
			{
			int i, c = s[0] | 32;
			for (i = 1; i <= NPROG; i++)
				{
				int k = (dsel + i + NPROG) % NPROG;
				if ((progs[k].label[0] | 32) == c) { dsel = k; break; }
				}
			}
		else return 0;
		}
	app_dirty(a);
	return 1;
	}

static void desk_pen(App *a, int type, int x, int y)
	{
	int i, cx, cy, w;
	if (type != PEN_DOWN) return;
	for (i = 0; i < NPROG; i++)
		{
		cell(i, &cx, &cy, &w);
		if (x >= cx && x < cx + w && y >= cy && y < cy + CELLH - 8)
			{
			/* one tap selects, a second opens */
			if (dsel == i) desk_run(i);
			else { dsel = i; app_dirty(a); }
			return;
			}
		}
	if (y < TH) { menu_open(a, 0); return; }
	if (dsel >= 0) { dsel = -1; app_dirty(a); }
	}

static void desk_cmd(App *a, int cmd)
	{
	(void)a;
	if (cmd == D_OPEN) desk_open_sel();
	}

static void desk_tick(App *a)
	{
	minute_tick(a);
	app_dirty(a);
	toolbar_draw();
	}

const AppClass desk_class = { "PsiLinux", I_P_PENGUIN, 0, 0, dmenus, 4, dtb, 4,
	desk_init, desk_draw, desk_key, desk_pen, desk_cmd, 0, desk_tick, 0 };

/* ---- the clock face (the toolbar's, and Clock's) ---- */

void clock_face(int cx, int cy, int r, int secs, int small)
	{
	time_t t = time(0);
	struct tm *tm = localtime(&t);
	int i;
	double a;
	disc(cx, cy, r, BLACK);
	if (small) disc(cx, cy, r - 2, DARK);
	disc(cx, cy, r - (small ? 3 : 3), WHITE);
	for (i = 0; i < 60; i++)
		{
		double s = sin(i * M_PI / 30), c = cos(i * M_PI / 30);
		int big = i % 5 == 0, q = i % 15 == 0;
		double r1;
		if (!big && small) continue;
		r1 = r - (small ? 3 : 5) - (q ? (small ? 6 : 9) : big ? (small ? 4 : 6) : 2);
		line(cx + (int)lround(r1 * s), cy - (int)lround(r1 * c),
			cx + (int)lround((r - (small ? 4 : 6)) * s), cy - (int)lround((r - (small ? 4 : 6)) * c),
			BLACK, small ? (q ? 2 : 1) : (q ? 3 : big ? 2 : 1));
		}
	if (!small)
		{
		textc(&f_small, cx - 30, cy + r / 3, 60, "PsiLinux", DARK);
		}
	a = (tm->tm_hour % 12 + tm->tm_min / 60.0) * M_PI / 6;
	line(cx, cy, cx + (int)lround(r * 0.5 * sin(a)), cy - (int)lround(r * 0.5 * cos(a)), BLACK, small ? 3 : 5);
	a = (tm->tm_min + (secs ? tm->tm_sec / 60.0 : 0)) * M_PI / 30;
	line(cx, cy, cx + (int)lround(r * 0.78 * sin(a)), cy - (int)lround(r * 0.78 * cos(a)), BLACK, small ? 2 : 3);
	if (secs)
		{
		a = tm->tm_sec * M_PI / 30;
		line(cx - (int)lround(r * 0.15 * sin(a)), cy + (int)lround(r * 0.15 * cos(a)),
			cx + (int)lround(r * 0.85 * sin(a)), cy - (int)lround(r * 0.85 * cos(a)), DARK, 1);
		}
	disc(cx, cy, small ? 2 : 4, BLACK);
	}

/* ---- the toolbar ---- */

static int tbpress = -1;
static const TbBut ftb[] = {
	{ I_T_PROGS, "Programs", C_PROGRAMS }, { I_T_SWITCH, "Switch", C_SWITCH }, { I_T_CLOSE, "Close", C_CLOSE } };
#define TBY 28
#define TBH 36

static App *tb_app(void) { return cur ? cur->app : desk; }

static void tb_buttons(const TbBut **b, int *n, const char **name)
	{
	App *a = tb_app();
	if (a && a->k == &dialog_class)
		{
		/* (a dialog: its program's toolbar) */
		Client *c;
		a = 0;
		for (c = clients; c; c = c->next) if (c != cur) a = c->app ? c->app : 0;
		if (!a) a = desk;
		}
	if (a) { *b = a->k->tb; *n = a->k->ntb; *name = a->k->name; }
	else { *b = ftb; *n = 3; *name = cur ? cur->name : "PsiLinux"; }
	}

void toolbar_draw(void)
	{
	const TbBut *b; int n, i;
	const char *name;
	const BFont *tf = &f_title;
	if (!tbwin)
		{
		XSetWindowAttributes sa;
		sa.override_redirect = True;
		sa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask;
		sa.background_pixmap = None;
		tbwin = XCreateWindow(dpy, root, SW - TBW, 0, TBW, SH, 0, CopyFromParent, InputOutput, CopyFromParent,
			CWOverrideRedirect | CWEventMask | CWBackPixmap, &sa);
		XMapRaised(dpy, tbwin);
		}
	if (!toolbar_on) return;
	tb_buttons(&b, &n, &name);
	D = backbuf;
	fill(0, 0, TBW, SH, LIGHT);
	fill(0, 0, 1, SH, BLACK);
	/* the name */
	box(2, 2, TBW - 3, 24, BLACK);
	fill(3, 3, TBW - 5, 22, WHITE);
	if (tw(tf, name, -1) > TBW - 8) tf = &f_bold;
	if (tw(tf, name, -1) > TBW - 8) tf = &f_dense;
	textc(tf, 2, 3 + (22 - tf->h) / 2 + 1, TBW - 3, name, BLACK);
	for (i = 0; i < 4; i++)
		{
		int y = TBY + i * TBH, down = i == tbpress;
		const char *l, *sp;
		if (i >= n) { continue; }
		bevel(2, y, TBW - 3, TBH - 1, down);
		pic(b[i].pic, 4 + down, y + 7 + down);
		l = b[i].label;
		sp = strchr(l, ' ');
		{
		/* the words to the right of the picture; two words on two lines */
		int lx = 4 + pic_right(b[i].pic) + 3 + down;
		if (sp)
			{
			text(&f_dense, lx, y + 4 + down, l, sp - l, BLACK);
			text(&f_dense, lx, y + 18 + down, sp + 1, -1, BLACK);
			}
		else text(&f_dense, lx, y + (TBH - 1 - f_dense.h) / 2 + down, l, -1, BLACK);
		}
		}
	/* the clock */
	{
	int cy = TBY + 4 * TBH + (SH - TBY - 4 * TBH) / 2;
	clock_face(TBW / 2 + 1, cy, 31, 0, 1);
	}
	show(tbwin, 0, 0, TBW, SH);
	}

static int tb_hit(int x, int y)
	{
	const TbBut *b; int n; const char *name;
	tb_buttons(&b, &n, &name);
	if (x < 2 || x >= TBW - 1) return -2;
	if (y < 27) return -1;
	if (y >= TBY && y < TBY + 4 * TBH && (y - TBY) / TBH < n) return (y - TBY) / TBH;
	if (y >= TBY + 4 * TBH) return 10;
	return -2;
	}

/* the list of open programs (from the toolbar's name) */
static MenuItem tl_items[17];
static Menu tl_menu = { "Programs", tl_items, 0 };

static void task_list(void)
	{
	Client *c;
	int n = 0;
	static char labels[16][48];
	tl_items[n].label = "PsiLinux"; tl_items[n].key = 0; tl_items[n].cmd = C_TASK; tl_items[n].flags = MI_CHECK;
	n++;
	for (c = clients; c && n < 16; c = c->next)
		{
		if (c->app && c->app->k == &dialog_class) continue;
		if (c->app || !c->title[0] || !strcasecmp(c->title, c->name)) snprintf(labels[n], sizeof labels[n], "%s", c->name);
		else snprintf(labels[n], sizeof labels[n], "%s - %.30s", c->name, c->title);
		tl_items[n].label = labels[n]; tl_items[n].key = 0; tl_items[n].cmd = C_TASK + n; tl_items[n].flags = MI_CHECK;
		n++;
		}
	tl_items[0].flags |= n > 1 ? MI_SEP : 0;
	tl_menu.n = n;
	popup_open(cur ? cur->app : desk, &tl_menu, SW - TBW, 2);
	}

int toolbar_event(XEvent *e)
	{
	int i;
	if (e->type == ButtonPress)
		{
		i = tb_hit(e->xbutton.x, e->xbutton.y);
		if (i == -1) { task_list(); return 1; }
		if (i == 10) { app_open(&clock_class, 0); return 1; }
		if (i >= 0) { tbpress = i; toolbar_draw(); }
		return 1;
		}
	if (e->type == MotionNotify && tbpress >= 0)
		{
		if (tb_hit(e->xmotion.x, e->xmotion.y) != tbpress) { tbpress = -1; toolbar_draw(); }
		return 1;
		}
	if (e->type == ButtonRelease && tbpress >= 0)
		{
		const TbBut *b; int n; const char *name;
		App *a = tb_app();
		i = tbpress;
		tbpress = -1;
		toolbar_draw();
		tb_buttons(&b, &n, &name);
		if (a && a->k == &dialog_class) return 1;
		if (i < n) do_cmd(cur && !cur->app ? 0 : a, b[i].cmd);
		return 1;
		}
	return 0;
	}

/* ---- small things the programs share ---- */

int readfile(const char *path, char *buf, int max)
	{
	FILE *f = fopen(path, "r");
	int n;
	if (!f) return -1;
	n = fread(buf, 1, max - 1, f);
	fclose(f);
	buf[n < 0 ? 0 : n] = 0;
	return n;
	}

void fmt_kb(char *b, long kb)
	{
	if (kb >= 1000) sprintf(b, "%ld,%03ld KB", kb / 1000, kb % 1000);
	else sprintf(b, "%ld KB", kb);
	}
