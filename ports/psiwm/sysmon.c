/* sysmon.c - System: the memory (the Psion's and Linux's) and the running
 * programs, with a way to end one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include "psiwm.h"

enum { S_END = 1, S_REFRESH };
static const MenuItem sf[] = { { "End program", "Ctrl+D", S_END, MI_SEP }, { "Close", "Ctrl+E", C_CLOSE, 0 } };
static const MenuItem sv[] = { { "Refresh", "Ctrl+R", S_REFRESH, MI_SEP }, { "Show toolbar", "Ctrl+T", C_TOOLBAR, MI_CHECK } };
static const MenuItem st_[] = { { "Help", "Shift+Ctrl+H", C_HELP, 0 },
	{ "About PsiLinux…", "Shift+Ctrl+A", C_ABOUT, 0 } };
static const Menu smenus[] = { { "File", sf, 2 }, { "View", sv, 2 }, { "Tools", st_, 2 } };
static const TbBut stb[] = { { I_T_STOP, "End program", S_END }, { I_T_REFRESH, "Refresh", S_REFRESH } };

typedef struct { int pid; char comm[24]; char state; long kb; } Proc;
typedef struct
	{
	Proc p[64]; int n, sel, top;
	long total, freek, avail, cached, reserve, psifree;
	long uptime;
	} Sys;

#define LW 236                  /* the memory panel */
#define RH 17

static long field(const char *b, const char *name)
	{
	const char *p = strstr(b, name);
	if (!p) return -1;
	p += strlen(name);
	while (*p == ':' || *p == ' ' || *p == '\t') p++;
	return atol(p);
	}

static int pcmp(const void *a, const void *b) { return ((const Proc *)a)->pid - ((const Proc *)b)->pid; }

static void refresh(App *a)
	{
	Sys *s = a->p;
	char b[2048], path[300];
	DIR *d;
	struct dirent *de;
	int keep = s->n ? s->p[s->sel].pid : -1, i;
	snprintf(path, sizeof path, "%s/meminfo", procroot);
	if (readfile(path, b, sizeof b) > 0)
		{
		s->total = field(b, "MemTotal"); s->freek = field(b, "MemFree");
		s->avail = field(b, "MemAvailable"); s->cached = field(b, "Cached");
		}
	snprintf(path, sizeof path, "%s/psilinux/mem", procroot);
	s->reserve = s->psifree = -1;
	if (readfile(path, b, sizeof b) > 0)
		{
		s->psifree = field(b, "free");
		s->reserve = field(b, "reserve");
		}
	snprintf(path, sizeof path, "%s/uptime", procroot);
	s->uptime = readfile(path, b, sizeof b) > 0 ? atol(b) : -1;
	s->n = 0;
	if ((d = opendir(procroot)))
		{
		while ((de = readdir(d)) && s->n < 64)
			{
			Proc *p = &s->p[s->n];
			char *o, *c;
			long pages = 0;
			if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
			snprintf(path, sizeof path, "%s/%s/stat", procroot, de->d_name);
			if (readfile(path, b, sizeof b) <= 0) continue;
			o = strchr(b, '('); c = strrchr(b, ')');
			if (!o || !c) continue;
			p->pid = atoi(b);
			snprintf(p->comm, sizeof p->comm, "%.*s", (int)(c - o - 1) < 23 ? (int)(c - o - 1) : 23, o + 1);
			p->state = c[2];
			{
			/* field 24 is rss in pages */
			char *q = c + 2;
			int f;
			for (f = 3; f < 24 && q; f++) { q = strchr(q, ' '); if (q) q++; }
			if (q) pages = atol(q);
			}
			p->kb = pages * 4;
			s->n++;
			}
		closedir(d);
		}
	qsort(s->p, s->n, sizeof s->p[0], pcmp);
	s->sel = 0;
	for (i = 0; i < s->n; i++) if (s->p[i].pid == keep) s->sel = i;
	a->tick_at = now_ms() + 3000;
	app_dirty(a);
	}

