/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * fbcon.c - text console on the UEFI/VBE linear framebuffer Limine hands
 * over (32 bits per pixel), drawn with the Spleen 8x16 font from OpenBSD
 * (kernel/font/spleen8x16.h, BSD-2-Clause, Frederic Cambus).
 *
 * Text lives in a ring of NLINES lines: the screen shows `rows` of them
 * starting at line `top` (absolute line numbers since boot), and the lines
 * above `top` that the ring still holds are the scroll-back, which Page
 * Up/Down show (`view_back` lines back; new output returns to the live
 * view). A copy of what is on screen (`drawn`) lets fbcon_flush() redraw
 * only cells that changed, so the framebuffer (write-combining memory) is
 * never read. Each line has an attribute (normal, or the red header of
 * the panic screen). Serial stays the primary console; this is the
 * screen copy. Callers hold the console lock (console.c).
 */
#include <magia/boot.h>
#include <magia/fbcon.h>
#include <magia/kbd.h>
#include <magia/klog.h>

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
#define NLINES 1024  /* screen + scroll-back; more than MAX_ROWS */
#define CURSOR 0x100 /* `drawn` flag: cell shown inverted */
#define ATTR_SHIFT 9 /* `drawn`: line attribute above the cursor bit */
#define A_NORMAL 0
#define A_ALERT 1    /* white on red: the panic header */

int fbcon_cols, fbcon_rows;
static volatile uint32_t *fb;
static uint64_t pitch32; /* pixels per scan line */
static uint32_t fg, bg, alert_fg, alert_bg;
static int cx, cy;           /* cursor on the live screen */
static uint64_t top;         /* absolute line shown on row 0 of the live screen */
static int view_back;        /* lines scrolled back from the live screen */
static uint8_t lines[NLINES][MAX_COLS];
static uint8_t lattr[NLINES];
static uint16_t drawn[MAX_ROWS][MAX_COLS];

uint32_t fbcon_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	const struct boot_fb *f = &bootinfo.fb;
	return ((uint32_t)r << f->red_shift) | ((uint32_t)g << f->green_shift) | ((uint32_t)b << f->blue_shift);
}

static uint8_t *line(uint64_t abs) { return lines[abs % NLINES]; }

static void clear_line(uint64_t abs)
{
	memset(line(abs), ' ', MAX_COLS);
	lattr[abs % NLINES] = A_NORMAL;
}

const uint8_t *fbcon_glyph(uint8_t c)
{
	if (c < spleen8x16.firstchar || c >= spleen8x16.firstchar + spleen8x16.numchars)
		c = '?';
	return spleen8x16_data + (c - spleen8x16.firstchar) * FH;
}

static void draw_cell(int col, int row, uint8_t c, bool inv, int attr)
{
	const uint8_t *g = fbcon_glyph(c);
	uint32_t cf = attr == A_ALERT ? alert_fg : fg, cb = attr == A_ALERT ? alert_bg : bg;
	uint32_t f = inv ? cb : cf, b = inv ? cf : cb;
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
	fg = fbcon_rgb(0xc0, 0xc0, 0xc0);
	bg = fbcon_rgb(0, 0, 0);
	alert_fg = fbcon_rgb(0xff, 0xff, 0xff);
	alert_bg = fbcon_rgb(0xb0, 0, 0);
	for (uint64_t y = 0; y < f->height; y++)
		for (uint64_t x = 0; x < f->width; x++)
			fb[y * pitch32 + x] = bg;
	memset(lines, ' ', sizeof(lines));
	for (int r = 0; r < MAX_ROWS; r++)
		for (int c = 0; c < MAX_COLS; c++)
			drawn[r][c] = ' ';
	cx = cy = 0;
	top = 0;
	view_back = 0;
}

bool fbcon_active(void) { return fb != NULL; }

static void newline(void)
{
	cx = 0;
	if (++cy < fbcon_rows)
		return;
	top++; /* the old row 0 becomes scroll-back */
	clear_line(top + (uint64_t)fbcon_rows - 1);
	cy = fbcon_rows - 1;
}

void fbcon_putc(char ch)
{
	uint8_t c = (uint8_t)ch;
	if (!fb)
		return;
	view_back = 0; /* new output: back to the live screen */
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
	line(top + (uint64_t)cy)[cx] = c;
	if (++cx >= fbcon_cols)
		newline();
}

