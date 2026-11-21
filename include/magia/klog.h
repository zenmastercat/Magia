/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * klog.h - the kernel log: every console byte is kept in a ring in memory
 * (kernel/klog.c), so a machine without a serial port can show it with
 * `dmesg` and the panic screen. Positions are absolute byte counts since
 * boot; the ring holds [head - size, head).
 */
#ifndef MAGIA_KLOG_H
#define MAGIA_KLOG_H

#include <magia/kernel.h>

struct klog {
	char *buf;
	uint32_t size; /* a power of two */
	uint64_t head; /* bytes written since boot */
};

#define KLOG_SIZE (64 * 1024)
extern struct klog kernel_log;

void klog_put(struct klog *l, const char *s, size_t n);
/* Copy up to n bytes from absolute position pos (clamped to what the ring
 * still holds); returns the number copied. */
size_t klog_read(const struct klog *l, uint64_t pos, char *dst, size_t n);
/* Start of the first whole line still in the ring (head if none). */
uint64_t klog_first(const struct klog *l);
/* Start of the last `nlines` lines; an unfinished last line counts. */
uint64_t klog_tail(const struct klog *l, unsigned nlines);

void klog_dump_tail(unsigned nlines); /* dmesg [n]: print, without logging it */

#endif
