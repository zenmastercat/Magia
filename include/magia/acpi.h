/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * acpi.h - what the Rust ACPI table parser (rust/src/acpi.rs) found.
 * Layout must match `CAcpiInfo` there; rs_acpi_parse() refuses a size
 * mismatch.
 */
#ifndef MAGIA_ACPI_H
#define MAGIA_ACPI_H

#include <magia/kernel.h>

#define ACPI_MAX_CPUS    256
#define ACPI_MAX_IOAPICS 16
#define ACPI_MAX_ISOS    32
#define ACPI_MAX_NMIS    16
#define ACPI_MAX_MCFG    8
#define ACPI_MAX_TABLES  64

#define ACPI_FOUND_MADT 0x1
#define ACPI_FOUND_HPET 0x2
#define ACPI_FOUND_MCFG 0x4
#define ACPI_FOUND_FADT 0x8
#define ACPI_FOUND_TRUNCATED 0x100 /* more entries than Magia's arrays hold */

/* MPS INTI flags of an interrupt source override */
#define ACPI_INTI_POLARITY(f) ((f) & 3)        /* 0 conforms, 1 high, 3 low  */
#define ACPI_INTI_TRIGGER(f)  (((f) >> 2) & 3) /* 0 conforms, 1 edge, 3 level */

struct acpi_info {
	uint32_t found;
	uint32_t rsdp_revision;
	uint32_t ncpus, cpus_disabled;
	uint32_t cpu_apic_id[ACPI_MAX_CPUS];
	uint64_t lapic_addr;
	uint32_t pcat_compat;
	uint32_t nioapics;
	uint32_t ioapic_id[ACPI_MAX_IOAPICS];
	uint32_t ioapic_addr[ACPI_MAX_IOAPICS];
	uint32_t ioapic_gsi_base[ACPI_MAX_IOAPICS];
	uint32_t nisos;
	uint8_t iso_irq[ACPI_MAX_ISOS];
	uint16_t iso_flags[ACPI_MAX_ISOS];
	uint32_t iso_gsi[ACPI_MAX_ISOS];
	uint32_t nnmis;
	uint8_t nmi_lint[ACPI_MAX_NMIS];
	uint16_t nmi_flags[ACPI_MAX_NMIS];
	uint32_t nmi_uid[ACPI_MAX_NMIS];
	uint32_t madt_bad_entries;
	uint64_t hpet_addr;
	uint32_t hpet_min_tick;
	uint32_t nmcfg;
	uint64_t mcfg_base[ACPI_MAX_MCFG];
	uint16_t mcfg_segment[ACPI_MAX_MCFG];
	uint8_t mcfg_start_bus[ACPI_MAX_MCFG];
	uint8_t mcfg_end_bus[ACPI_MAX_MCFG];
	uint32_t fadt_revision, fadt_flags, fadt_iapc_boot_arch;
	uint32_t sci_irq, pm_tmr_blk, has_8042;
	uint32_t ntables;
	char table_sig[ACPI_MAX_TABLES][4];
	/* power (stage 2b): reboot through the reset register, poweroff in S5 */
	uint64_t reset_addr, pm1a_cnt, pm1b_cnt, sleep_ctl;
	uint32_t smi_cmd;
	uint8_t reset_space, reset_value, reset_ok;
	uint8_t pm1a_space, pm1b_space, sleep_ctl_space; /* ACPI_GAS_* */
	uint8_t acpi_enable, hw_reduced;
	uint8_t s5_found, slp_typa, slp_typb;
	uint8_t s5_error; /* 0, or where the \_S5 search stopped (rust/src/aml.rs AmlError + 1) */
	uint8_t s5_scan;  /* 1: \_S5 found by the byte-scan fallback, not the object-level walk */
	uint8_t s5_walk_op;    /* with s5_scan: the AML byte the walk stopped at ... */
	uint32_t s5_walk_stop; /* ... and its offset in the table */
	uint8_t s5_walk_bytes[8]; /* the DSDT bytes there */
};

#define ACPI_GAS_MEMORY 0
#define ACPI_GAS_IO 1
#define ACPI_GAS_PCI_CONFIG 2

extern struct acpi_info acpi;

/* power.c: ACPI reset register / S5, with fallbacks; never return. */
__noreturn void acpi_reboot(void);
__noreturn void acpi_poweroff(void);
void acpi_power_dump(void); /* one line for hwinfo */
enum preg { PREG_RESET, PREG_PM1, PREG_SMI };
bool power_reg_ok(uint8_t space, uint64_t addr, unsigned len, enum preg kind); /* plausible register? */
volatile uint8_t *power_mmio(uint64_t pa, unsigned len); /* every page of the range, or NULL */
extern int acpi_status; /* 0, or -code from rs_acpi_parse (no ACPI: -100) */

/* Rust: parse the tables reachable from the RSDP into *out (0 or -code);
 * bios: the RSDP came from the BIOS-area scan (16-byte aligned). */
int32_t rs_acpi_parse(uint64_t rsdp_pa, int32_t bios, struct acpi_info *out, size_t size);
/* C, for Rust: direct-map pointer to [pa, pa+len) if it is ACPI or firmware
 * memory in the memory map, else NULL. */
const uint8_t *magia_acpi_map(uint64_t pa, size_t len);

/* Global system interrupt and INTI flags for ISA IRQ `irq` (identity
 * mapped, edge, active high unless an override says otherwise). */
uint32_t acpi_isa_gsi(int irq, uint16_t *flags);
void acpi_sig_printable(const char in[4], char out[5]); /* non-printable -> ? */

#endif
