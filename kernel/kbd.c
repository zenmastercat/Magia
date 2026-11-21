/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * kbd.c - PS/2 keyboard on the i8042 controller (ports 0x60/0x64, IRQ 1).
 *
 * The controller is left in its PC/AT configuration: scancode set 2 from
 * the keyboard, translated to set 1 by the controller, which kbd_map.c
 * turns into ASCII. Characters go into the same input ring as COM1, so
 * the magia> monitor works from the keyboard and the serial line alike.
 * Many current boards (the Core Ultra target included) have no i8042 and
 * need USB HID (stage 2): the driver is skipped when the ACPI FADT says
 * there is no 8042, or when the controller does not answer.
 * Written from the IBM PC/AT technical reference; OpenBSD's
 * sys/dev/ic/pckbc.c shows the same command sequence.
 */
#include <magia/acpi.h>
#include <magia/kbd.h>
#include <magia/linker_set.h>
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

bool kbd_present;
volatile uint64_t kbd_bytes;
static struct kbd_state kst;

static bool wait_write(void)
{
	for (int i = 0; i < 100000; i++)
		if (!(inb(KBC_STATUS) & ST_IBF))
			return true;
	return false;
}

static bool wait_read(void)
{
	for (int i = 0; i < 100000; i++)
		if (inb(KBC_STATUS) & ST_OBF)
			return true;
	return false;
}

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

void kbd_init(void)
{
	if (cmdline_has("nokbd") || ((acpi.found & ACPI_FOUND_FADT) && !acpi.has_8042)) {
		kprintf("kbd: no i8042 (FADT), keyboard input needs USB (stage 2)\n");
		return;
	}
	if (inb(KBC_STATUS) == 0xff || !kbc_cmd(CMD_KBD_OFF) || !kbc_cmd(CMD_AUX_OFF)) {
		kprintf("kbd: i8042 does not respond\n");
		return;
	}
	for (int i = 0; i < 32 && (inb(KBC_STATUS) & ST_OBF); i++)
		(void)inb(KBC_DATA); /* drop stale bytes */
	if (!kbc_cmd(CMD_READ_CFG) || !wait_read()) {
		kprintf("kbd: i8042 does not answer\n");
		return;
	}
	uint8_t cfg = inb(KBC_DATA);
	cfg = (uint8_t)((cfg | CFG_KBD_INT | CFG_XLATE) & ~(CFG_AUX_INT | CFG_KBD_CLKOFF));
	if (!kbc_cmd(CMD_WRITE_CFG) || !wait_write())
		return;
	outb(KBC_DATA, cfg);
	irq_register(IRQ_KBD, kbd_irq);
	kbc_cmd(CMD_KBD_ON);
	irq_enable(IRQ_KBD);
	kbd_present = true;
	kprintf("kbd: PS/2 keyboard on i8042 (scancode set 1, US layout)\n");
}

static void kbd_sysinit(void) { kbd_init(); }
SYSINIT(kbd, 40001, kbd_sysinit); /* after interrupts (SI_ORDER_INTR) */
