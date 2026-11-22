/* epoc - your Psion from PsiLinux's shell.
 *
 *   epoc open Letter.docx          open a file in its program, as a tap on the System screen does
 *   epoc run Word                  start a program, or switch to it
 *   epoc ps, epoc tasks            EPOC's processes, and the programs on the task list
 *   epoc clip get | set            EPOC's clipboard, as text, through a pipe
 *   epoc note "Backup done"        an infoprint
 *   ... (epoc help)
 *
 * It asks through /dev/epoc (common/lxepoc.h): one write, one read for each
 * request. The kernel makes Linux paths EPOC names and PsiLinux.app does
 * the work. Only programs on the Psion's own terminal may: not over ssh.
 *
 * For scripts: lists are tab-separated lines (-H adds a heading, --json makes
 * JSON), nothing is printed when a command just works, errors go to stderr,
 * and the exit code says what happened (epoc help exit).
 *
 * Built static with PsiLinux's toolchain (lx-cc), shipped beside the runner
 * (/psion/bin/epoc); xdg-open and open are links to it. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../common/lxepoc.h"

#define EPOC_VERSION "0.38"
#define HDR ((int)sizeof(LxeHeader))

static int opt_heading, opt_json, opt_quiet, opt_force;
static const char *dev_path = LXE_DEVICE;
static volatile sig_atomic_t interrupted;

static void on_signal(int s) { (void)s; interrupted = 1; }

static void err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void err(const char *fmt, ...)
{
	va_list ap;
	fputs("epoc: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---- requests ------------------------------------------------------------------ */

typedef struct {
	LxeHeader h;
	char *text;                 /* the answer's text (NUL-ended) */
	int len;
} answer_t;

static char req_buf[HDR + LXE_MAX];
static char ans_buf[HDR + LXE_MAX + 1];

/* Builds a request: the header, then each string with its NUL. Returns its
 * length, or -1 if too long. (Kept apart from the device for the host tests.) */
int epoc_encode(char *out, int max, int op, unsigned flags, unsigned arg, int nargs, const char *const *args)
{
	LxeHeader h;
	int o = HDR, i;
	memset(&h, 0, sizeof h);
	for (i = 0; i < nargs; i++) {
		int n = (int)strlen(args[i]) + 1;
		if (o + n > max || o + n > HDR + LXE_MAX) return -1;
		memcpy(out + o, args[i], n);
		o += n;
	}
	h.magic = LXE_MAGIC;
	h.version = LXE_VERSION;
	h.op = (unsigned short)op;
	h.flags = flags;
	h.arg = arg;
	h.len = (unsigned)(o - HDR);
	memcpy(out, &h, HDR);
	return o;
}

/* Checks an answer: 0, or -1 if it is not one */
int epoc_decode(const char *buf, int n, int op, answer_t *a)
{
	if (n < HDR) return -1;
	memcpy(&a->h, buf, HDR);
	if (a->h.magic != LXE_MAGIC || a->h.len != (unsigned)(n - HDR) || (op && a->h.op != op)) return -1;
	a->text = (char *)buf + HDR;
	a->len = n - HDR;
	return 0;
}

static int fd = -1;

static int open_dev(void)
{
	if (fd >= 0) return 0;
	fd = open(dev_path, O_RDWR | O_CLOEXEC);
	if (fd >= 0) return 0;
	if (errno == EACCES || errno == EPERM) {
		err("only programs on the Psion's own terminal may use EPOC (not over ssh, not from a daemon)");
		return LXE_REFUSED;
	}
	if (errno == ENOENT) {
		err("%s is not there: this is not PsiLinux 0.36 or later", dev_path);
		return LXE_NOAPP;
	}
	err("%s: %s", dev_path, strerror(errno));
	return LXE_FAILED;
}

/* Sends a request and waits for its answer. Returns LXE_* (the answer's
 * status), and prints EPOC's words if it failed (unless quiet_fail). */
