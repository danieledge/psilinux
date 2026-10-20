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
#define LX_SHARED_VERSION 4
#define LX_KBD_SIZE      2048
#define LX_OUT_SIZE      16384

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
	} LxShared;

#endif
