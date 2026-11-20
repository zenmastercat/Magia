/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* cpu.h - CPU tables (GDT/TSS/IDT), per-CPU data, feature flags. */
#ifndef MAGIA_CPU_H
#define MAGIA_CPU_H

#include <magia/kernel.h>

#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UBASE 0x18 /* STAR user base: SS = +8, CS = +16       */
#define GDT_UDATA 0x20
#define GDT_UCODE 0x28
#define GDT_TSS   0x30
#define USER_CS (GDT_UCODE | 3)
#define USER_DS (GDT_UDATA | 3)

struct thread;
/* Accessed from assembly through %gs: keep offsets in sync with
 * PCPU_* in arch/x86_64/syscall.S. */
struct percpu {
	uint64_t self;        /* 0  */
	uint64_t kstack_top;  /* 8  */
	uint64_t user_rsp;    /* 16 */
	struct thread *curthread; /* 24 */
};
extern struct percpu cpu0;

struct cpu_features {
	bool nx, smep, smap, rdrand, pdpe1gb, invariant_tsc;
	char vendor[13];
	char brand[49];
};
extern struct cpu_features cpu_feat;
extern bool smap_enabled, smep_enabled, umip_enabled;

void gdt_init(void);
void tss_set_rsp0(uint64_t rsp0);
void idt_init(void);
void cpu_init(void);
void cpu_report(void);
void syscall_init(void);

#endif
