/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* trap.h - trap frames, IRQ routing, PIC, timer. */
#ifndef MAGIA_TRAP_H
#define MAGIA_TRAP_H

#include <magia/kernel.h>

/* Layout must match the push order in arch/x86_64/isr.S. */
struct trapframe {
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
	uint64_t vector, error;
	uint64_t rip, cs, rflags, rsp, ss; /* pushed by the CPU */
};

#define IRQ_BASE 32
#define IRQ_TIMER 0
#define IRQ_KBD   1
#define IRQ_COM1  4
#define T_SYSCALL 0x100 /* pseudo vector stored by the syscall stub */

typedef void (*irq_handler_t)(struct trapframe *tf);
void irq_register(int irq, irq_handler_t h);
void irq_enable(int irq); /* unmask ISA IRQ `irq` (8259 or I/O APIC) */
void irq_register_vector(int vec, irq_handler_t h); /* LAPIC timer/error vectors */
extern volatile uint64_t vec_counts[2]; /* LAPIC timer, LAPIC error interrupts */
void trap_dispatch(struct trapframe *tf);
void trap_dump(const struct trapframe *tf);

/*
 * Expected-fault recovery (used by ktest to prove W^X, NX, SMEP, SMAP):
 *   struct kjmp j;
 *   int v = ksetjmp(&j);
 *   if (v == 0) { curthread()->fault_jmp = &j; <access that must fault>; }
 *   curthread()->fault_jmp = NULL;   // v == vector + 1 if it faulted
 * A kernel-mode exception while fault_jmp is armed resumes at ksetjmp
 * returning vector + 1, instead of panicking.
 */
struct kjmp {
	uint64_t rbx, rbp, r12, r13, r14, r15, rsp, rip;
};
int ksetjmp(struct kjmp *j) __attribute__((returns_twice));
__noreturn void klongjmp(struct kjmp *j, int v);

/* kernel-mode #BP counter, used by ktest to prove exceptions return */
extern volatile uint64_t breakpoint_hits;

/* pic.c */
void pic_init(void);
void pic_eoi(int irq);
void pic_unmask(int irq);
void pic_mask(int irq);
void pic_mask_all(void);
bool pic_spurious(int irq); /* true if IRQ 7/15 was spurious */
extern volatile uint64_t irq_counts[16];

/* timer.c */
#define HZ 1000
extern volatile uint64_t ticks;
void timer_init(void);
uint64_t now_ns(void);
uint64_t tsc_hz(void);
void delay_ms(uint64_t ms); /* busy wait */
uint64_t uptime_ms(void);

#endif
