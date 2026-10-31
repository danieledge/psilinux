/* calc.c - Calc: a pocket calculator, for the pen and the keys. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "psiwm.h"

enum { K_CLEAR = 1, K_COPY, K_PASTE };
static const MenuItem kf[] = { { "Close", "Ctrl+E", C_CLOSE, 0 } };
static const MenuItem ke[] = {
	{ "Copy", "Ctrl+C", K_COPY, 0 }, { "Paste", "Ctrl+V", K_PASTE, MI_SEP }, { "Clear", "Ctrl+D", K_CLEAR, 0 } };
static const MenuItem kv[] = { { "Show toolbar", "Ctrl+T", C_TOOLBAR, MI_CHECK } };
static const MenuItem kt[] = { { "Help", "Shift+Ctrl+H", C_HELP, 0 },
	{ "About PsiLinux…", "Shift+Ctrl+A", C_ABOUT, 0 } };
static const Menu kmenus[] = { { "File", kf, 1 }, { "Edit", ke, 3 }, { "View", kv, 1 }, { "Tools", kt, 2 } };
static const TbBut ktb[] = { { I_T_CLEAR, "Clear", K_CLEAR }, { I_T_COPY, "Copy", K_COPY }, { I_T_PASTE, "Paste", K_PASTE } };

/* the keys: label, the character it stands for */
static const char *const KL[5][4] = {
	{ "C", "±", "%", "÷" }, { "7", "8", "9", "×" }, { "4", "5", "6", "−" }, { "1", "2", "3", "+" }, { "0", ".", "⌫", "=" } };
static const char KC[5][4] = {
	{ 'c', 'n', '%', '/' }, { '7', '8', '9', '*' }, { '4', '5', '6', '-' }, { '1', '2', '3', '+' }, { '0', '.', 'b', '=' } };

typedef struct { char ent[24]; double acc; char op; int fresh, press, err; } Calc;
#define BW 54
#define BH 26
#define DH 46

static void c_init(App *a, const char *arg)
	{
	Calc *c = calloc(1, sizeof *c);
	(void)arg;
	strcpy(c->ent, "0");
	c->fresh = 1; c->press = -1;
	a->p = c;
	}

static void show_num(Calc *c, double v)
	{
	if (isnan(v) || isinf(v)) { strcpy(c->ent, "0"); c->err = 1; return; }
	snprintf(c->ent, sizeof c->ent, "%.10g", v);
	if (strlen(c->ent) > 14) snprintf(c->ent, sizeof c->ent, "%.6g", v);
	}

static void press(App *a, int k)
	{
	Calc *c = a->p;
	double v = atof(c->ent);
	c->err = 0;
	if (k >= '0' && k <= '9')
		{
		if (c->fresh || !strcmp(c->ent, "0")) { c->ent[0] = 0; c->fresh = 0; }
		if (strlen(c->ent) < 14) { int n = strlen(c->ent); c->ent[n] = k; c->ent[n + 1] = 0; }
		}
	else if (k == '.')
		{
		if (c->fresh) { strcpy(c->ent, "0"); c->fresh = 0; }
		if (!strchr(c->ent, '.')) strcat(c->ent, ".");
		}
	else if (k == 'b')
		{
		int n = strlen(c->ent);
		if (c->fresh || n <= 1 || (n == 2 && c->ent[0] == '-')) { strcpy(c->ent, "0"); c->fresh = 1; }
		else c->ent[n - 1] = 0;
		}
	else if (k == 'c') { strcpy(c->ent, "0"); c->acc = 0; c->op = 0; c->fresh = 1; }
	else if (k == 'n') { if (strcmp(c->ent, "0")) show_num(c, -v); }
	else if (k == '%') { show_num(c, c->op ? c->acc * v / 100 : v / 100); c->fresh = 1; }
	else
		{
		/* an operator, or =: work out what is pending first */
		if (c->op && !(c->fresh && k != '='))
			{
			switch (c->op)
				{
			case '+': v = c->acc + v; break;
			case '-': v = c->acc - v; break;
			case '*': v = c->acc * v; break;
			case '/':
				if (v == 0) { infoprint("Cannot divide by zero"); c->op = 0; c->fresh = 1; strcpy(c->ent, "0"); return; }
				v = c->acc / v; break;
				}
			show_num(c, v);
			}
		c->acc = v;
		c->op = k == '=' ? 0 : k;
		c->fresh = 1;
		}
	app_dirty(a);
	}

