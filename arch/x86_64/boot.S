/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * boot.S - kernel entry from the Limine boot protocol.
 *
 * Limine enters _start in 64-bit long mode with paging on (its own tables:
 * the kernel image at its slid address, all RAM at the HHDM), interrupts
 * off and rsp on a stack in bootloader-reclaimable memory. We move to the
 * kernel's own boot stack at once, seed the stack-protector guard, copy
 * Limine's answers (kernel/boot.c) and call kmain(). The kernel is a PIE:
 * every symbol reference here is RIP-relative.
 */

	.section .text.entry, "ax"
	.code64
	.global _start
_start:
	cli
	cld
	leaq boot_kstack_top(%rip), %rsp
	xorl %ebp, %ebp

	/* Seed the stack-protector guard before any C code runs: RDRAND if
	 * CPUID.1:ECX[30] says it exists, mixed with the TSC. */
	movl $1, %eax
	cpuid
	rdtsc
	shlq $32, %rdx
	orq %rax, %rdx
	movq %rdx, %r8
	movl $1, %eax
	cpuid
	testl $(1 << 30), %ecx
	jz 2f
	movl $16, %ecx
1:	rdrand %rax
	jc 3f
	loop 1b
	jmp 2f
3:	xorq %rax, %r8
2:	andq $-256, %r8 /* low byte 0: string overreads stop at the canary */
	movq %r8, __stack_chk_guard(%rip)

	call boot_early
	call kmain
4:	cli
	hlt
	jmp 4b

	.section .bss
	.align 16
	.global boot_kstack, boot_kstack_top
boot_kstack:
	.skip 16384
boot_kstack_top:
