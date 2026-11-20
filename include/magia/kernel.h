/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * kernel.h - common definitions shared by every C file in Magia.
 */
#ifndef MAGIA_KERNEL_H
#define MAGIA_KERNEL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAGIA_NAME    "Magia"
#define MAGIA_RELEASE "0.2.0"

/* Virtual memory layout (see docs/ARCHITECTURE.md). */
#define KERNEL_VMA     0xffffffff80000000UL /* kernel link base (-2 GiB); Limine adds the KASLR slide */
#define USER_TOP       0x0000800000000000UL /* first non-canonical address */
#define USER_WIN_BASE  0x0000000100000000UL /* user windows start at 4 GiB */

#define PAGE_SIZE  4096UL
#define PAGE_SHIFT 12

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define __noreturn  __attribute__((noreturn))
#define __packed    __attribute__((packed))
#define __unused    __attribute__((unused))
#define __aligned(n) __attribute__((aligned(n)))
#define __section(s) __attribute__((section(s)))
#define __used      __attribute__((used))

#define STATIC_ASSERT(c, msg) _Static_assert(c, msg)

/* kernel/boot.c: the direct map of physical memory starts at dmap_base
 * (Limine's randomised HHDM offset); the image is loaded at
 * kernel_virt_base (link base + KASLR slide) from kernel_phys_base. */
extern uint64_t dmap_base, kernel_virt_base, kernel_phys_base;

static inline void *phys_to_virt(uint64_t pa) { return (void *)(pa + dmap_base); }
static inline uint64_t virt_to_phys(const void *va)
{
	uint64_t v = (uint64_t)va;
	if (v >= kernel_virt_base)
		return v - kernel_virt_base + kernel_phys_base;
	return v - dmap_base;
}

/* printf.c */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);

/* console.c: writes go to COM1 and the VGA text console. */
void console_init(void);
void console_write(const char *s, size_t n);
void console_putc(char c);
void console_input_init(void);  /* enable the COM1 receive interrupt */
int console_getc(void);         /* -1 if no input is pending */
int console_getc_wait(void);    /* sleeps until a byte arrives */

/* panic.c */
__noreturn void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void backtrace_from(uint64_t rbp, uint64_t rip);
const char *ksym_lookup(uint64_t addr, uint64_t *offset);
extern volatile int panicking;

#define KASSERT(cond, ...)                                                   \
	do {                                                                 \
		if (unlikely(!(cond)))                                       \
			panic("assertion \"%s\" failed at %s:%d", #cond,     \
			      __FILE__, __LINE__);                           \
	} while (0)

/* string.c (the compiler may also emit calls to these) */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
size_t strlcpy(char *dst, const char *src, size_t size);
long katol(const char *s, bool *ok);

/* main.c */
extern char kernel_cmdline[256];
bool cmdline_has(const char *word);
extern bool ktest_exit_mode;

/* Machine control */
__noreturn void machine_halt(void);
__noreturn void machine_reboot(void);
void qemu_exit(uint32_t code);

#endif
