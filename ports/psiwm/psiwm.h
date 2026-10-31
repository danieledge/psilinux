/* psiwm.h - PsiLinux's window manager and desktop, for the Psion Series 5mx.
 * One program: the window manager (wm.c), the EIKON-style drawing and
 * controls (ui.c: menus, dialogs, messages), the desktop and toolbar
 * (desk.c) and the built-in programs (files.c, notes.c, calc.c, clock.c,
 * sysmon.c). One copy of Xlib for all of them. */
#ifndef PSIWM_H
#define PSIWM_H
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <sys/types.h>
#include "font.h"
#include "icons.h"

#define PSIWM_VERSION "0.10"
#define SW 640                  /* the screen */
#define SH 240
#define TBW 76                  /* the toolbar, on the right */
#define TH 18                   /* a window's title band */
#define FB 3                    /* a floating window's border */
#define SHD 3                   /* and its shadow */

/* greys: the 16 the Psion shows, by level (grey = 17 * level) */
enum { BLACK = 0, DARK = 5, GREY = 8, MID = 10, LIGHT = 12, PALE = 13, PALER = 14, WHITE = 15 };

extern Display *dpy;
extern Window root;
extern unsigned long pix[16];
extern Drawable D;              /* where drawing goes: the back buffer */
extern Pixmap backbuf;
extern int ww;                  /* the workspace's width (SW less the toolbar) */
extern Atom A_PROTOCOLS, A_DELETE, A_CLIPBOARD, A_TARGETS, A_UTF8;
extern int toolbar_on;
extern const char *procroot;    /* "/proc" (tests: another) */

/* ---- drawing (ui.c) ---- */
void ui_init(void);
void fill(int x, int y, int w, int h, int g);
void fill2(int x, int y, int w, int h, int g1, int g2);    /* a 1-pixel check of two greys */
void box(int x, int y, int w, int h, int g);
void line(int x1, int y1, int x2, int y2, int g, int width);
void disc(int x, int y, int r, int g);
int tw(const BFont *f, const char *s, int n);
int text(const BFont *f, int x, int y, const char *s, int n, int g);
int textfit(const BFont *f, int x, int y, const char *s, int maxw, int g);   /* ends in "…" if too wide */
void textc(const BFont *f, int x, int y, int w, const char *s, int g);       /* centred in w */
void pic(int id, int x, int y);
int pic_right(int id);                                     /* the last column it draws in */
void bevel(int x, int y, int w, int h, int down);         /* an EIKON button's face */
void show(Window w, int x, int y, int cw, int ch);        /* the back buffer to a window */
long now_ms(void);

/* ---- menus ---- */
enum { MI_SEP = 1, MI_CHECK = 2 };          /* a line after it; a tick box */
enum { MS_DIM = 1, MS_TICK = 2 };           /* state() answers */
typedef struct { const char *label, *key; int cmd, flags; } MenuItem;
typedef struct { const char *title; const MenuItem *items; int n; } Menu;
typedef struct { int pic; const char *label; int cmd; } TbBut;

/* ---- programs ---- */
typedef struct App App;
typedef struct Client Client;
typedef struct AppClass
	{
	const char *name;
	int icon;
	int w, h;                       /* 0: the whole workspace */
	const Menu *menus; int nmenus;
	const TbBut *tb; int ntb;
	void (*init)(App *, const char *arg);
	void (*draw)(App *);
	int (*key)(App *, KeySym ks, unsigned st, const char *s, int n);   /* 1: used */
	void (*pen)(App *, int type, int x, int y);   /* PEN_DOWN, PEN_MOVE, PEN_UP */
	void (*cmd)(App *, int cmd);
	int (*state)(App *, int cmd);
	void (*tick)(App *);
	void (*fini)(App *);
	} AppClass;
enum { PEN_DOWN, PEN_MOVE, PEN_UP };

struct App
	{
	const AppClass *k;
	Window win;
	int w, h;
	Client *cl;
	int dirty;
	long tick_at;                   /* now_ms() of the next tick, 0: none */
	char title[96];                 /* the title band's words, e.g. the file */
	void *p;                        /* the program's own */
	App *next;
	};

struct Client
	{
	Client *next;
	Window win, frame;
	App *app;                       /* a built-in program, or 0 */
	char name[32], title[96];
	int x, y, w, h;                 /* the frame */
	int cw, ch;                     /* the window in it */
	int floating, ignore_unmap, delete_ok, mapped;
	};

/* the commands every program has (the window manager's) */
enum
	{
	C_CLOSE = 900, C_SWITCH, C_PROGRAMS, C_TOOLBAR, C_ABOUT, C_FULL, C_TASKLIST, C_HELP,
	C_RUN = 940,                    /* + a program's number (desk.c) */
	C_TASK = 960                    /* + an open program's number */
	};

/* wm.c */
extern Client *clients, *cur;
extern App *apps, *modal;
App *app_open(const AppClass *k, const char *arg);
void app_close(App *a);
void app_dirty(App *a);
App *app_find(const AppClass *k);
void app_retitle(App *a);
void focus(Client *c);              /* 0: the desktop */
void raise_desktop(void);
void wm_cmd(App *a, int cmd);
int wm_state(App *a, int cmd);
void layout_all(void);
void spawn(const char *const *argv, const char *label);
void close_client(Client *c);
int nclients(void);
Client *client_of_app(App *a);
void x_flush_dirty(void);

/* ui.c: menus, dialogs, messages */
void menu_open(App *a, int which);     /* the focused program's menu bar */
void popup_open(App *a, const Menu *m, int x, int y);
int menu_event(XEvent *e);             /* 1: the menu used it */
int menu_shortcut(App *a, const Menu *menus, int n, KeySym ks, unsigned st);
extern int menu_up;
void infoprint(const char *fmt, ...);
void busy(const char *s);
void msg_tick(void);
long msg_next(void);
void msgs_raise(void);

enum { DB_OKCANCEL, DB_YESNO, DB_CONTINUE };
enum { DR_CANCEL = 0, DR_OK = 1 };
typedef void (*DlgDone)(App *owner, int ok, char *const *edits);
void dialog(App *owner, const char *title, const char *const *lines, const char *const *edits, int buttons, DlgDone done);
void about_dialog(App *owner);
extern const AppClass dialog_class;

/* the line editor (dialogs, Calc) */
typedef struct { char s[256]; int n, cur, all; } Line;
int line_key(Line *l, KeySym ks, unsigned st, const char *s, int n);
void line_draw(Line *l, const BFont *f, int x, int y, int w, int focused);

/* the clipboard (X's CLIPBOARD) */
void clip_set(const char *s, int n);
typedef void (*ClipDone)(App *a, const char *s, int n);
void clip_get(App *a, ClipDone done);  /* now if it is ours, else when the owner answers */
int clip_event(XEvent *e);

/* desk.c */
extern const AppClass desk_class;
extern App *desk;
extern Window tbwin;
void toolbar_draw(void);
int toolbar_event(XEvent *e);
void clock_face(int cx, int cy, int r, int secs, int small);
void desk_run(int i);

/* the programs */
extern const AppClass files_class, notes_class, calc_class, clock_class, sysmon_class, help_class;
int client_icon(Client *c);           /* its program icon (40x40, or the penguin) */
void switcher_open(int back);
int switcher_event(XEvent *e);
extern int switcher_up;
void notes_open_file(const char *path);

/* utilities */
int readfile(const char *path, char *buf, int max);
void fmt_kb(char *b, long kb);
#endif
