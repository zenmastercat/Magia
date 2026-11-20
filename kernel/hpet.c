/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * hpet.c - the HPET main counter as a reference clock (calibrating the TSC
 * and the LAPIC timer). Only the free-running counter is used; no HPET
 * timer interrupts. Written from the IA-PC HPET specification 1.0a.
 */
#include <magia/acpi.h>
#include <magia/apic.h>
#include <magia/mm.h>
#include <magia/x86.h>

#define HPET_GCAP_ID    0x000 /* bits 63:32 period in fs, bit 13: 64-bit counter */
#define HPET_GEN_CONF   0x010
#define HPET_CONF_ENABLE 0x1
#define HPET_CONF_LEGACY 0x2
#define HPET_MAIN_COUNTER 0x0f0
#define HPET_CAP_COUNT_64 (1ULL << 13)
#define HPET_MAX_PERIOD 100000000ULL /* spec: at most 100 ns */

uint64_t hpet_period_fs;
static volatile uint64_t *hpet;
static uint64_t counter_mask;

static uint64_t rd(uint32_t off) { return hpet[off / 8]; }
static void wr(uint32_t off, uint64_t v) { hpet[off / 8] = v; }

bool hpet_present(void) { return hpet != NULL; }

bool hpet_init(void)
{
	if (!(acpi.found & ACPI_FOUND_HPET) || cmdline_has("nohpet"))
		return false;
	volatile uint64_t *h = vmm_map_mmio(acpi.hpet_addr, 1024);
	if (!h)
		return false;
	uint64_t cap = h[HPET_GCAP_ID / 8];
	uint64_t period = cap >> 32;
	if (period == 0 || period > HPET_MAX_PERIOD)
		return false; /* not a working HPET (all ones on a missing device) */
	hpet = h;
	hpet_period_fs = period;
	counter_mask = (cap & HPET_CAP_COUNT_64) ? ~0ULL : 0xffffffffULL;
	wr(HPET_GEN_CONF, (rd(HPET_GEN_CONF) & ~HPET_CONF_LEGACY) | HPET_CONF_ENABLE);
	return true;
}

uint64_t hpet_counter(void) { return rd(HPET_MAIN_COUNTER) & counter_mask; }

void hpet_wait_ms(uint32_t ms)
{
	uint64_t ticks = (uint64_t)ms * 1000000000000ULL / hpet_period_fs;
	uint64_t t0 = hpet_counter();
	while (((hpet_counter() - t0) & counter_mask) < ticks)
		cpu_relax();
}
