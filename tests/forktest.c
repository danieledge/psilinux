/* forktest.c - fork without an MMU: the child's changes must not reach the
 * parent, and fork-then-exec, popen and system() must work */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int counter = 10;
static int fails, passes;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (ok) passes++; else fails++; }

static int child_work(int *heap)
{
	counter = 99;                 /* the parent must still see 10 */
	heap[0] = 77;
	return 5;
}

int main(void)
{
	int *heap = malloc(64 * sizeof(int));
	int st = -1, local = 1;
	pid_t pid;
	char buf[128];
	FILE *f;
	heap[0] = 1;
	pid = fork();
	if (pid == 0) {
		local = 42;
		_exit(child_work(heap));
	}
	check(pid > 0, "fork returned a pid");
	check(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 5, "the child's exit status");
	check(counter == 10, "a global changed by the child is put back");
	check(heap[0] == 1, "heap memory changed by the child is put back");
	check(local == 1, "a local changed by the child is put back");
	pid = fork();
	if (pid == 0) {
		execl("/bin/echo", "echo", "child exec ok", (char *)NULL);
		_exit(127);
	}
	check(waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 0, "fork then exec");
	f = popen("echo from popen", "r");
	buf[0] = 0;
	if (f) { fgets(buf, sizeof buf, f); pclose(f); }
	check(!strcmp(buf, "from popen\n"), "popen");
	check(system("exit 3") == 3 << 8, "system");
	printf("%d passed, %d failed\n", passes, fails);
	return fails;
}
