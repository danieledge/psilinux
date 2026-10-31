/* wm.c - the window manager: frames, focus, the keys every program shares,
 * starting programs, and the main loop. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include "psiwm.h"
#include "splash.h"

Display *dpy;
Window root;
unsigned long pix[16];
int ww = SW - TBW, toolbar_on = 1;
Atom A_PROTOCOLS, A_DELETE, A_CLIPBOARD, A_TARGETS, A_UTF8;
static Atom A_NAME, A_NETNAME;
const char *procroot = "/proc";
Client *clients, *cur;
App *apps, *modal;
static int sigfd[2];
static long busy_until;

/* ---- the menus of a program that is not one of ours ---- */
static const MenuItem wf_items[] = {
	{ "Switch program", "Ctrl+Tab", C_SWITCH, MI_SEP },
	{ "Close", "Ctrl+E", C_CLOSE, 0 } };
static const MenuItem wv_items[] = {
	{ "Full screen", 0, C_FULL, MI_CHECK },
	{ "Show toolbar", "Ctrl+T", C_TOOLBAR, MI_CHECK } };
static const MenuItem wt_items[] = {
	{ "Programs", "Ctrl+Space", C_PROGRAMS, MI_SEP },
	{ "Help", "Shift+Ctrl+H", C_HELP, 0 },
	{ "About PsiLinux…", "Shift+Ctrl+A", C_ABOUT, 0 } };
const Menu wm_menus[] = { { "File", wf_items, 2 }, { "View", wv_items, 2 }, { "Tools", wt_items, 3 } };
const int wm_nmenus = 3;

static Client *find(Window w)
	{
	Client *c;
	for (c = clients; c; c = c->next)
		if (c->win == w || c->frame == w) return c;
	return 0;
	}

static App *app_of(Window w)
	{
	App *a;
	for (a = apps; a; a = a->next)
		if (a->win == w) return a;
	return 0;
	}

Client *client_of_app(App *a)
	{
	return a ? a->cl : 0;
	}

App *app_find(const AppClass *k)
	{
	App *a;
	for (a = apps; a; a = a->next)
		if (a->k == k) return a;
	return 0;
	}

int nclients(void)
	{
	int n = 0;
	Client *c;
	for (c = clients; c; c = c->next) n++;
	return n;
	}

void app_dirty(App *a) { if (a) a->dirty = 1; }

/* ---- frames ---- */

static void frame_draw(Client *c)
	{
	int active = c == cur, fw, fh;
	const char *t = c->title[0] ? c->title : c->name;
	D = backbuf;
	if (!c->floating)
		{
		fill2(0, 0, c->w, TH - 1, LIGHT, PALER);
		fill(0, TH - 1, c->w, 1, BLACK);
		textfit(&f_bold, 6, 2, t, c->w - 12, BLACK);
		fill(0, TH, c->w, c->h - TH, WHITE);
		show(c->frame, 0, 0, c->w, c->h);
		return;
		}
	fw = c->w - SHD; fh = c->h - SHD;
	fill(SHD, SHD, fw, fh, DARK);
	box(0, 0, fw, fh, BLACK);
	box(1, 1, fw - 2, fh - 2, active ? WHITE : PALE);
	box(2, 2, fw - 4, fh - 4, LIGHT);
	if (active) fill2(FB, FB, fw - 2 * FB, TH - 1, MID, PALE);
	else fill(FB, FB, fw - 2 * FB, TH - 1, LIGHT);
	fill(FB, FB + TH - 1, fw - 2 * FB, 1, DARK);
	fill(FB, FB + TH, fw - 2 * FB, fh - 2 * FB - TH, LIGHT);     /* (under a shaped window) */
	{
	int w = tw(&f_bold, t, -1);
	if (active)
		{
		/* (the words on a plain patch, so they read well on the pattern) */
		int pw = w + 10 < fw - 2 * FB - 4 ? w + 10 : fw - 2 * FB - 4;
		fill(FB + 4, FB + 1, pw, TH - 3, PALE);
		}
	textfit(&f_bold, FB + 9, FB + 1, t, fw - 2 * FB - 14, BLACK);
	}
	show(c->frame, 0, 0, c->w, c->h);
	}

static void send_configure(Client *c)
	{
	XConfigureEvent e;
	memset(&e, 0, sizeof e);
	e.type = ConfigureNotify;
	e.event = e.window = c->win;
	if (c->floating) { e.x = c->x + FB; e.y = c->y + FB + TH; }
	else { e.x = c->x; e.y = c->y + TH; }
	e.width = c->cw; e.height = c->ch;
	XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&e);
	}

static void place(Client *c)
	{
	if (!c->floating)
		{
		c->x = 0; c->y = 0; c->w = ww; c->h = SH;
		c->cw = ww; c->ch = SH - TH;
		XShapeCombineMask(dpy, c->frame, ShapeBounding, 0, 0, None, ShapeSet);
		XMoveResizeWindow(dpy, c->frame, 0, 0, ww, SH);
		XMoveResizeWindow(dpy, c->win, 0, TH, c->cw, c->ch);
		}
	else
		{
		XRectangle r[2];
		int fw = c->cw + 2 * FB, fh = c->ch + TH + 2 * FB;
		c->w = fw + SHD; c->h = fh + SHD;
		if (c->x + c->w > ww) c->x = ww - c->w;
		if (c->y + c->h > SH) c->y = SH - c->h;
		if (c->x < 0) c->x = 0;
		if (c->y < 0) c->y = 0;
		XMoveResizeWindow(dpy, c->frame, c->x, c->y, c->w, c->h);
		XMoveResizeWindow(dpy, c->win, FB, FB + TH, c->cw, c->ch);
		r[0].x = 0; r[0].y = 0; r[0].width = fw; r[0].height = fh;
		r[1].x = SHD; r[1].y = SHD; r[1].width = fw; r[1].height = fh;
		XShapeCombineRectangles(dpy, c->frame, ShapeBounding, 0, 0, r, 2, ShapeSet, Unsorted);
		}
	if (c->app) { c->app->w = c->cw; c->app->h = c->ch; app_dirty(c->app); }
	send_configure(c);
	}

