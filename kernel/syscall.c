/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * syscall.c - syscall/sysret MSR setup and the C side of system calls.
 * Validation and the syscall table live in Rust (rust/src/syscall.rs);
 * this file only moves registers in and out.
 */
#include <magia/cpu.h>
#include <magia/linker_set.h>
#include <magia/rust.h>
#include <magia/syscall.h>
#include <magia/sysctl.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

void syscall_entry(void);
void syscall_c(struct trapframe *tf);
__noreturn void syscall_bad_return(struct trapframe *tf);

volatile uint64_t syscall_count;
SYSCTL_U64_RO(kern_syscalls, "kern.syscalls", &syscall_count, "system calls since boot");

void syscall_init(void)
{
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
	/* STAR[47:32]: kernel CS (SS = +8). STAR[63:48]: sysret base;
	 * 64-bit sysret loads CS = base + 16, SS = base + 8 (both | 3). */
	wrmsr(MSR_STAR, ((uint64_t)GDT_UBASE << 48) | ((uint64_t)GDT_KCODE << 32));
	wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
	wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_TF | RFLAGS_DF | RFLAGS_AC);
}

static void maybe_exit(struct thread *t)
{
	if (t->exit_pending)
		thread_exit(t->exit_status);
}

/* From arch/x86_64/syscall.S, interrupts enabled, on the kernel stack. */
void syscall_c(struct trapframe *tf)
{
	struct thread *t = curthread();
	t->user_mode_used = true;
	syscall_count++;
	uint64_t args[6] = { tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9 };
	tf->rax = (uint64_t)rs_syscall(t->pid, tf->rax, args);
	maybe_exit(t);
}

/* From syscall.S when the return rip is not a canonical user address. */
void syscall_bad_return(struct trapframe *tf)
{
	struct thread *t = curthread();
	kprintf("magia: pid %d: refusing sysret to non-canonical rip %p, killing the process\n", t->pid,
		(void *)tf->rip);
	uint64_t args[6] = { 128 + 13, 0, 0, 0, 0, 0 };
	if (t->pid > 0)
		rs_syscall(t->pid, SYS_exit, args);
	thread_exit(128 + 13);
}

/* Kernel threads acting as processes (ktest) enter the same dispatcher. */
int64_t ksyscall(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	struct thread *t = curthread();
	syscall_count++;
	uint64_t args[6] = { a0, a1, a2, a3, a4, a5 };
	int64_t r = rs_syscall(t->pid, nr, args);
	maybe_exit(t);
	return r;
}

static void syscall_sysinit(void) { syscall_init(); }
SYSINIT(syscall, SI_ORDER_SYSCALL, syscall_sysinit);
