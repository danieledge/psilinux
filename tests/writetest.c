/* writetest.c - a blocking write to a pipe or a stream socket writes
 * everything, however much bigger than the pipe (8 KB) it is, as Linux's
 * does; a non-blocking one may write less. Each writer is a program of its
 * own (exec'd: a fork's parent waits until its child execs), and the
 * reader reads the way BusyBox's wget does: non-blocking, through stdio,
 * with poll. Alpine's ssl_client counts on it: after a short write it went
 * on from the wrong place in its buffer, and wget https got 8 KB of zeros
 * in every 16 KB (0.37). A signal while it waits: it returns what went.
 * And a pipe that only a waiting fork parent reads holds all that is written.
 *   writetest            the checks; prints "N passed, M failed"
 *   writetest -w HOW FD  (the writer)
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define N 40000                 /* a call's worth: five pipes full */
#define CALLS 3
static unsigned char src[N * CALLS], dst[N * CALLS];
static int fails, passes;

static void check(int ok, const char *what)
{
	if (ok) { printf("ok   %s\n", what); passes++; }
	else { printf("FAIL %s\n", what); fails++; }
}

static void on_alarm(int sig) { (void)sig; }

static void fill(void)
{
	int i;
	for (i = 0; i < N * CALLS; i++) src[i] = (unsigned char)(i * 7 + 1 + (i >> 8)) | 1;
}

/* the writer: CALLS calls of N bytes each, by HOW; exits 0 if every call wrote all N */
static int writer(const char *how, int fd)
{
	int c, bad = 0;
	fill();
	for (c = 0; c < CALLS; c++) {
		const unsigned char *p = src + c * N;
		struct iovec iov[3] = { { (void *)p, 100 }, { (void *)(p + 100), N - 200 }, { (void *)(p + N - 100), 100 } };
		struct msghdr m;
		ssize_t r;
		memset(&m, 0, sizeof m);
		m.msg_iov = iov; m.msg_iovlen = 3;
		if (!strcmp(how, "write")) r = write(fd, p, N);
		else if (!strcmp(how, "writev")) r = writev(fd, iov, 3);
		else if (!strcmp(how, "send")) r = send(fd, p, N, 0);
		else if (!strcmp(how, "sendmsg")) r = sendmsg(fd, &m, 0);
		else if (!strcmp(how, "nonblock")) {
			/* non-blocking: short writes, then EAGAIN, are right; it goes on with poll */
			size_t done = 0;
			fcntl(fd, F_SETFL, O_NONBLOCK);
			while (done < N) {
				r = write(fd, p + done, N - done);
				if (r < 0 && errno == EAGAIN) { struct pollfd q = { fd, POLLOUT, 0 }; poll(&q, 1, 2000); continue; }
				if (r <= 0) return 3;
				done += r;
			}
			r = N;
		} else if (!strcmp(how, "signal")) {
			/* a signal while it waits for room: it returns what it wrote
			 * (the reader starts after 2 s), then goes on */
			struct sigaction sa;
			size_t done;
			memset(&sa, 0, sizeof sa);
			sa.sa_handler = on_alarm;          /* (no SA_RESTART) */
			sigaction(SIGALRM, &sa, 0);
			alarm(c == 0 ? 1 : 0);
			r = write(fd, p, N);
			if (c == 0 && (r <= 0 || r >= N)) { fprintf(stderr, "writetest: interrupted write gave %d\n", (int)r); return 5; }
			for (done = r > 0 ? r : 0; done < N; done += r)
				if ((r = write(fd, p + done, N - done)) <= 0) return 6;
			r = N;
		} else return 4;
		if (r != N) { fprintf(stderr, "writetest: %s wrote %d of %d\n", how, (int)r, N); bad = 1; }
	}
	return bad;
}

