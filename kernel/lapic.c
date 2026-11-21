/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * lapic.c - the local APIC, xAPIC (MMIO) or x2APIC (MSRs), and its timer.
 *
 * Design after FreeBSD sys/x86/x86/local_apic.c (BSD-2-Clause), written
 * from the Intel SDM vol. 3A ch. 11: x2APIC is used whenever the CPU has
 * it ("nox2apic" turns it off); the tick comes from the TSC-deadline timer
 * when the CPU has one, else from the LAPIC timer in periodic mode,
 * calibrated against the HPET (PIT fallback, see timer.c).
 *
 * Limine (base revision 5+) leaves every LVT entry masked, including
 * LINT0, through which the 8259 PIC's ExtINT reaches the CPU in "virtual
 * wire" mode; the legacy path (noapic) reopens it.
 */
#include <magia/apic.h>
#include <magia/acpi.h>
#include <magia/mm.h>
#include <magia/trap.h>
#include <magia/x86.h>

bool lapic_x2;
uint32_t lapic_bsp_id;
uint64_t lapic_timer_hz;
volatile uint64_t lapic_errors;
static volatile uint32_t *lapic_mmio;
static uint64_t deadline_step;
static uint64_t deadline; /* TSC-deadline mode: the deadline armed now */

uint32_t lapic_read(uint32_t reg)
{
	if (lapic_x2)
		return (uint32_t)rdmsr(MSR_X2APIC_BASE + reg);
	return lapic_mmio[reg * LAPIC_MEM_MUL / 4];
}

void lapic_write(uint32_t reg, uint32_t v)
{
	if (lapic_x2)
		wrmsr(MSR_X2APIC_BASE + reg, v);
	else
		lapic_mmio[reg * LAPIC_MEM_MUL / 4] = v;
}

void lapic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

bool cpu_has_x2apic(void)
{
	uint32_t a, b, c, d;
	cpuid(1, 0, &a, &b, &c, &d);
	return (c >> 21) & 1;
}

bool cpu_has_tsc_deadline(void)
{
	uint32_t a, b, c, d;
	cpuid(1, 0, &a, &b, &c, &d);
	return (c >> 24) & 1;
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
	if (!lapic_x2 && !(lapic_mmio = vmm_map_mmio(base & APIC_BASE_ADDR, PAGE_SIZE)))
		panic("lapic: APIC base %p is RAM, refusing to map it", (void *)(base & APIC_BASE_ADDR));
}

bool lapic_present(void) { return lapic_mmio || lapic_x2; }

/* Legacy mode: LINT0 = ExtINT (the 8259), LINT1 = NMI, APIC enabled. */
void lapic_virtual_wire(void)
{
	lapic_probe();
	lapic_write(LAPIC_TPR, 0);
	lapic_write(LAPIC_SVR, APIC_SVR_SWEN | VEC_SPURIOUS);
	lapic_write(LAPIC_LVT_LINT0, APIC_LVT_DM_EXTINT);
	lapic_write(LAPIC_LVT_LINT1, APIC_LVT_DM_NMI);
}

static void lapic_error_irq(struct trapframe *tf)
{
	(void)tf;
	lapic_write(LAPIC_ESR, 0); /* latch, then read */
	lapic_errors++;
}

/* NMI wiring of LINT0/1 from the MADT (default: LINT1 = NMI). */
static uint32_t lint_lvt(int lint)
{
	for (uint32_t i = 0; i < acpi.nnmis; i++) {
		if (acpi.nmi_lint[i] != lint)
			continue;
		uint32_t v = APIC_LVT_DM_NMI;
		if (ACPI_INTI_POLARITY(acpi.nmi_flags[i]) == 3)
			v |= APIC_LVT_IIPP_INTALO;
		if (ACPI_INTI_TRIGGER(acpi.nmi_flags[i]) == 3)
			v |= APIC_LVT_TM;
		return v;
	}
	return lint == 1 ? APIC_LVT_DM_NMI : APIC_LVT_M;
}

