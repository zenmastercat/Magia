/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * lapic.c - the local APIC (xAPIC through MMIO, x2APIC through MSRs).
 *
 * Limine (base revision 5+) leaves every LVT entry masked, including LINT0,
 * through which the 8259 PIC's ExtINT reaches the CPU in "virtual wire"
 * mode. The legacy PIC + PIT path therefore needs LINT0 reopened.
 */
#include <magia/apic.h>
#include <magia/mm.h>
#include <magia/x86.h>

bool lapic_x2;              /* x2APIC mode (MSR access) */
static volatile uint32_t *lapic_mmio;

uint32_t lapic_read(uint32_t reg)
{
	if (lapic_x2)
		return (uint32_t)rdmsr(MSR_X2APIC_BASE + (reg >> 4));
	return lapic_mmio[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t v)
{
	if (lapic_x2)
		wrmsr(MSR_X2APIC_BASE + (reg >> 4), v);
	else
		lapic_mmio[reg / 4] = v;
}

/* Find the LAPIC and its access mode; map the xAPIC page uncached. */
void lapic_probe(void)
{
	if (lapic_mmio || lapic_x2)
		return;
	uint64_t base = rdmsr(MSR_APIC_BASE);
	if (!(base & APIC_BASE_EN)) {
		base |= APIC_BASE_EN;
		wrmsr(MSR_APIC_BASE, base);
	}
	lapic_x2 = (base & APIC_BASE_X2) != 0;
	if (!lapic_x2)
		lapic_mmio = vmm_map_mmio(base & APIC_BASE_ADDR, PAGE_SIZE);
}

/* Legacy mode: LINT0 = ExtINT (the 8259), LINT1 = NMI, APIC enabled. */
void lapic_virtual_wire(void)
{
	lapic_probe();
	lapic_write(LAPIC_TPR, 0);
	lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC);
	lapic_write(LAPIC_LVT_LINT0, LAPIC_DM_EXTINT);
	lapic_write(LAPIC_LVT_LINT1, LAPIC_DM_NMI);
}