static void s_init(App *a, const char *arg)
	{
	(void)arg;
	a->p = calloc(1, sizeof(Sys));
	refresh(a);
	}

static void bar(int x, int y, int w, int h, long used, long cached, long total)
	{
	int u = total > 0 ? (int)(w * used / total) : 0, c = total > 0 ? (int)(w * cached / total) : 0;
	box(x, y, w, h, BLACK);
	fill(x + 1, y + 1, w - 2, h - 2, WHITE);
	if (u > 2) fill(x + 1, y + 1, u - 1, h - 2, DARK);
	if (c > 0) fill2(x + u, y + 1, c, h - 2, MID, WHITE);
	}

static void legend(int x, int y, int kind, const char *label, long kb)
	{
	char v[32];
	box(x, y + 3, 10, 10, BLACK);
	if (kind == 0) fill(x + 1, y + 4, 8, 8, DARK);
	else if (kind == 1) fill2(x + 1, y + 4, 8, 8, MID, WHITE);
	else fill(x + 1, y + 4, 8, 8, WHITE);
	text(&f_normal, x + 16, y + 1, label, -1, BLACK);
	if (kb >= 0) { fmt_kb(v, kb); text(&f_bold, x + LW - 26 - tw(&f_bold, v, -1), y + 1, v, -1, BLACK); }
	}

static void s_draw(App *a)
	{
	Sys *s = a->p;
	int x = LW + 1, y, i, nr;
	char b[64];
	long used = s->total >= 0 && s->freek >= 0 ? s->total - s->freek - (s->cached > 0 ? s->cached : 0) : 0;
	fill(0, 0, a->w, a->h, WHITE);
	/* memory */
	fill(0, 0, LW, a->h, LIGHT);
	fill(LW, 0, 1, a->h, BLACK);
	text(&f_title, 10, 6, "Memory", -1, BLACK);
	if (s->total > 0) { fmt_kb(b, s->total); text(&f_normal, LW - 12 - tw(&f_normal, b, -1), 8, b, -1, DARK); }
	bar(10, 28, LW - 20, 18, used, s->cached > 0 ? s->cached : 0, s->total);
	legend(10, 52, 0, "In use", used);
	legend(10, 70, 1, "Kept for next time", s->cached);
	legend(10, 88, 2, "Free", s->freek);
	fill(10, 110, LW - 20, 1, MID);
	text(&f_bold, 10, 116, "For Linux programs", -1, BLACK);
	fmt_kb(b, s->avail > 0 ? s->avail : 0);
	text(&f_bold, LW - 16 - tw(&f_bold, b, -1), 116, b, -1, BLACK);
	if (s->reserve >= 0)
		{
		text(&f_normal, 10, 134, "Kept for EPOC programs", -1, BLACK);
		fmt_kb(b, s->reserve);
		text(&f_normal, LW - 16 - tw(&f_normal, b, -1), 134, b, -1, BLACK);
		}
	if (s->psifree >= 0)
		{
		text(&f_normal, 10, 152, "The Psion's free memory", -1, BLACK);
		fmt_kb(b, s->psifree);
		text(&f_normal, LW - 16 - tw(&f_normal, b, -1), 152, b, -1, BLACK);
		}
	if (s->uptime >= 0)
		{
		snprintf(b, sizeof b, "Linux has run for %ld:%02ld", s->uptime / 3600, s->uptime / 60 % 60);
		text(&f_small, 10, a->h - 18, b, -1, DARK);
		}
	/* the programs */
	fill(x, 0, a->w - x, RH, PALE);
	fill(x, RH - 1, a->w - x, 1, MID);
	text(&f_bold, x + 26, 1, "Program", -1, BLACK);
	text(&f_bold, x + 150, 1, "Number", -1, BLACK);
	text(&f_bold, a->w - 10 - tw(&f_bold, "Memory", -1), 1, "Memory", -1, BLACK);
	nr = (a->h - RH) / RH;
	if (s->sel < s->top) s->top = s->sel;
	if (s->sel >= s->top + nr) s->top = s->sel - nr + 1;
	for (i = s->top, y = RH; i < s->n && i < s->top + nr; i++, y += RH)
		{
		Proc *p = &s->p[i];
		int g = BLACK;
		if (i == s->sel) { fill(x, y, a->w - x, RH, BLACK); g = WHITE; }
		{
		/* a picture for what it is: a terminal or shell, X, or a program */
		const char *c = p->comm;
		int ic = p->pid == 1 || !strcmp(c, "init") ? I_S_GEAR
			: strstr(c, "sh") || strstr(c, "term") || !strcmp(c, "login") ? I_S_TERM
			: c[0] == 'X' || c[0] == 'x' || !strcmp(c, "psiwm") || !strcmp(c, "links") ? I_S_PROG : I_S_GEAR;
		if (i == s->sel) fill(x + 4, y + 1, 18, RH - 2, WHITE);
		pic(ic, x + 5, y + 1);
		}
		textfit(&f_normal, x + 26, y + 1, p->comm, 118, g);
		snprintf(b, sizeof b, "%d", p->pid);
		text(&f_normal, x + 150, y + 1, b, -1, g);
		if (p->kb > 0) { fmt_kb(b, p->kb); text(&f_normal, a->w - 10 - tw(&f_normal, b, -1), y + 1, b, -1, g); }
		}
	}