static void centre(Client *c)
	{
	Client *o;
	int fw = c->cw + 2 * FB + SHD, fh = c->ch + TH + 2 * FB + SHD, k;
	c->x = (ww - fw) / 2; c->y = (SH - fh) / 2;
	/* (not exactly over another floating window) */
	for (k = 0; k < 6; k++)
		{
		for (o = clients; o; o = o->next)
			if (o != c && o->floating && o->x == c->x && o->y == c->y) break;
		if (!o) break;
		c->x += 16; c->y += 12;
		}
	}

static void set_names(Client *c)
	{
	XClassHint ch;
	char *n = 0;
	if (c->app) return;
	if (!c->name[0] && XGetClassHint(dpy, c->win, &ch))
		{
		snprintf(c->name, sizeof c->name, "%s", ch.res_class ? ch.res_class : ch.res_name ? ch.res_name : "");
		if (ch.res_name) XFree(ch.res_name);
		if (ch.res_class) XFree(ch.res_class);
		}
	{
	/* _NET_WM_NAME (UTF-8) first, then WM_NAME */
	Atom type; int fmt; unsigned long nitems, after; unsigned char *d = 0;
	if (XGetWindowProperty(dpy, c->win, A_NETNAME, 0, 64, False, A_UTF8, &type, &fmt, &nitems, &after, &d) == Success && d && nitems)
		{
		snprintf(c->title, sizeof c->title, "%s", (char *)d);
		XFree(d);
		}
	else
		{
		if (d) XFree(d);
		if (XFetchName(dpy, c->win, &n) && n) { snprintf(c->title, sizeof c->title, "%s", n); XFree(n); }
		}
	}
	if (!c->name[0]) snprintf(c->name, sizeof c->name, "%s", c->title[0] ? c->title : "Program");
	}

static Client *manage(Window w, App *a)
	{
	Client *c = calloc(1, sizeof *c), **pp;
	XWindowAttributes wa;
	XSetWindowAttributes sa;
	Window tr = None;
	c->win = w;
	c->app = a;
	XGetWindowAttributes(dpy, w, &wa);
	if (a)
		{
		a->cl = c;
		snprintf(c->name, sizeof c->name, "%s", a->k->name);
		snprintf(c->title, sizeof c->title, "%s", a->title[0] ? a->title : a->k->name);
		c->cw = a->w; c->ch = a->h;
		c->floating = a->k->w != 0 || a->k == &dialog_class;
		}
	else
		{
		XSizeHints h; long sup;
		Atom *pr; int n, i;
		set_names(c);
		c->cw = wa.width; c->ch = wa.height;
		if (XGetWMNormalHints(dpy, w, &h, &sup) && (h.flags & PMaxSize) && h.max_width < ww - 40)
			{ c->cw = h.max_width; c->ch = h.max_height; }
		XGetTransientForHint(dpy, w, &tr);
		c->floating = tr != None || (c->cw <= ww * 7 / 10 && c->ch <= (SH - TH) * 8 / 10);
		if (XGetWMProtocols(dpy, w, &pr, &n))
			{
			for (i = 0; i < n; i++) if (pr[i] == A_DELETE) c->delete_ok = 1;
			XFree(pr);
			}
		XSelectInput(dpy, w, PropertyChangeMask | StructureNotifyMask);
		/* a tap in it focuses it first (then goes on to it) */
		XGrabButton(dpy, AnyButton, AnyModifier, w, False, ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);
		}
	if (c->floating)
		{
		if (c->cw > ww - 2 * FB - SHD) c->cw = ww - 2 * FB - SHD;
		if (c->ch > SH - TH - 2 * FB - SHD) c->ch = SH - TH - 2 * FB - SHD;
		centre(c);
		}
	sa.override_redirect = True;
	sa.event_mask = SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask | ButtonReleaseMask
		| ButtonMotionMask | ExposureMask;
	sa.background_pixmap = None;
	c->frame = XCreateWindow(dpy, root, 0, 0, 10, 10, 0, CopyFromParent, InputOutput, CopyFromParent,
		CWOverrideRedirect | CWEventMask | CWBackPixmap, &sa);
	XSetWindowBorderWidth(dpy, w, 0);
	XAddToSaveSet(dpy, w);
	if (!a && wa.map_state == IsViewable) c->ignore_unmap++;
	XReparentWindow(dpy, w, c->frame, 0, TH);
	place(c);
	for (pp = &clients; *pp; pp = &(*pp)->next) ;
	*pp = c;
	XMapWindow(dpy, w);
	XMapWindow(dpy, c->frame);
	c->mapped = 1;
	{
	long data[2] = { NormalState, None };
	XChangeProperty(dpy, w, XInternAtom(dpy, "WM_STATE", False), XInternAtom(dpy, "WM_STATE", False), 32,
		PropModeReplace, (unsigned char *)data, 2);
	}
	if (busy_until) { busy(0); busy_until = 0; }
	focus(c);
	return c;
	}

static void unmanage(Client *c, int destroyed)
	{
	Client **pp;
	for (pp = &clients; *pp && *pp != c; pp = &(*pp)->next) ;
	if (*pp) *pp = c->next;
	if (!destroyed && !c->app)
		{
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		XReparentWindow(dpy, c->win, root, c->x, c->y);
		XRemoveFromSaveSet(dpy, c->win);
		}
	XDestroyWindow(dpy, c->frame);
	if (cur == c)
		{
		/* the one used most recently is at the list's end */
		Client *n = 0, *o;
		cur = 0;
		for (o = clients; o; o = o->next) n = o;
		focus(n);
		}
	free(c);
	toolbar_draw();
	}

/* ---- focus and stacking ---- */