void lapic_init(void)
{
	uint64_t base = rdmsr(MSR_APIC_BASE) | APIC_BASE_EN;
	if (!(base & APIC_BASE_X2) && cpu_has_x2apic() && !cmdline_has("nox2apic"))
		base |= APIC_BASE_X2; /* xAPIC -> x2APIC is a legal switch while enabled */
	wrmsr(MSR_APIC_BASE, base);
	lapic_x2 = (base & APIC_BASE_X2) != 0;
	if (!lapic_x2 && !lapic_mmio && !(lapic_mmio = vmm_map_mmio(base & APIC_BASE_ADDR, PAGE_SIZE)))
		panic("lapic: APIC base %p is RAM, refusing to map it", (void *)(base & APIC_BASE_ADDR));

	uint32_t id = lapic_read(LAPIC_ID);
	lapic_bsp_id = lapic_x2 ? id : id >> APIC_ID_SHIFT;
	lapic_write(LAPIC_TPR, 0);
	lapic_write(LAPIC_SVR, APIC_SVR_SWEN | VEC_SPURIOUS);
	lapic_write(LAPIC_LVT_LINT0, lint_lvt(0));
	lapic_write(LAPIC_LVT_LINT1, lint_lvt(1));
	lapic_write(LAPIC_LVT_TIMER, APIC_LVTT_M | VEC_LAPIC_TIMER);
	irq_register_vector(VEC_LAPIC_ERROR, lapic_error_irq);
	lapic_write(LAPIC_ESR, 0);
	lapic_write(LAPIC_LVT_ERROR, VEC_LAPIC_ERROR);
	lapic_write(LAPIC_ESR, 0);
	lapic_eoi();
}

/* Count the LAPIC timer (divide by 16) down for 10 ms of the reference
 * clock; returns the timer frequency in Hz. Interrupts must be off. */
uint64_t lapic_timer_calibrate(void (*wait_ms)(uint32_t))
{
	uint64_t best = ~0ULL;
	lapic_write(LAPIC_DCR_TIMER, APIC_TDCR_16);
	lapic_write(LAPIC_LVT_TIMER, APIC_LVTT_M | APIC_LVTT_TM_ONE_SHOT | VEC_LAPIC_TIMER);
	for (int i = 0; i < 3; i++) { /* shortest of three: filters vCPU stalls */
		lapic_write(LAPIC_ICR_TIMER, APIC_TIMER_MAX_COUNT);
		wait_ms(10);
		uint64_t d = APIC_TIMER_MAX_COUNT - lapic_read(LAPIC_CCR_TIMER);
		if (d < best)
			best = d;
	}
	lapic_write(LAPIC_ICR_TIMER, 0);
	lapic_timer_hz = best * 100;
	return lapic_timer_hz;
}

void lapic_timer_start(uint32_t hz)
{
	if (timer_mode == TIMER_TSC_DEADLINE) {
		deadline_step = tsc_hz() / hz;
		lapic_write(LAPIC_LVT_TIMER, APIC_LVTT_TM_TSCDLT | VEC_LAPIC_TIMER);
		__asm__ volatile("mfence" ::: "memory"); /* SDM: order the LVT write before the MSR */
		deadline = rdtsc() + deadline_step;
		wrmsr(MSR_TSC_DEADLINE, deadline);
	} else {
		lapic_write(LAPIC_DCR_TIMER, APIC_TDCR_16);
		lapic_write(LAPIC_LVT_TIMER, APIC_LVTT_TM_PERIODIC | VEC_LAPIC_TIMER);
		lapic_write(LAPIC_ICR_TIMER, (uint32_t)MAX(lapic_timer_hz / hz, 1ULL));
	}
}

void lapic_timer_rearm(void)
{
	if (timer_mode == TIMER_TSC_DEADLINE) {
		deadline = rdtsc() + deadline_step;
		wrmsr(MSR_TSC_DEADLINE, deadline);
	}
}

uint64_t lapic_deadline(void) { return timer_mode == TIMER_TSC_DEADLINE ? deadline : 0; }
