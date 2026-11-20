/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * timer.c - interrupt controller and clock bring-up, the HZ (1000) tick and
 * nanosecond time from the TSC.
 *
 * APIC mode (default when the MADT lists an I/O APIC): the 8259s are
 * remapped and fully masked, ISA IRQs go through the I/O APIC, and the
 * tick is the LAPIC's TSC-deadline timer (if the CPU has it) or its timer
 * in periodic mode. The TSC and the LAPIC timer are calibrated against
 * the HPET; without an HPET, against PIT channel 2.
 * Legacy mode ("noapic", or no usable MADT): 8259 + PIT channel 0 at HZ,
 * with LAPIC LINT0 reopened as a virtual wire.
 */
#include <magia/acpi.h>
#include <magia/apic.h>
#include <magia/linker_set.h>
#include <magia/sysctl.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

#define PIT_FREQ 1193182ULL

volatile uint64_t ticks;
bool apic_mode;
enum timer_mode timer_mode = TIMER_PIT;
static uint64_t tsc_freq;   /* Hz */
static uint64_t tsc_base;   /* TSC at calibration */
static uint64_t ns_mult;    /* ns per TSC tick, 32.32 fixed point */

SYSCTL_U64_RO(kern_ticks, "kern.ticks", &ticks, "timer interrupts since boot (HZ=1000)");
SYSCTL_U64_RO(kern_tsc_hz, "kern.tsc_hz", &tsc_freq, "calibrated TSC frequency in Hz");

/* Busy-wait on PIT channel 2 (one-shot) for `count` PIT ticks. */
static void pit2_wait(uint16_t count)
{
	uint8_t p61 = inb(0x61);
	outb(0x61, (p61 & ~0x02) | 0x01); /* gate on, speaker off */
	outb(0x43, 0xb0);                 /* ch2, lobyte/hibyte, mode 0 */
	outb(0x42, count & 0xff);
	outb(0x42, count >> 8);
	/* restart the count by toggling the gate */
	p61 = inb(0x61);
	outb(0x61, p61 & ~0x01);
	outb(0x61, p61 | 0x01);
	while (!(inb(0x61) & 0x20))
		cpu_relax();
}

static void pit_wait_ms(uint32_t ms)
{
	if (ms == 10) {
		pit2_wait((uint16_t)(PIT_FREQ / 100));
		return;
	}
	while (ms--)
		pit2_wait((uint16_t)(PIT_FREQ / 1000));
}

/* The reference clock for calibration: HPET if present, else the PIT. */
static void ref_wait_ms(uint32_t ms)
{
	if (hpet_present())
		hpet_wait_ms(ms);
	else
		pit_wait_ms(ms);
}

static void tsc_calibrate(void)
{
	uint64_t best = ~0ULL;
	/* take the shortest of three 10 ms windows to filter out SMIs/vCPU stalls */
	for (int i = 0; i < 3; i++) {
		uint64_t t0 = rdtsc();
		ref_wait_ms(10);
		uint64_t d = rdtsc() - t0;
		if (d < best)
			best = d;
	}
	tsc_freq = best * 100;
	if (tsc_freq < 1000000)
		tsc_freq = 1000000;
	ns_mult = (1000000000ULL << 32) / tsc_freq;
	tsc_base = rdtsc();
}

uint64_t now_ns(void)
{
	uint64_t d = rdtsc() - tsc_base;
	return (uint64_t)(((unsigned __int128)d * ns_mult) >> 32);
}

uint64_t tsc_hz(void) { return tsc_freq; }
uint64_t uptime_ms(void) { return ticks; }

void delay_ms(uint64_t ms)
{
	uint64_t end = now_ns() + ms * 1000000ULL;
	while (now_ns() < end)
		cpu_relax();
}

static void timer_irq(struct trapframe *tf)
{
	(void)tf;
	ticks++;
	lapic_timer_rearm();
	sched_tick();
}

static void pit_start(void)
{
	uint16_t div = (uint16_t)((PIT_FREQ + HZ / 2) / HZ);
	outb(0x43, 0x34); /* ch0, lobyte/hibyte, mode 2 (rate generator) */
	outb(0x40, div & 0xff);
	outb(0x40, div >> 8);
}

void timer_init(void)
{
	hpet_init();
	tsc_calibrate();
	if (!apic_mode) {
		pit_start();
		irq_register(IRQ_TIMER, timer_irq);
		irq_enable(IRQ_TIMER);
		timer_mode = TIMER_PIT;
		return;
	}
	timer_mode = cpu_has_tsc_deadline() && !cmdline_has("notscdeadline") ? TIMER_TSC_DEADLINE
									  : TIMER_LAPIC_PERIODIC;
	lapic_timer_calibrate(ref_wait_ms);
	irq_register_vector(VEC_LAPIC_TIMER, timer_irq);
	lapic_timer_start(HZ);
}

static const char *const timer_names[] = { "PIT", "LAPIC periodic", "TSC deadline" };

static void intr_sysinit(void)
{
	pic_init(); /* remapped to 32..47 so stray 8259 interrupts are recognisable */
	apic_mode = !cmdline_has("noapic") && (acpi.found & ACPI_FOUND_MADT) && acpi.nioapics > 0;
	if (apic_mode) {
		pic_mask_all();
		lapic_init();
		apic_mode = ioapic_init();
	}
	if (!apic_mode)
		lapic_virtual_wire(); /* Limine masked LINT0, the 8259's path */
	timer_init();
	kprintf("intr: %s, %s; timer: %s at %d Hz, TSC %lu.%03lu MHz (%s reference)",
		apic_mode ? "I/O APIC" : "8259 PIC (noapic)",
		apic_mode ? (lapic_x2 ? "x2APIC" : "xAPIC") : "LAPIC virtual wire", timer_names[timer_mode], HZ,
		tsc_freq / 1000000, (tsc_freq / 1000) % 1000, hpet_present() ? "HPET" : "PIT");
	if (timer_mode == TIMER_LAPIC_PERIODIC)
		kprintf(", LAPIC timer %lu kHz", lapic_timer_hz / 1000);
	kprintf("\n");
}
SYSINIT(intr, SI_ORDER_INTR, intr_sysinit);