void fbcon_flush(void)
{
	if (!fb)
		return;
	for (int r = 0; r < fbcon_rows; r++) {
		uint64_t abs = top - (uint64_t)view_back + (uint64_t)r;
		const uint8_t *l = line(abs);
		uint16_t a = (uint16_t)(lattr[abs % NLINES] << ATTR_SHIFT);
		for (int c = 0; c < fbcon_cols; c++) {
			bool cur = view_back == 0 && r == cy && c == cx;
			uint16_t want = l[c] | (cur ? CURSOR : 0) | a;
			if (drawn[r][c] != want) {
				draw_cell(c, r, l[c], cur, a >> ATTR_SHIFT);
				drawn[r][c] = want;
			}
		}
	}
}

/* Scroll-back lines still in the ring above the live screen. */
int fbcon_history(void)
{
	uint64_t keep = NLINES - (uint64_t)fbcon_rows;
	return (int)MIN(top, keep);
}

static void do_scroll(void *arg)
{
	int n = view_back + *(int *)arg;
	view_back = n < 0 ? 0 : MIN(n, fbcon_history());
	fbcon_flush();
}

void fbcon_scroll(int lines_back)
{
	if (!fb)
		return;
	if (lines_back > NLINES)
		lines_back = NLINES;
	if (lines_back < -NLINES)
		lines_back = -NLINES;
	console_locked(do_scroll, &lines_back);
}

int fbcon_scrolled(void) { return view_back; }

/* Start a fresh screen; what was on it stays in the scroll-back. */
static void new_page(void)
{
	view_back = 0;
	top += (uint64_t)cy + 1;
	for (int r = 0; r < fbcon_rows; r++)
		clear_line(top + (uint64_t)r);
	cx = cy = 0;
}

static void do_new_page(void *arg)
{
	(void)arg;
	new_page();
	fbcon_flush();
}

void fbcon_new_page(void)
{
	if (fb)
		console_locked(do_new_page, NULL);
}

/* Write s into row `row` of the live screen, cut at the screen width;
 * tabs become spaces and other control bytes are dropped. */
static void put_row(int row, const char *s, size_t n, int attr)
{
	uint8_t *l = line(top + (uint64_t)row);
	int c = 0;
	for (size_t i = 0; i < n && c < fbcon_cols; i++) {
		uint8_t ch = (uint8_t)s[i];
		if (ch == '\t')
			ch = ' ';
		if (ch >= 0x20 && ch != 0x7f)
			l[c++] = ch;
	}
	lattr[(top + (uint64_t)row) % NLINES] = (uint8_t)attr;
}

/* The panic report: two red header rows (the message, and what to do),
 * then the newest kernel log lines - the panic message and backtrace
 * among them - one per row, cut at the screen width. */
static void do_panic_screen(void *arg)
{
	const char *msg = arg;
	char hdr[MAX_COLS + 1];
	new_page();
	int n = ksnprintf(hdr, sizeof(hdr), " MAGIA PANIC: %s", msg);
	put_row(0, hdr, (size_t)MIN(n, (int)sizeof(hdr) - 1), A_ALERT);
	static const char hint[] = " Photograph this screen. The newest kernel log lines follow (dmesg).";
	put_row(1, hint, sizeof(hint) - 1, A_ALERT);
	int nlog = fbcon_rows - 3; /* rows 2 .. rows-2; the cursor rests on the last row */
	if (nlog > 0) {
		uint64_t p = klog_tail(&kernel_log, (unsigned)nlog), end = kernel_log.head;
		char buf[MAX_COLS];
		for (int r = 2; r < 2 + nlog && p < end; r++) {
			size_t len = 0;
			char ch = 0;
			/* one log line: copy what fits, skip the rest up to '\n' */
			while (p < end && klog_read(&kernel_log, p, &ch, 1) == 1) {
				p++;
				if (ch == '\n')
					break;
				if (len < sizeof(buf))
					buf[len++] = ch;
			}
			put_row(r, buf, len, A_NORMAL);
		}
	}
	cy = fbcon_rows - 1;
	cx = 0;
	fbcon_flush();
}

void fbcon_panic_screen(const char *msg)
{
	if (fb && fbcon_rows >= 4)
		console_locked(do_panic_screen, (void *)msg);
}

/* For ktest: the character shown at a cell, and where the cursor is. */
uint8_t fbcon_char_at(int col, int row) { return line(top - (uint64_t)view_back + (uint64_t)row)[col]; }
void fbcon_cursor(int *col, int *row)
{
	*col = cx;
	*row = cy;
}
