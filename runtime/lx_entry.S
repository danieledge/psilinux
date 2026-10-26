@ lx_entry.S - the program's side of PsiLinux's system calls, and the helpers
@ musl takes from the runner's table (AT_SYSINFO) instead of the Linux kuser
@ helpers at 0xffff0fxx, which EPOC has not got.
@
@ Assembled by both toolchains: gcc 3.0 (arm-pe, EPOC) and gcc 14
@ (arm-linux-musleabi, the host harness), so only the plain ARM syntax both
@ understand is used. Returns are MOV PC, LR, not BX LR: the netBook's
@ SA-1100 has no BX, and every caller is ARM code.
@
@  lx_entry_sys    the system call entry: r0-r6 the arguments, r7 the number,
@                  lr the return address (it is entered by __psi_svc's
@                  "ldr pc", or by a call). Returns like an svc: r0 the result,
@                  every other register kept, but ip, lr and the flags.
@  lx_entry_cas    compare-and-swap, as kuser_cmpxchg: r0 old, r1 new, r2 the
@                  address; r0 = 0 if it stored. Uses r3, ip.
@  lx_entry_gettp  the thread pointer, in r0; every other register kept.
@  lx_entry_barrier  nothing (one CPU, no write buffer reordering to see).
@  lx_resume       loads a whole context (r0-r12, sp, lr, pc) and goes there.
@  lx_sigtramp     a signal handler's return, if the program gave no
@                  SA_RESTORER: rt_sigreturn.

	.text
	.align 2

	.global lx_entry_sys
lx_entry_sys:
	stmfd sp!, {r0-r12, lr}        @ the frame: r0..r12 and the return address
	mov r0, sp
	bl lx_trap                     @ the kernel answers into frame[0]
	ldmfd sp!, {r0-r12, lr}
	mov pc, lr

	.global lx_entry_gettp
lx_entry_gettp:
	stmfd sp!, {r0-r3, ip, lr}     @ (six words: sp stays 8-aligned)
	mov r0, sp
	bl lx_gettp_c
	add sp, sp, #4
	ldmfd sp!, {r1-r3, ip, lr}
	mov pc, lr

	.global lx_entry_barrier
lx_entry_barrier:
	mov pc, lr

	.global lx_entry_cas
lx_entry_cas:
	@ A lock taken with SWP (atomic on ARMv4) around the compare and the
	@ store, because a Linux thread is a real EPOC thread: it can be
	@ preempted between them by another thread of the same program.
#ifdef __ELF__
	ldr ip, 9f
8:	add ip, pc, ip
#else
	ldr ip, =lx_cas_lock
#endif
	mov r3, sp                     @ (the holder's sp: if it dies holding the
	swp r3, r3, [ip]               @  lock, the kernel knows it and lets it go)
	cmp r3, #0
	bne 2f
	ldr r3, [r2]
	subs r3, r0, r3                @ 0 if *r2 == old
	streq r1, [r2]
	mov r0, r3
	mov r3, #0
	str r3, [ip]                   @ unlock
	mov pc, lr
2:	@ another thread holds the lock (it was preempted inside it): give way
	stmfd sp!, {r0-r2, lr}
	bl bk_cas_yield
	ldmfd sp!, {r0-r2, lr}
	b lx_entry_cas
#ifdef __ELF__
	.align 2
9:	.word lx_cas_lock - 8b - 8
#else
	.ltorg
#endif

	.global lx_resume
lx_resume:
	@ r0 = the context: r0..r12 at 0..48, sp 52, lr 56, pc 60
	ldr sp, [r0, #52]
	ldr lr, [r0, #60]
	stmfd sp!, {lr}                @ the pc, just below the new sp
	ldr lr, [r0, #56]
	ldmia r0, {r0-r12}
	ldmfd sp!, {pc}

	.global lx_resume_copy
lx_resume_copy:
	@ r0 = the context, r1 = where, r2 = from, r3 = bytes (a multiple of 4).
	@ Copies without touching the stack (the copy may land on it), then
	@ goes on as lx_resume.
1:	subs r3, r3, #4
	ldrge ip, [r2, r3]
	strge ip, [r1, r3]
	bgt 1b
	b lx_resume

	.global lx_sigtramp
lx_sigtramp:
	mov r7, #173                   @ rt_sigreturn
	bl lx_entry_sys

	.data
	.align 2
	.global lx_cas_lock
lx_cas_lock:
	.word 0

#ifdef __ELF__
	.section .note.GNU-stack,"",%progbits
#endif
