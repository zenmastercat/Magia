/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * ioapic.c - I/O APICs from the ACPI MADT: ISA IRQs routed to the BSP.
 *
 * Each I/O APIC is two registers (index at +0, window at +0x10); pin p's
 * redirection entry is the 64-bit pair at index 0x10 + 2p. ISA IRQ n goes
 * to the global system interrupt the MADT override names (identity
 * otherwise), with the override's polarity and trigger, edge/high by ISA
 * default, delivered fixed to the BSP at vector 32 + n.
 * Written from the 82093AA datasheet; semantics as in FreeBSD
 * sys/x86/x86/io_apic.c and sys/x86/acpica/madt.c.
 */
#include <magia/acpi.h>
#include <magia/apic.h>
#include <magia/mm.h>
#include <magia/spinlock.h>
#include <magia/trap.h>

struct ioapic {
	volatile uint32_t *regs;
	uint32_t gsi_base, npins;
};
static struct ioapic ioapics[ACPI_MAX_IOAPICS];
static int nioapics;
static spinlock_t ioapic_lock = SPINLOCK_INIT("ioapic");

static uint32_t io_read(const struct ioapic *io, uint32_t idx)
{
	io->regs[0] = idx;
	return io->regs[IOAPIC_WINDOW / 4];
}

static void io_write(const struct ioapic *io, uint32_t idx, uint32_t v)
{
	io->regs[0] = idx;
	io->regs[IOAPIC_WINDOW / 4] = v;
}

static struct ioapic *find(uint32_t gsi, uint32_t *pin)
{
	for (int i = 0; i < nioapics; i++)
		if (gsi >= ioapics[i].gsi_base && gsi - ioapics[i].gsi_base < ioapics[i].npins) {
			*pin = gsi - ioapics[i].gsi_base;
			return &ioapics[i];
		}
	return NULL;
}

bool ioapic_init(void)
{
	for (uint32_t i = 0; i < acpi.nioapics && i < ACPI_MAX_IOAPICS; i++) {
		struct ioapic *io = &ioapics[nioapics];
		io->regs = vmm_map_mmio(acpi.ioapic_addr[i], IOAPIC_WND_SIZE);
		if (!io->regs)
			continue;
		io->gsi_base = acpi.ioapic_gsi_base[i];
		io->npins = ((io_read(io, IOAPIC_VER) & IOART_VER_MAXREDIR) >> MAXREDIRSHIFT) + 1;
		for (uint32_t p = 0; p < io->npins; p++) {
			io_write(io, IOAPIC_REDTBL + 2 * p, IOART_INTMSET);
			io_write(io, IOAPIC_REDTBL + 2 * p + 1, 0);
		}
		nioapics++;
	}
	return nioapics > 0;
}

bool ioapic_read_rte(uint32_t gsi, struct ioapic_rte *rte)
{
	uint32_t pin;
	struct ioapic *io = find(gsi, &pin);
	if (!io)
		return false;
	spin_lock(&ioapic_lock);
	rte->lo = io_read(io, IOAPIC_REDTBL + 2 * pin);
	rte->hi = io_read(io, IOAPIC_REDTBL + 2 * pin + 1);
	spin_unlock(&ioapic_lock);
	return true;
}

int ioapic_route_isa(int irq)
{
	uint16_t flags;
	uint32_t gsi = acpi_isa_gsi(irq, &flags), pin;
	struct ioapic *io = find(gsi, &pin);
	if (!io || irq < 0 || irq >= 16)
		return -1;
	uint32_t lo = IOART_DELFIXED | IOART_DESTPHY | (uint32_t)(IRQ_BASE + irq);
	if (ACPI_INTI_POLARITY(flags) == 3)
		lo |= IOART_INTALO; /* ISA default (0 "conforms") is active high */
	if (ACPI_INTI_TRIGGER(flags) == 3)
		lo |= IOART_TRGRLVL; /* ISA default is edge */
	/* shortcut: 8-bit physical destination (xAPIC format); BSP ids >= 255
	 * need interrupt remapping (VT-d), stage 4 SMP work. */
	uint32_t hi = (lapic_bsp_id & 0xff) << APIC_ID_SHIFT;
	spin_lock(&ioapic_lock);
	io_write(io, IOAPIC_REDTBL + 2 * pin + 1, hi);
	io_write(io, IOAPIC_REDTBL + 2 * pin, lo);
	spin_unlock(&ioapic_lock);
	return (int)gsi;
}
