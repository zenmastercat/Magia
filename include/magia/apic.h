/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* apic.h - local APIC, I/O APIC and HPET interfaces. */
#ifndef MAGIA_APIC_H
#define MAGIA_APIC_H

#include <magia/kernel.h>

#define MSR_APIC_BASE   0x1b
#define APIC_BASE_X2    (1ULL << 10)
#define APIC_BASE_EN    (1ULL << 11)
#define APIC_BASE_ADDR  0x000ffffffffff000ULL
#define MSR_X2APIC_BASE 0x800 /* x2APIC register r is MSR 0x800 + r/16 */

#define LAPIC_ID        0x020
#define LAPIC_VER       0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0b0
#define LAPIC_SVR       0x0f0
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360

#define LAPIC_SVR_ENABLE   0x100
#define LAPIC_SPURIOUS_VEC 0xff
#define LAPIC_DM_NMI    0x400
#define LAPIC_DM_EXTINT 0x700
#define LAPIC_LVT_MASKED 0x10000

extern bool lapic_x2;
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t v);
void lapic_probe(void);
void lapic_virtual_wire(void);

#endif
