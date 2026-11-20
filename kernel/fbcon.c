/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * fbcon.c - text console on the UEFI/VBE linear framebuffer Limine hands
 * over (32 bits per pixel), drawn with the Spleen 8x16 font from OpenBSD
 * (kernel/font/spleen8x16.h, BSD-2-Clause, Frederic Cambus).
 *
 * The console keeps a character grid (`shadow`) and a copy of what is on
 * screen (`drawn`); console_write() updates the grid and fbcon_flush()
 * redraws only cells that changed, so scrolling one line costs a redraw
 * of the non-blank cells, and the framebuffer (write-combining memory) is
 * never read. Serial stays the primary console; this is the screen copy.
 */
#include <magia/boot.h>
#include <magia/fbcon.h>

/* What spleen8x16.h needs from OpenBSD's <dev/wscons/wsconsio.h>. */
typedef unsigned char u_char;
struct wsdisplay_font {
	const char *name;
	int index, firstchar, numchars, encoding;
	int fontwidth, fontheight, stride;
	int bitorder, byteorder;
	void *cookie;
	void *data;
};
#define WSDISPLAY_FONTENC_ISO 0
#define WSDISPLAY_FONTORDER_L2R 1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "font/spleen8x16.h"
#pragma GCC diagnostic pop

#define FW 8
#define FH 16
#define MAX_COLS 512 /* 4096 pixels */
#define MAX_ROWS 256 /* 4096 pixels */
#define CURSOR 0x100 /* `drawn` flag: cell shown inverted */

int fbcon_cols, fbcon_rows;
static volatile uint32_t *fb;
static uint64_t pitch32; /* pixels per scan line */
static uint32_t fg, bg;
static int cx, cy;
static uint8_t shadow[MAX_ROWS][MAX_COLS];
static uint16_t drawn[MAX_ROWS][MAX_COLS];

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
	const struct boot_fb *f = &bootinfo.fb;
	return ((uint32_t)r << f->red_shift) | ((uint32_t)g << f->green_shift) | ((uint32_t)b << f->blue_shift);
}

const uint8_t *fbcon_glyph(uint8_t c)
{
	if (c < spleen8x16.firstchar || c >= spleen8x16.firstchar + spleen8x16.numchars)
		c = '?';
	return spleen8x16_data + (c - spleen8x16.firstchar) * FH;
}

static void draw_cell(int col, int row, uint8_t c, bool inv)
{
	const uint8_t *g = fbcon_glyph(c);
	uint32_t f = inv ? bg : fg, b = inv ? fg : bg;
	volatile uint32_t *p = fb + (uint64_t)row * FH * pitch32 + (uint64_t)col * FW;
	for (int y = 0; y < FH; y++, p += pitch32) {
		uint8_t bits = g[y];
		for (int x = 0; x < FW; x++)
			p[x] = (bits & (0x80 >> x)) ? f : b;
	}
}

uint32_t fbcon_fg(void) { return fg; }
uint32_t fbcon_bg(void) { return bg; }

uint32_t fbcon_pixel(int x, int y)
{
	return fb ? fb[(uint64_t)y * pitch32 + (uint64_t)x] : 0;
}

void fbcon_init(void)
{
	const struct boot_fb *f = &bootinfo.fb;
	if (!f->pa || f->bpp != 32 || f->width < FW || f->height < FH)
		return;
	fb = phys_to_virt(f->pa);
	pitch32 = f->pitch / 4;
	fbcon_cols = (int)MIN(f->width / FW, (uint64_t)MAX_COLS);
	fbcon_rows = (int)MIN(f->height / FH, (uint64_t)MAX_ROWS);
	fg = rgb(0xc0, 0xc0, 0xc0);
	bg = rgb(0, 0, 0);
	for (uint64_t y = 0; y < f->height; y++)
		for (uint64_t x = 0; x < f->width; x++)
			fb[y * pitch32 + x] = bg;
	memset(shadow, ' ', sizeof(shadow));
	for (int r = 0; r < MAX_ROWS; r++)
		for (int c = 0; c < MAX_COLS; c++)
			drawn[r][c] = ' ';
	cx = cy = 0;
}

bool fbcon_active(void) { return fb != NULL; }

static void newline(void)
{
	cx = 0;
	if (++cy < fbcon_rows)
		return;
	memmove(shadow[0], shadow[1], (size_t)(fbcon_rows - 1) * MAX_COLS);
	memset(shadow[fbcon_rows - 1], ' ', MAX_COLS);
	cy = fbcon_rows - 1;
}

void fbcon_putc(char ch)
{
	uint8_t c = (uint8_t)ch;
	if (!fb)
		return;
	switch (c) {
	case '\n':
		newline();
		return;
	case '\r':
		cx = 0;
		return;
	case '\b':
		if (cx > 0)
			cx--;
		return;
	case '\t':
		do
			fbcon_putc(' ');
		while (cx % 8);
		return;
	}
	if (c < 0x20 || c == 0x7f)
		return; /* other control characters (and ANSI escapes' ESC) */
	shadow[cy][cx] = c;
	if (++cx >= fbcon_cols)
		newline();
}

void fbcon_flush(void)
{
	if (!fb)
		return;
	for (int r = 0; r < fbcon_rows; r++)
		for (int c = 0; c < fbcon_cols; c++) {
			uint16_t want = shadow[r][c] | ((r == cy && c == cx) ? CURSOR : 0);
			if (drawn[r][c] != want) {
				draw_cell(c, r, shadow[r][c], want & CURSOR);
				drawn[r][c] = want;
			}
		}
}

/* For ktest: the character at a cell, and where the cursor is. */
uint8_t fbcon_char_at(int col, int row) { return shadow[row][col]; }
void fbcon_cursor(int *col, int *row)
{
	*col = cx;
	*row = cy;
}