void focus(Client *c)
	{
	Client *old = cur;
	if (modal && modal->cl && c != modal->cl) c = modal->cl;
	cur = c;
	if (c)
		{
		XRaiseWindow(dpy, c->frame);
		XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
		/* to the end of the list: the list is oldest first, Ctrl+Tab goes on
		 * from the end */
		{
		Client **pp;
		for (pp = &clients; *pp && *pp != c; pp = &(*pp)->next) ;
		if (*pp) { *pp = c->next; c->next = 0; for (pp = &clients; *pp; pp = &(*pp)->next) ; *pp = c; }
		}
		frame_draw(c);
		}
	else
		{
		XRaiseWindow(dpy, desk->win);
		XSetInputFocus(dpy, desk->win, RevertToPointerRoot, CurrentTime);
		app_dirty(desk);
		}
	if (old && old != c && find(old->win)) frame_draw(old);
	msgs_raise();
	toolbar_draw();
	}

void raise_desktop(void) { focus(0); }

/* ---- Ctrl+Tab: the open programs in a row, as the Extras bar shows
 * programs; Tab moves on, letting go of Ctrl (or Enter, or a tap) switches ---- */

int switcher_up;
static Window swwin;
static Client *swl[16];
static int swn, swsel, sww, swh;
#define SWC 58                  /* a cell */

static void switcher_draw(void)
	{
	int i, x0 = 12;
	Client *c = swl[swsel];
	const char *t;
	D = backbuf;
	fill(SHD, SHD, sww, swh, DARK);
	fill(0, 0, sww, swh, LIGHT);
	box(0, 0, sww, swh, BLACK);
	box(1, 1, sww - 2, swh - 2, WHITE);
	fill2(3, 3, sww - 6, TH - 1, MID, PALE);
	fill(3, 3 + TH - 1, sww - 6, 1, DARK);
	fill(7, 4, tw(&f_bold, "Switch to", -1) + 10, TH - 3, PALE);
	text(&f_bold, 12, 4, "Switch to", -1, BLACK);
	for (i = 0; i < swn; i++)
		{
		int x = x0 + i * SWC, y = TH + 10, ic = client_icon(swl[i]);
		if (i == swsel)
			{
			fill(x, y, SWC - 4, SWC - 4, WHITE);
			box(x, y, SWC - 4, SWC - 4, BLACK);
			box(x + 1, y + 1, SWC - 6, SWC - 6, BLACK);
			}
		if (ic == I_P_PENGUIN) pic(ic, x + (SWC - 4 - 48) / 2, y + 2);
		else pic(ic, x + (SWC - 4 - 40) / 2, y + (SWC - 4 - 40) / 2);
		}
	t = c ? (c->title[0] && strcasecmp(c->title, c->name) ? c->title : "") : "the desktop";
	textc(&f_bold, 0, TH + SWC + 12, sww, c ? c->name : "PsiLinux", BLACK);
	if (*t) { char b[80]; snprintf(b, sizeof b, "%s", t); textfit(&f_normal, 12, TH + SWC + 28, b, sww - 24, DARK);
		/* (centre it when it fits) */
		if (tw(&f_normal, b, -1) < sww - 24) { fill(12, TH + SWC + 28, sww - 24, f_normal.h, LIGHT); textc(&f_normal, 0, TH + SWC + 28, sww, b, DARK); } }
	show(swwin, 0, 0, sww + SHD, swh + SHD);
	}