static int ask_q(int op, unsigned flags, unsigned arg, int nargs, const char *const *args, answer_t *a, int quiet_fail)
{
	int n, r;
	answer_t tmp;
	if (!a) a = &tmp;
	memset(a, 0, sizeof *a);
	if ((r = open_dev()) != 0) return r;
	n = epoc_encode(req_buf, sizeof req_buf, op, flags, arg, nargs, args);
	if (n < 0) { err("too long (at most %d bytes)", LXE_MAX); return LXE_USAGE; }
	if (write(fd, req_buf, n) != n) {
		if (errno == EINTR) return LXE_INTERRUPTED;
		err("%s: %s", dev_path, strerror(errno));
		return LXE_FAILED;
	}
	for (;;) {
		n = (int)read(fd, ans_buf, sizeof ans_buf - 1);
		if (n >= 0 || errno != EINTR) break;
		if (interrupted) return LXE_INTERRUPTED;
	}
	if (n < 0) { err("%s: %s", dev_path, strerror(errno)); return LXE_FAILED; }
	if (epoc_decode(ans_buf, n, op, a) < 0) { err("a garbled answer"); return LXE_FAILED; }
	a->text[a->len] = 0;
	r = a->h.status;
	if (r != LXE_OK && !quiet_fail) {
		static const char *const why[] = { "", "EPOC could not do it", "", "not found", "refused", "not available on this machine",
			"", "PsiLinux's window does not answer", "this PsiLinux does not know that request (update it)", "" };
		const char *w = a->len ? a->text : (r >= 0 && r <= LXE_INTERRUPTED) ? why[r] : "failed";
		char t[200];
		int k;
		snprintf(t, sizeof t, "%s", w);
		k = (int)strlen(t);
		while (k && t[k - 1] == '\n') t[--k] = 0;
		if (a->h.err) err("%s (EPOC error %d)", t, a->h.err);
		else err("%s", t);
	}
	if (r < 0 || r > 99) r = LXE_FAILED;
	return r;
}

static int ask(int op, unsigned flags, unsigned arg, int nargs, const char *const *args, answer_t *a)
{
	return ask_q(op, flags, arg, nargs, args, a, 0);
}

/* ---- output: tab-separated lines, a heading, or JSON ----------------------------- */

static void json_str(const char *s, int n)
{
	int i;
	putchar('"');
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\') printf("\\%c", c);
		else if (c == '\n') fputs("\\n", stdout);
		else if (c == '\t') fputs("\\t", stdout);
		else if (c < 32) printf("\\u%04x", c);
		else putchar(c);
	}
	putchar('"');
}

