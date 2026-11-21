/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* x86.h - thin inline wrappers around privileged x86_64 instructions. */
#ifndef MAGIA_X86_H
#define MAGIA_X86_H

#include <magia/kernel.h>

#define MSR_EFER          0xc0000080
#define MSR_STAR          0xc0000081
#define MSR_LSTAR         0xc0000082
#define MSR_SFMASK        0xc0000084
#define MSR_GS_BASE       0xc0000101
#define MSR_KERNEL_GS_BASE 0xc0000102

#define EFER_SCE (1ULL << 0)
#define EFER_LME (1ULL << 8)
#define EFER_NXE (1ULL << 11)

#define CR0_WP (1ULL << 16)
#define CR4_PGE  (1ULL << 7)
#define CR4_SMEP (1ULL << 20)
#define CR4_SMAP (1ULL << 21)

#define RFLAGS_IF (1ULL << 9)
#define RFLAGS_TF (1ULL << 8)
#define RFLAGS_DF (1ULL << 10)
#define RFLAGS_AC (1ULL << 18)

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(port)); }
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port)
{
	uint8_t v;
	__asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}
static inline uint16_t inw(uint16_t port)
{
	uint16_t v;
	__asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}
static inline void io_wait(void) { outb(0x80, 0); }

static inline uint64_t rdmsr(uint32_t msr)
{
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v)
{
	__asm__ volatile("wrmsr" ::"c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

#define DEF_CR(n)                                                            \
	static inline uint64_t read_cr##n(void)                              \
	{                                                                    \
		uint64_t v;                                                  \
		__asm__ volatile("mov %%cr" #n ", %0" : "=r"(v));            \
		return v;                                                    \
	}                                                                    \
	static inline void write_cr##n(uint64_t v)                           \
	{                                                                    \
		__asm__ volatile("mov %0, %%cr" #n ::"r"(v) : "memory");     \
	}
DEF_CR(0)
DEF_CR(3)
DEF_CR(4)
static inline uint64_t read_cr2(void)
{
	uint64_t v;
	__asm__ volatile("mov %%cr2, %0" : "=r"(v));
	return v;
}

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
	__asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static inline uint64_t rdtsc(void)
{
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt" ::: "memory"); }
static inline void cpu_relax(void) { __asm__ volatile("pause" ::: "memory"); }
static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory"); }

static inline uint64_t read_rflags(void)
{
	uint64_t f;
	__asm__ volatile("pushfq; popq %0" : "=r"(f));
	return f;
}

/* Save the interrupt flag and disable interrupts. */
static inline uint64_t irq_save(void)
{
	uint64_t f = read_rflags();
	cli();
	return f;
}
static inline void irq_restore(uint64_t f)
{
	if (f & RFLAGS_IF)
		sti();
}
static inline bool irqs_enabled(void) { return (read_rflags() & RFLAGS_IF) != 0; }

static inline uint64_t read_rbp(void)
{
	uint64_t v;
	__asm__ volatile("mov %%rbp, %0" : "=r"(v));
	return v;
}

#endif
