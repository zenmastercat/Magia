/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * kbd.c - PS/2 keyboard on the i8042 controller (ports 0x60/0x64, IRQ 1).
 *
 * The controller is left in its PC/AT configuration: scancode set 2 from
 * the keyboard, translated to set 1 by the controller, which kbd_map.c
 * turns into ASCII. Characters go into the same input ring as COM1, so
 * the magia> monitor works from the keyboard and the serial line alike.
 * Laptops (the ThinkPad X13 Gen 1 included) have their keyboard on the
 * embedded controller's i8042; desktops like the Core Ultra target have
 * none. The controller is probed even when the ACPI FADT's 8042 flag is
 * clear, because several laptop firmwares clear it wrongly; every wait is
 * bounded by the TSC, so a missing controller costs at most a few
 * milliseconds. `nokbd` skips the driver; `fadt8042=0` (a test hook)
 * treats the flag as clear.
 * Written from the IBM PC/AT technical reference; OpenBSD's
 * sys/dev/ic/pckbc.c shows the same command sequence.
 */
#include <magia/acpi.h>
#include <magia/kbd.h>
#include <magia/linker_set.h>
#include <magia/sysctl.h>
#include <magia/trap.h>
#include <magia/x86.h>

#define KBC_DATA   0x60
#define KBC_STATUS 0x64
#define KBC_CMD    0x64
#define ST_OBF     0x01 /* output buffer full: a byte to read */
#define ST_IBF     0x02 /* input buffer full: controller busy  */
#define ST_AUX     0x20 /* the byte is from the mouse port     */
#define CMD_READ_CFG  0x20
#define CMD_WRITE_CFG 0x60
#define CMD_AUX_OFF   0xa7
#define CMD_KBD_OFF   0xad
#define CMD_KBD_ON    0xae
#define CFG_KBD_INT   0x01
#define CFG_AUX_INT   0x02
#define CFG_KBD_CLKOFF 0x10
#define CFG_XLATE     0x40
#define KBC_WAIT_MS   20 /* per controller handshake */

bool kbd_present;
volatile uint64_t kbd_bytes;
uint64_t kbd_probe_us; /* time the probe took */
static struct kbd_state kst;

SYSCTL_U64_RO(hw_kbd_probe_us, "hw.kbd.probe_us", &kbd_probe_us, "microseconds the i8042 probe took");

/* Wait until (status & mask) == want, at most KBC_WAIT_MS. */
static bool kbc_wait(uint8_t mask, uint8_t want)
{
	uint64_t end = now_ns() + KBC_WAIT_MS * 1000000ULL;
	do {
		if ((inb(KBC_STATUS) & mask) == want)
			return true;
		cpu_relax();
	} while (now_ns() < end);
	return false;
}

static bool wait_write(void) { return kbc_wait(ST_IBF, 0); }
static bool wait_read(void) { return kbc_wait(ST_OBF, ST_OBF); }

static bool kbc_cmd(uint8_t c)
{
	if (!wait_write())
		return false;
	outb(KBC_CMD, c);
	return true;
}

static void kbd_irq(struct trapframe *tf)
{
	(void)tf;
	uint8_t st;
	while ((st = inb(KBC_STATUS)) & ST_OBF) {
		uint8_t sc = inb(KBC_DATA);
		if (st & ST_AUX)
			continue; /* mouse byte */
		kbd_bytes++;
		int c = kbd_translate(&kst, sc);
		if (c == KBD_PGUP || c == KBD_PGDN) {
			for (const char *s = c == KBD_PGUP ? VT_SEQ_PGUP : VT_SEQ_PGDN; *s; s++)
				console_rx_push((uint8_t)*s);
		} else if (c >= 0) {
			console_rx_push((uint8_t)c);
		}
	}
}

/* Find and set up the controller; false (with a log line) if it is not
 * there. Every step is bounded: a missing i8042 reads 0xff or never
 * clears its input-buffer-full bit. */
static bool kbd_probe(void)
{
	if (inb(KBC_STATUS) == 0xff || !kbc_cmd(CMD_KBD_OFF) || !kbc_cmd(CMD_AUX_OFF)) {
		kprintf("kbd: no i8042 (the controller does not respond)\n");
		return false;
	}
	for (int i = 0; i < 32 && (inb(KBC_STATUS) & ST_OBF); i++)
		(void)inb(KBC_DATA); /* drop stale bytes */
	if (!kbc_cmd(CMD_READ_CFG) || !wait_read()) {
		kprintf("kbd: no i8042 (the controller does not answer)\n");
		return false;
	}
	uint8_t cfg = inb(KBC_DATA);
	cfg = (uint8_t)((cfg | CFG_KBD_INT | CFG_XLATE) & ~(CFG_AUX_INT | CFG_KBD_CLKOFF));
	if (!kbc_cmd(CMD_WRITE_CFG) || !wait_write()) {
		kprintf("kbd: no i8042 (configuration not accepted)\n");
		return false;
	}
	outb(KBC_DATA, cfg);
	return true;
}

void kbd_init(void)
{
	if (cmdline_has("nokbd")) {
		kprintf("kbd: skipped (nokbd)\n");
		return;
	}
	bool flag = !(acpi.found & ACPI_FOUND_FADT) || acpi.has_8042;
	if (cmdline_has("fadt8042=0"))
		flag = false; /* test hook: firmware that clears the flag */
	if (!flag)
		kprintf("kbd: FADT says no i8042; probing anyway (some laptop firmware clears the flag wrongly)\n");
	uint64_t t0 = now_ns();
	bool ok = kbd_probe();
	kbd_probe_us = (now_ns() - t0) / 1000;
	if (!ok)
		return;
	irq_register(IRQ_KBD, kbd_irq);
	kbc_cmd(CMD_KBD_ON);
	irq_enable(IRQ_KBD);
	kbd_present = true;
	kprintf("kbd: PS/2 keyboard on i8042 (scancode set 1, US layout)%s\n",
		flag ? "" : " although the FADT flag is clear");
}

static void kbd_sysinit(void) { kbd_init(); }
SYSINIT(kbd, 40001, kbd_sysinit); /* after interrupts (SI_ORDER_INTR) */
