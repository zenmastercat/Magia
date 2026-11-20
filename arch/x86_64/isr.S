/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * isr.S - exception and IRQ entry stubs.
 *
 * Every stub pushes (error code or 0, vector) and jumps to trap_common,
 * which saves the general registers in struct trapframe order
 * (include/magia/trap.h) and calls trap_dispatch(tf). swapgs is done when
 * the trap came from ring 3, so %gs always points at per-CPU data in the
 * kernel. Also: ksetjmp/klongjmp for expected-fault recovery.
 */

	.section .text
	.code64

.macro ISR_NOERR n
	.align 16
isr\n:
	pushq $0
	pushq $\n
	jmp trap_common
.endm

.macro ISR_ERR n
	.align 16
isr\n:
	pushq $\n
	jmp trap_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
ISR_NOERR \n
.endr
ISR_NOERR 255

trap_common:
	cld
	testb $3, 24(%rsp)	/* CS of the interrupted context */
	jz 1f
	swapgs
1:
	pushq %rax
	pushq %rbx
	pushq %rcx
	pushq %rdx
	pushq %rsi
	pushq %rdi
	pushq %rbp
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	movq %rsp, %rdi
	xorl %ebp, %ebp
	call trap_dispatch
	.global trap_return
trap_return:
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %r11
	popq %r10
	popq %r9
	popq %r8
	popq %rbp
	popq %rdi
	popq %rsi
	popq %rdx
	popq %rcx
	popq %rbx
	popq %rax
	addq $16, %rsp		/* vector, error */
	testb $3, 8(%rsp)
	jz 2f
	cli
	swapgs
2:
	iretq

	.section .rodata
	.align 8
	.global isr_table
isr_table:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.quad isr\n
.endr
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
	.quad isr\n
.endr
	.quad isr255

/* ---------------- ksetjmp / klongjmp ---------------- */
	.section .text
	.global ksetjmp, klongjmp
ksetjmp:
	movq %rbx, 0(%rdi)
	movq %rbp, 8(%rdi)
	movq %r12, 16(%rdi)
	movq %r13, 24(%rdi)
	movq %r14, 32(%rdi)
	movq %r15, 40(%rdi)
	leaq 8(%rsp), %rax	/* rsp after our return */
	movq %rax, 48(%rdi)
	movq (%rsp), %rax	/* return address */
	movq %rax, 56(%rdi)
	xorl %eax, %eax
	ret

klongjmp:
	movl %esi, %eax
	movq 0(%rdi), %rbx
	movq 8(%rdi), %rbp
	movq 16(%rdi), %r12
	movq 24(%rdi), %r13
	movq 32(%rdi), %r14
	movq 40(%rdi), %r15
	movq 48(%rdi), %rsp
	jmp *56(%rdi)

/* ---------------- user memory copy with fault fixup ---------------- */
/*
 * long uaccess_copy(void *dst, const void *src, size_t n): returns 0, or
 * -EFAULT (-14) if a page fault hits inside the copy. trap.c redirects a
 * kernel #PF whose rip lies in [uaccess_copy, uaccess_copy_end) to
 * uaccess_fixup. The caller has validated the range and opened the SMAP
 * window (stac); this only guards against a concurrent unmap.
 */
	.global uaccess_copy, uaccess_copy_end, uaccess_fixup
uaccess_copy:
	movq %rdx, %rcx
	rep movsb
	xorl %eax, %eax
	ret
uaccess_copy_end:
uaccess_fixup:
	movq $-14, %rax
	ret
