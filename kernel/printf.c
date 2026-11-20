/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * printf.c - a small, bounded printf for the kernel.
 * Supports %d %i %u %x %X %p %s %c %% with flags '-', '0', width,
 * precision (.N or .* for strings) and length modifiers l, ll, z.
 */
#include <magia/kernel.h>

struct out {
	char *buf;
	size_t size, pos;
};

static void put(struct out *o, char c)
{
	if (o->pos + 1 < o->size)
		o->buf[o->pos] = c;
	o->pos++;
}

static void put_num(struct out *o, unsigned long long v, int base, bool upper, bool neg,
		    int width, bool zero, bool left)
{
	char tmp[24];
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int n = 0;
	do {
		tmp[n++] = digits[v % base];
		v /= base;
	} while (v);
	int len = n + (neg ? 1 : 0);
	if (!left && !zero)
		for (; width > len; width--)
			put(o, ' ');
	if (neg)
		put(o, '-');
	if (!left && zero)
		for (; width > len; width--)
			put(o, '0');
	while (n)
		put(o, tmp[--n]);
	if (left)
		for (; width > len; width--)
			put(o, ' ');
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct out o = { buf, size, 0 };
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put(&o, *fmt);
			continue;
		}
		fmt++;
		bool left = false, zero = false;
		int width = 0, prec = -1, lng = 0;
		for (;; fmt++) {
			if (*fmt == '-')
				left = true;
			else if (*fmt == '0')
				zero = true;
			else
				break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		}
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else {
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
			}
		}
		while (*fmt == 'l' || *fmt == 'z') {
			lng++;
			fmt++;
		}
		switch (*fmt) {
		case 'd':
		case 'i': {
			long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
			put_num(&o, v < 0 ? -(unsigned long long)v : (unsigned long long)v, 10, false, v < 0,
				width, zero, left);
			break;
		}
		case 'u':
		case 'x':
		case 'X': {
			unsigned long long v = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
			put_num(&o, v, *fmt == 'u' ? 10 : 16, *fmt == 'X', false, width, zero, left);
			break;
		}
		case 'p':
			put(&o, '0');
			put(&o, 'x');
			put_num(&o, (uintptr_t)va_arg(ap, void *), 16, false, false, 16, true, false);
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			int len = (int)strnlen(s, prec >= 0 ? (size_t)prec : (size_t)-1);
			if (!left)
				for (int w = width; w > len; w--)
					put(&o, ' ');
			for (int i = 0; i < len; i++)
				put(&o, s[i]);
			if (left)
				for (int w = width; w > len; w--)
					put(&o, ' ');
			break;
		}
		case 'c':
			put(&o, (char)va_arg(ap, int));
			break;
		case '%':
			put(&o, '%');
			break;
		case '\0':
			fmt--;
			break;
		default:
			put(&o, '%');
			put(&o, *fmt);
		}
	}
	if (size)
		buf[o.pos < size ? o.pos : size - 1] = '\0';
	return (int)o.pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = kvsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

void kvprintf(const char *fmt, va_list ap)
{
	char buf[512];
	int n = kvsnprintf(buf, sizeof(buf), fmt, ap);
	console_write(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

void kprintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	kvprintf(fmt, ap);
	va_end(ap);
}
