/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* t_console.c - ktests for the framebuffer console and the PS/2 keyboard (03x). */
#include <magia/acpi.h>
#include <magia/fbcon.h>
#include <magia/kbd.h>
#include <magia/ktest.h>
#include <magia/x86.h>

/* Print a probe line, find it in the console grid, and read the glyph
 * pixels back from the framebuffer: every pixel must be the foreground or
 * background colour the Spleen bitmap says. */
KTEST(030, fbcon_glyph_pixels_read_back)
{
	KT_ASSERT(fbcon_active());
	if (!fbcon_active())
		return;
	KT_ASSERT(fbcon_cols >= 80 && fbcon_rows >= 25);
	static const char probe[] = "fbcon-probe Mg@~#";
	uint64_t f = irq_save(); /* nothing else may print or scroll meanwhile */
	kprintf("%s\n", probe);
	int col, row;
	fbcon_cursor(&col, &row);
	row -= 1; /* the probe is on the line above the cursor */
	bool text_ok = row >= 0;
	for (int i = 0; probe[i] && text_ok; i++)
		text_ok = fbcon_char_at(i, row) == (uint8_t)probe[i];
	int bad = 0, fgpix = 0;
	for (int i = 0; probe[i] && text_ok; i++) {
		const uint8_t *g = fbcon_glyph((uint8_t)probe[i]);
		for (int y = 0; y < 16; y++)
			for (int x = 0; x < 8; x++) {
				uint32_t want = (g[y] & (0x80 >> x)) ? fbcon_fg() : fbcon_bg();
				uint32_t got = fbcon_pixel(i * 8 + x, row * 16 + y);
				bad += got != want;
				fgpix += want == fbcon_fg();
			}
	}
	irq_restore(f);
	KT_ASSERT(text_ok);
	KT_EQ(bad, 0);
	KT_ASSERT(fgpix > 100); /* the glyphs really have ink */
	KT_ASSERT(fbcon_fg() != fbcon_bg());
	/* glyphs differ: 'M' and 'g' are not the same bitmap */
	KT_ASSERT(memcmp(fbcon_glyph('M'), fbcon_glyph('g'), 16) != 0);
	/* out-of-font bytes fall back to '?' instead of reading outside the table */
	KT_EQ(fbcon_glyph(0x01)[6], fbcon_glyph('?')[6]);
}

KTEST(031, kbd_scancode_translation)
{
	KT_EQ(kbd_map_selftest(), 0);
	struct kbd_state st = { 0 };
	/* every byte value is safe to feed, in any state */
	for (int pre = 0; pre < 2; pre++)
		for (int sc = 0; sc < 256; sc++) {
			if (pre)
				kbd_translate(&st, 0xe0);
			int c = kbd_translate(&st, (uint8_t)sc);
			KT_ASSERT((c >= -1 && c < 256) || c == KBD_PGUP || c == KBD_PGDN);
		}
	/* QEMU q35 has an i8042, except in the desktop-like boot (i8042=off) */
	KT_EQ(kbd_present, !cmdline_has("kt-desktop"));
}

/* The i8042 is probed even when the FADT flag is clear (QEMU clears it
 * with i8042=off; `fadt8042=0` simulates firmware that clears it wrongly),
 * and a missing controller is given up on quickly. */
KTEST(032, i8042_probe_ignores_the_fadt_flag)
{
	bool desktop = cmdline_has("kt-desktop");
	bool flag_clear = cmdline_has("fadt8042=0") || ((acpi.found & ACPI_FOUND_FADT) && !acpi.has_8042);
	KT_EQ(flag_clear, desktop || cmdline_has("fadt8042=0"));
	if (flag_clear)
		KT_ASSERT(kt_log_has(0, "kbd: FADT says no i8042; probing anyway"));
	KT_EQ(kbd_present, !desktop);
	if (desktop)
		KT_ASSERT(kt_log_has(0, "kbd: no i8042 (the controller does not respond)"));
	KT_ASSERT(kbd_probe_us < 100000); /* even with no controller: < 0.1 s */
}
