/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * timer.c - PIT channel 0 at HZ (1000) for the scheduler tick, and the
 * TSC (calibrated against PIT channel 2) for nanosecond timestamps.
 */
#include <magia/apic.h>
#include <magia/linker_set.h>
#include <magia/sysctl.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

#define PIT_FREQ 1193182ULL

volatile uint64_t ticks;
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

static void tsc_calibrate(void)
{
	uint64_t best = ~0ULL;
	/* take the shortest of three 10 ms windows to filter out SMIs/vCPU stalls */
	for (int i = 0; i < 3; i++) {
		uint64_t t0 = rdtsc();
		pit2_wait((uint16_t)(PIT_FREQ / 100));
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
	sched_tick();
}

void timer_init(void)
{
	tsc_calibrate();
	uint16_t div = (uint16_t)((PIT_FREQ + HZ / 2) / HZ);
	outb(0x43, 0x34); /* ch0, lobyte/hibyte, mode 2 (rate generator) */
	outb(0x40, div & 0xff);
	outb(0x40, div >> 8);
	irq_register(IRQ_TIMER, timer_irq);
	pic_unmask(IRQ_TIMER);
}

static void intr_sysinit(void)
{
	lapic_virtual_wire(); /* Limine masked LINT0, the 8259 path */
	pic_init();
	timer_init();
	kprintf("timer: PIT %d Hz, TSC %lu.%03lu MHz\n", HZ, tsc_freq / 1000000,
		(tsc_freq / 1000) % 1000);
}
SYSINIT(intr, SI_ORDER_INTR, intr_sysinit);
