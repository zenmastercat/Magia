/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * power.c - reboot and poweroff through ACPI, without AML execution.
 *
 * Reboot: the FADT reset register (ACPI 6.5 4.8.3.6; on Intel PCs I/O port
 * 0xcf9), then the 8042 reset pulse and a triple fault (machine_reboot in
 * panic.c). Poweroff: enter S5 (ACPI 6.5 16.1.7): the sleep type values
 * come from the `\_S5` package, which rust/src/aml.rs finds in the DSDT
 * (or an SSDT); if the firmware left the machine in legacy mode, ACPI
 * mode is switched on through SMI_CMD first; then SLP_TYPx and SLP_EN go
 * into PM1a (and PM1b) control, or into the sleep control register on
 * hardware-reduced ACPI. The order of writes follows OpenBSD's
 * acpi_sleep_pm (ideas; written from the specification).
 * QEMU's isa-debug-exit stays only as the ktest exit path (qemu_exit).
 * shortcut: no _PTS/_GTS methods are run (no AML interpreter), which some
 * firmware wants before S5; reset registers in PCI configuration space
 * are not written (every Intel PC uses I/O 0xcf9).
 */
#include <magia/acpi.h>
#include <magia/hwinfo.h>
#include <magia/mm.h>
#include <magia/trap.h>
#include <magia/x86.h>

#define PM1_SCI_EN 0x0001
#define PM1_SLP_TYP(x) (((x) & 7u) << 10)
#define PM1_SLP_TYP_MASK (7u << 10)
#define PM1_SLP_EN 0x2000
#define SLEEP_CTL_TYP(x) (((x) & 7u) << 2) /* hardware-reduced sleep control register */
#define SLEEP_CTL_EN 0x20

static const char *space_name(uint8_t s) { return s == ACPI_GAS_IO ? "I/O" : s == ACPI_GAS_MEMORY ? "memory" : "PCI"; }

static volatile uint8_t *mmio(uint64_t pa) { return vmm_map_mmio(pa & ~(PAGE_SIZE - 1), PAGE_SIZE) ? (volatile uint8_t *)phys_to_virt(pa) : NULL; }

static bool reg_read16(uint8_t space, uint64_t addr, uint16_t *v)
{
	if (space == ACPI_GAS_IO) {
		*v = inw((uint16_t)addr);
		return true;
	}
	volatile uint8_t *p = space == ACPI_GAS_MEMORY ? mmio(addr) : NULL;
	if (p)
		*v = *(volatile uint16_t *)p;
	return p != NULL;
}

static bool reg_write(uint8_t space, uint64_t addr, uint16_t v, bool wide)
{
	if (space == ACPI_GAS_IO) {
		if (wide)
			outw((uint16_t)addr, v);
		else
			outb((uint16_t)addr, (uint8_t)v);
		return true;
	}
	volatile uint8_t *p = space == ACPI_GAS_MEMORY ? mmio(addr) : NULL;
	if (!p)
		return false;
	if (wide)
		*(volatile uint16_t *)p = v;
	else
		*p = (uint8_t)v;
	return true;
}

void acpi_reboot(void)
{
	cli();
	if (acpi.reset_ok && acpi.reset_space != ACPI_GAS_PCI_CONFIG) {
		kprintf("reboot: ACPI reset register (%s 0x%lx = 0x%02x)\n", space_name(acpi.reset_space), acpi.reset_addr,
			acpi.reset_value);
		reg_write(acpi.reset_space, acpi.reset_addr, acpi.reset_value, false);
		delay_ms(500);
		kprintf("reboot: the reset register did not reset; trying the 8042 and a triple fault\n");
	} else {
		kprintf("reboot: no usable ACPI reset register; the 8042 and a triple fault\n");
	}
	machine_reboot();
}

/* Legacy-mode firmware (SCI_EN clear) hands ACPI over when ACPI_ENABLE
 * is written to SMI_CMD; wait up to 3 s for SCI_EN. */
