/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * switch.S - kernel thread context switch.
 *
 * void switch_context(uint64_t *old_rsp, uint64_t new_rsp)
 * Saves the callee-saved registers on the old stack, stores the old stack
 * pointer, loads the new one and pops the new thread's registers. Called
 * with interrupts disabled from schedule(); caller-saved registers are
 * already dead by the C calling convention.
 *
 * A new thread's stack is pre-built by thread_create() so that the first
 * switch "returns" into thread_start_asm with r12 = struct thread *.
 */
	.section .text
	.code64
	.global switch_context, thread_start_asm

switch_context:
	pushq %rbp
	pushq %rbx
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	movq %rsp, (%rdi)
	movq %rsi, %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
	ret

thread_start_asm:
	movq %r12, %rdi
	xorl %ebp, %ebp
	call thread_entry_c
	ud2