/* one writer, how, into a pipe (sock 0) or a socketpair (1): all of it, right? */
static void one(const char *self, const char *how, int sock)
{
	int sv[2], pid, st = -1, i, bad = 0;
	size_t got = 0;
	char what[80], fdname[16];
	FILE *f;
	if (sock ? socketpair(AF_UNIX, SOCK_STREAM, 0, sv) : pipe(sv)) { check(0, "pipe/socketpair"); return; }
	/* (a pipe's write end is sv[1]; either end of a socketpair writes) */
	pid = vfork();
	if (pid == 0) {
		close(sv[0]);
		sprintf(fdname, "%d", sv[1]);
		execl(self, self, "-w", how, fdname, (char *)0);
		_exit(9);
	}
	close(sv[1]);
	if (!strcmp(how, "signal")) sleep(2);
	f = fdopen(sv[0], "r");
	fcntl(sv[0], F_SETFL, O_NONBLOCK);
	while (got < sizeof dst) {
		size_t want = sizeof dst - got < 8192 ? sizeof dst - got : 8192;
		size_t n = fread(dst + got, 1, want, f);
		got += n;
		if (n == 0) {
			struct pollfd q = { sv[0], POLLIN, 0 };
			if (feof(f)) break;
			clearerr(f);
			if (poll(&q, 1, 5000) <= 0) break;
		}
	}
	fclose(f);
	waitpid(pid, &st, 0);
	for (i = 0; i < (int)got; i++) if (src[i] != dst[i]) bad++;
	snprintf(what, sizeof what, "%s of %d KB into a %s: each call wrote it all", how, N / 1000, sock ? "socketpair" : "pipe");
	check(WIFEXITED(st) && WEXITSTATUS(st) == 0, what);
	snprintf(what, sizeof what, "%s into a %s: %d KB, as written", how, sock ? "socketpair" : "pipe", N * CALLS / 1000);
	check(got == sizeof dst && !bad, what);
}

/* A fork child (no exec: here its parent waits until it is done) writes
 * 40 KB into a pipe that the parent reads only after: bash's $(...). With
 * via, it does so from a program it starts and waits for: $(a | b). The
 * pipe has to hold it all (0.37: it grows, as nobody else can read it). */
static void forked(const char *self, int via)
{
	int p[2], pid, st = -1;
	size_t got = 0;
	ssize_t n;
	char what[80], fdname[16];
	if (pipe(p)) { check(0, "pipe"); return; }
	pid = fork();
	if (pid == 0) {
		close(p[0]);
		if (via) {
			int w = vfork();
			if (w == 0) {
				sprintf(fdname, "%d", p[1]);
				execl(self, self, "-w", "write", fdname, (char *)0);
				_exit(9);
			}
			waitpid(w, &st, 0);
			_exit(WIFEXITED(st) ? WEXITSTATUS(st) : 8);
		}
		_exit(writer("write", p[1]));
	}
	close(p[1]);
	waitpid(pid, &st, 0);
	memset(dst, 0, sizeof dst);
	while (got < sizeof dst && (n = read(p[0], dst + got, sizeof dst - got)) > 0) got += n;
	close(p[0]);
	snprintf(what, sizeof what, "a fork child writes %d KB%s that its parent reads after", N * CALLS / 1000, via ? " through a program" : "");
	check(WIFEXITED(st) && WEXITSTATUS(st) == 0 && got == sizeof dst && !memcmp(src, dst, sizeof dst), what);
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "-w")) return writer(argv[2], atoi(argv[3]));
	setvbuf(stdout, 0, _IOLBF, 0);
	fill();
	one(argv[0], "write", 0);
	one(argv[0], "writev", 0);
	one(argv[0], "nonblock", 0);
	one(argv[0], "write", 1);
	one(argv[0], "writev", 1);
	one(argv[0], "send", 1);
	one(argv[0], "sendmsg", 1);
	one(argv[0], "nonblock", 1);
	one(argv[0], "signal", 0);
	one(argv[0], "signal", 1);
	forked(argv[0], 0);
	forked(argv[0], 1);
	printf("%d passed, %d failed\n", passes, fails);
	return fails != 0;
}
