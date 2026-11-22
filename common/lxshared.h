/* lxshared.h - memory shared between PsiLinux.app and psilxrun.exe (the
 * runner), as psishared.h is between PsiTerm and psissh.
 *
 * The app creates the global chunk, fills in the settings and starts the
 * runner. Each ring has one writer and one reader, so no locks:
 *   kbd: the app writes typed bytes, the runner reads (the terminal's input)
 *   out: the runner writes what programs print, the app reads (-> libvterm)
 * Doorbells (psibell.h) wake the other side: the app rings eng_bell when it
 * leaves keys, a resize, a request or room in 'out'; the runner rings
 * app_bell when it leaves output or its state changes. */
#ifndef LXSHARED_H
#define LXSHARED_H

#ifndef PSISHARED_H
/* (PsiBell comes from PsiTerm's psishared.h) */
#include "psishared.h"
#endif

#define LX_SHARED_NAME   "PsiLinuxTerm"
#define LX_SHARED_MAGIC  0x584e4c50u      /* 'PLNX' */
#define LX_SHARED_VERSION 11
#define LX_KBD_SIZE      2048
#define LX_OUT_SIZE      16384
#define LX_EPOC_BUF      16032         /* (6) an EPOC request or answer (lxepoc.h: header + LXE_MAX) */

enum
	{
	LXS_STARTING = 0,
	LXS_RUNNING,
	LXS_EXITED,                    /* the first program ended: exit_status */
	LXS_FAILED                     /* the runner could not start it: message says why */
	};

typedef struct
	{
	unsigned int magic;
	unsigned int version;
	volatile int rows;
	volatile int cols;
	volatile unsigned int resize_seq;  /* the app adds 1 after changing rows/cols */
	volatile int state;                /* LXS_* (the runner) */
	volatile int exit_status;          /* the wait status of the first program */
	volatile int quit;                 /* the app asks the runner to end every program and stop */
	char root[128];                    /* the Linux folder, e.g. "D:\Linux\" */
	char program[128];                 /* the first program, e.g. "/bin/sh" */
	char args[256];                    /* its arguments after argv[0], NUL separated, ending "\0\0" */
	char message[160];                 /* the runner's words, when it failed */

	volatile unsigned int kbd_head;    /* written by the app */
	volatile unsigned int kbd_tail;    /* written by the runner */
	unsigned char kbd[LX_KBD_SIZE];

	volatile unsigned int out_head;    /* written by the runner */
	volatile unsigned int out_tail;    /* written by the app */
	unsigned char out[LX_OUT_SIZE];

	unsigned int bell_magic;           /* PSI_BELL_MAGIC: the app rings eng_bell */
	PsiBell app_bell;                  /* the runner rings, the app waits */
	PsiBell eng_bell;                  /* the app rings, the runner waits */
	volatile unsigned int app_beat;    /* the app adds 1 twice a second; the runner ends if it stops */

	/* for the status line: what runs now (the runner updates them) */
	volatile int nprocs;
	volatile unsigned int mem_used;    /* bytes the programs have */
	volatile unsigned int mem_free;    /* bytes the Psion has free */
	char fg_name[32];                  /* the foreground job's program */
	volatile unsigned int info_seq;    /* +1 when any of those change */
	char release[32];                  /* Alpine mode: /etc/alpine-release (the runner), for About */
	volatile unsigned int reserve;     /* (3) bytes of RAM Linux leaves free for EPOC's programs, 0 = none (the app; may change while running) */
	int log_on;                        /* (4) 1: Tools > Keep a log is ticked (the app); else the runner
	                                      writes no PsiLinux.log at all. Read when the runner starts */
	volatile unsigned int fb_seq;      /* (5) the runner adds 1 when the Linux screen (lxfb.h) opens or closes */
	volatile int fb_on;                /* (5) 1: a program has /dev/fb0 open: the chunk LX_FB_NAME is there */

	/* (6) EPOC requests (lxepoc.h, the epoc command), one at a time: the
	 * runner writes epoc_req and adds 1 to epoc_req_seq, and rings app_bell;
	 * the app does it, writes epoc_ans, sets epoc_ans_seq to epoc_req_seq
	 * and rings eng_bell */
	volatile int epoc_app;             /* the app sets 1: it answers them */
	volatile unsigned int epoc_req_seq;
	volatile unsigned int epoc_ans_seq;
	volatile unsigned int epoc_req_len;
	volatile unsigned int epoc_ans_len;
	unsigned char epoc_req[LX_EPOC_BUF];
	unsigned char epoc_ans[LX_EPOC_BUF];

	volatile int welcome_off;          /* (7) 1: Tools > Show welcome banner is not ticked (the app; may
	                                      change while running): /proc/psilinux/welcome says 0 */

	/* (8) memory (0.38): the app's side, for /proc/psilinux/mem, and its asking
	 * the runner to give back what it can (the launcher's "trim" and "sleep",
	 * psion-core ssh/psimem.h) */
	volatile unsigned int app_heap;    /* the app's heap, committed bytes (the app, each tick) */
	volatile unsigned int app_heap_used;   /* ...of which allocated */
	unsigned int start_free;           /* the Psion's free RAM as the app started (the app) */
	volatile unsigned int trim_seq;    /* the app adds 1: the runner gives back its caches now */
	volatile int app_behind;           /* 1: PsiLinux is not the program in front */

	/* (9) sleep (0.39): Linux's memory to the disk while it is idle behind */
	volatile int sleep_after;          /* seconds behind before it sleeps; 0: never (the app, Preferences) */
	volatile unsigned int sleep_req;   /* the app adds 1: sleep now if idle (the launcher's "sleep") */
	volatile int sleep_state;          /* LXS_AWAKE... (the runner) */
	volatile unsigned int sleep_seq;   /* the runner adds 1 at each sleep */
	volatile int sleep_freed_kb;       /* what the last sleep gave back */

	/* (10) Coda's memory service (0.46: psion-core psimemlvl.h): PsiLinux is
	 * on its list of programs that answer "trim" and "sleep" */
	volatile int busy;                 /* 1: a program runs on the terminal, has the screen, or a connection is
	                                      open (the runner, each second): not to be put to sleep or closed */
	volatile unsigned int sleep_now_req;   /* the app adds 1: sleep now if idle, whatever In the background
	                                      says (Free memory's "Put PsiLinux to sleep", "sleepnow") */
	volatile unsigned int sleep_now_ans;   /* the runner sets it to sleep_now_req once it has tried */
	volatile int sleep_now_result;     /* ...0: asleep; else -errno, and sleep_why says why in words */
	char sleep_why[48];

	/* (11) what PsiLinux holds (0.47, RN-18: the status line, /proc's "in
	 * all" and Coda's Free memory each said something else) */
	volatile unsigned int held;        /* the runner's: its Linux chunks, heap, threads, tables (each second) */
	volatile unsigned int app_held;    /* the window's (the app, every 5 s) */
	} LxShared;

enum { LXS_AWAKE = 0, LXS_ASLEEP, LXS_NOROOM, LXS_NOCARD };   /* sleep_state: NOROOM: asleep, and no RAM to wake; (0.40) NOCARD: asleep, its card out */

#endif
