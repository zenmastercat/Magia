/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* kbd.h - PS/2 (i8042) keyboard: scancode translation and the driver. */
#ifndef MAGIA_KBD_H
#define MAGIA_KBD_H

#include <stdbool.h>
#include <stdint.h>

struct kbd_state {
	bool shift_l, shift_r, ctrl, alt, caps, e0;
};

/* kbd_map.c: one scancode-set-1 byte -> ASCII, or -1 for none. */
int kbd_translate(struct kbd_state *st, uint8_t sc);
int kbd_map_selftest(void); /* number of failed checks */

/* kbd.c */
extern bool kbd_present;
extern volatile uint64_t kbd_bytes; /* scancode bytes received */
void kbd_init(void);

#endif
