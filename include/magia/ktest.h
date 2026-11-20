/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * ktest.h - in-kernel self tests registered through a linker set.
 *
 *   KTEST(310, cap_no_absolute_paths) { ... KT_EQ(x, y); ... }
 *
 * The number is a 3-digit sort key: tests run in ascending order. Each test
 * runs in a fresh process (its own fd table, root jail, no capability mode)
 * created through the same path as every other process.
 */
#ifndef MAGIA_KTEST_H
#define MAGIA_KTEST_H

#include <magia/kernel.h>
#include <magia/linker_set.h>

struct ktest {
	const char *name;
	const char *file;
	void (*fn)(void);
};
MAGIA_SET_DECLARE(ktest, const struct ktest);

#define KTEST(order, tname)                                                  \
	static void ktest_fn_##tname(void);                                  \
	static const struct ktest __ktest_##tname = { #tname, __FILE__, ktest_fn_##tname }; \
	MAGIA_SET(ktest, order, __ktest_##tname);                            \
	static void ktest_fn_##tname(void)

extern volatile int ktest_failures;
extern volatile uint64_t ktest_checks;
void ktest_fail(const char *file, int line, const char *expr);
void ktest_fail_eq(const char *file, int line, const char *a, const char *b, long va, long vb);
int ktest_run_all(void); /* returns number of failed tests */

/*
 * Sort-key ranges (3 digits):
 *   0xx boot, CPU, traps     1xx memory        2xx threads, scheduler
 *   3xx Capsicum             4xx jails         5xx VFS, devfs, pipes
 *   6xx kqueue               7xx uring, futex  8xx sysctl, syscall table
 *   9xx ring 3, monitor
 */

/* Helpers for tests that act as processes (kernel/ktest/ktest.c). */
uint64_t kt_umem(size_t len);                         /* mmap RW user window */
void kt_uput(uint64_t uva, const void *src, size_t n); /* copyout or fail    */
void kt_uget(void *dst, uint64_t uva, size_t n);       /* copyin or fail     */
uint64_t kt_ustr(uint64_t uva, const char *s);         /* copy s incl. NUL   */
int kt_spawn(const char *name, void (*fn)(void *), void *arg); /* child pid */
int kt_wait(int pid);                                  /* waitpid -> status  */

#define KT_ASSERT(c)                                                         \
	do {                                                                 \
		ktest_checks++;                                              \
		if (!(c))                                                    \
			ktest_fail(__FILE__, __LINE__, #c);                  \
	} while (0)

#define KT_EQ(a, b)                                                          \
	do {                                                                 \
		long _kt_a = (long)(a), _kt_b = (long)(b);                   \
		ktest_checks++;                                              \
		if (_kt_a != _kt_b)                                          \
			ktest_fail_eq(__FILE__, __LINE__, #a, #b, _kt_a, _kt_b); \
	} while (0)

#endif
