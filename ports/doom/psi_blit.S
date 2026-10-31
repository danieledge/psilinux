@ psi_blit.S - Doom's rows doubled across into PsiLinux's /dev/fb0
@ (i_psilinux.c), in ARMv4T ARM code for the Psion's ARM710T.
@
@   void psi_dbl_row(uint32_t *d, uint32_t *d2, const uint32_t *s, int n)
@
@ n words of s (4 pixels each, n even, n >= 2) -> 2n words of d, and of d2
@ too unless it is 0 (a row drawn twice). Each pixel twice: a b c d ->
@ a a b b, c c d d. 8 ALU instructions a word, with LDM/STM: about 10
@ instructions a word in all, against 15 for what GCC makes of the C.
@ (All words aligned: Doom's screen and the frame buffer are.)

	.text
	.arm
	.align	2
	.global	psi_dbl_row
	.type	psi_dbl_row, %function
psi_dbl_row:
	stmfd	sp!, {r4-r11, lr}
	ldr	lr, =0x00ff00ff		@ M: bytes 0 and 2
	cmp	r1, #0
	beq	2f
1:	@ two rows
	ldmia	r2!, {r4, r5}
	@ r4 = [a b c d] -> r6 = [a a b b], r7 = [c c d d]
	and	r8, r4, #0xff		@ [a 0 0 0]
	and	r9, lr, r4, lsl #8	@ [0 0 b 0]
	orr	r8, r8, r9		@ [a 0 b 0]
	orr	r6, r8, r8, lsl #8	@ [a a b b]
	mov	r8, r4, lsr #16		@ [c d 0 0]
	orr	r8, r8, r8, lsl #8	@ [c c|d d 0]
	and	r8, r8, lr		@ [c 0 d 0]
	orr	r7, r8, r8, lsl #8	@ [c c d d]
	@ r5 -> r10, r11
	and	r8, r5, #0xff
	and	r9, lr, r5, lsl #8
	orr	r8, r8, r9
	orr	r10, r8, r8, lsl #8
	mov	r8, r5, lsr #16
	orr	r8, r8, r8, lsl #8
	and	r8, r8, lr
	orr	r11, r8, r8, lsl #8
	stmia	r0!, {r6, r7, r10, r11}
	stmia	r1!, {r6, r7, r10, r11}
	subs	r3, r3, #2
	bgt	1b
	ldmfd	sp!, {r4-r11, lr}
	bx	lr
2:	@ one row
	ldmia	r2!, {r4, r5}
	and	r8, r4, #0xff
	and	r9, lr, r4, lsl #8
	orr	r8, r8, r9
	orr	r6, r8, r8, lsl #8
	mov	r8, r4, lsr #16
	orr	r8, r8, r8, lsl #8
	and	r8, r8, lr
	orr	r7, r8, r8, lsl #8
	and	r8, r5, #0xff
	and	r9, lr, r5, lsl #8
	orr	r8, r8, r9
	orr	r10, r8, r8, lsl #8
	mov	r8, r5, lsr #16
	orr	r8, r8, r8, lsl #8
	and	r8, r8, lr
	orr	r11, r8, r8, lsl #8
	stmia	r0!, {r6, r7, r10, r11}
	subs	r3, r3, #2
	bgt	2b
	ldmfd	sp!, {r4-r11, lr}
	bx	lr
	.ltorg
	.size	psi_dbl_row, .-psi_dbl_row

@   void psi_grey_row(uint32_t *d, const uint32_t *s, const uint8_t *lut, int n)
@
@ The same, for the Psion's own 16 greys (/dev/fb0 at depth 4): n words of s
@ (n even, n >= 2) -> n words of d, each pixel one byte, lut[pixel]: its two
@ grey nibbles, dithered (the row's half of the 2x2 pattern; i_psilinux.c
@ makes the tables). LDRB with a shifted index: 3 instructions a pixel. The
@ app then copies the bytes as they are, with no conversion of its own.

	.global	psi_grey_row
	.type	psi_grey_row, %function
psi_grey_row:
	stmfd	sp!, {r4-r9, lr}
1:	ldmia	r1!, {r4, r5}
	and	r6, r4, #0xff
	ldrb	r6, [r2, r6]
	and	r7, r4, #0xff00
	ldrb	r7, [r2, r7, lsr #8]
	orr	r6, r6, r7, lsl #8
	and	r7, r4, #0xff0000
	ldrb	r7, [r2, r7, lsr #16]
	orr	r6, r6, r7, lsl #16
	ldrb	r7, [r2, r4, lsr #24]
	orr	r6, r6, r7, lsl #24
	and	r8, r5, #0xff
	ldrb	r8, [r2, r8]
	and	r9, r5, #0xff00
	ldrb	r9, [r2, r9, lsr #8]
	orr	r8, r8, r9, lsl #8
	and	r9, r5, #0xff0000
	ldrb	r9, [r2, r9, lsr #16]
	orr	r8, r8, r9, lsl #16
	ldrb	r9, [r2, r5, lsr #24]
	orr	r8, r8, r9, lsl #24
	stmia	r0!, {r6, r8}
	subs	r3, r3, #2
	bgt	1b
	ldmfd	sp!, {r4-r9, lr}
	bx	lr
	.size	psi_grey_row, .-psi_grey_row

	.section .note.GNU-stack,"",%progbits