static void acpi_mode_on(void)
{
	uint16_t v;
	if (!reg_read16(acpi.pm1a_space, acpi.pm1a_cnt, &v) || (v & PM1_SCI_EN) || !acpi.smi_cmd || !acpi.acpi_enable ||
	    acpi.smi_cmd > 0xffff)
		return;
	kprintf("poweroff: firmware in legacy mode, enabling ACPI through SMI_CMD 0x%x\n", acpi.smi_cmd);
	outb((uint16_t)acpi.smi_cmd, acpi.acpi_enable);
	for (int i = 0; i < 300; i++) {
		delay_ms(10);
		if (reg_read16(acpi.pm1a_space, acpi.pm1a_cnt, &v) && (v & PM1_SCI_EN))
			return;
	}
	kprintf("poweroff: SCI_EN did not come on; trying S5 anyway\n");
}

void acpi_poweroff(void)
{
	if (!acpi.s5_found) {
		kprintf("poweroff: no \\_S5 sleep type in the DSDT or SSDTs (reader stopped with %u); halting - "
			"switch the machine off by hand\n", acpi.s5_error);
		machine_halt();
	}
	if (acpi.hw_reduced && acpi.sleep_ctl) {
		kprintf("poweroff: ACPI S5 (hardware-reduced: SLP_TYP %u into the sleep control register)\n", acpi.slp_typa);
		cli();
		reg_write(acpi.sleep_ctl_space, acpi.sleep_ctl, SLEEP_CTL_TYP(acpi.slp_typa) | SLEEP_CTL_EN, false);
	} else if (acpi.pm1a_cnt) {
		acpi_mode_on();
		kprintf("poweroff: ACPI S5 (SLP_TYPa %u into PM1a control %s 0x%lx", acpi.slp_typa,
			space_name(acpi.pm1a_space), acpi.pm1a_cnt);
		if (acpi.pm1b_cnt)
			kprintf(", SLP_TYPb %u into PM1b %s 0x%lx", acpi.slp_typb, space_name(acpi.pm1b_space), acpi.pm1b_cnt);
		kprintf(")\n");
		cli();
		uint16_t a = 0, b = 0;
		reg_read16(acpi.pm1a_space, acpi.pm1a_cnt, &a);
		if (acpi.pm1b_cnt)
			reg_read16(acpi.pm1b_space, acpi.pm1b_cnt, &b);
		/* sleep type first, then SLP_EN (ACPI 6.5 16.1.7) */
		a = (uint16_t)((a & ~(PM1_SLP_TYP_MASK | PM1_SLP_EN)) | PM1_SLP_TYP(acpi.slp_typa));
		b = (uint16_t)((b & ~(PM1_SLP_TYP_MASK | PM1_SLP_EN)) | PM1_SLP_TYP(acpi.slp_typb));
		reg_write(acpi.pm1a_space, acpi.pm1a_cnt, a, true);
		if (acpi.pm1b_cnt)
			reg_write(acpi.pm1b_space, acpi.pm1b_cnt, b, true);
		reg_write(acpi.pm1a_space, acpi.pm1a_cnt, a | PM1_SLP_EN, true);
		if (acpi.pm1b_cnt)
			reg_write(acpi.pm1b_space, acpi.pm1b_cnt, b | PM1_SLP_EN, true);
	} else {
		kprintf("poweroff: the FADT has no PM1a control block; halting - switch the machine off by hand\n");
		machine_halt();
	}
	delay_ms(2000);
	kprintf("poweroff: still running after the S5 write; halting - switch the machine off by hand\n");
	machine_halt();
}

void acpi_power_dump(void)
{
	kprintf("power: reset %s", acpi.reset_ok ? space_name(acpi.reset_space) : "none (8042/triple fault)");
	if (acpi.reset_ok)
		kprintf(" 0x%lx=0x%02x", acpi.reset_addr, acpi.reset_value);
	kprintf(", PM1a %s 0x%lx", space_name(acpi.pm1a_space), acpi.pm1a_cnt);
	if (acpi.pm1b_cnt)
		kprintf(", PM1b 0x%lx", acpi.pm1b_cnt);
	if (acpi.s5_found)
		kprintf(", \\_S5 %u/%u", acpi.slp_typa, acpi.slp_typb);
	else
		kprintf(", no \\_S5 (%u)", acpi.s5_error);
	kprintf("%s\n", acpi.hw_reduced ? ", hardware-reduced" : "");
}
