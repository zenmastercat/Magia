/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * acpi.c - hand the firmware tables to the Rust parser (rust/src/acpi.rs)
 * and keep its results in `acpi` for the APIC, HPET and later PCIe code.
 * C only decides which physical memory the parser may read: ranges the
 * memory map marks as ACPI or firmware data, all of which the direct map
 * covers.
 */
#include <magia/acpi.h>
#include <magia/boot.h>
#include <magia/linker_set.h>

struct acpi_info acpi;
int acpi_status = -100;

static bool acpi_type(uint64_t t)
{
	return t == MEM_ACPI_RECLAIM || t == MEM_ACPI_NVS || t == MEM_RESERVED_MAPPED;
}

const uint8_t *magia_acpi_map(uint64_t pa, size_t len)
{
	uint64_t end;
	if (len == 0 || __builtin_add_overflow(pa, (uint64_t)len, &end))
		return NULL;
	/* every byte must lie in ACPI-type entries (they may be adjacent) */
	uint64_t p = pa;
	while (p < end) {
		bool hit = false;
		for (size_t i = 0; i < bootinfo.nmmap; i++) {
			const struct boot_mmap_entry *m = &bootinfo.mmap[i];
			if (acpi_type(m->type) && p >= m->base && p - m->base < m->length) {
				p = m->base + m->length;
				hit = true;
				break;
			}
		}
		if (!hit)
			return NULL;
	}
	return phys_to_virt(pa);
}

uint32_t acpi_isa_gsi(int irq, uint16_t *flags)
{
	*flags = 0;
	for (uint32_t i = 0; i < acpi.nisos; i++)
		if (acpi.iso_irq[i] == irq) {
			*flags = acpi.iso_flags[i];
			return acpi.iso_gsi[i];
		}
	return (uint32_t)irq;
}

/* Firmware bytes: printable ASCII only, so a table cannot send terminal
 * escape sequences to the console. */
void acpi_sig_printable(const char in[4], char out[5])
{
	for (int i = 0; i < 4; i++)
		out[i] = in[i] >= 0x20 && in[i] <= 0x7e ? in[i] : '?';
	out[4] = 0;
}

static void acpi_sysinit(void)
{
	if (!bootinfo.rsdp_pa) {
		kprintf("acpi: no RSDP from the bootloader\n");
		return;
	}
	acpi_status = rs_acpi_parse(bootinfo.rsdp_pa, bootinfo.firmware == BOOT_FW_BIOS, &acpi, sizeof(acpi));
	if (acpi_status) {
		kprintf("acpi: tables rejected (error %d, RSDP at %p)\n", acpi_status, (void *)bootinfo.rsdp_pa);
		return;
	}
	kprintf("acpi: rev %u, %u tables:", acpi.rsdp_revision, acpi.ntables);
	for (uint32_t i = 0; i < acpi.ntables && i < ACPI_MAX_TABLES; i++) {
		char sig[5];
		acpi_sig_printable(acpi.table_sig[i], sig);
		kprintf(" %s", sig);
	}
	kprintf("\nacpi: %u CPUs (%u disabled), %u I/O APIC, %u overrides, LAPIC %p", acpi.ncpus,
		acpi.cpus_disabled, acpi.nioapics, acpi.nisos, (void *)acpi.lapic_addr);
	if (acpi.found & ACPI_FOUND_HPET)
		kprintf(", HPET %p", (void *)acpi.hpet_addr);
	if (acpi.found & ACPI_FOUND_MCFG)
		kprintf(", ECAM %p buses %u-%u", (void *)acpi.mcfg_base[0], acpi.mcfg_start_bus[0],
			acpi.mcfg_end_bus[0]);
	if (acpi.found & ACPI_FOUND_FADT)
		kprintf(", 8042 %s", acpi.has_8042 ? "yes" : "no");
	kprintf("%s\n", acpi.found & ACPI_FOUND_TRUNCATED ? " (truncated)" : "");
}
SYSINIT(acpi, 30001, acpi_sysinit); /* after kmem, before interrupts (SI_ORDER_INTR) */