static int is_number(const char *s, int n)
{
	int i = 0;
	if (n == 0) return 0;
	if (s[0] == '-' && n > 1) i = 1;
	if (s[i] == '0' && n > i + 1) return 0;        /* (0x... and 007 stay strings) */
	for (; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
	return 1;
}

static void json_val(const char *s, int n)
{
	if (is_number(s, n)) fwrite(s, 1, n, stdout);
	else json_str(s, n);
}

/* field k of a tab-separated line [s, e) */
static int field(const char *s, const char *e, int k, const char **fs)
{
	const char *p = s;
	while (k-- > 0) {
		while (p < e && *p != '\t') p++;
		if (p >= e) { *fs = e; return 0; }
		p++;
	}
	*fs = p;
	{ const char *q = p; while (q < e && *q != '\t') q++; return (int)(q - p); }
}

/* a list: one object a line, these columns */
static void print_table(const char *text, const char *const *cols)
{
	const char *s = text;
	int ncol = 0, first = 1, i;
	while (cols[ncol]) ncol++;
	if (opt_json) putchar('[');
	else if (opt_heading) {
		for (i = 0; i < ncol; i++) printf("%s%s", i ? "\t" : "", cols[i]);
		putchar('\n');
	}
	while (*s) {
		const char *e = strchr(s, '\n');
		if (!e) e = s + strlen(s);
		if (opt_json) {
			printf("%s\n {", first ? "" : ",");
			for (i = 0; i < ncol; i++) {
				const char *f;
				int n = field(s, e, i, &f);
				printf("%s\"%s\": ", i ? ", " : "", cols[i]);
				json_val(f, n);
			}
			putchar('}');
		} else {
			fwrite(s, 1, e - s, stdout);
			putchar('\n');
		}
		first = 0;
		s = *e ? e + 1 : e;
	}
	if (opt_json) puts(first ? "]" : "\n]");
}

/* a value (or a TAB-separated row of them, or every 'disk' line's: an
 * array) as JSON */
static void json_row(const char *k, int kl, const char *v, const char *e)
{
	int multi = memchr(v, '\t', e - v) != 0 || (kl == 4 && strncmp(k, "disk", 4) == 0);
	if (multi) {
		const char *f = v;
		int n = 0;
		putchar('[');
		while (f <= e) {
			const char *g = memchr(f, '\t', e - f);
			if (!g) g = e;
			printf("%s", n++ ? ", " : "");
			json_val(f, (int)(g - f));
			f = g + 1;
		}
		putchar(']');
	} else json_val(v, (int)(e - v));
}

/* key<TAB>value lines; only one key's value(s) with 'key' (0.37, RN-18:
 * with --json, as {"key": value} too: it was printed plain) */
static int print_keys(const char *text, const char *key)
{
	const char *s = text;
	int first = 1, found = 0;
	if (opt_json && !key) putchar('{');
	else if (opt_heading && !key) puts("key\tvalue");
	while (*s) {
		const char *e = strchr(s, '\n'), *t;
		int kl;
		if (!e) e = s + strlen(s);
		t = memchr(s, '\t', e - s);
		kl = t ? (int)(t - s) : (int)(e - s);
		if (key) {
			if ((int)strlen(key) == kl && strncmp(s, key, kl) == 0) {
				if (opt_json) {
					/* (a key there more than once - disk - gives one
					 * object a line, as plain gives one line each) */
					putchar('{');
					json_str(s, kl);
					fputs(": ", stdout);
					json_row(s, kl, t ? t + 1 : e, e);
					putchar('}');
				} else if (t) fwrite(t + 1, 1, e - t - 1, stdout);
				putchar('\n');
				found = 1;
			}
		} else if (opt_json) {
			/* (a key with several values, or there more than once - disk -
			 * gives an array of them) */
			printf("%s\n ", first ? "" : ",");
			json_str(s, kl);
			fputs(": ", stdout);
			json_row(s, kl, t ? t + 1 : e, e);
		} else {
			fwrite(s, 1, e - s, stdout);
			putchar('\n');
		}
		first = 0;
		s = *e ? e + 1 : e;
	}
	if (opt_json && !key) puts(first ? "}" : "\n}");
	if (key && !found) { err("no %s", key); return LXE_NOTFOUND; }
	return LXE_OK;
}

/* ---- the commands ---------------------------------------------------------------- */

/* "C:\..." or "c:/..." is already an EPOC name; anything else is a Linux path */
static int is_epoc_name(const char *s)
{
	return ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':' && (s[2] == '\\' || s[2] == 0);
}

static unsigned path_flag(const char *s, unsigned f) { return is_epoc_name(s) ? 0 : f; }

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int nap(double s)
{
	struct timespec ts;
	ts.tv_sec = (time_t)s;
	ts.tv_nsec = (long)((s - ts.tv_sec) * 1e9);
	nanosleep(&ts, 0);
	return interrupted ? LXE_INTERRUPTED : 0;
}

/* until EPOC's thread or process 'id' has ended */
static int wait_gone(unsigned id, double timeout)
{
	double until = timeout > 0 ? now_s() + timeout : 0;
	for (;;) {
		int r = ask_q(LXE_OP_ALIVE, 0, id, 0, 0, 0, 1);
		if (r == LXE_NOTFOUND) return LXE_OK;
		if (r != LXE_OK) return r;
		if (until && now_s() >= until) { err("still running after %g s", timeout); return LXE_BUSY; }
		if (nap(1)) return LXE_INTERRUPTED;
	}
}

static double opt_timeout;

/* (0.38, RR-05) is thread 'id' a task on the task list, and is it in front?
 * (TASKS: id, uid, caption, document, "fg" or "") */
static int task_state(const char *text, unsigned id, int *fg)
{
	const char *s = text;
	while (s && *s) {
		const char *e = strchr(s, '\n');
		if (!e) e = s + strlen(s);
		if ((unsigned)strtoul(s, 0, 10) == id) {
			*fg = e - s >= 3 && !strncmp(e - 3, "\tfg", 3);
			return 1;
		}
		s = *e ? e + 1 : 0;
	}
	return 0;
}

/* (0.38, RR-05) until the program of thread 'id' has started: its window is
 * on the task list and, unless it was started behind (--bg), in front.
 * EPOC starts a program by itself and answers at once; this is what makes
 * `time epoc run Calc` the time Calc takes to start. A program that is on
 * the list but not brought to the front (another came first) counts as
 * started after 2 s more. Gives up after --timeout S (60 s as standard). */
static int wait_up(unsigned id, int bg, double timeout)
{
	double until = now_s() + (timeout > 0 ? timeout : 60), listed = 0;
	for (;;) {
		answer_t a;
		int fg = 0, r = ask_q(LXE_OP_TASKS, 0, 0, 0, 0, &a, 1);
		if (r != LXE_OK) return r;
		if (task_state(a.text, id, &fg)) {
			if (bg || fg) return LXE_OK;
			if (!listed) listed = now_s();
			else if (now_s() - listed >= 2) return LXE_OK;
		} else if (ask_q(LXE_OP_ALIVE, 0, id, 0, 0, 0, 1) == LXE_NOTFOUND) {
			err("the program ended as it started");
			return LXE_FAILED;
		}
		if (now_s() >= until) { err("not started after %g s (it may still be starting)", timeout > 0 ? timeout : 60); return LXE_BUSY; }
		if (nap(0.2)) return LXE_INTERRUPTED;
	}
}

static int cmd_open(int argc, char **argv)
{
	int i, wait = 0, bg = 0, nowait = 0, r = LXE_OK, any = 0;
	for (i = 0; i < argc; i++) {
		answer_t a;
		const char *args[1];
		int k;
		if (!strcmp(argv[i], "--wait") || !strcmp(argv[i], "-w")) { wait = 1; continue; }
		if (!strcmp(argv[i], "--no-wait")) { nowait = 1; continue; }
		if (!strcmp(argv[i], "--bg") || !strcmp(argv[i], "-b")) { bg = 1; continue; }
		if (!strncmp(argv[i], "http://", 7) || !strncmp(argv[i], "https://", 8)) {
			err("%s: EPOC opens files, not web addresses", argv[i]);
			r = LXE_UNSUPPORTED;
			continue;
		}
		any = 1;
		args[0] = argv[i];
		k = ask(LXE_OP_OPEN, path_flag(argv[i], LXE_F_PATH) | (bg ? LXE_F_BG : 0), 0, 1, args, &a);
		if (k == LXE_OK && !nowait && a.h.arg) k = wait_up(a.h.arg, bg, opt_timeout);
		if (k == LXE_OK && wait && a.h.arg) k = wait_gone(a.h.arg, opt_timeout);
		if (k != LXE_OK) r = k;
	}
	if (!any && r == LXE_OK) { err("open what? (epoc open FILE...)"); return LXE_USAGE; }
	return r;
}

static int cmd_run(int argc, char **argv)
{
	int i, wait = 0, nowait = 0;
	unsigned fl = 0;
	const char *args[2];
	int n = 0, r;
	answer_t a;
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--wait") || !strcmp(argv[i], "-w")) wait = 1;
		else if (!strcmp(argv[i], "--no-wait")) nowait = 1;
		else if (!strcmp(argv[i], "--new") || !strcmp(argv[i], "-n")) fl |= LXE_F_NEW;
		else if (!strcmp(argv[i], "--bg") || !strcmp(argv[i], "-b")) fl |= LXE_F_BG;
		else if (n < 2) args[n++] = argv[i];
		else { err("run: one program and at most one file"); return LXE_USAGE; }
	}
	if (!n) { err("run what? (epoc run PROGRAM [FILE]; epoc apps lists them)"); return LXE_USAGE; }
	if (n == 2) fl |= path_flag(args[1], LXE_F_PATH2);
	r = ask(LXE_OP_RUN, fl, 0, n, args, &a);
	/* (0.38) a program (its answer: uid, caption, started or switched)
	 * until it has started; an .exe has no window to wait for */
	if (r == LXE_OK && !nowait && a.h.arg && a.text && strstr(a.text, "\tstarted"))
		r = wait_up(a.h.arg, (fl & LXE_F_BG) != 0, opt_timeout);
	if (r == LXE_OK && wait && a.h.arg) r = wait_gone(a.h.arg, opt_timeout);
	return r;
}

