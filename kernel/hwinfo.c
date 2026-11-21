/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * hwinfo.c - `hwinfo`: what a bug report from a real machine needs, on one
 * screen. Neither target machine has a serial port, so the user
 * photographs this: CPU, firmware, memory, ACPI facts, then every PCI
 * function (BDF vendor:device class/subclass/prog-if) and every USB
 * controller and device (port path, VID:PID, speed, driver), packed into
 * as many columns of HWINFO_W characters as the screen is wide.
 */
#include <magia/acpi.h>
#include <magia/boot.h>
#include <magia/cpu.h>
#include <magia/fbcon.h>
#include <magia/hwinfo.h>
#include <magia/kbd.h>
#include <magia/mm.h>
#include <magia/pci.h>

static char row[600];
static int row_len, per_row, in_row;

static void flush_row(void)
{
	if (in_row) {
		kprintf("%.*s\n", row_len, row);
		row_len = in_row = 0;
	}
}

void hwinfo_add(const char *fmt, ...)
{
	char e[HWINFO_W];
	va_list ap;
	va_start(ap, fmt);
	int n = kvsnprintf(e, sizeof(e) - 1, fmt, ap); /* leaves room for a separating space */
	va_end(ap);
	n = MIN(n, (int)sizeof(e) - 2);
	int w = in_row + 1 < per_row ? HWINFO_W : n; /* no padding after the last column */
	row_len += ksnprintf(row + row_len, sizeof(row) - (size_t)row_len, "%-*s", w, e);
	if (++in_row >= per_row)
		flush_row();
}

static void section(const char *title)
{
	flush_row();
	kprintf("%s\n", title);
}

void hwinfo(void)
{
	int cols = fbcon_active() ? fbcon_cols : 80;
	per_row = MAX(1, MIN(cols / HWINFO_W, (int)(sizeof(row) / HWINFO_W)));
	row_len = in_row = 0;
	fbcon_new_page();
	kprintf("Magia " MAGIA_RELEASE " hwinfo | %s | %s | %lu MiB RAM | %s\n", cpu_feat.brand,
		bootinfo.firmware == BOOT_FW_BIOS ? "BIOS" : "UEFI", pmm_total_pages() >> 8,
		fbcon_active() ? "framebuffer" : "no framebuffer");
	kprintf("acpi: rev %u, FADT %s, 8042 flag %s, i8042 %s, SCI %u\n", acpi.rsdp_revision,
		acpi.found & ACPI_FOUND_FADT ? "yes" : "no", acpi.has_8042 ? "set" : "clear",
		kbd_present ? "keyboard found" : "none", acpi.sci_irq);
	acpi_power_dump();
	char t[16];
	ksnprintf(t, sizeof(t), "PCI (%d):", pci_ndevs);
	section(t);
	for (int i = 0; i < pci_ndevs; i++) {
		const struct pci_dev *d = &pci_devs[i];
		hwinfo_add("%02x:%02x.%u %04x:%04x %02x%02x%02x", d->bus, d->dev, d->fn, d->vendor, d->device,
			   d->class, d->subclass, d->prog_if);
	}
	section("USB (cN = controller, path = root port.hub port...):");
	usb_hwinfo();
	flush_row();
}
