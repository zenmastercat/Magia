/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* kbd.h - PS/2 (i8042) keyboard: scancode translation and the driver. */
#ifndef MAGIA_KBD_H
#define MAGIA_KBD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct kbd_state {
	bool shift_l, shift_r, ctrl, alt, caps, e0;
};

/* Keys beyond ASCII. kbd_translate returns KBD_PGUP/KBD_PGDN for Page
 * Up/Down; the drivers send them on as VT220 sequences (ESC [5~ / [6~),
 * as a serial terminal does, and vt_decode() turns those back into
 * VT_PGUP/VT_PGDN for the monitor. */
#define KBD_PGUP 0x101
#define KBD_PGDN 0x102
#define VT_PGUP KBD_PGUP
#define VT_PGDN KBD_PGDN
#define VT_SEQ_PGUP "\x1b[5~"
#define VT_SEQ_PGDN "\x1b[6~"

struct vt_dec {
	int state, num;
};
int vt_decode(struct vt_dec *d, int c); /* console.c: c, -1 (consumed), VT_PGUP/DN */
void console_write_nolog(const char *s, size_t n);
void console_locked(void (*fn)(void *), void *arg);

/* kbd_map.c: one scancode-set-1 byte -> ASCII, KBD_PGUP/KBD_PGDN, or -1. */
int kbd_translate(struct kbd_state *st, uint8_t sc);
int kbd_map_selftest(void); /* number of failed checks */

/* kbd.c */
extern bool kbd_present;
extern volatile uint64_t kbd_bytes; /* scancode bytes received */
void kbd_init(void);

#endif