static int cmd_list(int op, const char *const *cols)
{
	answer_t a;
	int r = ask(op, 0, 0, 0, 0, &a);
	if (r == LXE_OK) print_table(a.text, cols);
	return r;
}

static int cmd_keys(int op, int argc, char **argv)
{
	answer_t a;
	int r = ask(op, 0, 0, 0, 0, &a);
	if (r == LXE_OK) r = print_keys(a.text, argc ? argv[0] : 0);
	return r;
}

static int cmd_target(int op, int argc, char **argv)
{
	int i, r = LXE_OK, any = 0;
	for (i = 0; i < argc; i++) {
		const char *args[1];
		int k;
		if (!strcmp(argv[i], "--force") || !strcmp(argv[i], "-f")) continue;
		any = 1;
		args[0] = argv[i];
		k = ask(op, opt_force ? LXE_F_FORCE : 0, 0, 1, args, 0);
		if (k != LXE_OK) r = k;
	}
	if (!any) { err("%s what? (a name from epoc %s, a 0xUID or an id)", op == LXE_OP_KILL ? "kill" : "close", op == LXE_OP_KILL ? "ps" : "tasks"); return LXE_USAGE; }
	return r;
}

static int read_all(char *buf, int max)
{
	int n = 0;
	for (;;) {
		ssize_t k = read(0, buf + n, max - n);
		if (k < 0 && errno == EINTR && !interrupted) continue;
		if (k <= 0) break;
		n += (int)k;
		if (n >= max) { char c; if (read(0, &c, 1) == 1) return -1; break; }
	}
	return n;
}

static char clip_buf[LXE_MAX];

