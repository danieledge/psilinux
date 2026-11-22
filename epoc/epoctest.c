/* epoctest.c - host tests of the epoc command's request format and output
 * (common/lxepoc.h, epoc.c), with the host's gcc:
 *   make -C epoc test
 * The whole path (the kernel's /dev/epoc, its path names, a pretend app) is
 * tests/epoctest.sh, on psihost. */
#define EPOC_NO_MAIN
#include "epoc.c"
#include <stddef.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* what f printed, run with stdout to a file */
static char out[8192];
static void capture(void (*f)(void *), void *arg)
{
	FILE *t = tmpfile();
	int saved = dup(1), n;
	fflush(stdout);
	dup2(fileno(t), 1);
	f(arg);
	fflush(stdout);
	dup2(saved, 1);
	close(saved);
	rewind(t);
	n = (int)fread(out, 1, sizeof out - 1, t);
	out[n < 0 ? 0 : n] = 0;
	fclose(t);
}

static void table_ps(void *a) { print_table((const char *)a, ps_cols); }
static void keys_info(void *a) { print_keys((const char *)a, 0); }
static void key_battery(void *a) { print_keys((const char *)a, "battery_percent"); }

int main(void)
{
	char buf[HDR + LXE_MAX + 8];
	LxeHeader h;
	answer_t a;
	int n;

	/* the header: 28 bytes, fixed offsets (the Psion's compilers lay it out the same) */
	CHECK(sizeof(LxeHeader) == 28);
	CHECK(offsetof(LxeHeader, op) == 6 && offsetof(LxeHeader, flags) == 8 && offsetof(LxeHeader, status) == 12);
	CHECK(offsetof(LxeHeader, err) == 16 && offsetof(LxeHeader, arg) == 20 && offsetof(LxeHeader, len) == 24);

	/* a request: the strings with their NULs, little-endian header */
	{
		const char *args[2] = { "Word", "/psion/c/Documents/Letter" };
		n = epoc_encode(buf, sizeof buf, LXE_OP_RUN, LXE_F_PATH2, 7, 2, args);
		CHECK(n == HDR + 5 + 26);
		CHECK((unsigned char)buf[0] == 0x45 && buf[1] == 'P' && buf[2] == 'O' && buf[3] == 'C');   /* "EPOC" */
		memcpy(&h, buf, HDR);
		CHECK(h.version == LXE_VERSION && h.op == LXE_OP_RUN && h.flags == LXE_F_PATH2 && h.arg == 7 && h.len == 31);
		CHECK(!strcmp(buf + HDR, "Word") && !strcmp(buf + HDR + 5, "/psion/c/Documents/Letter"));
	}
	/* too long: refused, never overrun */
	{
		static char big[LXE_MAX + 10];
		const char *args[1] = { big };
		memset(big, 'x', sizeof big - 1);
		CHECK(epoc_encode(buf, sizeof buf, LXE_OP_CLIPSET, 0, 0, 1, args) == -1);
		big[LXE_MAX - 1] = 0;          /* (LXE_MAX with its NUL: fits) */
		CHECK(epoc_encode(buf, sizeof buf, LXE_OP_CLIPSET, 0, 0, 1, args) == HDR + LXE_MAX);
	}
	/* answers: checked before use */
	{
		memset(&h, 0, sizeof h);
		h.magic = LXE_MAGIC; h.version = LXE_VERSION; h.op = LXE_OP_PS; h.status = LXE_OK; h.len = 3;
		memcpy(buf, &h, HDR);
		memcpy(buf + HDR, "abc", 3);
		CHECK(epoc_decode(buf, HDR + 3, LXE_OP_PS, &a) == 0 && a.len == 3 && !strncmp(a.text, "abc", 3));
		CHECK(epoc_decode(buf, HDR + 2, LXE_OP_PS, &a) == -1);         /* a short read */
		CHECK(epoc_decode(buf, HDR + 3, LXE_OP_TASKS, &a) == -1);      /* another request's answer */
		CHECK(epoc_decode(buf, HDR - 1, 0, &a) == -1);
		h.magic = 0;
		memcpy(buf, &h, HDR);
		CHECK(epoc_decode(buf, HDR + 3, LXE_OP_PS, &a) == -1);
	}
	/* EPOC names are passed as they are, Linux paths get the kernel's PATH flag */
	CHECK(is_epoc_name("C:\\Documents\\Letter") && is_epoc_name("d:\\") && is_epoc_name("C:"));
	CHECK(!is_epoc_name("/psion/c/Letter") && !is_epoc_name("Letter") && !is_epoc_name("C:x"));
	CHECK(path_flag("Letter.docx", LXE_F_PATH) == LXE_F_PATH && path_flag("C:\\x", LXE_F_PATH) == 0);
	/* JSON numbers: plain decimals only (UIDs and 007 stay strings) */
	CHECK(is_number("212", 3) && is_number("-1", 2) && is_number("0", 1));
	CHECK(!is_number("0x1000007F", 10) && !is_number("007", 3) && !is_number("", 0) && !is_number("1.5", 3));

	/* output: tab-separated as EPOC said, -H a heading, --json objects */
	{
		const char *ps = "77\tWord\t0x1000007F\t212\t\tZ:\\System\\Apps\\Word\\Word.app\t\n5\tEFile\t0x100000B6\t48\t\tZ:\\x\tsys\n";
		capture(table_ps, (void *)ps);
		CHECK(!strcmp(out, ps));
		opt_heading = 1;
		capture(table_ps, (void *)ps);
		CHECK(!strncmp(out, "id\tname\tuid\tmem_kb\tpriority\tfile\tsys\n77\t", 39));
		opt_heading = 0;
		opt_json = 1;
		capture(table_ps, (void *)ps);
		CHECK(strstr(out, "{\"id\": 77, \"name\": \"Word\", \"uid\": \"0x1000007F\", \"mem_kb\": 212, \"priority\": \"\", \"file\": \"Z:\\\\System\\\\Apps\\\\Word\\\\Word.app\", \"sys\": \"\"}") != 0);
		CHECK(out[0] == '[' && strstr(out, "\"sys\": \"sys\"}\n]") != 0);
		capture(keys_info, (void *)"machine\tPsion netBook\nfree_kb\t9000\ndisk\tC\tInternal\t16384\t8000\n");
		CHECK(strstr(out, "\"machine\": \"Psion netBook\"") && strstr(out, "\"free_kb\": 9000") && strstr(out, "\"disk\": [\"C\", \"Internal\", 16384, 8000]"));
		/* (0.37, RN-18) one key: plain, and with --json an object */
		capture(key_battery, (void *)"machine\tPsion netBook\nbattery_percent\t94\n");
		CHECK(!strcmp(out, "{\"battery_percent\": 94}\n"));
		opt_json = 0;
		capture(key_battery, (void *)"machine\tPsion netBook\nbattery_percent\t94\n");
		CHECK(!strcmp(out, "94\n"));
	}
	/* (0.38, RR-05) a task on the list by its thread id, in front or not */
	{
		const char *t = "101\t0x1000007F\tWord\tC:\\Documents\\Letter\t\n1012\t0x10000101\tCalc\t\tfg\n";
		int fg = -1;
		CHECK(task_state(t, 101, &fg) && fg == 0);
		CHECK(task_state(t, 1012, &fg) && fg == 1);
		CHECK(!task_state(t, 10, &fg) && !task_state("", 101, &fg));
		CHECK(task_state("7\t0x1\tA\t\tfg", 7, &fg) && fg == 1);    /* (no last newline) */
	}
	printf("epoctest: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