void switcher_open(int back)
	{
	Client *c;
	int i;
	XRectangle r[2];
	if (modal || menu_up) return;
	/* most recent first: the list's end is the most recent */
	swn = 0;
	for (c = clients; c; c = c->next) if (!(c->app && c->app->k == &dialog_class)) swn++;
	if (!swn) { infoprint("No other programs open"); return; }
	if (swn > 8) swn = 8;
	{
	Client *all[64]; int n = 0;
	for (c = clients; c && n < 64; c = c->next) if (!(c->app && c->app->k == &dialog_class)) all[n++] = c;
	swn = 0;
	if (!cur) swl[swn++] = 0;
	for (i = n - 1; i >= 0 && swn < 9; i--) swl[swn++] = all[i];
	if (cur) swl[swn++] = 0;
	}
	swsel = back ? swn - 1 : 1;
	sww = swn * SWC + 20; if (sww < 220) sww = 220;
	swh = TH + SWC + 52;
	if (!swwin)
		{
		XSetWindowAttributes a;
		a.override_redirect = True;
		a.event_mask = ExposureMask | ButtonPressMask | KeyPressMask | KeyReleaseMask;
		swwin = XCreateWindow(dpy, root, 0, 0, 10, 10, 0, CopyFromParent, InputOutput, CopyFromParent,
			CWOverrideRedirect | CWEventMask, &a);
		}
	XMoveResizeWindow(dpy, swwin, (ww - sww - SHD) / 2, (SH - swh - SHD) / 2, sww + SHD, swh + SHD);
	r[0].x = 0; r[0].y = 0; r[0].width = sww; r[0].height = swh;
	r[1].x = SHD; r[1].y = SHD; r[1].width = sww; r[1].height = swh;
	XShapeCombineRectangles(dpy, swwin, ShapeBounding, 0, 0, r, 2, ShapeSet, Unsorted);
	XMapRaised(dpy, swwin);
	XGrabKeyboard(dpy, swwin, False, GrabModeAsync, GrabModeAsync, CurrentTime);
	XGrabPointer(dpy, swwin, False, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
	switcher_up = 1;
	switcher_draw();
	}

static void switcher_close(int go)
	{
	Client *c = swl[swsel];
	XUngrabKeyboard(dpy, CurrentTime);
	XUngrabPointer(dpy, CurrentTime);
	XUnmapWindow(dpy, swwin);
	switcher_up = 0;
	if (go) focus(c);
	}

int switcher_event(XEvent *e)
	{
	if (!switcher_up) return 0;
	switch (e->type)
		{
	case Expose: if (e->xexpose.window != swwin) return 0; switcher_draw(); return 1;
	case KeyRelease:
		{
		KeySym k = XLookupKeysym(&e->xkey, 0);
		if (k == XK_Control_L || k == XK_Control_R) switcher_close(1);
		return 1;
		}
	case KeyPress:
		{
		KeySym k = XLookupKeysym(&e->xkey, 0);
		switch (k)
			{
		case XK_Tab: swsel = (swsel + ((e->xkey.state & ShiftMask) ? swn - 1 : 1)) % swn; break;
		case XK_Right: swsel = (swsel + 1) % swn; break;
		case XK_Left: swsel = (swsel + swn - 1) % swn; break;
		case XK_Return: case XK_KP_Enter: case XK_space: switcher_close(1); return 1;
		case XK_Escape: switcher_close(0); return 1;
		default: return 1;
			}
		switcher_draw();
		return 1;
		}
	case ButtonPress:
		{
		int x = e->xbutton.x - 12, y = e->xbutton.y - TH - 10;
		if (e->xbutton.window == swwin && x >= 0 && y >= 0 && y < SWC && x / SWC < swn)
			{ swsel = x / SWC; switcher_close(1); }
		else switcher_close(0);
		return 1;
		}
		}
	return 0;
	}

void close_client(Client *c)
	{
	if (!c) return;
	if (c->app) { app_close(c->app); return; }
	if (c->delete_ok)
		{
		XEvent e;
		memset(&e, 0, sizeof e);
		e.xclient.type = ClientMessage;
		e.xclient.window = c->win;
		e.xclient.message_type = A_PROTOCOLS;
		e.xclient.format = 32;
		e.xclient.data.l[0] = A_DELETE;
		e.xclient.data.l[1] = CurrentTime;
		XSendEvent(dpy, c->win, False, NoEventMask, &e);
		}
	else XKillClient(dpy, c->win);
	}

void layout_all(void)
	{
	Client *c;
	ww = toolbar_on ? SW - TBW : SW;
	for (c = clients; c; c = c->next) place(c);
	XMoveResizeWindow(dpy, desk->win, 0, 0, ww, SH);
	desk->w = ww;
	app_dirty(desk);
	if (toolbar_on) { XMapRaised(dpy, tbwin); toolbar_draw(); }
	else XUnmapWindow(dpy, tbwin);
	for (c = clients; c; c = c->next) frame_draw(c);
	}

void wm_cmd(App *a, int cmd)
	{
	Client *c = a ? a->cl : cur;
	if (a == desk) c = 0;
	switch (cmd)
		{
	case C_CLOSE:
		if (!c) infoprint("Nothing to close");
		else close_client(c);
		break;
	case C_SWITCH: switcher_open(0); break;
	case C_PROGRAMS: focus(0); break;
	case C_TOOLBAR: toolbar_on = !toolbar_on; layout_all(); break;
	case C_ABOUT: about_dialog(a); break;
	case C_HELP: app_open(&help_class, 0); break;
	case C_FULL:
		if (c && !c->app)
			{
			c->floating = !c->floating;
			if (c->floating) { XWindowAttributes wa; XGetWindowAttributes(dpy, c->win, &wa);
				c->cw = c->cw > ww * 2 / 3 ? ww * 2 / 3 : c->cw; c->ch = c->ch > 150 ? 150 : c->ch; centre(c); }
			place(c);
			frame_draw(c);
			}
		break;
	default:
		if (cmd >= C_RUN && cmd < C_TASK) desk_run(cmd - C_RUN);
		else if (cmd >= C_TASK)
			{
			int i = cmd - C_TASK - 1;
			Client *o;
			if (i < 0) { focus(0); break; }
			for (o = clients; o && i > 0; o = o->next, i--) ;
			if (o) focus(o);
			}
		}
	}

int wm_state(App *a, int cmd)
	{
	Client *c = a ? a->cl : cur;
	if (a == desk) c = 0;
	switch (cmd)
		{
	case C_TOOLBAR: return toolbar_on ? MS_TICK : 0;
	case C_FULL: return !c || c->app ? MS_DIM : c->floating ? 0 : MS_TICK;
	case C_CLOSE: return c ? 0 : MS_DIM;
		}
	if (cmd >= C_TASK)
		{
		Client *o = clients;
		int i = cmd - C_TASK - 1;
		if (i < 0) return cur ? 0 : MS_TICK;
		for (; o && i > 0; o = o->next) if (!(o->app && o->app->k == &dialog_class)) i--;
		return o && o == cur ? MS_TICK : 0;
		}
	return 0;
	}

/* ---- our own programs' windows ---- */

App *app_open(const AppClass *k, const char *arg)
	{
	App *a, **pp;
	XSetWindowAttributes sa;
	if (k != &dialog_class && k != &desk_class && (a = app_find(k)))
		{
		focus(a->cl);
		return a;
		}
	a = calloc(1, sizeof *a);
	a->k = k;
	a->w = k->w ? k->w : ww;
	a->h = k->h ? k->h : k == &desk_class ? SH : SH - TH;
	if (k->init) k->init(a, arg);
	sa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | KeyPressMask;
	sa.background_pixmap = None;
	a->win = XCreateWindow(dpy, root, 0, 0, a->w, a->h, 0, CopyFromParent, InputOutput, CopyFromParent,
		CWEventMask | CWBackPixmap, &sa);
	XStoreName(dpy, a->win, k->name);
	for (pp = &apps; *pp; pp = &(*pp)->next) ;
	*pp = a;
	a->dirty = 1;
	if (k == &desk_class)
		{
		sa.override_redirect = True;
		XChangeWindowAttributes(dpy, a->win, CWOverrideRedirect, &sa);
		XMapWindow(dpy, a->win);
		return a;
		}
	if (k == &dialog_class) modal = a;
	manage(a->win, a);
	return a;
	}

void app_retitle(App *a)
	{
	if (!a->cl) return;
	snprintf(a->cl->title, sizeof a->cl->title, "%s", a->title[0] ? a->title : a->k->name);
	frame_draw(a->cl);
	}

void app_close(App *a)
	{
	App **pp;
	Client *c = a->cl;
	if (a->k->fini) a->k->fini(a);
	for (pp = &apps; *pp && *pp != a; pp = &(*pp)->next) ;
	if (*pp) *pp = a->next;
	if (modal == a) modal = 0;
	if (c) unmanage(c, 1);
	XDestroyWindow(dpy, a->win);
	free(a);
	}

void x_flush_dirty(void)
	{
	App *a;
	for (a = apps; a; a = a->next)
		if (a->dirty)
			{
			a->dirty = 0;
			D = backbuf;
			a->k->draw(a);
			show(a->win, 0, 0, a->w, a->h);
			}
	}

/* ---- starting programs ---- */

static void on_child(int s)
	{
	int e = errno;
	(void)s;
	if (write(sigfd[1], "c", 1) < 0) {}
	errno = e;
	}

void spawn(const char *const *argv, const char *label)
	{
	int p[2], err = 0;
	pid_t pid;
	char buf[96];
	if (pipe(p)) return;
	fcntl(p[1], F_SETFD, FD_CLOEXEC);
	pid = fork();
	if (pid == 0)
		{
		const char *const *av = argv;
#ifdef PSIWM_DEMO
		/* (tests under qemu-arm: run the program through it) */
		const char *wrap = getenv("PSIWM_EXEC_WRAP");
		const char *v[16];
		if (wrap) { int i; v[0] = wrap; for (i = 0; argv[i] && i < 14; i++) v[i + 1] = argv[i]; v[i + 1] = 0; av = v; }
#endif
		close(p[0]);
		setsid();
		signal(SIGCHLD, SIG_DFL);
		execvp(av[0], (char *const *)av);
		err = errno;
		if (write(p[1], &err, sizeof err) < 0) {}
		_exit(127);
		}
	close(p[1]);
	if (pid < 0 || read(p[0], &err, sizeof err) == sizeof err)
		infoprint("Could not start %s - not installed", label);
	else
		{
		snprintf(buf, sizeof buf, "Starting %s…", label);
		busy(buf);
		busy_until = now_ms() + 8000;
		}
	close(p[0]);
	}

/* ---- events ---- */

static struct { Client *c; int x0, y0, fx, fy, moved, active; App *tap; } drag;

static void outline(int x, int y, int w, int h)
	{
	static GC xg;
	if (!xg)
		{
		XGCValues v;
		v.function = GXinvert; v.subwindow_mode = IncludeInferiors; v.line_width = 2;
		v.plane_mask = pix[BLACK] ^ pix[WHITE];
		xg = XCreateGC(dpy, root, GCFunction | GCSubwindowMode | GCLineWidth | GCPlaneMask, &v);
		}
	XDrawRectangle(dpy, root, xg, x + 1, y + 1, w - 3, h - 3);
	}

static void frame_press(Client *c, XButtonEvent *e)
	{
	focus(c);
	if (e->y >= (c->floating ? FB + TH : TH)) return;
	drag.c = c; drag.x0 = e->x_root; drag.y0 = e->y_root; drag.fx = c->x; drag.fy = c->y;
	drag.moved = 0; drag.active = 1;
	}

static void frame_motion(XMotionEvent *e)
	{
	int dx = e->x_root - drag.x0, dy = e->y_root - drag.y0;
	Client *c = drag.c;
	if (!drag.active || !c->floating) return;
	if (!drag.moved && dx * dx + dy * dy < 16) return;
	if (!drag.moved) { XGrabServer(dpy); drag.moved = 1; }
	else outline(c->x, c->y, c->w - SHD, c->h - SHD);
	c->x = drag.fx + dx; c->y = drag.fy + dy;
	if (c->x + c->w > ww) c->x = ww - c->w;
	if (c->y + c->h > SH) c->y = SH - c->h;
	if (c->x < 0) c->x = 0;
	if (c->y < 0) c->y = 0;
	outline(c->x, c->y, c->w - SHD, c->h - SHD);
	}

static void frame_release(void)
	{
	Client *c = drag.c;
	if (!drag.active) return;
	drag.active = 0;
	if (drag.moved)
		{
		outline(c->x, c->y, c->w - SHD, c->h - SHD);
		XUngrabServer(dpy);
		XMoveWindow(dpy, c->frame, c->x, c->y);
		send_configure(c);
		}
	else
		/* a tap on the title: its menus (the Psion's Menu key goes to EPOC) */
		menu_open(c->app, 0);
	}

static int is_mod(KeySym k)
	{
	return k == XK_Shift_L || k == XK_Shift_R || k == XK_Control_L || k == XK_Control_R;
	}

void key_event(XKeyEvent *e)
	{
	char s[16];
	KeySym ks, k0;
	int n = XLookupString(e, s, sizeof s, &ks, 0);
	App *a;
	unsigned st = e->state;
	k0 = XLookupKeysym(e, 0);
	if (is_mod(k0)) return;
	/* the keys every program shares (grabbed on the root) */
	if (switcher_up) { XEvent ev; ev.xkey = *e; switcher_event(&ev); return; }
	if ((st & ControlMask) && k0 == XK_Tab) { switcher_open(st & ShiftMask); return; }
	if (((st & ControlMask) && k0 == XK_space) || k0 == XK_Menu)
		{
		if (cur && !cur->app) menu_open(0, 0);
		else menu_open(cur ? cur->app : desk, 0);
		return;
		}
	a = app_of(e->window);
	if (!a) return;
	if ((st & ControlMask) && a->k->nmenus && !modal)
		if (menu_shortcut(a, a->k->menus, a->k->nmenus, k0, st)) return;
	if ((st & ControlMask) && !a->k->nmenus && a != desk && a->k != &dialog_class)
		if (menu_shortcut(a, wm_menus, wm_nmenus, k0, st)) return;
	if (a->k->key) a->k->key(a, ks, st, s, n);
	}

static void grab_keys(void)
	{
	static const unsigned mods[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
	int i;
	KeyCode tab = XKeysymToKeycode(dpy, XK_Tab), sp = XKeysymToKeycode(dpy, XK_space), mn = XKeysymToKeycode(dpy, XK_Menu);
	for (i = 0; i < 4; i++)
		{
		XGrabKey(dpy, tab, ControlMask | mods[i], root, True, GrabModeAsync, GrabModeAsync);
		XGrabKey(dpy, tab, ControlMask | ShiftMask | mods[i], root, True, GrabModeAsync, GrabModeAsync);
		XGrabKey(dpy, sp, ControlMask | mods[i], root, True, GrabModeAsync, GrabModeAsync);
		if (mn) XGrabKey(dpy, mn, mods[i], root, True, GrabModeAsync, GrabModeAsync);
		}
	}

extern int ui_expose(XEvent *e);

static void handle(XEvent *e)
	{
	Client *c;
	App *a;
	if (menu_event(e)) return;
	if (e->type != KeyPress && switcher_event(e)) return;
	if (clip_event(e)) return;
	switch (e->type)
		{
	case Expose:
		if (e->xexpose.count) break;
		if (ui_expose(e)) break;
		if (e->xexpose.window == tbwin) { toolbar_draw(); break; }
		if ((a = app_of(e->xexpose.window))) { app_dirty(a); break; }
		if ((c = find(e->xexpose.window)) && e->xexpose.window == c->frame) frame_draw(c);
		break;
	case MapRequest:
		if ((c = find(e->xmaprequest.window))) XMapWindow(dpy, c->win);
		else
			{
			XWindowAttributes wa;
			if (XGetWindowAttributes(dpy, e->xmaprequest.window, &wa) && !wa.override_redirect)
				manage(e->xmaprequest.window, 0);
			}
		break;
	case ConfigureRequest:
		{
		XConfigureRequestEvent *r = &e->xconfigurerequest;
		c = find(r->window);
		if (c && c->win == r->window)
			{
			if (c->floating && (r->value_mask & (CWWidth | CWHeight)))
				{
				if (r->value_mask & CWWidth) c->cw = r->width;
				if (r->value_mask & CWHeight) c->ch = r->height;
				if (c->cw > ww - 2 * FB - SHD) c->cw = ww - 2 * FB - SHD;
				if (c->ch > SH - TH - 2 * FB - SHD) c->ch = SH - TH - 2 * FB - SHD;
				place(c);
				frame_draw(c);
				}
			else send_configure(c);
			}
		else
			{
			XWindowChanges ch;
			ch.x = r->x; ch.y = r->y; ch.width = r->width; ch.height = r->height;
			ch.border_width = r->border_width; ch.sibling = r->above; ch.stack_mode = r->detail;
			XConfigureWindow(dpy, r->window, r->value_mask, &ch);
			}
		break;
		}
	case UnmapNotify:
		if ((c = find(e->xunmap.window)) && c->win == e->xunmap.window && !c->app)
			{
			if (c->ignore_unmap > 0) c->ignore_unmap--;
			else unmanage(c, 0);
			}
		break;
	case DestroyNotify:
		if ((c = find(e->xdestroywindow.window)) && c->win == e->xdestroywindow.window && !c->app)
			unmanage(c, 1);
		break;
	case PropertyNotify:
		if ((c = find(e->xproperty.window)) && (e->xproperty.atom == XA_WM_NAME || e->xproperty.atom == A_NETNAME))
			{
			set_names(c);
			frame_draw(c);
			}
		break;
	case ButtonPress:
		if (e->xbutton.window == tbwin) { if (!modal) toolbar_event(e); break; }
		if ((a = app_of(e->xbutton.window)))
			{
			if (modal && a != modal) break;
			if (a == desk ? cur != 0 : a->cl != cur) focus(a->cl);
			drag.tap = a;
			if (a->k->pen) a->k->pen(a, PEN_DOWN, e->xbutton.x, e->xbutton.y);
			break;
			}
		if ((c = find(e->xbutton.window)))
			{
			if (c->win == e->xbutton.window)
				{
				/* a foreign program: focus it, then let the tap go on */
				if (!modal) focus(c);
				XAllowEvents(dpy, modal ? AsyncPointer : ReplayPointer, CurrentTime);
				}
			else if (!modal || (modal && c == modal->cl)) frame_press(c, &e->xbutton);
			}
		break;
	case MotionNotify:
		while (XCheckTypedWindowEvent(dpy, e->xmotion.window, MotionNotify, e)) ;
		if (e->xmotion.window == tbwin) { toolbar_event(e); break; }
		if (drag.active) { frame_motion(&e->xmotion); break; }
		if ((a = app_of(e->xmotion.window)) && a == drag.tap && a->k->pen)
			a->k->pen(a, PEN_MOVE, e->xmotion.x, e->xmotion.y);
		break;
	case ButtonRelease:
		if (e->xbutton.window == tbwin) { toolbar_event(e); break; }
		if (drag.active) { frame_release(); break; }
		if ((a = app_of(e->xbutton.window)) && a == drag.tap)
			{
			drag.tap = 0;
			if (a->k->pen) a->k->pen(a, PEN_UP, e->xbutton.x, e->xbutton.y);
			}
		break;
	case KeyPress:
		key_event(&e->xkey);
		break;
	case MappingNotify:
		XRefreshKeyboardMapping(&e->xmapping);
		break;
		}
	}

#ifdef PSIWM_DEMO
/* ---- a script, for screenshots: PSIWM_DEMO="run 0;wait 500;key s c;..." ---- */
static char *demo, *demop;
static long demo_at;

static void demo_key(const char *name, int ctrl, int shift)
	{
	XEvent e;
	KeySym ks = XStringToKeysym(name);
	Window fw; int rev;
	memset(&e, 0, sizeof e);
	XGetInputFocus(dpy, &fw, &rev);
	e.xkey.type = KeyPress;
	e.xkey.display = dpy;
	e.xkey.window = (ctrl && (ks == XK_Tab || ks == XK_space)) ? root : fw;
	e.xkey.root = root;
	e.xkey.keycode = XKeysymToKeycode(dpy, ks);
	e.xkey.state = (ctrl ? ControlMask : 0) | (shift ? ShiftMask : 0);
	if (menu_up) { handle(&e); return; }
	if (!app_of(e.xkey.window) && e.xkey.window != root)
		{
		/* a foreign program: send it the key */
		e.xkey.subwindow = None; e.xkey.same_screen = True; e.xkey.time = CurrentTime;
		XSendEvent(dpy, fw, True, KeyPressMask, &e);
		return;
		}
	key_event(&e.xkey);
	}

static void demo_pen(int x, int y, int type)
	{
	XEvent e;
	Window w = root, ch;
	int wx = x, wy = y;
	memset(&e, 0, sizeof e);
	/* the deepest window there */
	while (XTranslateCoordinates(dpy, root, w, x, y, &wx, &wy, &ch) && ch != None) w = ch;
	XTranslateCoordinates(dpy, root, w, x, y, &wx, &wy, &ch);
	e.type = type;
	e.xbutton.window = w; e.xbutton.root = root;
	e.xbutton.x = wx; e.xbutton.y = wy; e.xbutton.x_root = x; e.xbutton.y_root = y;
	e.xbutton.button = 1;
	e.xbutton.state = type == ButtonPress ? 0 : Button1Mask;
	if (menu_up)
		{
		/* (the menu has the pointer: as the grab would report it) */
		Window m = 0;
		int i;
		XTranslateCoordinates(dpy, root, w, x, y, &wx, &wy, &ch);
		(void)m; (void)i;
		}
	handle(&e);
	}

static void demo_step(void)
	{
	char *s, *cmdl;
	if (!demop || !*demop || now_ms() < demo_at) return;
	cmdl = demop;
	s = strchr(demop, ';');
	if (s) { *s = 0; demop = s + 1; } else demop += strlen(demop);
	demo_at = now_ms() + 300;
	{
	char a[64] = "", b[64] = "", c2[64] = "", d[64] = "";
	sscanf(cmdl, " %63s %63s %63s %63s", a, b, c2, d);
	if (!strcmp(a, "run")) desk_run(atoi(b));
	else if (!strcmp(a, "wait")) demo_at = now_ms() + atoi(b);
	else if (!strcmp(a, "key")) demo_key(b, strchr(c2, 'c') != 0, strchr(c2, 's') != 0);
	else if (!strcmp(a, "type"))
		{
		char *p;
		for (p = b; *p; p++)
			{
			char k[2] = { *p, 0 };
			const char *n = *p == '_' ? "space" : *p == '.' ? "period" : *p == '-' ? "minus" : *p == '(' ? "9" : *p == ')' ? "0" : k;
			demo_key(n, 0, (*p >= 'A' && *p <= 'Z') || *p == '(' || *p == ')');
			}
		}
	else if (!strcmp(a, "tap")) { demo_pen(atoi(b), atoi(c2), ButtonPress); demo_pen(atoi(b), atoi(c2), ButtonRelease); }
	else if (!strcmp(a, "down")) demo_pen(atoi(b), atoi(c2), ButtonPress);
	else if (!strcmp(a, "up")) demo_pen(atoi(b), atoi(c2), ButtonRelease);
	else if (!strcmp(a, "menu")) menu_open(cur ? cur->app : desk, atoi(b));
	else if (!strcmp(a, "info")) { char *p; for (p = cmdl + 5; *p; p++) if (*p == '_') *p = ' '; infoprint("%s", cmdl + 5); }
	else if (!strcmp(a, "open")) notes_open_file(b);
	else if (!strcmp(a, "exec")) { const char *v[] = { b, c2[0] ? c2 : 0, 0 }; spawn(v, b); }
	else if (!strcmp(a, "move")) { if (cur && cur->floating) { cur->x = atoi(b); cur->y = atoi(c2); place(cur); } }
	else if (!strcmp(a, "focus")) { Client *c; for (c = clients; c; c = c->next) if (!strcasecmp(c->name, b)) focus(c); }
	else if (!strcmp(a, "shot")) { XWarpPointer(dpy, None, root, 0, 0, 0, 0, SW - 20, SH - 50); XSync(dpy, False); FILE *f = fopen(b, "w"); if (f) fclose(f); }
	else if (!strcmp(a, "quit")) exit(0);
	}
	}
#endif

/* ---- the start-up screen (splash.c), as psisplash left it, finished off ---- */
static Window spwin;
static long sp_t0;

static void splash_show(const char *msg, int pc)
	{
	static unsigned char *buf;
	XImage *im;
	int x, y;
	if (getenv("PSIWM_NOSPLASH")) return;
	if (!msg)
		{
		/* (on for a second at least, so it is seen, not flashed) */
		long left = sp_t0 + 1200 - now_ms();
		if (!spwin) return;
		if (left > 0) usleep(left * 1000);
		XDestroyWindow(dpy, spwin);
		spwin = 0;
		free(buf); buf = 0;
		return;
		}
	if (!buf && !(buf = malloc(SPW * SPH))) return;
	if (!spwin)
		{
		XSetWindowAttributes a;
		a.override_redirect = True;
		a.background_pixel = pix[WHITE];
		spwin = XCreateWindow(dpy, root, 0, 0, SW, SH, 0, CopyFromParent, InputOutput, CopyFromParent,
			CWOverrideRedirect | CWBackPixel, &a);
		XMapRaised(dpy, spwin);
		sp_t0 = now_ms();
		}
	{
	char ver[32] = "", b[256], path[300], *p;
	snprintf(path, sizeof path, "%s/version", procroot);
	if (readfile(path, b, sizeof b) > 0 && (p = strstr(b, "(PsiLinux ")))
		snprintf(ver, sizeof ver, "%.*s", (int)strcspn(p + 10, ",)"), p + 10);
	splash_draw(buf, msg, pc, ver[0] ? ver : "?");
	}
	/* one row at a time (the whole screen at once is 150 KB more) */
	im = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 8, ZPixmap, 0, 0, SW, 1, 8, 0);
	if (!im) return;
	im->data = malloc(im->bytes_per_line);
	for (y = 0; y < SH; y++)
		{
		for (x = 0; x < SW; x++) XPutPixel(im, x, 0, pix[buf[y * SW + x]]);
		XPutImage(dpy, spwin, DefaultGC(dpy, DefaultScreen(dpy)), im, 0, 0, 0, y, SW, 1);
		}
	XDestroyImage(im);
	XRaiseWindow(dpy, spwin);
	XSync(dpy, False);
	}

static int xerr(Display *d, XErrorEvent *e)
	{
	(void)d;
	if (e->error_code == BadAccess && e->request_code == 2 /* ChangeWindowAttributes */)
		{
		fprintf(stderr, "psiwm: another window manager is running\n");
		exit(1);
		}
	return 0;       /* (windows going away under us: ignore) */
	}

int main(int argc, char **argv)
	{
	int i, xfd;
	Window *kids, d1, d2;
	unsigned n;
	(void)argc; (void)argv;
	setenv("DISPLAY", ":0", 0);
	if (!(dpy = XOpenDisplay(0))) { fprintf(stderr, "psiwm: cannot open display %s\n", getenv("DISPLAY")); return 1; }
	xfd = ConnectionNumber(dpy);
	fcntl(xfd, F_SETFD, FD_CLOEXEC);
	root = DefaultRootWindow(dpy);
#ifdef PSIWM_DEMO
	if (getenv("PSIWM_PROC")) procroot = getenv("PSIWM_PROC");
#endif
	for (i = 0; i < 16; i++)
		{
		XColor c;
		c.red = c.green = c.blue = (unsigned short)(i * 17 * 257);
		c.flags = DoRed | DoGreen | DoBlue;
		pix[i] = XAllocColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), &c) ? c.pixel
			: i < 8 ? BlackPixel(dpy, DefaultScreen(dpy)) : WhitePixel(dpy, DefaultScreen(dpy));
		}
	XSetErrorHandler(xerr);
	XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | PropertyChangeMask);
	XSync(dpy, False);
	A_PROTOCOLS = XInternAtom(dpy, "WM_PROTOCOLS", False);
	A_DELETE = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	A_CLIPBOARD = XInternAtom(dpy, "CLIPBOARD", False);
	A_TARGETS = XInternAtom(dpy, "TARGETS", False);
	A_UTF8 = XInternAtom(dpy, "UTF8_STRING", False);
	A_NAME = XA_WM_NAME;
	A_NETNAME = XInternAtom(dpy, "_NET_WM_NAME", False);
	(void)A_NAME;
	ui_init();
	{
	/* no pointer on the screen: the pen is the pointer */
	static char z[1];
	Pixmap p = XCreateBitmapFromData(dpy, root, z, 1, 1);
	XColor c; memset(&c, 0, sizeof c);
	XDefineCursor(dpy, root, XCreatePixmapCursor(dpy, p, p, &c, &c, 0, 0));
	}
	XSetWindowBackground(dpy, root, pix[WHITE]);
	splash_show("Starting the desktop…", 70);
	desk = app_open(&desk_class, 0);
	grab_keys();
	if (XQueryTree(dpy, root, &d1, &d2, &kids, &n))
		{
		unsigned k;
		for (k = 0; k < n; k++)
			{
			XWindowAttributes wa;
			if (kids[k] == desk->win || kids[k] == tbwin) continue;
			if (XGetWindowAttributes(dpy, kids[k], &wa) && !wa.override_redirect && wa.map_state == IsViewable && !app_of(kids[k]))
				manage(kids[k], 0);
			}
		if (kids) XFree(kids);
		}
	focus(0);
	splash_show("Starting the desktop…", 100);
	splash_show(0, 0);
	if (pipe(sigfd) == 0)
		{
		fcntl(sigfd[0], F_SETFL, O_NONBLOCK); fcntl(sigfd[1], F_SETFL, O_NONBLOCK);
		fcntl(sigfd[0], F_SETFD, FD_CLOEXEC); fcntl(sigfd[1], F_SETFD, FD_CLOEXEC);
		}
	signal(SIGCHLD, on_child);
	signal(SIGPIPE, SIG_IGN);
