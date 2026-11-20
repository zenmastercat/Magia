/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * t_intr.c - ktests for the interrupt controllers and timers (05x). They
 * run in both modes: APIC (default) and legacy 8259 + PIT ("noapic"); the
 * harness boots both, under TCG and (when /dev/kvm exists) KVM, where the
 * CPU has x2APIC and the TSC-deadline timer.
 */
#include <magia/acpi.h>
#include <magia/apic.h>
#include <magia/ktest.h>
#include <magia/trap.h>
#include <magia/x86.h>

#define COM1 0x3f8

KTEST(050, interrupt_controller_mode)
{
	if (cmdline_has("noapic")) {
		KT_ASSERT(!apic_mode);
		KT_EQ(timer_mode, TIMER_PIT);
		KT_EQ(inb(0x21) & 1, 0); /* PIT line open on the 8259 */
		KT_EQ(lapic_read(LAPIC_LVT_LINT0) & (APIC_LVT_M | APIC_LVT_DM), APIC_LVT_DM_EXTINT);
		return;
	}
	KT_ASSERT(apic_mode);
	KT_EQ(inb(0x21), 0xff); /* both 8259s fully masked */
	KT_EQ(inb(0xa1), 0xff);
	KT_ASSERT(lapic_read(LAPIC_SVR) & APIC_SVR_ENABLE);
	KT_EQ(lapic_read(LAPIC_SVR) & APIC_SVR_VECTOR, VEC_SPURIOUS);
	KT_EQ(lapic_x2, cpu_has_x2apic() && !cmdline_has("nox2apic"));
	KT_EQ((rdmsr(MSR_APIC_BASE) & APIC_BASE_X2) != 0, lapic_x2);
	KT_EQ(timer_mode == TIMER_TSC_DEADLINE, cpu_has_tsc_deadline() && !cmdline_has("notscdeadline"));
	uint32_t lvt = lapic_read(LAPIC_LVT_TIMER);
	KT_EQ(lvt & APIC_LVTT_VECTOR, VEC_LAPIC_TIMER);
	KT_EQ(lvt & APIC_LVTT_M, 0);
	KT_EQ(lvt & APIC_LVTT_TM, timer_mode == TIMER_TSC_DEADLINE ? APIC_LVTT_TM_TSCDLT : APIC_LVTT_TM_PERIODIC);
	KT_EQ(lapic_errors, 0);
	/* the BSP's id is in the MADT's CPU list */
	bool listed = false;
	for (uint32_t i = 0; i < acpi.ncpus; i++)
		listed |= acpi.cpu_apic_id[i] == lapic_bsp_id;
	KT_ASSERT(listed);
}

KTEST(051, timer_ticks_at_hz)
{
	uint64_t t0 = ticks, v0 = vec_counts[VEC_LAPIC_TIMER];
	delay_ms(50); /* TSC busy-wait: the tick must keep coming meanwhile */
	uint64_t dt = ticks - t0;
	KT_ASSERT(dt >= 25 && dt <= 100);
	if (apic_mode)
		KT_ASSERT(vec_counts[VEC_LAPIC_TIMER] - v0 >= dt); /* through the LAPIC vector */
	else
		KT_EQ(vec_counts[VEC_LAPIC_TIMER], v0);
}

KTEST(052, timer_calibration_sanity)
{
	KT_ASSERT(tsc_hz() >= 100000000ULL && tsc_hz() <= 20000000000ULL);
	KT_ASSERT(hpet_present()); /* QEMU q35 has one */
	if (!hpet_present())
		return;
	KT_ASSERT(hpet_period_fs > 0 && hpet_period_fs <= 100000000ULL);
	/* TSC and HPET agree on 20 ms within 20 % */
	uint64_t h0 = hpet_counter(), t0 = rdtsc();
	hpet_wait_ms(20);
	uint64_t hns = (hpet_counter() - h0) * hpet_period_fs / 1000000;
	uint64_t tns = (rdtsc() - t0) * 1000000000ULL / tsc_hz();
	KT_ASSERT(hns >= 20000000ULL);
	KT_ASSERT(tns * 5 >= hns * 4 && tns * 4 <= hns * 5);
	if (timer_mode == TIMER_LAPIC_PERIODIC)
		KT_ASSERT(lapic_timer_hz >= 1000000ULL && lapic_timer_hz <= 10000000000ULL);
}

/* COM1's interrupt really arrives through the routed path: the UART in
 * loopback receives its own byte, raising IRQ 4. */
KTEST(053, com1_irq_routing)
{
	uint16_t flags;
	uint32_t gsi = acpi_isa_gsi(IRQ_COM1, &flags);
	if (apic_mode) {
		struct ioapic_rte rte = { 0, 0 };
		KT_ASSERT(ioapic_read_rte(gsi, &rte));
		KT_EQ(rte.lo & IOART_INTVEC, IRQ_BASE + IRQ_COM1);
		KT_EQ(rte.lo & IOART_INTMASK, 0);
		KT_EQ(rte.lo & IOART_DELMOD, IOART_DELFIXED);
		KT_EQ(rte.lo & IOART_DESTMOD, IOART_DESTPHY);
		KT_EQ(rte.hi >> APIC_ID_SHIFT, lapic_bsp_id & 0xff);
		/* the PIT's pin stays masked: the tick comes from the LAPIC */
		uint32_t pit = acpi_isa_gsi(IRQ_TIMER, &flags);
		KT_ASSERT(ioapic_read_rte(pit, &rte));
		KT_EQ(rte.lo & IOART_INTMASK, IOART_INTMSET);
		KT_ASSERT(!ioapic_read_rte(10000, &rte)); /* no such GSI */
	}
	uint64_t before = irq_counts[IRQ_COM1];
	uint64_t f = irq_save(); /* no console output while the UART loops back */
	uint8_t mcr = inb(COM1 + 4);
	outb(COM1 + 4, mcr | 0x10);
	outb(COM1, 'Z');
	for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 1); i++)
		cpu_relax();
	outb(COM1 + 4, mcr);
	irq_restore(f);
	for (int i = 0; i < 100 && irq_counts[IRQ_COM1] == before; i++)
		delay_ms(1);
	KT_ASSERT(irq_counts[IRQ_COM1] > before);
	int c, got = 0;
	while ((c = console_getc()) >= 0)
		got |= c == 'Z';
	KT_ASSERT(got);
}