static int cmd_clip(int argc, char **argv)
{
	answer_t a;
	if (argc == 0 || !strcmp(argv[0], "get")) {
		int r = ask(LXE_OP_CLIPGET, 0, 0, 0, 0, &a);
		if (r == LXE_OK) {
			fwrite(a.text, 1, a.len, stdout);
			if (a.len && a.text[a.len - 1] != '\n' && isatty(1)) putchar('\n');
		}
		return r;
	}
	if (!strcmp(argv[0], "set")) {
		const char *args[1];
		int n, i, o = 0;
		if (argc > 1) {
			for (i = 1; i < argc; i++) {
				int k = (int)strlen(argv[i]);
				if (o + k + 2 > (int)sizeof clip_buf) { err("too long for the clipboard (at most %d bytes)", LXE_MAX - 1); return LXE_FAILED; }
				if (i > 1) clip_buf[o++] = ' ';
				memcpy(clip_buf + o, argv[i], k);
				o += k;
			}
			n = o;
		} else {
			n = read_all(clip_buf, sizeof clip_buf - 1);
			if (n < 0) { err("too long for the clipboard (at most %d bytes)", LXE_MAX - 1); return LXE_FAILED; }
		}
		clip_buf[n] = 0;
		if ((int)strlen(clip_buf) != n) { err("the clipboard takes text: there is a NUL byte in it"); return LXE_FAILED; }
		args[0] = clip_buf;
		return ask(LXE_OP_CLIPSET, 0, 0, 1, args, 0);
	}
	err("clip get | clip set [TEXT]");
	return LXE_USAGE;
}

static void join(char *out, int max, int argc, char **argv)
{
	int i, o = 0;
	out[0] = 0;
	for (i = 0; i < argc && o < max - 1; i++) o += snprintf(out + o, max - o, "%s%s", i ? " " : "", argv[i]);
}

static int cmd_note(int argc, char **argv)
{
	char t[512];
	const char *args[1];
	if (!argc) { err("note what? (epoc note TEXT)"); return LXE_USAGE; }
	join(t, sizeof t, argc, argv);
	args[0] = t;
	return ask(LXE_OP_NOTE, 0, 0, 1, args, 0);
}

static int cmd_alert(int op, int argc, char **argv)
{
	const char *args[4];
	int n = 0, i;
	answer_t a;
	int r;
	for (i = 0; i < argc && n < 4; i++) args[n++] = argv[i];
	if (!n) { err("%s what? (epoc %s LINE [LINE2])", op == LXE_OP_ALERT ? "alert" : "confirm", op == LXE_OP_ALERT ? "alert" : "confirm"); return LXE_USAGE; }
	if (n == 1) args[n++] = "";
	r = ask(op, 0, 0, n, args, &a);
	if (op == LXE_OP_CONFIRM && r == LXE_OK) return a.h.arg ? LXE_OK : LXE_FAILED;
	return r;
}

static int cmd_beep(int argc, char **argv)
{
	const char *args[1];
	unsigned f = argc ? (unsigned)atoi(argv[0]) : 0;
	args[0] = argc > 1 ? argv[1] : "200";
	return ask(LXE_OP_BEEP, 0, f, 1, args, 0);
}

static int cmd_shot(int argc, char **argv)
{
	const char *args[1];
	answer_t a;
	int i, n = 0, r;
	double delay = 0;
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--delay") && i + 1 < argc) delay = atof(argv[++i]);
		else if (!n) args[n++] = argv[i];
	}
	if (delay > 0 && nap(delay)) return LXE_INTERRUPTED;
	r = ask(LXE_OP_SHOT, n ? path_flag(args[0], LXE_F_PATH) : 0, 0, n, args, &a);
	if (r == LXE_OK && !opt_quiet) fwrite(a.text, 1, a.len, stdout);
	return r;
}

static int cmd_print(int argc, char **argv)
{
	const char *args[2];
	int n = 0;
	if (!argc) { err("print what? (epoc print FILE [PRINTER])"); return LXE_USAGE; }
	args[n++] = argv[0];
	if (argc > 1) args[n++] = argv[1];
	return ask(LXE_OP_PRINT, path_flag(argv[0], LXE_F_PATH), 0, n, args, 0);
}

static int cmd_path(int argc, char **argv)
{
	int i, r = LXE_OK;
	if (!argc) { err("path of what? (epoc path FILE)"); return LXE_USAGE; }
	for (i = 0; i < argc; i++) {
		const char *args[1];
		answer_t a;
		int k;
		args[0] = argv[i];
		k = ask(LXE_OP_PATH, LXE_F_PATH, 0, 1, args, &a);
		if (k == LXE_OK) fwrite(a.text, 1, a.len, stdout);
		else r = k;
	}
	return r;
}