static void c_draw(App *a)
	{
	Calc *c = a->p;
	int r, k, x0 = (a->w - 4 * (BW + 4) + 4) / 2;
	char opl[4] = "";
	fill(0, 0, a->w, a->h, LIGHT);
	/* the display */
	fill(x0, 6, 4 * (BW + 4) - 4, DH - 8, WHITE);
	box(x0, 6, 4 * (BW + 4) - 4, DH - 8, BLACK);
	fill(x0 + 1, 7, 4 * (BW + 4) - 6, 1, MID);
	fill(x0 + 1, 7, 1, DH - 10, MID);
	{
	/* (the big font has no comma: its own digits, minus as a dash) */
	const char *e = c->ent;
	int w;
	if (e[0] == '-') e++;
	w = tw(&f_big, e, -1);
	text(&f_big, x0 + 4 * (BW + 4) - 12 - w, 6 + (DH - 8 - f_big.h) / 2 + 1, e, -1, BLACK);
	if (c->ent[0] == '-') text(&f_big, x0 + 4 * (BW + 4) - 12 - w - tw(&f_big, "−", -1), 6 + (DH - 8 - f_big.h) / 2 + 1, "−", -1, BLACK);
	}
	if (c->op) { const char *o = c->op == '*' ? "×" : c->op == '/' ? "÷" : c->op == '-' ? "−" : "+"; snprintf(opl, sizeof opl, "%s", o); }
	if (opl[0]) text(&f_title, x0 + 8, 6 + (DH - 8 - f_title.h) / 2, opl, -1, DARK);
	for (r = 0; r < 5; r++)
		for (k = 0; k < 4; k++)
			{
			int x = x0 + k * (BW + 4), y = DH + 4 + r * (BH + 4), dn = c->press == r * 4 + k;
			bevel(x, y, BW, BH, dn);
			if (k == 3 || r == 0)
				fill(x + 2, y + 2, BW - 5 - !dn, BH - 5 - !dn, dn ? MID : PALE);
			if (r == 0 && k == 0 || r == 4 && k == 3)
				fill(x + 2, y + 2, BW - 5 - !dn, BH - 5 - !dn, dn ? GREY : MID);
			if (r == 4 && k == 2)
				{
				/* a backspace arrow, drawn */
				int cx = x + BW / 2 + dn, cy = y + BH / 2 - 1 + dn;
				line(cx - 7, cy, cx + 7, cy, BLACK, 2);
				line(cx - 7, cy, cx - 3, cy - 4, BLACK, 2);
				line(cx - 7, cy, cx - 3, cy + 4, BLACK, 2);
				}
			else textc(&f_title, x + dn, y + (BH - f_title.h) / 2 + dn, BW, KL[r][k], BLACK);
			}
	}

static int hit(App *a, int x, int y)
	{
	int x0 = (a->w - 4 * (BW + 4) + 4) / 2, k = (x - x0) / (BW + 4), r = (y - DH - 4) / (BH + 4);
	if (x < x0 || y < DH + 4 || k > 3 || r > 4) return -1;
	if ((x - x0) % (BW + 4) >= BW || (y - DH - 4) % (BH + 4) >= BH) return -1;
	return r * 4 + k;
	}

static void c_pen(App *a, int type, int x, int y)
	{
	Calc *c = a->p;
	int h = hit(a, x, y);
	if (type == PEN_DOWN) c->press = h;
	else if (type == PEN_MOVE) { if (h != c->press) c->press = -1; }
	else
		{
		if (h >= 0 && h == c->press) { c->press = -1; press(a, KC[h / 4][h % 4]); }
		c->press = -1;
		}
	app_dirty(a);
	}

static void pasted(App *a, const char *s, int n)
	{
	Calc *c = a->p;
	char b[32];
	double v;
	char *e;
	if (!s || n <= 0) { infoprint("Nothing to paste"); return; }
	snprintf(b, sizeof b, "%.*s", n < 31 ? n : 31, s);
	v = strtod(b, &e);
	if (e == b) { infoprint("Not a number"); return; }
	show_num(c, v);
	c->fresh = 1;
	app_dirty(a);
	}

static void c_cmd(App *a, int cmd)
	{
	Calc *c = a->p;
	switch (cmd)
		{
	case K_CLEAR: press(a, 'c'); break;
	case K_COPY: clip_set(c->ent, strlen(c->ent)); infoprint("Copied"); break;
	case K_PASTE: clip_get(a, pasted); break;
		}
	}

static int c_key(App *a, KeySym ks, unsigned st, const char *s, int n)
	{
	(void)st;
	if (ks == XK_Return || ks == XK_KP_Enter) { press(a, '='); return 1; }
	if (ks == XK_BackSpace || ks == XK_Delete) { press(a, 'b'); return 1; }
	if (ks == XK_Escape) { press(a, 'c'); return 1; }
	if (n == 1 && strchr("0123456789.+-*/=%", s[0])) { press(a, s[0]); return 1; }
	if (n == 1 && (s[0] == 'x' || s[0] == 'X')) { press(a, '*'); return 1; }
	if (n == 1 && s[0] == ',') { press(a, '.'); return 1; }
	return 0;
	}

static void c_fini(App *a) { free(a->p); }

const AppClass calc_class = { "Calc", I_P_CALC, 4 * (BW + 4) + 12, DH + 4 + 5 * (BH + 4) + 4, kmenus, 4, ktb, 3,
	c_init, c_draw, c_key, c_pen, c_cmd, 0, 0, c_fini };