static void ended(App *a, int ok, char *const *ed)
	{
	Sys *s = a->p;
	(void)ed;
	if (!ok || !s->n) return;
	if (kill(s->p[s->sel].pid, SIGTERM)) infoprint("Could not end it");
	else infoprint("Ended");
	refresh(a);
	}

static void s_cmd(App *a, int cmd)
	{
	Sys *s = a->p;
	if (cmd == S_REFRESH) { refresh(a); infoprint("Refreshed"); return; }
	if (cmd == S_END)
		{
		static char q[64];
		static const char *const l[] = { "Any changes not saved in it will be lost", 0 };
		if (!s->n) { infoprint("No program selected"); return; }
		if (s->p[s->sel].pid == 1) { infoprint("Not available - Linux's first program"); return; }
		if (s->p[s->sel].pid == getpid()) { infoprint("Not available - the desktop itself"); return; }
		snprintf(q, sizeof q, "End \"%s\"?", s->p[s->sel].comm);
		dialog(a, q, l, 0, DB_YESNO, ended);
		}
	}

static int s_key(App *a, KeySym ks, unsigned st, const char *str, int n)
	{
	Sys *s = a->p;
	(void)st; (void)str; (void)n;
	switch (ks)
		{
	case XK_Up: if (s->sel > 0) s->sel--; break;
	case XK_Down: if (s->sel < s->n - 1) s->sel++; break;
	case XK_Home: s->sel = 0; break;
	case XK_End: s->sel = s->n - 1; break;
	case XK_Delete: s_cmd(a, S_END); return 1;
	default: return 0;
		}
	app_dirty(a);
	return 1;
	}

static void s_pen(App *a, int type, int x, int y)
	{
	Sys *s = a->p;
	int i;
	if (type != PEN_DOWN || x <= LW || y < RH) return;
	i = s->top + (y - RH) / RH;
	if (i < s->n) { s->sel = i; app_dirty(a); }
	}

static void s_tick(App *a)
	{
	/* (only while it is the program in front) */
	if (a->cl != cur) { a->tick_at = now_ms() + 3000; return; }
	refresh(a);
	}
static void s_fini(App *a) { free(a->p); }

const AppClass sysmon_class = { "System", I_P_SYSTEM, 0, 0, smenus, 3, stb, 2,
	s_init, s_draw, s_key, s_pen, s_cmd, 0, s_tick, s_fini };
