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
#include <magia/cpu.h>
#include <magia/ktest.h>
#include <magia/trap.h>
#include <magia/x86.h>

#define COM1 0x3f8

KTEST(050, interrupt_controller_mode)
{
	if (cmdline_has("noapic")) {
		KT_ASSERT(!apic_mode);
		/* the PIT ticks only when asked to: a laptop may gate it */
		bool pit = cmdline_has("pittick");
		KT_EQ(timer_mode == TIMER_PIT, pit);
		KT_EQ(inb(0x21) & 1, pit ? 0 : 1); /* PIT line open on the 8259 only then */
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
	if (timer_mode != TIMER_PIT)
		KT_ASSERT(vec_counts[VEC_LAPIC_TIMER] - v0 >= dt); /* through the LAPIC vector */
	else
		KT_EQ(vec_counts[VEC_LAPIC_TIMER], v0);
}

KTEST(052, timer_calibration_sanity)
{
	KT_ASSERT(tsc_hz() >= 100000000ULL && tsc_hz() <= 20000000000ULL);
	KT_EQ(hpet_present(), !cmdline_has("nohpet")); /* QEMU q35 has one */
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

/* Where the TSC frequency came from, in the documented order: CPUID 0x15
 * (Intel, invariant TSC), then the HPET, then the PIT (bounded: a gated
 * PIT is detected, not waited on forever), then CPUID 0x16, then a guess.
 * `nopit` pretends the PIT is gated, `nohpet` hides the HPET. */
KTEST(054, tsc_source_order)
{
	const char *s = tsc_source_name();
	bool known = false;
	static const char *const names[] = { "cpuid15", "hpet", "pit", "cpuid16", "guess" };
	for (size_t i = 0; i < ARRAY_SIZE(names); i++)
		known |= strcmp(s, names[i]) == 0;
	KT_ASSERT(known);
	bool c15 = strcmp(s, "cpuid15") == 0;
	KT_ASSERT(!c15 || cpu_feat.invariant_tsc);
	if (cmdline_has("nocpuidtsc"))
		KT_ASSERT(!c15 && strcmp(s, "cpuid16") != 0);
	if (!c15 && !cmdline_has("nohpet"))
		KT_ASSERT(strcmp(s, "hpet") == 0);
	if (cmdline_has("nohpet") && cmdline_has("nopit")) {
		KT_ASSERT(strcmp(s, "hpet") != 0 && strcmp(s, "pit") != 0);
		KT_ASSERT(timer_mode != TIMER_PIT); /* the LAPIC timer ticks */
	}
	if (cmdline_has("nopit") && !c15 && !hpet_present())
		KT_ASSERT(pit_dead); /* the bounded wait gave up and said so */
	/* nothing in the log says the boot waited on a dead PIT for long */
	KT_ASSERT(ticks > 0);
}

/* The CPUID 0x15 arithmetic on leaves QEMU does not model (it leaves
 * 0x15/0x16 zero, even with +invtsc under KVM). */
KTEST(055, tsc_hz_from_cpuid_leaves)
{
	/* ThinkPad X13 Gen 1, i5-10210U (Comet Lake, family 6 model 0x8e):
	 * crystal 0 -> 24 MHz from the SDM table; 1.6 GHz base */
	struct tsc_cpuid cml = { true, true, 0x16, 6, 0x8e, 3, 200, 0, 1600 };
	KT_EQ(tsc_hz_from_cpuid(&cml), 1600000000ULL);
	/* Core Ultra 9 285K-like: 38.4 MHz crystal reported, 3.7 GHz base */
	struct tsc_cpuid arl = { true, true, 0x23, 6, 0xc6, 2, 193, 38400000, 3700 };
	KT_EQ(tsc_hz_from_cpuid(&arl), 3705600000ULL);
	/* no 0x16: no cross-check, the value stands */
	struct tsc_cpuid no16 = arl;
	no16.max_leaf = 0x15;
	KT_EQ(tsc_hz_from_cpuid(&no16), 3705600000ULL);
	/* crystal 0, unknown model: derived from the base frequency */
	struct tsc_cpuid derive = { true, true, 0x16, 6, 0x99, 1, 100, 0, 2500 };
	KT_EQ(tsc_hz_from_cpuid(&derive), 2500000000ULL);
	/* the table says 24 MHz but the base frequency disagrees by > 10 %: refused */
	struct tsc_cpuid wrong = cml;
	wrong.base_mhz = 1000;
	KT_EQ(tsc_hz_from_cpuid(&wrong), 0);
	/* unusable: QEMU's zeros, no invariant TSC, not Intel, old CPU */
	struct tsc_cpuid qemu = { true, true, 0x23, 6, 0xc6, 0, 0, 0, 0 };
	KT_EQ(tsc_hz_from_cpuid(&qemu), 0);
	struct tsc_cpuid var = arl;
	var.invariant = false;
	KT_EQ(tsc_hz_from_cpuid(&var), 0);
	var = arl;
	var.intel = false;
	KT_EQ(tsc_hz_from_cpuid(&var), 0);
	var = arl;
	var.max_leaf = 0xd;
	KT_EQ(tsc_hz_from_cpuid(&var), 0);
	struct tsc_cpuid unknown = { true, true, 0x15, 6, 0x99, 1, 100, 0, 0 };
	KT_EQ(tsc_hz_from_cpuid(&unknown), 0);
	/* review L1: extreme register values must not wrap 64 bits into a
	 * plausible-looking frequency; anything outside 100 MHz..20 GHz is
	 * refused */
	struct tsc_cpuid big = { true, true, 0x15, 6, 0x99, 1, 0xffffffffu, 0xffffffffu, 0 };
	KT_EQ(tsc_hz_from_cpuid(&big), 0); /* 1.8e19 Hz: fits 64 bits, but absurd */
	struct tsc_cpuid wrap = { true, true, 0x15, 6, 0x99, 1, 0xffffffffu, 0x40000000u, 0 };
	KT_EQ(tsc_hz_from_cpuid(&wrap), 0); /* crystal * num overflows 64 bits */
	struct tsc_cpuid dwrap = { true, true, 0x16, 6, 0x99, 0xffffffffu, 1, 0, 0xffff };
	KT_EQ(tsc_hz_from_cpuid(&dwrap), 0); /* base * 1e6 * den overflows when deriving the crystal */
	struct tsc_cpuid wrap16 = { true, true, 0x16, 6, 0x99, 1, 0x8000u, 0x10000000u, 0x1000 };
	KT_EQ(tsc_hz_from_cpuid(&wrap16), 0); /* the product wraps to a value near the base: still refused */
	struct tsc_cpuid slow = { true, true, 0x15, 6, 0x99, 1000, 1, 24000000, 0 };
	KT_EQ(tsc_hz_from_cpuid(&slow), 0); /* 24 kHz */
}
