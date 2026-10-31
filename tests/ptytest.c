/* ptytest.c - pseudo-terminals (lx_pty.c): posix_openpt, grantpt, unlockpt,
 * ptsname; the line discipline between master and slave; a shell on the
 * slave as its controlling terminal; the window size; hangup.
 *   lx-cc -static-pie -o ptytest ptytest.c */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* everything the master can read within ms */
static int drain(int m, char *buf, int max, int ms)
{
	int n = 0;
	struct pollfd pf = { m, POLLIN, 0 };
	while (n < max - 1 && poll(&pf, 1, ms) == 1) {
		int r = (int)read(m, buf + n, max - 1 - n);
		if (r <= 0) break;
		n += r;
		ms = 300;
	}
	buf[n] = 0;
	return n;
}

int main(void)
{
	char buf[512], *name;
	int m = posix_openpt(O_RDWR | O_NOCTTY), s, st;
	struct winsize ws = { 30, 100, 0, 0 };
	pid_t pid;
	CHECK(m >= 0, "posix_openpt: %s", strerror(errno));
	if (m < 0) return 1;
	CHECK(grantpt(m) == 0, "grantpt");
	name = ptsname(m);
	CHECK(name && !strncmp(name, "/dev/pts/", 9), "ptsname %s", name ? name : "(none)");
	CHECK(open(name, O_RDWR | O_NOCTTY) < 0, "locked slave refused");
	CHECK(unlockpt(m) == 0, "unlockpt");
	s = open(name, O_RDWR | O_NOCTTY);
	CHECK(s >= 0, "open %s: %s", name, strerror(errno));
	CHECK(isatty(s) && !strcmp(ttyname(s), name), "ttyname %s", isatty(s) ? ttyname(s) : "(not a tty)");
	/* typed at the master: a line on the slave, echoed back */
	CHECK(write(m, "hello\r", 6) == 6, "type");
	CHECK(read(s, buf, sizeof buf) == 6 && !memcmp(buf, "hello\n", 6), "slave reads the line");
	CHECK(drain(m, buf, sizeof buf, 1000) == 7 && !strcmp(buf, "hello\r\n"), "echo: \"%s\"", buf);
	/* the slave's output: \n becomes \r\n */
	CHECK(write(s, "out\n", 4) == 4, "slave writes");
	CHECK(drain(m, buf, sizeof buf, 1000) == 5 && !strcmp(buf, "out\r\n"), "master reads \"%s\"", buf);
	CHECK(ioctl(m, TIOCSWINSZ, &ws) == 0, "TIOCSWINSZ on the master");
	close(s);
	/* a shell on the slave, as xterm runs one */
	pid = fork();
	if (pid == 0) {
		int t;
		setsid();
		t = open(name, O_RDWR);
		if (t < 0) _exit(10);
		dup2(t, 0); dup2(t, 1); dup2(t, 2);
		if (t > 2) close(t);
		execl("/bin/sh", "sh", "-c", "tty; stty size; [ -t 0 ] && echo interactive", (char *)0);
		_exit(11);
	}
	drain(m, buf, sizeof buf, 5000);
	CHECK(strstr(buf, name) != 0, "the shell's tty: \"%s\"", buf);
	CHECK(strstr(buf, "30 100") != 0, "stty size: \"%s\"", buf);
	CHECK(strstr(buf, "interactive") != 0, "stdin is a terminal");
	CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && !WEXITSTATUS(st), "shell status %x", st);
	/* every slave closed: the master reads EIO */
	CHECK(read(m, buf, sizeof buf) < 0 && errno == EIO, "EIO after the slave has gone (%s)", strerror(errno));
	close(m);
	/* busybox script runs a program on a pty of its own */
	printf("ptytest: %d passed, %d failed\n", passes, fails);
	return fails != 0;
}
