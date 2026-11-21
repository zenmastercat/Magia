/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * bench.h - the kernel-side benchmark harness (kernel/bench.c) and its
 * statistics (kernel/bench_stats.c, also built and tested on the host).
 * Numbers are Magia-internal: a comparison with Linux needs the
 * user-space suite of stage 3 (docs/TESTING.md).
 */
#ifndef MAGIA_BENCH_H
#define MAGIA_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* bench_stats.c */
struct bench_stats {
	uint64_t n;                           /* samples */
	uint64_t min, max, median, p99, mean; /* cycles */
};
void bench_sort(uint64_t *v, size_t n);
void bench_compute(uint64_t *v, size_t n, struct bench_stats *out);
uint64_t bench_cycles_to_ns10(uint64_t cycles, uint64_t tsc_hz); /* tenths of ns */
size_t bench_fmt_ns10(char *buf, size_t size, uint64_t ns10);    /* "123.4" */

/* bench.c */
#define BENCH_MIN_N 1000
#define BENCH_MAX_N 16384
#define BENCH_COUNT 9
struct bench_result {
	const char *name;
	bool skipped;
	struct bench_stats s;
};
extern struct bench_result bench_results[BENCH_COUNT];
/* Run every benchmark with n samples (clamped to BENCH_MIN_N..MAX_N)
 * after a warm-up, print BENCH lines; returns 0 or -errno. */
int bench_run(unsigned n);

#endif
