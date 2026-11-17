/* lxepoc.h - "EPOC requests": how a Linux program asks EPOC to do something
 * (open a file in its program, start a program, the clipboard, an
 * infoprint...). Shared by the epoc command (epoc/epoc.c, a Linux program),
 * the kernel (runtime/lx_epoc.c), the runner (runner/bk_epoc.cpp) and the
 * app (app/lxepoc.cpp), which does the work.
 *
 *   epoc  --write(request)-->  /dev/epoc  (the kernel: checks it, turns Linux
 *         <--read(answer)---               paths into EPOC ones, answers a few
 *                                          itself, queues the rest)
 *         the runner  --LxShared's epoc_* fields + the app's doorbell-->  PsiLinux.app
 *                     <--the answer in the same place + the runner's doorbell--
 *
 * A request is one write: an LxeHeader, then 'len' bytes. The answer is one
 * read: an LxeHeader (its 'status' and 'err' filled in), then 'len' bytes.
 * Arguments are UTF-8 strings, each ending in NUL. Answers are UTF-8 text:
 * lists are lines of tab-separated fields (the epoc command adds headings or
 * makes JSON of them). Everything is little-endian, as on the Psion.
 *
 * Versions: LXE_VERSION is the request format. A newer epoc command talking
 * to an older kernel gets LXE_BADVERSION and says so; requests only ever get
 * new numbers, never new meanings. */
#ifndef LXEPOC_H
#define LXEPOC_H

#define LXE_MAGIC     0x434f5045u     /* "EPOC" */
#define LXE_VERSION   1
#define LXE_MAX       16000           /* the most bytes after the header, either way */
#define LXE_DEVICE    "/dev/epoc"

typedef struct
	{
	unsigned int magic;               /* LXE_MAGIC */
	unsigned short version;           /* LXE_VERSION */
	unsigned short op;                /* LXE_OP_* (the answer: the same) */
	unsigned int flags;               /* LXE_F_* (the request) */
	int status;                       /* the answer: LXE_OK... (the request: 0) */
	int err;                          /* the answer: EPOC's error code, if it gave one (KErrNotFound -1...) */
	unsigned int arg;                 /* a number the request or answer carries (an id, a count) */
	unsigned int len;                 /* bytes after the header */
	} LxeHeader;                      /* 28 bytes */

/* requests (the ones marked K the kernel answers itself; the rest go to the app) */
enum
	{
	LXE_OP_HELLO = 1,       /* K  -> "proto\t1\npsilinux\t0.36\napp\t1\n" (app: 0 = the app does not answer) */
	LXE_OP_SYSWRITE = 2,    /* K  arg 0: ask, 1: allow writing to \System (needs LXE_F_FORCE), 2: stop -> "on"/"off" */
	LXE_OP_PATH = 3,        /* K  a Linux path (LXE_F_PATH) -> its EPOC name ("C:\Documents\Letter") */
	LXE_OP_OPEN = 16,       /*    the file (an EPOC name) -> arg: the program's thread id; "uid\tcaption\n" */
	LXE_OP_RUN = 17,        /*    a program (caption, file name or 0xUID), [a file] -> arg: thread id; "uid\tcaption\tstarted|switched\n" */
	LXE_OP_TASKS = 18,      /*    -> lines: thread id, uid, caption, document, "fg" or "" */
	LXE_OP_PS = 19,         /*    -> lines: process id, name, uid, memory KB, priority, file, "sys" or "" */
	LXE_OP_CLOSE = 20,      /*    a task (caption, 0xUID or thread id) -> asks it to close (TApaTask::EndTask) */
	LXE_OP_KILL = 21,       /*    a process (name or id) -> ends it; system processes refused (LXE_F_FORCE for some) */
	LXE_OP_INFO = 22,       /*    -> lines: key, value */
	LXE_OP_CLIPGET = 23,    /*    -> the clipboard's text */
	LXE_OP_CLIPSET = 24,    /*    text -> on the clipboard */
	LXE_OP_NOTE = 25,       /*    text -> an infoprint */
	LXE_OP_BEEP = 26,       /*    arg: frequency (0: EPOC's own beep); a second string, ms */
	LXE_OP_ALERT = 27,      /*    line 1, line 2 -> a message; answered when OK is pressed */
	LXE_OP_CONFIRM = 28,    /*    line 1, line 2, [yes, no] -> arg 1: yes, 0: no */
	LXE_OP_SHOT = 29,       /*    [the EPOC file to write] -> the file written */
	LXE_OP_TIME = 30,       /*    -> lines: key, value */
	LXE_OP_LOCALE = 31,     /*    -> lines: key, value */
	LXE_OP_PRINTERS = 32,   /*    -> lines: name, file */
	LXE_OP_PRINT = 33,      /*    a file [, printer] -> (EPOC has no "print this file": LXE_UNSUPPORTED) */
	LXE_OP_ALIVE = 34,      /*    arg: a thread or process id -> LXE_OK while it runs, LXE_NOTFOUND after */
	LXE_OP_APPS = 35,       /*    -> lines: uid, caption, file */
	LXE_OP_LAST = 35
	};

/* request flags */
#define LXE_F_FORCE   1u      /* --force: do what is otherwise refused */
#define LXE_F_PATH    2u      /* the first argument is a Linux path: the kernel makes it EPOC's */
#define LXE_F_PATH2   4u      /* ...and so is the second */
#define LXE_F_NEW     8u      /* run: a new copy, even if the program is running */
#define LXE_F_BG      16u     /* open/run: in the background (not brought to the front) */

/* answers (the epoc command's exit codes are these too) */
enum
	{
	LXE_OK = 0,
	LXE_FAILED = 1,         /* EPOC said no ('err' says why) */
	LXE_USAGE = 2,          /* (the epoc command's own: a bad command line) */
	LXE_NOTFOUND = 3,       /* no such program, task, process or file */
	LXE_REFUSED = 4,        /* not allowed: a system process, \System, not from the Psion's own terminal */
	LXE_UNSUPPORTED = 5,    /* not on this machine, or EPOC cannot */
	LXE_BUSY = 6,           /* (the epoc command's own: --wait gave up, a timeout) */
	LXE_NOAPP = 7,          /* PsiLinux's window does not answer (an older PsiLinux.app?) */
	LXE_BADVERSION = 8,     /* a request this kernel does not know */
	LXE_INTERRUPTED = 9     /* (the epoc command's own: a signal) */
	};

#endif
