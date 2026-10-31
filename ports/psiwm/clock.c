/* clock.c - Clock: the time, large, and the date. */
#include <stdio.h>
#include <time.h>
#include "psiwm.h"

enum { K_SECS = 1 };
static int secs = 1;

static const MenuItem cf[] = { { "Close", "Ctrl+E", C_CLOSE, 0 } };
static const MenuItem cv[] = {
	{ "Show seconds", 0, K_SECS, MI_CHECK | MI_SEP },
	{ "Show toolbar", "Ctrl+T", C_TOOLBAR, MI_CHECK } };
static const MenuItem ct[] = { { "Help", "Shift+Ctrl+H", C_HELP, 0 },
	{ "About PsiLinux…", "Shift+Ctrl+A", C_ABOUT, 0 } };
static const Menu cmenus[] = { { "File", cf, 1 }, { "View", cv, 2 }, { "Tools", ct, 2 } };
static const TbBut ctb[] = { { I_T_SECS, "Seconds", K_SECS }, { I_T_PROGS, "Programs", C_PROGRAMS } };

static void schedule(App *a)
	{
	time_t t = time(0);
	a->tick_at = now_ms() + (secs ? 1000 - (now_ms() % 1000) : (60 - localtime(&t)->tm_sec) * 1000L);
	}

static void c_init(App *a, const char *arg) { (void)arg; schedule(a); }

static void c_draw(App *a)
	{
	char b[64];
	time_t t = time(0);
	struct tm *tm = localtime(&t);
	int r = (a->h - 16) / 2, cx = 10 + r, cy = a->h / 2, x = cx + r + 18, w = a->w - x - 8, tw_;
	fill(0, 0, a->w, a->h, LIGHT);
	clock_face(cx, cy, r, secs, 0);
	/* the time, large, on a white panel */
	fill(x, 16, w, f_big.h + 12, WHITE);
	box(x, 16, w, f_big.h + 12, BLACK);
	fill(x + 1, 17, w - 2, 1, MID); fill(x + 1, 17, 1, f_big.h + 10, MID);
	strftime(b, sizeof b, "%H:%M", tm);
	tw_ = tw(&f_big, b, -1);
	text(&f_big, x + (w - tw_) / 2 - (secs ? 10 : 0), 22, b, -1, BLACK);
	if (secs)
		{
		strftime(b, sizeof b, "%S", tm);
		text(&f_bold, x + (w + tw_) / 2 - 6, 22 + f_big.h - f_bold.h - 4, b, -1, DARK);
		}
	strftime(b, sizeof b, "%A", tm);
	textc(&f_title, x, 30 + f_big.h, w, b, BLACK);
	strftime(b, sizeof b, "%e %B %Y", tm);
	textc(&f_normal, x, 30 + f_big.h + 20, w, b[0] == ' ' ? b + 1 : b, BLACK);
	{
	/* the day of the year and the week */
	strftime(b, sizeof b, "Day %j - week %V", tm);
	textc(&f_small, x, a->h - f_small.h - 10, w, b, DARK);
	}
	}

static void c_cmd(App *a, int cmd)
	{
	if (cmd == K_SECS)
		{
		secs = !secs;
		infoprint(secs ? "Seconds shown" : "Seconds not shown");
		schedule(a);
		app_dirty(a);
		}
	}

static int c_state(App *a, int cmd) { (void)a; return cmd == K_SECS && secs ? MS_TICK : 0; }

static void c_tick(App *a) { schedule(a); app_dirty(a); }

static int c_key(App *a, KeySym ks, unsigned st, const char *s, int n)
	{
	(void)st; (void)s; (void)n;
	if (ks == XK_Escape) { app_close(a); return 1; }
	return 0;
	}

const AppClass clock_class = { "Clock", I_P_CLOCK, 330, 176, cmenus, 3, ctb, 2,
	c_init, c_draw, c_key, 0, c_cmd, c_state, c_tick, 0 };
