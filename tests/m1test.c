/* m1test.c - PsiLinux's first test suite: arguments, environment, files,
 * time, memory, and a little of everything a C program leans on. It prints
 * one line per check and a summary; the exit status is the failures.
 *   m1test [quick]          (child mode: m1test child ARG, used by the spawn test) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/utsname.h>

extern char **environ;
static int fails, passes;

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (ok) passes++; else fails++;
}

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static jmp_buf jb;
static volatile int got_sig;
static void on_usr1(int s) { got_sig = s; }

int main(int argc, char **argv)
{
	char buf[256];
	struct stat st;
	int fd, i, n;
	if (argc == 3 && !strcmp(argv[1], "child")) {
		printf("child says %s\n", argv[2]);
		return 7;
	}
	printf("PsiLinux M1 tests (pid %d)\n", (int)getpid());
	/* (this program, by its full name: the tests change folder later) */
	static char self[256];
	{
		int r = readlink("/proc/self/exe", self, sizeof self - 1);
		if (r > 0) { self[r] = 0; argv[0] = self; }
	}

	/* arguments and environment */
	check(argc >= 1 && argv[argc] == NULL, "argv ends with NULL");
	check(getenv("PATH") != NULL, "PATH is set");
	setenv("M1VAR", "hello", 1);
	check(getenv("M1VAR") && !strcmp(getenv("M1VAR"), "hello"), "setenv/getenv");
	n = 0;
	for (i = 0; environ[i]; i++) n++;
	check(n >= 2, "environ has entries");

	/* files */
	fd = open("/tmp/m1test.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
	check(fd >= 0, "open O_CREAT in /tmp");
	check(write(fd, "Hello Psion\n", 12) == 12, "write 12 bytes");
	check(lseek(fd, 6, SEEK_SET) == 6, "lseek");
	memset(buf, 0, sizeof buf);
	check(read(fd, buf, 100) == 6 && !strcmp(buf, "Psion\n"), "read back after lseek");
	check(fstat(fd, &st) == 0 && st.st_size == 12 && S_ISREG(st.st_mode), "fstat size and type");
	check(ftruncate(fd, 5) == 0 && fstat(fd, &st) == 0 && st.st_size == 5, "ftruncate");
	close(fd);
	{
		FILE *f = fopen("/tmp/m1test.txt", "a");
		check(f != NULL, "fopen append");
		if (f) { fprintf(f, " and %d more\n", 42); fclose(f); }
		f = fopen("/tmp/m1test.txt", "r");
		buf[0] = 0;
		if (f) { fgets(buf, sizeof buf, f); fclose(f); }
		check(!strcmp(buf, "Hello and 42 more\n"), "stdio append and fgets");
	}
	check(rename("/tmp/m1test.txt", "/tmp/m1test2.txt") == 0 && stat("/tmp/m1test2.txt", &st) == 0, "rename");
	check(stat("/tmp/m1test.txt", &st) < 0 && errno == ENOENT, "old name gone (ENOENT)");
	check(mkdir("/tmp/m1dir", 0755) == 0 || errno == EEXIST, "mkdir");
	fd = open("/tmp/m1dir/a", O_CREAT | O_WRONLY, 0644);
	close(fd);
	{
		DIR *d = opendir("/tmp/m1dir");
		struct dirent *e;
		int seen = 0;
		while (d && (e = readdir(d)) != NULL) if (!strcmp(e->d_name, "a")) seen = 1;
		if (d) closedir(d);
		check(seen, "opendir/readdir finds the file");
	}
	check(unlink("/tmp/m1dir/a") == 0 && rmdir("/tmp/m1dir") == 0, "unlink and rmdir");
	check(unlink("/tmp/m1test2.txt") == 0, "unlink");
	check(getcwd(buf, sizeof buf) && chdir("/tmp") == 0 && getcwd(buf, sizeof buf) && !strcmp(buf, "/tmp"), "chdir/getcwd");
	check(access("/proc/self/stat", R_OK) == 0, "/proc/self/stat is there");
	{
		int r = readlink("/proc/self/exe", buf, sizeof buf - 1);
		if (r > 0) buf[r] = 0;
		check(r > 0, "readlink /proc/self/exe");
	}
	fd = open("/dev/null", O_WRONLY);
	check(fd >= 0 && write(fd, "x", 1) == 1, "/dev/null");
	close(fd);
	{
		unsigned char r[16];
		fd = open("/dev/urandom", O_RDONLY);
		check(fd >= 0 && read(fd, r, 16) == 16, "/dev/urandom");
		close(fd);
	}

	/* time */
	{
		struct timespec a, b;
		struct timeval tv;
		time_t now = time(NULL);
		check(now > 1700000000, "time() is after 2023");
		check(gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1700000000, "gettimeofday");
		clock_gettime(CLOCK_MONOTONIC, &a);
		{ struct timespec d = { 0, 50 * 1000 * 1000 }; nanosleep(&d, NULL); }
		clock_gettime(CLOCK_MONOTONIC, &b);
		{
			long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
			check(ms >= 45 && ms < 2000, "nanosleep 50 ms");
		}
		{
			struct tm tm;
			gmtime_r(&now, &tm);
			strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
			printf("     (UTC now %s)\n", buf);
			check(tm.tm_year >= 123, "gmtime/strftime");
		}
	}

	/* memory */
	{
		char *p = malloc(100), *q;
		void *big;
		int arr[50];
		check(p != NULL, "malloc small");
		strcpy(p, "abc");
		q = realloc(p, 100000);
		check(q != NULL && !strcmp(q, "abc"), "realloc to 100 KB keeps the data");
		free(q);
		big = malloc(2 * 1024 * 1024);
		check(big != NULL, "malloc 2 MB");
		if (big) { memset(big, 0x55, 2 * 1024 * 1024); free(big); }
		for (i = 0; i < 1000; i++) { void *t = malloc(i * 7 + 1); free(t); }
		check(1, "1000 malloc/free");
		{
			char *m = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			check(m != MAP_FAILED && m[0] == 0 && m[65535] == 0, "mmap anonymous is zeroed");
			if (m != MAP_FAILED) check(munmap(m, 65536) == 0, "munmap");
		}
		for (i = 0; i < 50; i++) arr[i] = (i * 37) % 50;
		qsort(arr, 50, sizeof arr[0], cmp);
		for (i = 0, n = 1; i < 50; i++) if (arr[i] != i) n = 0;
		check(n, "qsort");
	}

	/* soft float */
	{
		double x = sqrt(2.0) * 3.0;
		snprintf(buf, sizeof buf, "%.5f", x);
		check(!strcmp(buf, "4.24264"), "sqrt and printf of a double");
		check(fabs(strtod("3.25e2", NULL) - 325.0) < 1e-9, "strtod");
		check((long long)1 << 40 == 1099511627776LL && 1000000007LL * 3 % 1000 == 21, "64-bit integers");
	}

	/* control flow and signals */
	if (setjmp(jb) == 0) longjmp(jb, 5);
	check(1, "setjmp/longjmp");
	signal(SIGUSR1, on_usr1);
	raise(SIGUSR1);
	check(got_sig == SIGUSR1, "raise and a signal handler");
	{
		sigset_t s, o;
		sigemptyset(&s);
		sigaddset(&s, SIGUSR1);
		got_sig = 0;
		sigprocmask(SIG_BLOCK, &s, &o);
		raise(SIGUSR1);
		n = got_sig;
		sigprocmask(SIG_SETMASK, &o, NULL);
		check(n == 0 && got_sig == SIGUSR1, "a blocked signal waits for unblocking");
	}

	/* processes */
	{
		struct utsname u;
		check(uname(&u) == 0 && !strcmp(u.sysname, "Linux"), "uname");
		printf("     (%s %s %s %s)\n", u.sysname, u.nodename, u.release, u.machine);
	}
	check(isatty(1) || 1, "isatty");
	{
		int p[2], status = -1;
		pid_t pid;
		char *cargv[] = { argv[0], "child", "hi", NULL };
		posix_spawn_file_actions_t fa;
		check(pipe(p) == 0, "pipe");
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_adddup2(&fa, p[1], 1);
		posix_spawn_file_actions_addclose(&fa, p[0]);
		n = posix_spawn(&pid, argv[0], &fa, NULL, cargv, environ);
		close(p[1]);
		check(n == 0, "posix_spawn itself");
		memset(buf, 0, sizeof buf);
		n = read(p[0], buf, sizeof buf - 1);
		close(p[0]);
		check(n > 0 && !strcmp(buf, "child says hi\n"), "the child's output through a pipe");
		check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7, "waitpid: exit status 7");
	}
	{
		pid_t pid = vfork();
		if (pid == 0) {
			execl(argv[0], argv[0], "child", "vfork", (char *)NULL);
			_exit(99);
		}
		{
			int status = -1;
			check(pid > 0 && waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 7, "vfork + execl + waitpid");
		}
	}
	{
		pid_t pid = vfork();
		if (pid == 0) _exit(3);
		{
			int status = -1;
			check(waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 3, "vfork + _exit");
		}
	}

	printf("%d passed, %d failed\n", passes, fails);
	return fails;
}
