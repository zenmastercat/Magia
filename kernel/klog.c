/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * klog.c - the kernel log ring (see include/magia/klog.h).
 *
 * console_write() appends every byte here under the console lock (or with
 * the lock ignored while panicking), from the first boot message on: the
 * buffer is static, so no allocator is needed. Neither machine Magia
 * targets has a serial port, so this ring is what `dmesg`, the panic
 * screen and a photograph of the screen show.
 */
#include <magia/klog.h>

static char kernel_log_buf[KLOG_SIZE];
struct klog kernel_log = { kernel_log_buf, KLOG_SIZE, 0 };

void klog_put(struct klog *l, const char *s, size_t n)
{
	if (n > l->size) { /* only the end of a huge write fits */
		l->head += n - l->size;
		s += n - l->size;
		n = l->size;
	}
	for (size_t i = 0; i < n; i++)
		l->buf[(l->head + i) & (l->size - 1)] = s[i];
	l->head += n;
}

static uint64_t oldest(const struct klog *l) { return l->head > l->size ? l->head - l->size : 0; }

size_t klog_read(const struct klog *l, uint64_t pos, char *dst, size_t n)
{
	if (pos < oldest(l))
		pos = oldest(l);
	size_t i = 0;
	for (; i < n && pos + i < l->head; i++)
		dst[i] = l->buf[(pos + i) & (l->size - 1)];
	return i;
}

static char at(const struct klog *l, uint64_t pos) { return l->buf[pos & (l->size - 1)]; }

uint64_t klog_first(const struct klog *l)
{
	uint64_t p = oldest(l);
	if (p == 0)
		return 0;
	/* the byte before p must be a newline that is still in the ring */
	for (p = p + 1; p < l->head; p++)
		if (at(l, p - 1) == '\n')
			return p;
	return l->head;
}

uint64_t klog_tail(const struct klog *l, unsigned nlines)
{
	uint64_t first = klog_first(l), p = l->head;
	if (nlines == 0 || p == first)
		return l->head;
	if (at(l, p - 1) == '\n')
		p--; /* the newline ending the last line */
	for (; p > first; p--)
		if (at(l, p - 1) == '\n' && --nlines == 0)
			return p;
	return first;
}

void console_write_nolog(const char *s, size_t n); /* console.c */

void klog_dump_tail(unsigned nlines)
{
	uint64_t p = nlines ? klog_tail(&kernel_log, nlines) : klog_first(&kernel_log);
	uint64_t end = kernel_log.head; /* fixed now: what we print is not logged */
	char chunk[256];
	while (p < end) {
		size_t n = klog_read(&kernel_log, p, chunk, MIN(sizeof(chunk), (size_t)(end - p)));
		if (n == 0)
			break;
		console_write_nolog(chunk, n);
		p += n;
	}
}
