/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * timer.c - interrupt controller and clock bring-up, the HZ (1000) tick and
 * nanosecond time from the TSC.
 *
 * TSC frequency, first source that works (kern.tsc_source, and the boot
 * log, say which):
 *   1. CPUID leaf 0x15 (crystal clock x TSC ratio) on an Intel CPU with an
 *      invariant TSC, the crystal from the table in the SDM when the CPU
 *      leaves it 0, cross-checked against leaf 0x16's base frequency;
 *   2. the HPET;
 *   3. PIT channel 2. Laptop firmware may gate the 8254 ("8254 Timer
 *      Clock Gating", e.g. the ThinkPad X13 Gen 1): its registers take
 *      writes but it never counts, so every PIT wait is bounded by the TSC
 *      and a dead PIT is logged, not waited for;
 *   4. CPUID 0x16's base frequency; 5. a guess, logged loudly.
 * The tick is the LAPIC's TSC-deadline timer (if the CPU has it) or its
 * timer in periodic mode, calibrated against the TSC - in APIC mode and
 * in legacy mode (`noapic`: 8259 for ISA interrupts, LAPIC LINT0 as a
 * virtual wire) alike. PIT channel 0 ticks only when there is no LAPIC
 * timer, or with `pittick` (a test of that path).
 * Test hooks: `nopit` pretends the PIT is gated, `nohpet` hides the HPET,
 * `nocpuidtsc` skips CPUID 0x15/0x16.
 */
#include <magia/acpi.h>
#include <magia/apic.h>
#include <magia/cpu.h>
#include <magia/linker_set.h>
#include <magia/sysctl.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

#define PIT_FREQ 1193182ULL
/* A 10 ms PIT wait that takes longer than this many TSC cycles means a
 * gated PIT: 40x the window at 1 GHz, 8x at 5 GHz. */
#define PIT_TIMEOUT_CYCLES 400000000ULL

volatile uint64_t ticks;
bool apic_mode;
enum timer_mode timer_mode = TIMER_PIT;
static uint64_t tsc_freq;   /* Hz */
static uint64_t tsc_base;   /* TSC at calibration */
static uint64_t ns_mult;    /* ns per TSC tick, 32.32 fixed point */
static char tsc_source[16] = "none";
bool pit_dead;
volatile uint64_t timer_irq_tsc;      /* TSC when the newest tick's handler ran */
volatile uint64_t timer_deadline_tsc; /* the TSC deadline of the newest tick (TSC-deadline mode) */

SYSCTL_U64_RO(kern_ticks, "kern.ticks", &ticks, "timer interrupts since boot (HZ=1000)");
SYSCTL_U64_RO(kern_tsc_hz, "kern.tsc_hz", &tsc_freq, "TSC frequency in Hz");
SYSCTL_STR_RO(kern_tsc_source, "kern.tsc_source", tsc_source,
	      "where the TSC frequency came from: cpuid15, hpet, pit, cpuid16, guess");

const char *tsc_source_name(void) { return tsc_source; }

/* Busy-wait on PIT channel 2 (one-shot) for `count` PIT ticks; false if
 * the PIT does not count (gated, or missing). */
static bool pit2_wait(uint16_t count)
{
	if (pit_dead)
		return false;
	bool gated = cmdline_has("nopit"); /* test hook: a PIT that never counts */
	uint8_t p61 = inb(0x61);
	outb(0x61, (p61 & ~0x02) | 0x01); /* gate on, speaker off */
	outb(0x43, 0xb0);                 /* ch2, lobyte/hibyte, mode 0 */
	outb(0x42, count & 0xff);
	outb(0x42, count >> 8);
	/* restart the count by toggling the gate */
	p61 = inb(0x61);
	outb(0x61, p61 & ~0x01);
	outb(0x61, p61 | 0x01);
	uint64_t t0 = rdtsc();
	while (gated || !(inb(0x61) & 0x20)) {
		if (rdtsc() - t0 > PIT_TIMEOUT_CYCLES) {
			pit_dead = true;
			kprintf("timer: PIT channel 2 does not count (gated?); not using the PIT\n");
			return false;
		}
		cpu_relax();
	}
	return true;
}

/* The CPU's own word on the TSC (Intel SDM vol. 3, 18.7.3): leaf 0x15
 * gives the crystal frequency and the TSC/crystal ratio; leaf 0x16 the
 * base frequency, which the TSC runs at. Pure, so ktest can feed it the
 * leaves of CPUs QEMU does not model. Returns 0 if unusable. */
uint64_t tsc_hz_from_cpuid(const struct tsc_cpuid *l)
{
	if (!l->intel || !l->invariant || l->max_leaf < 0x15 || l->den == 0 || l->num == 0)
		return 0;
	uint32_t base_mhz = l->max_leaf >= 0x16 ? l->base_mhz : 0;
	uint64_t crystal = l->crystal;
	if (crystal == 0) { /* SDM table 18-85 for CPUs that leave it 0 */
		uint32_t m = l->model;
		if (l->family == 6 && (m == 0x4e || m == 0x5e || m == 0x8e || m == 0x9e || m == 0xa5 || m == 0xa6))
			crystal = 24000000; /* Skylake .. Comet Lake client: the X13 Gen 1 is 0x8e */
		else if (l->family == 6 && m == 0x5c) /* Goldmont */
			crystal = 19200000;
		else if (l->family == 6 && m == 0x55) /* Skylake-X / Cascade Lake */
			crystal = 25000000;
		else if (base_mhz) /* derive it from the base frequency */
			crystal = (uint64_t)base_mhz * 1000000 * l->den / l->num;
		else
			return 0;
	}
	uint64_t hz = crystal * l->num / l->den;
	/* cross-check with the base frequency: a table or firmware error
	 * shows up as a big disagreement */
	if (base_mhz && (hz / 1000000 > base_mhz * 11ULL / 10 || hz / 1000000 < base_mhz * 9ULL / 10))
		return 0;
	return hz;
}

static uint64_t tsc_from_cpuid15(void)
{
	struct tsc_cpuid l = { 0 };
	uint32_t a, b, c, d;
	cpuid(0, 0, &l.max_leaf, &b, &c, &d);
	l.intel = strcmp(cpu_feat.vendor, "GenuineIntel") == 0;
	l.invariant = cpu_feat.invariant_tsc;
	cpuid(1, 0, &a, &b, &c, &d);
	l.family = (a >> 8) & 0xf;
	l.model = ((a >> 4) & 0xf) | ((a >> 12) & 0xf0);
	if (l.max_leaf >= 0x15)
		cpuid(0x15, 0, &l.den, &l.num, &l.crystal, &d);
	if (l.max_leaf >= 0x16) {
		cpuid(0x16, 0, &a, &b, &c, &d);
		l.base_mhz = a & 0xffff;
	}
	uint64_t hz = tsc_hz_from_cpuid(&l);
	if (!hz && l.intel && l.invariant && l.den && l.num)
		kprintf("timer: CPUID 0x15 (%u/%u, crystal %u Hz) disagrees with the base frequency (%u MHz); not using it\n",
			l.num, l.den, l.crystal, l.base_mhz);
	return hz;
}

/* For a photograph of a machine where the TSC source surprises: the raw
 * leaves. */
static void cpuid_tsc_report(void)
{
	uint32_t max, a, b, c, d;
	cpuid(0, 0, &max, &b, &c, &d);
	kprintf("timer: CPUID max leaf 0x%x, invariant TSC %s", max, cpu_feat.invariant_tsc ? "yes" : "no");
	if (max >= 0x15) {
		cpuid(0x15, 0, &a, &b, &c, &d);
		kprintf(", 0x15: %u/%u crystal %u Hz", b, a, c);
	}
	if (max >= 0x16) {
		cpuid(0x16, 0, &a, &b, &c, &d);
		kprintf(", 0x16: base %u MHz", a & 0xffff);
	}
	kprintf("\n");
}

static void set_tsc_hz(uint64_t hz, const char *src)
{
	tsc_freq = hz < 1000000 ? 1000000 : hz;
	ns_mult = (1000000000ULL << 32) / tsc_freq;
	tsc_base = rdtsc();
	strlcpy(tsc_source, src, sizeof(tsc_source));
}

static void tsc_calibrate(void)
{
	if (!cmdline_has("nocpuidtsc")) {
		uint64_t hz = tsc_from_cpuid15();
		if (hz) {
			set_tsc_hz(hz, "cpuid15");
			return;
		}
	}
	/* measured: the shortest of three 10 ms windows filters out SMIs and vCPU stalls */
	uint64_t best = ~0ULL;
	const char *src = hpet_present() ? "hpet" : "pit";
	for (int i = 0; i < 3; i++) {
		uint64_t t0 = rdtsc();
		if (hpet_present())
			hpet_wait_ms(10);
		else if (!pit2_wait((uint16_t)(PIT_FREQ / 100)))
			break;
		uint64_t d = rdtsc() - t0;
		if (d < best)
			best = d;
	}
	if (best != ~0ULL) {
		set_tsc_hz(best * 100, src);
		return;
	}
	uint32_t max, a, b, c, d;
	cpuid(0, 0, &max, &b, &c, &d);
	if (max >= 0x16 && !cmdline_has("nocpuidtsc")) {
		cpuid(0x16, 0, &a, &b, &c, &d);
		if (a & 0xffff) {
			set_tsc_hz((uint64_t)(a & 0xffff) * 1000000, "cpuid16");
			return;
		}
	}
	kprintf("timer: no clock to measure the TSC against (no CPUID 0x15/0x16, HPET or PIT); assuming 2 GHz\n");
	cpuid_tsc_report();
	set_tsc_hz(2000000000ULL, "guess");
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

static void tsc_wait_ms(uint32_t ms) { delay_ms(ms); }

static void timer_irq(struct trapframe *tf)
{
	(void)tf;
	timer_irq_tsc = rdtsc();
	timer_deadline_tsc = lapic_deadline();
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
	if (!lapic_present() || (!apic_mode && cmdline_has("pittick"))) {
		pit_start(); /* shortcut: no check that a gated PIT ticks; every x86-64 CPU has a LAPIC */
		irq_register(IRQ_TIMER, timer_irq);
		irq_enable(IRQ_TIMER);
		timer_mode = TIMER_PIT;
		return;
	}
	timer_mode = cpu_has_tsc_deadline() && !cmdline_has("notscdeadline") ? TIMER_TSC_DEADLINE
									  : TIMER_LAPIC_PERIODIC;
	lapic_timer_calibrate(tsc_wait_ms);
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
	kprintf("intr: %s, %s; timer: %s at %d Hz, TSC %lu.%03lu MHz (from %s)",
		apic_mode ? "I/O APIC" : "8259 PIC (noapic)",
		apic_mode ? (lapic_x2 ? "x2APIC" : "xAPIC") : "LAPIC virtual wire", timer_names[timer_mode], HZ,
		tsc_freq / 1000000, (tsc_freq / 1000) % 1000, tsc_source);
	if (timer_mode == TIMER_LAPIC_PERIODIC)
		kprintf(", LAPIC timer %lu kHz", lapic_timer_hz / 1000);
	kprintf("\n");
}
SYSINIT(intr, SI_ORDER_INTR, intr_sysinit);