static int cmd_syswrite(int argc, char **argv)
{
	answer_t a;
	unsigned arg = 0;
	int r;
	if (argc && !strcmp(argv[0], "on")) arg = 1;
	else if (argc && !strcmp(argv[0], "off")) arg = 2;
	else if (argc) { err("syswrite [on --force | off]"); return LXE_USAGE; }
	r = ask(LXE_OP_SYSWRITE, opt_force ? LXE_F_FORCE : 0, arg, 0, 0, &a);
	if (r == LXE_OK && (!arg || !opt_quiet)) fwrite(a.text, 1, a.len, stdout);
	return r;
}

static int cmd_wait_file(int argc, char **argv)
{
	int i, gone = 0, change = 0;
	const char *path = 0;
	struct stat st0;
	int had;
	double until;
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--gone")) gone = 1;
		else if (!strcmp(argv[i], "--change")) change = 1;
		else path = argv[i];
	}
	if (!path) { err("wait-file [--gone | --change] [--timeout S] PATH"); return LXE_USAGE; }
	had = stat(path, &st0) == 0;
	until = opt_timeout > 0 ? now_s() + opt_timeout : 0;
	for (;;) {
		struct stat st;
		int there = stat(path, &st) == 0;
		if (gone && !there) return LXE_OK;
		if (!gone && !change && there) return LXE_OK;
		if (change && there != had) return LXE_OK;
		if (change && there && (st.st_mtime != st0.st_mtime || st.st_size != st0.st_size)) return LXE_OK;
		if (until && now_s() >= until) return LXE_BUSY;
		if (nap(1)) return LXE_INTERRUPTED;
	}
}

static int battery(int *pc, int *mains)
{
	answer_t a;
	int r = ask(LXE_OP_INFO, 0, 0, 0, 0, &a);
	char *s;
	*pc = -1;
	*mains = 0;
	if (r != LXE_OK) return r;
	if ((s = strstr(a.text, "battery_percent\t")) != 0) *pc = atoi(s + 16);
	if ((s = strstr(a.text, "mains\t")) != 0) *mains = !strncmp(s + 6, "yes", 3);
	return LXE_OK;
}

static int cmd_wait_battery(int argc, char **argv)
{
	int below = 0, want = -1, i;
	double until;
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--below")) below = 1;
		else want = atoi(argv[i]);
	}
	if (want < 0 || want > 100) { err("wait-battery [--below] [--timeout S] PERCENT"); return LXE_USAGE; }
	until = opt_timeout > 0 ? now_s() + opt_timeout : 0;
	for (;;) {
		int pc, mains, r = battery(&pc, &mains);
		if (r != LXE_OK) return r;
		if (pc < 0) { err("this Psion does not say how full its battery is"); return LXE_UNSUPPORTED; }
		if (below ? pc < want : pc >= want) return LXE_OK;
		if (until && now_s() >= until) return LXE_BUSY;
		if (nap(30)) return LXE_INTERRUPTED;
	}
}

