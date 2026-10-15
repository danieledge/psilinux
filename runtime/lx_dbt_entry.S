@ lx_dbt_entry.S - Alpine mode: the translated code's ways into the runtime
@ (lx_dbt.h). Assembled by gcc 3.0 (arm-pe, EPOC) and gcc 14 (the host
@ harness), so plain ARMv4 syntax only; MRS and MSR are written as words
@ (the two assemblers spell them differently).
@
@ A translated thread's registers are the guest's: r0-r14 and the flags.
@ Two words below the guest's sp are free for the runtime (no signal or
@ interrupt ever writes there while guest code runs: signals are delivered
@ by the runtime itself).
@
@  dbt_dispatch  (copied into the code cache by dbt_init, so that blocks can
@                reach it with a branch) an indirect branch: the guest
@                target is at [sp, #-4]. Looks it up in lx_cpu.hash; on a
@                hit goes straight there, otherwise enters the runtime
@                (DBT_MISS, the target in lx_cpu.target). Also enters the
@                runtime when lx_cpu.yield is set.
@  dbt_enter     r0 = a descriptor ({reason, ...}, in the block), the guest's
@                r0 at [sp, #-8]. Saves every register and the flags in the
@                thread's frame (t->dframe), moves to the thread's own stack
@                and calls dbt_runtime(descriptor, frame), which returns the
@                host address to go on at.
@  dbt_continue  r0 = a host address: loads lx_cpu.cur's frame (registers,
@                flags) and goes there.

	.text
	.align 2

@ ---- the dispatcher (position independent: it is copied) ----------------
@ The last two words (the addresses of lx_cpu and dbt_enter) are filled in
@ by dbt_init in the copy.
	.global dbt_dispatch_start
dbt_dispatch_start:
	str r0, [sp, #-8]
	str r1, [sp, #-12]
	str r2, [sp, #-16]
	.word 0xE10F2000            @ mrs r2, cpsr
	str r2, [sp, #-20]
	ldr r1, 3f                  @ &lx_cpu
	ldr r2, [r1, #8]            @ lx_cpu.yield
	cmp r2, #0
	bne 1f
	ldr r0, [sp, #-4]           @ the guest target
	ldr r2, [r1, #12]           @ lx_cpu.hash
	mov r1, r0, lsl #18         @ ((target >> 2) & 4095) * 8  (DBT_HASH_BITS 12)
	add r2, r2, r1, lsr #17
	ldr r1, [r2]
	cmp r1, r0
	bne 1f
	ldr r2, [r2, #4]            @ the host code
	str r2, [sp, #-24]
	ldr r2, [sp, #-20]
	.word 0xE128F002            @ msr cpsr_f, r2
	ldr r2, [sp, #-16]
	ldr r1, [sp, #-12]
	ldr r0, [sp, #-8]
	ldr pc, [sp, #-24]
1:	ldr r0, [sp, #-4]           @ a miss (or a yield): the runtime decides
	ldr r1, 3f
	str r0, [r1, #4]            @ lx_cpu.target
	ldr r2, [sp, #-20]
	.word 0xE128F002            @ msr cpsr_f, r2
	ldr r2, [sp, #-16]
	ldr r1, [sp, #-12]
	add r0, pc, #0              @ r0 = 2f (the descriptor)
	ldr pc, 4f                  @ dbt_enter (the guest's r0 is at [sp, #-8])
2:	.word 5                     @ DBT_MISS
	.word 0
3:	.word 0                     @ &lx_cpu     (dbt_init)
4:	.word 0                     @ &dbt_enter  (dbt_init)
	.global dbt_dispatch_end
dbt_dispatch_end:

@ ---- into the runtime ------------------------------------------------------
	.global dbt_enter
dbt_enter:
	str r1, [sp, #-12]
#ifdef __ELF__
	ldr r1, 7f
5:	add r1, pc, r1
#else
	ldr r1, =lx_cpu
#endif
	ldr r1, [r1]                @ lx_cpu.cur
	add r1, r1, #8              @ its frame (dframe, at the start of lx_thread), from r2
	stmia r1, {r2-r14}
	sub r1, r1, #8
	ldr r2, [sp, #-8]           @ the guest's r0
	str r2, [r1]
	ldr r2, [sp, #-12]          @ ...and r1
	str r2, [r1, #4]
	.word 0xE10F2000            @ mrs r2, cpsr
	str r2, [r1, #64]           @ dframe[16]
	ldr sp, [r1, #68]           @ t->hostsp
	bl dbt_runtime              @ (r0 the descriptor, r1 the frame)
	@ on into dbt_continue with r0 = where to go

	.global dbt_continue
dbt_continue:
#ifdef __ELF__
	ldr r1, 8f
6:	add r1, pc, r1
#else
	ldr r1, =lx_cpu
#endif
	ldr r1, [r1]                @ lx_cpu.cur
	str r0, [r1, #60]           @ dframe[15]: the host address
	ldr r0, [r1, #64]
	.word 0xE128F000            @ msr cpsr_f, r0
	ldmia r1, {r0-r15}

#ifdef __ELF__
	.align 2
7:	.word lx_cpu - 5b - 8
8:	.word lx_cpu - 6b - 8
#else
	.ltorg
#endif

#ifdef __ELF__
	.section .note.GNU-stack,"",%progbits
#endif
