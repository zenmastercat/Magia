/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * string.c - the freestanding memory/string routines the kernel (and the
 * compiler, and the Rust core library) expect. The bulk copies use
 * `rep movsb`/`rep stosb`, which is short and fast on modern CPUs (ERMS)
 * and cannot be turned back into a recursive memcpy call by the optimizer.
 */
#include <magia/kernel.h>

void *memcpy(void *dst, const void *src, size_t n)
{
	void *d = dst;
	__asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	if (d == s || n == 0)
		return dst;
	if (d < s || d >= s + n)
		return memcpy(dst, src, n);
	/* overlapping, copy backwards */
	d += n - 1;
	s += n - 1;
	__asm__ volatile("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	void *d = dst;
	__asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;
	for (size_t i = 0; i < n; i++)
		if (x[i] != y[i])
			return x[i] < y[i] ? -1 : 1;
	return 0;
}

/* Rust's core may call bcmp. */
int bcmp(const void *a, const void *b, size_t n);
int bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

size_t strnlen(const char *s, size_t max)
{
	size_t n = 0;
	while (n < max && s[n])
		n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (unsigned char)*a - (unsigned char)*b;
		if (!*a)
			return 0;
	}
	return 0;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
	size_t len = strlen(src);
	if (size) {
		size_t n = len < size - 1 ? len : size - 1;
		memcpy(dst, src, n);
		dst[n] = '\0';
	}
	return len;
}

/* Parse a signed decimal or 0x-prefixed hex number. *ok is false on junk. */
long katol(const char *s, bool *ok)
{
	bool neg = false;
	unsigned long v = 0;
	int base = 10, digits = 0;
	if (*s == '-')
		neg = true, s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		base = 16, s += 2;
	for (; *s; s++, digits++) {
		int d;
		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (base == 16 && *s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (base == 16 && *s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			break;
		v = v * base + d;
	}
	if (ok)
		*ok = digits > 0 && *s == '\0';
	return neg ? -(long)v : (long)v;
}
