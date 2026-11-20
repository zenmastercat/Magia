/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * syscall.S - `syscall` entry from ring 3 and the first entry into ring 3.
 *
 * On `syscall` the CPU saves rip in rcx and rflags in r11, loads CS/SS from
 * STAR and masks rflags with SFMASK (IF, TF, DF, AC cleared: interrupts off,
 * SMAP enforced). We swapgs to reach per-CPU data, switch to the thread's
 * kernel stack, and build a struct trapframe identical to the one trap
 * entry builds, so trap and syscall code share one frame format.
 */

#define PCPU_KSTACK   8
#define PCPU_USER_RSP 16
#define USER_CS (0x28 | 3)
#define USER_DS (0x20 | 3)
#define T_SYSCALL 0x100

	.section .text
	.code64
	.global syscall_entry, enter_user

syscall_entry:
	swapgs
	movq %rsp, %gs:PCPU_USER_RSP
	movq %gs:PCPU_KSTACK, %rsp
	pushq $USER_DS			/* ss     */
	pushq %gs:PCPU_USER_RSP		/* rsp    */
	pushq %r11			/* rflags */
	pushq $USER_CS			/* cs     */
	pushq %rcx			/* rip    */
	pushq $0			/* error  */
	pushq $T_SYSCALL		/* vector */
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
	xorl %ebp, %ebp
	movq %rsp, %rdi
	sti
	call syscall_c
	cli
	/*
	 * sysretq with a non-canonical rcx faults in ring 0 *after* the
	 * stack/GS state is already the user's on some CPUs. The rip we
	 * return to must be a user address (bits 63:47 clear); it always is
	 * today (the CPU put it there), but if a future change ever lets the
	 * kernel modify tf->rip, refuse to sysret: syscall_bad_return() kills
	 * the process instead (exit status 128 + 13).
	 */
	movq 136(%rsp), %rcx		/* tf->rip */
	shrq $47, %rcx
	jnz 3f
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
	addq $16, %rsp			/* vector, error */
	popq %rcx			/* user rip    */
	addq $8, %rsp			/* cs          */
	popq %r11			/* user rflags */
	popq %rsp			/* user rsp    */
	swapgs
	sysretq
3:	sti
	movq %rsp, %rdi
	call syscall_bad_return		/* does not return */
	ud2

/*
 * __noreturn void enter_user(rip, rsp, a0, a1, a2)
 * First entry of the current thread into ring 3, via iretq. All registers
 * other than the three arguments are cleared so no kernel data leaks.
 */
enter_user:
	cli
	pushq $USER_DS			/* ss     */
	pushq %rsi			/* rsp    */
	pushq $0x202			/* rflags: IF */
	pushq $USER_CS			/* cs     */
	pushq %rdi			/* rip    */
	movq %rdx, %rdi
	movq %rcx, %rsi
	movq %r8, %rdx
	xorl %eax, %eax
	xorl %ebx, %ebx
	xorl %ecx, %ecx
	xorl %ebp, %ebp
	xorl %r8d, %r8d
	xorl %r9d, %r9d
	xorl %r10d, %r10d
	xorl %r11d, %r11d
	xorl %r12d, %r12d
	xorl %r13d, %r13d
	xorl %r14d, %r14d
	xorl %r15d, %r15d
	swapgs
	iretq
