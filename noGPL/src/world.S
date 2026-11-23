// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#define W(reg) mrs reg, mpidr_el1; and x10, reg, #0xf; ubfx reg, reg, #8, #4; orr reg, x10, reg, lsl #4; \
	adrp x10, xnu_worlds; add x10, x10, :lo12:xnu_worlds; ldr reg, [x10, reg, lsl #3]

.macro TO_XNU_MM
	mrs	x10, ttbr0_el1
	str	x10, [x9, #416]
	mrs	x10, tcr_el1
	str	x10, [x9, #424]
	ldr	x10, [x9, #440]
	msr	tcr_el1, x10
	ldr	x10, [x9, #432]
	msr	ttbr0_el1, x10
	isb
	tlbi	vmalle1
	dsb	nsh
	isb
.endm

.macro TO_LINUX_MM
	mrs	x10, ttbr0_el1
	str	x10, [x9, #432]
	mrs	x10, tcr_el1
	str	x10, [x9, #440]
	ldr	x10, [x9, #424]
	msr	tcr_el1, x10
	ldr	x10, [x9, #416]
	msr	ttbr0_el1, x10
	isb
	tlbi	vmalle1
	dsb	nsh
	isb
.endm

	.text
	.globl	xnu_world_call
	.type	xnu_world_call, %function
xnu_world_call:
	W(x9)
	str	x18, [x9, #464]
	stp	x19, x20, [x9, #0]
	stp	x21, x22, [x9, #16]
	stp	x23, x24, [x9, #32]
	stp	x25, x26, [x9, #48]
	stp	x27, x28, [x9, #64]
	stp	x29, x30, [x9, #80]
	mov	x10, sp
	str	x10, [x9, #96]
	mov	x19, x0
	mov	x20, x1
	add	x0, x9, #328
	add	x1, x9, #240
	bl	fki_enter
	W(x9)
	str	x0, [x9, #104]
	msr	daifset, #0xf
	mrs	x10, tpidr_el0
	str	x10, [x9, #216]
	mrs	x10, tpidrro_el0
	str	x10, [x9, #224]
	TO_XNU_MM
	mov	x0, x20
	blr	x19
	W(x9)
	mov	x19, #0
	b	2f
xnu_world_abort:
	W(x9)
	mov	x19, #1
2:	TO_LINUX_MM
	ldr	x18, [x9, #464]
	ldr	x10, [x9, #216]
	msr	tpidr_el0, x10
	ldr	x10, [x9, #224]
	msr	tpidrro_el0, x10
	ldr	x10, [x9, #96]
	mov	sp, x10
	ldr	x0, [x9, #104]
	add	x1, x9, #240
	bl	fki_leave
	mov	x0, x19
	W(x9)
	ldp	x19, x20, [x9, #0]
	ldp	x21, x22, [x9, #16]
	ldp	x23, x24, [x9, #32]
	ldp	x25, x26, [x9, #48]
	ldp	x27, x28, [x9, #64]
	ldp	x29, x30, [x9, #80]
	ret
	.size	xnu_world_call, . - xnu_world_call

	.globl	xnu_world_fault_abort
	.type	xnu_world_fault_abort, %function
xnu_world_fault_abort:
	W(x9)
	add	x0, x9, #240
	add	x1, x9, #328
	bl	fki_pac_swap
	b	xnu_world_abort
	.size	xnu_world_fault_abort, . - xnu_world_fault_abort

	.globl	xnu_world_upcall
	.type	xnu_world_upcall, %function
xnu_world_upcall:
	hint	#34
	W(x9)
	stp	x19, x20, [x9, #112]
	stp	x21, x22, [x9, #128]
	stp	x23, x24, [x9, #144]
	stp	x25, x26, [x9, #160]
	stp	x27, x28, [x9, #176]
	stp	x29, x30, [x9, #192]
	mov	x10, sp
	str	x10, [x9, #208]
	mov	x19, x0
	mov	x20, x1
	mov	x21, x2
	mrs	x10, tpidr_el0
	str	x10, [x9, #448]
	mrs	x10, tpidrro_el0
	str	x10, [x9, #456]
	TO_LINUX_MM
	add	x0, x9, #240
	add	x1, x9, #328
	bl	fki_pac_swap
	mov	x0, x19
	mov	x1, x20
	mov	x2, x21
	bl	xnu_hook
	cbnz	x0, xnu_world_abort
	W(x9)
	ldr	x10, [x9, #448]
	msr	tpidr_el0, x10
	ldr	x10, [x9, #456]
	msr	tpidrro_el0, x10
	TO_XNU_MM
	add	x0, x9, #328
	add	x1, x9, #240
	bl	fki_pac_swap
	W(x9)
	ldr	x10, [x9, #208]
	mov	sp, x10
	ldp	x19, x20, [x9, #112]
	ldp	x21, x22, [x9, #128]
	ldp	x23, x24, [x9, #144]
	ldp	x25, x26, [x9, #160]
	ldp	x27, x28, [x9, #176]
	ldp	x29, x30, [x9, #192]
	ret
	.size	xnu_world_upcall, . - xnu_world_upcall

	.globl	xnu_fault_tramp
	.type	xnu_fault_tramp, %function
xnu_fault_tramp:
	sub	sp, sp, #256
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30, [sp, #240]
	.inst	0xd500419f
	mov	x0, sp
	bl	xnu_fault_deferred
	msr	elr_el1, x0
	ldr	x1, [sp, #248]
	msr	spsr_el1, x1
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30, [sp, #240]
	add	sp, sp, #256
	eret
	.size	xnu_fault_tramp, . - xnu_fault_tramp