static int cmd_wait(int argc, char **argv)
{
	answer_t a;
	const char *name;
	unsigned id = 0;
	char *end;
	if (!argc) { err("wait [--timeout S] ID|NAME"); return LXE_USAGE; }
	name = argv[0];
	id = (unsigned)strtoul(name, &end, 0);
	if (*end || !*name) {
		/* a name: its task's thread id, from the task list */
		int r = ask(LXE_OP_TASKS, 0, 0, 0, 0, &a);
		char *s = a.text;
		id = 0;
		if (r != LXE_OK) return r;
		while (s && *s) {
			char *e = strchr(s, '\n'), *t1 = strchr(s, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : 0, *t3 = t2 ? strchr(t2 + 1, '\t') : 0;
			if (t3 && (e == 0 || t3 < e) && (size_t)(t3 - t2 - 1) == strlen(name) && !strncasecmp(t2 + 1, name, t3 - t2 - 1)) { id = (unsigned)strtoul(s, 0, 10); break; }
			s = e ? e + 1 : 0;
		}
		if (!id) { err("%s is not running", name); return LXE_NOTFOUND; }
	}
	return wait_gone(id, opt_timeout);
}

static void completion(void)
{
	FILE *f = fopen("/psion/bin/epoc-completion.bash", "r");
	char b[512];
	size_t n;
	if (!f) { err("no /psion/bin/epoc-completion.bash"); return; }
	while ((n = fread(b, 1, sizeof b, f)) > 0) fwrite(b, 1, n, stdout);
	fclose(f);
}

static const char help_text[] =
"epoc " EPOC_VERSION " - your Psion from the shell (PsiLinux's epoc command; epoc version gives PsiLinux's own)\n"
"\n"
"usage: epoc [-H] [--json] [-q] [--force] [--timeout S] COMMAND [ARGS]\n"
"\n"
"Programs and files\n"
"  open [--wait] [--bg] FILE...    open files in their programs (as a tap on the System screen)\n"
"  run [--wait] [--new] [--bg] PROGRAM [FILE]\n"
"                                  start a program, or switch to it: a name (Word, Agenda),\n"
"                                  a 0xUID or an .app file; with FILE, open that file in it\n"
"                                  open and run return once the program has started (its window\n"
"                                  on the task list, in front unless --bg), so time epoc run Word\n"
"                                  is Word's start; --no-wait returns at once, as EPOC is asked;\n"
"                                  --wait also waits until the program closes\n"
"  apps                            the programs there are: uid, caption, file\n"
"  tasks                           the task list: id, uid, caption, document, fg\n"
"  ps                              EPOC's processes: id, name, uid, memory KB, priority, file, sys\n"
"  close NAME|0xUID|ID             ask a program to close (it saves, as from the task list)\n"
"  kill [--force] NAME|ID          end a process now; EPOC's own are refused\n"
"  wait NAME|ID                    until that program has ended\n"
"  path FILE                       the EPOC name of a file (/psion/c/Documents -> C:\\Documents)\n"
"\n"
"The machine\n"
"  info [KEY]                      machine, rom, edition, memory, battery, disks\n"
"  time [KEY], locale [KEY]        EPOC's clock and its time zone; its country settings\n"
"  printers                        the printer drivers there are\n"
"  print FILE [PRINTER]            (EPOC has no \"print this file\": open it and print from its program)\n"
"  shot [--delay S] [FILE]         a screenshot (Coda's Screenshot where it is there)\n"
"  wait-battery [--below] PERCENT  until the battery is at least (--below: under) PERCENT\n"
"  wait-file [--gone|--change] PATH  until the file is there (gone, changed)\n"
"\n"
"Talking to you\n"
"  note TEXT                       an infoprint\n"
"  beep [HZ [MS]]                  a beep\n"
"  alert LINE [LINE2]              a message; returns when it is dismissed\n"
"  confirm LINE [LINE2]            asks: exit 0 for yes, 1 for no\n"
"  clip get | clip set [TEXT]      EPOC's clipboard as text (set reads stdin without TEXT)\n"
"\n"
"PsiLinux\n"
"  syswrite [on --force | off]     let Linux change the \\System folders (off again at every start)\n"
"  version, help [exit], completion\n"
"\n"
"Options: -H a heading line, --json JSON, -q quiet, --force (kill, syswrite), --timeout S (wait, --wait,\n"
"  and a start: 60 s as standard)\n"
"The disks are /psion/c, /psion/d... (and /c, /d); Z: and every \\System folder are read-only.\n"
"xdg-open and open are epoc open. Only the Psion's own terminal may use it, never ssh.\n";

static const char exit_text[] =
"exit codes:\n"
"  0  done (confirm: yes)\n"
"  1  EPOC could not do it (confirm: no)\n"
"  2  a bad command line\n"
"  3  not found: no such program, task, process or file\n"
"  4  refused: a system process, \\System, or not the Psion's own terminal\n"
"  5  not available on this machine\n"
"  6  timed out (--timeout)\n"
"  7  PsiLinux's window does not answer\n"
"  8  PsiLinux is older than this request\n"
"  9  interrupted\n";

static const char *const tasks_cols[] = { "id", "uid", "caption", "document", "fg", 0 };
static const char *const ps_cols[] = { "id", "name", "uid", "mem_kb", "priority", "file", "sys", 0 };
static const char *const apps_cols[] = { "uid", "caption", "file", 0 };
static const char *const printers_cols[] = { "name", "file", 0 };

#ifndef EPOC_NO_MAIN
int main(int argc, char **argv)
{
	const char *me = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	const char *cmd;
	int i = 1;
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, 0);
	sigaction(SIGTERM, &sa, 0);
	if (getenv("EPOC_DEVICE")) dev_path = getenv("EPOC_DEVICE");   /* (tests) */
	if (!strcmp(me, "xdg-open") || !strcmp(me, "open")) {
		if (argc < 2 || !strcmp(argv[1], "--help")) { printf("usage: %s FILE...  (epoc open: the file in its EPOC program)\n", me); return argc < 2 ? LXE_USAGE : 0; }
		return cmd_open(argc - 1, argv + 1);
	}
	while (i < argc && argv[i][0] == '-' && argv[i][1]) {
		const char *o = argv[i];
		if (!strcmp(o, "-H")) opt_heading = 1;
		else if (!strcmp(o, "--json") || !strcmp(o, "-j")) opt_json = 1;
		else if (!strcmp(o, "-q") || !strcmp(o, "--quiet")) opt_quiet = 1;
		else if (!strcmp(o, "--force") || !strcmp(o, "-f")) opt_force = 1;
		else if (!strcmp(o, "--timeout") && i + 1 < argc) opt_timeout = atof(argv[++i]);
		else if (!strcmp(o, "--help") || !strcmp(o, "-h")) { fputs(help_text, stdout); return 0; }
		else if (!strcmp(o, "--version")) { puts("epoc " EPOC_VERSION); return 0; }
		else { err("no option %s (epoc help)", o); return LXE_USAGE; }
		i++;
	}
	if (i >= argc) { fputs(help_text, stderr); return LXE_USAGE; }
	cmd = argv[i++];
	/* options after the command too (epoc ps -H, epoc kill --force x) */
	{
		int j, k = i;
		for (j = i; j < argc; j++) {
			if (!strcmp(argv[j], "-H")) opt_heading = 1;
			else if (!strcmp(argv[j], "--json")) opt_json = 1;
			else if (!strcmp(argv[j], "-q")) opt_quiet = 1;
			else if (!strcmp(argv[j], "--force")) opt_force = 1;
			else if (!strcmp(argv[j], "--timeout") && j + 1 < argc) opt_timeout = atof(argv[++j]);
			else argv[k++] = argv[j];
		}
		argc = k;
	}
	argc -= i;
	argv += i;
	if (!strcmp(cmd, "help")) {
		if (argc && !strcmp(argv[0], "exit")) fputs(exit_text, stdout);
		else { fputs(help_text, stdout); fputs("\n", stdout); fputs(exit_text, stdout); }
		return 0;
	}
	if (!strcmp(cmd, "version")) {
		answer_t a;
		printf("epoc\t%s\n", EPOC_VERSION);
		if (ask_q(LXE_OP_HELLO, 0, 0, 0, 0, &a, 1) == LXE_OK) fwrite(a.text, 1, a.len, stdout);
		return 0;
	}
	if (!strcmp(cmd, "completion")) { completion(); return 0; }
	if (!strcmp(cmd, "open")) return cmd_open(argc, argv);
	if (!strcmp(cmd, "run") || !strcmp(cmd, "start")) return cmd_run(argc, argv);
	if (!strcmp(cmd, "apps")) return cmd_list(LXE_OP_APPS, apps_cols);
	if (!strcmp(cmd, "tasks")) return cmd_list(LXE_OP_TASKS, tasks_cols);
	if (!strcmp(cmd, "ps")) return cmd_list(LXE_OP_PS, ps_cols);
	if (!strcmp(cmd, "printers")) return cmd_list(LXE_OP_PRINTERS, printers_cols);
	if (!strcmp(cmd, "close")) return cmd_target(LXE_OP_CLOSE, argc, argv);
	if (!strcmp(cmd, "kill")) return cmd_target(LXE_OP_KILL, argc, argv);
	if (!strcmp(cmd, "info")) return cmd_keys(LXE_OP_INFO, argc, argv);
	if (!strcmp(cmd, "time")) return cmd_keys(LXE_OP_TIME, argc, argv);
	if (!strcmp(cmd, "locale")) return cmd_keys(LXE_OP_LOCALE, argc, argv);
	if (!strcmp(cmd, "clip")) return cmd_clip(argc, argv);
	if (!strcmp(cmd, "note")) return cmd_note(argc, argv);
	if (!strcmp(cmd, "beep")) return cmd_beep(argc, argv);
	if (!strcmp(cmd, "alert")) return cmd_alert(LXE_OP_ALERT, argc, argv);
	if (!strcmp(cmd, "confirm")) return cmd_alert(LXE_OP_CONFIRM, argc, argv);
	if (!strcmp(cmd, "shot")) return cmd_shot(argc, argv);
	if (!strcmp(cmd, "print")) return cmd_print(argc, argv);
	if (!strcmp(cmd, "path")) return cmd_path(argc, argv);
	if (!strcmp(cmd, "syswrite")) return cmd_syswrite(argc, argv);
	if (!strcmp(cmd, "wait")) return cmd_wait(argc, argv);
	if (!strcmp(cmd, "wait-file")) return cmd_wait_file(argc, argv);
	if (!strcmp(cmd, "wait-battery")) return cmd_wait_battery(argc, argv);
	err("no command %s (epoc help)", cmd);
	return LXE_USAGE;
}
#endif
