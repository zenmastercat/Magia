/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * apic.h - local APIC (xAPIC/x2APIC), I/O APIC and HPET.
 * Register names come from include/magia/apicreg.h (FreeBSD).
 */
#ifndef MAGIA_APIC_H
#define MAGIA_APIC_H

#include <magia/kernel.h>
#include <magia/apicreg.h>

#define MSR_APIC_BASE    0x1b
#define APIC_BASE_BSP    (1ULL << 8)
#define APIC_BASE_X2     (1ULL << 10)
#define APIC_BASE_EN     (1ULL << 11)
#define APIC_BASE_ADDR   0x000ffffffffff000ULL
#define MSR_X2APIC_BASE  0x800 /* x2APIC register index r is MSR 0x800 + r */
#define MSR_TSC_DEADLINE 0x6e0

/* IDT vectors: ISA IRQ n is 32 + n in both modes (IRQ_BASE). */
#define VEC_LAPIC_TIMER 48
#define VEC_LAPIC_ERROR 254
#define VEC_SPURIOUS    255

enum timer_mode { TIMER_PIT, TIMER_LAPIC_PERIODIC, TIMER_TSC_DEADLINE };

extern bool apic_mode;        /* LAPIC + I/O APIC in use (false: 8259 + PIT) */
extern bool lapic_x2;         /* x2APIC (MSR) access */
extern uint32_t lapic_bsp_id;
extern enum timer_mode timer_mode;
extern uint64_t lapic_timer_hz; /* LAPIC timer input clock / 16 */
extern volatile uint64_t lapic_errors;

/* lapic.c */
uint32_t lapic_read(uint32_t reg);  /* reg: enum LAPIC_REGISTERS index */
void lapic_write(uint32_t reg, uint32_t v);
void lapic_probe(void);
void lapic_virtual_wire(void);      /* legacy: LINT0 = ExtINT for the 8259 */
void lapic_init(void);              /* APIC mode: enable, LINTs, error LVT */
void lapic_eoi(void);
uint64_t lapic_timer_calibrate(void (*wait_ms)(uint32_t));
void lapic_timer_start(uint32_t hz);
void lapic_timer_rearm(void);       /* TSC-deadline mode: next tick */
bool cpu_has_x2apic(void);
bool cpu_has_tsc_deadline(void);

/* ioapic.c */
struct ioapic_rte {
	uint32_t lo, hi;
};
bool ioapic_init(void);             /* map all I/O APICs, mask every pin */
int ioapic_route_isa(int irq);      /* program and unmask ISA IRQ `irq` */
bool ioapic_read_rte(uint32_t gsi, struct ioapic_rte *rte);

/* hpet.c */
extern uint64_t hpet_period_fs;     /* main counter tick in femtoseconds */
bool hpet_init(void);
bool hpet_present(void);
uint64_t hpet_counter(void);
void hpet_wait_ms(uint32_t ms);

#endif
