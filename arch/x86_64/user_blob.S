/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * user_blob.S - tiny position-independent ring-3 programs used by ktest
 * (kernel/ktest/t_ring3.c). Magia has no userland and no ELF loader; these
 * blobs are copied into a user code window (U, R+X) and entered with
 * enter_user(). They exist only to prove the syscall/sysret path and that
 * ring-3 faults kill the process, not the kernel.
 */
#define SYS_exit        1
#define SYS_getpid      2
#define SYS_getppid     3
#define SYS_write       5
#define SYS_sched_yield 20

	.section .user_blob, "ax"
	.code64
	.global ublob_syscalls, ublob_syscalls_end
	.global ublob_hlt, ublob_hlt_end, ublob_kread, ublob_kread_end
	.global ublob_inb, ublob_inb_end, ublob_sgdt, ublob_sgdt_end

/*
 * rdi = user data page. Results (8-byte slots):
 *   0 getpid   8 write(1, msg)   16 syscall 9999   24 write(1, kernel ptr)
 *  32 callee-saved registers preserved (1)   40 rflags after syscall
 *  48 getppid  56 "done" marker after a long user-mode loop
 * then exit(42).
 */
ublob_syscalls:
	movq %rdi, %r15
	movl $SYS_getpid, %eax
	syscall
	movq %rax, 0(%r15)

	movl $SYS_write, %eax
	movl $1, %edi
	leaq ub_msg(%rip), %rsi
	movl $(ub_msg_end - ub_msg), %edx
	syscall
	movq %rax, 8(%r15)

	movl $9999, %eax
	syscall
	movq %rax, 16(%r15)

	movl $SYS_write, %eax
	movl $1, %edi
	movabsq $0xffffffff80100000, %rsi
	movl $8, %edx
	syscall
	movq %rax, 24(%r15)

	movabsq $0x1111111111111111, %rbx
	movabsq $0x2222222222222222, %rbp
	movabsq $0x3333333333333333, %r12
	movabsq $0x4444444444444444, %r13
	movabsq $0x5555555555555555, %r14
	movl $SYS_sched_yield, %eax
	syscall
	xorl %ecx, %ecx
	movabsq $0x1111111111111111, %rax
	cmpq %rax, %rbx
	jne 1f
	movabsq $0x2222222222222222, %rax
	cmpq %rax, %rbp
	jne 1f
	movabsq $0x3333333333333333, %rax
	cmpq %rax, %r12
	jne 1f
	movabsq $0x4444444444444444, %rax
	cmpq %rax, %r13
	jne 1f
	movabsq $0x5555555555555555, %rax
	cmpq %rax, %r14
	jne 1f
	movl $1, %ecx
1:	movq %rcx, 32(%r15)

	pushfq
	popq %rax
	movq %rax, 40(%r15)

	movl $SYS_getppid, %eax
	syscall
	movq %rax, 48(%r15)

	/* spin in ring 3 long enough to be preempted by the timer */
	movl $20000000, %ecx
2:	decl %ecx
	jnz 2b
	movl $0x656e6f64, %eax /* "done" */
	movq %rax, 56(%r15)

	movl $SYS_exit, %eax
	movl $42, %edi
	syscall
	ud2
ub_msg:
	.ascii "ring3: hello from user mode via syscall\n"
ub_msg_end:
ublob_syscalls_end:

/* Privileged instruction in ring 3: #GP, process killed (status 128+13). */
ublob_hlt:
	hlt
	ud2
ublob_hlt_end:

/* Read kernel memory from ring 3: #PF, process killed (status 128+14). */
ublob_kread:
	movabsq $0xffffffff80100000, %rax
	movq (%rax), %rax
	ud2
ublob_kread_end:

/* Port I/O without IOPL or an I/O bitmap: #GP. */
ublob_inb:
	inb $0x60, %al
	ud2
ublob_inb_end:

/* sgdt in ring 3: #GP when UMIP is on; otherwise it works and we exit(7). */
ublob_sgdt:
	subq $16, %rsp
	sgdt (%rsp)
	movl $SYS_exit, %eax
	movl $7, %edi
	syscall
	ud2
ublob_sgdt_end:

/*
 * Benchmark (kernel/bench.c): the null system call from ring 3, timed in
 * user mode as a user-space benchmark on Linux would be. rdi = u32 array,
 * rsi = samples, rdx = warm-up calls. Each sample: lfence; rdtsc; lfence;
 * syscall getpid; rdtscp; lfence (Intel's timing pattern), cycles stored
 * as u32. Then exit(0). rbx, r12-r15 survive the system call (ktest 900).
 */
	.global ublob_bench_getpid, ublob_bench_getpid_end
ublob_bench_getpid:
	movq %rdi, %r12
	movq %rsi, %r13
	movq %rdx, %r14
1:	testq %r14, %r14
	jz 2f
	movl $SYS_getpid, %eax
	syscall
	decq %r14
	jmp 1b
2:	xorl %r15d, %r15d
3:	cmpq %r13, %r15
	jae 4f
	lfence
	rdtsc
	lfence
	shlq $32, %rdx
	orq %rax, %rdx
	movq %rdx, %rbx
	movl $SYS_getpid, %eax
	syscall
	rdtscp
	lfence
	shlq $32, %rdx
	orq %rax, %rdx
	subq %rbx, %rdx
	movl %edx, (%r12,%r15,4)
	incq %r15
	jmp 3b
4:	movl $SYS_exit, %eax
	xorl %edi, %edi
	syscall
	ud2
ublob_bench_getpid_end:
