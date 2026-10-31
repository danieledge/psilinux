/* help.c - Help (Shift+Ctrl+H): the topics on the left, the words on the
 * right, as the Psion's own Help shows them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psiwm.h"

static const MenuItem hf[] = { { "Close", "Ctrl+E", C_CLOSE, 0 } };
static const MenuItem hv[] = { { "Show toolbar", "Ctrl+T", C_TOOLBAR, MI_CHECK } };
static const MenuItem ht[] = { { "About PsiLinux…", "Shift+Ctrl+A", C_ABOUT, 0 } };
static const Menu hmenus[] = { { "File", hf, 1 }, { "View", hv, 1 }, { "Tools", ht, 1 } };
static const TbBut htb[] = { { I_T_PROGS, "Programs", C_PROGRAMS }, { I_T_SWITCH, "Switch", C_SWITCH } };

/* a topic: its name, then paragraphs; a paragraph starting "*" is a heading */
static const char *const topics[][12] = {
	{ "The desktop",
	  "*Starting programs",
	  "The PsiLinux screen shows the programs. Tap an icon to select it and tap it again to open it, or move with the arrow keys and press Enter. Typing a program's first letter selects it.",
	  "*The toolbar",
	  "The toolbar on the right shows the program in front, its four commands and the time. Tap the program's name for the list of open programs, and the clock for Clock. Ctrl+T shows or hides the toolbar.",
	  0 },
	{ "Switching",
	  "*Switching programs",
	  "Hold Ctrl and press Tab: the open programs are shown in a row. Press Tab again to move along, and let go of Ctrl to switch. Shift+Ctrl+Tab goes the other way.",
	  "The Switch button on the toolbar shows the same row: tap the program you want.",
	  0 },
	{ "Menus",
	  "*Menus and shortcut keys",
	  "Press the Menu key, or tap a window's title, for its menus. Ctrl+Space opens them too. The arrow keys move, Enter chooses and Esc closes the menus.",
	  "Commands that open a dialog end in \"…\". Shortcut keys are shown beside the commands, and work without opening the menus.",
	  0 },
	{ "Files",
	  "*Files",
	  "Files shows the disks: the Internal disk (C), the Memory disk (D) and Linux's own files (/). Tap a folder twice to open it; Backspace goes up a folder.",
	  "Ctrl+C copies a file or folder and Ctrl+V pastes it into the folder shown; Ctrl+X moves it. Ctrl+D deletes, Ctrl+R renames, Shift+Ctrl+N creates a new folder. Text files open in Notes.",
	  0 },
	{ "Notes",
	  "*Notes",
	  "Notes edits plain text files. Shift and the arrow keys select text; Ctrl+X, Ctrl+C and Ctrl+V cut, copy and paste, also with the terminal.",
	  "Notes saves when you close it. New text is saved in the Documents folder. Ctrl+M zooms in, Shift+Ctrl+M zooms out.",
	  0 },
	{ "Other programs",
	  "*Calc, Clock & System",
	  "Calc works with the pen or the keys: Enter is =, Esc clears. Clock shows the time and date; tap the toolbar's clock to open it. System shows the memory and the running programs, and can end one (Ctrl+D).",
	  "*Terminal, Web & Doom",
	  "Terminal starts xterm and Web starts the Links browser. Doom needs the whole screen: the desktop closes while it runs and comes back after.",
	  0 },
};
#define NT (int)(sizeof topics / sizeof topics[0])
#define LW 150

typedef struct { int sel, top, total; } Help;

static void h_init(App *a, const char *arg) { (void)arg; a->p = calloc(1, sizeof(Help)); }

/* words wrapped to w; draws only when draw; returns the height */
static int para(const BFont *f, int x, int y, int w, const char *s, int draw, int clipy)
	{
	int h = 0;
	while (*s)
		{
		int n = 0, last = 0;
		while (s[n] && tw(f, s, n + 1) <= w) { n++; if (s[n] == ' ' || !s[n]) last = n; }
		if (s[n] && last) n = last;
		if (!n) n = 1;
		if (draw && y + h >= 0 && y + h < clipy) text(f, x, y + h, s, n, BLACK);
		h += f->h;
		s += n;
		while (*s == ' ') s++;
		}
	return h;
	}

static void h_draw(App *a)
	{
	Help *p = a->p;
	int i, y, x = LW + 14, w = a->w - LW - 28;
	fill(0, 0, a->w, a->h, WHITE);
	/* the topics */
	fill(0, 0, LW, a->h, PALE);
	fill(LW, 0, 1, a->h, BLACK);
	pic(I_P_HELP, (LW - 40) / 2, 6);
	for (i = 0; i < NT; i++)
		{
		int ty = 52 + i * 19;
		if (i == p->sel) { fill(4, ty, LW - 8, 18, BLACK); text(&f_bold, 10, ty + 1, topics[i][0], -1, WHITE); }
		else text(&f_bold, 10, ty + 1, topics[i][0], -1, BLACK);
		}
	/* the words */
	y = 8 - p->top;
	for (i = 1; topics[p->sel][i]; i++)
		{
		const char *s = topics[p->sel][i];
		if (*s == '*')
			{
			if (i > 1) y += 6;
			if (y >= -20 && y < a->h) text(&f_title, x, y, s + 1, -1, BLACK);
			y += f_title.h + 4;
			}
		else y += para(&f_normal, x, y, w, s, 1, a->h) + 6;
		}
	p->total = y + p->top;
	}

static int h_key(App *a, KeySym ks, unsigned st, const char *s, int n)
	{
	Help *p = a->p;
	(void)st; (void)s; (void)n;
	switch (ks)
		{
	case XK_Up: if (p->sel > 0) { p->sel--; p->top = 0; } break;
	case XK_Down: if (p->sel < NT - 1) { p->sel++; p->top = 0; } break;
	case XK_Page_Down: if (p->total > a->h) p->top = p->total - a->h + 8; break;
	case XK_Page_Up: p->top = 0; break;
	case XK_Escape: app_close(a); return 1;
	default: return 0;
		}
	app_dirty(a);
	return 1;
	}

static void h_pen(App *a, int type, int x, int y)
	{
	Help *p = a->p;
	int i = (y - 52) / 19;
	if (type != PEN_DOWN || x >= LW || y < 52 || i >= NT) return;
	p->sel = i; p->top = 0;
	app_dirty(a);
	}

static void h_fini(App *a) { free(a->p); }

const AppClass help_class = { "Help", I_P_HELP, 0, 0, hmenus, 3, htb, 2,
	h_init, h_draw, h_key, h_pen, 0, 0, 0, h_fini };
