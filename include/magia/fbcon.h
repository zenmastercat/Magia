/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* fbcon.h - framebuffer text console (kernel/fbcon.c). */
#ifndef MAGIA_FBCON_H
#define MAGIA_FBCON_H

#include <magia/kernel.h>

extern int fbcon_cols, fbcon_rows;
void fbcon_init(void);
bool fbcon_active(void);
void fbcon_putc(char c);  /* called with the console lock held */
void fbcon_flush(void);   /* draw the cells that changed */

/* for tests */
const uint8_t *fbcon_glyph(uint8_t c); /* 16 bytes, MSB = leftmost pixel */
uint32_t fbcon_pixel(int x, int y);
uint32_t fbcon_fg(void);
uint32_t fbcon_bg(void);
uint8_t fbcon_char_at(int col, int row);
void fbcon_cursor(int *col, int *row);

#endif