#ifdef PSIWM_DEMO
	if (getenv("PSIWM_DEMO")) { demo = strdup(getenv("PSIWM_DEMO")); demop = demo; demo_at = now_ms() + 800; }
#endif
	for (;;)
		{
		fd_set r;
		struct timeval tv;
		long now, next;
		App *a;
		while (XPending(dpy))
			{
			XEvent e;
			XNextEvent(dpy, &e);
			handle(&e);
			}
		x_flush_dirty();
		XFlush(dpy);
		now = now_ms();
		next = now + 60000;
		for (a = apps; a; a = a->next) if (a->tick_at && a->tick_at < next) next = a->tick_at;
		if (msg_next() && msg_next() < next) next = msg_next();
		if (busy_until && busy_until < next) next = busy_until;
#ifdef PSIWM_DEMO
		if (demop && *demop && demo_at < next) next = demo_at;
#endif
		if (next < now) next = now;
		tv.tv_sec = (next - now) / 1000;
		tv.tv_usec = (next - now) % 1000 * 1000;
		FD_ZERO(&r);
		FD_SET(xfd, &r);
		FD_SET(sigfd[0], &r);
		if (select((xfd > sigfd[0] ? xfd : sigfd[0]) + 1, &r, 0, 0, &tv) < 0 && errno != EINTR) break;
		if (FD_ISSET(sigfd[0], &r))
			{
			char b[16];
			while (read(sigfd[0], b, sizeof b) > 0) ;
			while (waitpid(-1, 0, WNOHANG) > 0) ;
			}
		now = now_ms();
		for (a = apps; a; a = a->next)
			if (a->tick_at && a->tick_at <= now) { a->tick_at = 0; if (a->k->tick) a->k->tick(a); }
		msg_tick();
		if (busy_until && now >= busy_until) { busy(0); busy_until = 0; }
#ifdef PSIWM_DEMO
		demo_step();
#endif
		}
	return 0;
	}
