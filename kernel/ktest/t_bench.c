/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* t_bench.c - ktests for the benchmark harness (92x). Speed is not tested
 * (TCG numbers mean nothing); the statistics and that every primitive
 * really ran are. The numbers for the paper come from the KVM run. */
#include <magia/bench.h>
#include <magia/ktest.h>
#include <magia/thread.h>
#include <magia/trap.h>

/* The statistics on known data. */
KTEST(920, bench_stats_known_answers)
{
	uint64_t v[100];
	for (int i = 0; i < 100; i++)
		v[i] = (uint64_t)(100 - i); /* 100 .. 1, reversed */
	struct bench_stats s;
	bench_compute(v, 100, &s);
	KT_EQ(s.n, 100);
	KT_EQ(s.min, 1);
	KT_EQ(s.max, 100);
	KT_EQ(s.median, 50);
	KT_EQ(s.p99, 99);
	KT_EQ(s.mean, 50);
	for (int i = 1; i < 100; i++)
		KT_ASSERT(v[i - 1] <= v[i]);
	bench_compute(v, 0, &s);
	KT_EQ(s.n, 0);
	KT_EQ(bench_cycles_to_ns10(3000, 3000000000ULL), 10000); /* 1000.0 ns */
	char b[16];
	KT_EQ(bench_fmt_ns10(b, sizeof(b), 1234), 5);
	KT_EQ(strcmp(b, "123.4"), 0);
}

/* Every primitive runs 1000 samples; the statistics are ordered. */
KTEST(921, bench_runs_every_primitive)
{
	uint64_t sw = context_switches;
	KT_EQ(bench_run(BENCH_MIN_N), 0);
	for (int i = 0; i < BENCH_COUNT; i++) {
		const struct bench_result *r = &bench_results[i];
		KT_ASSERT(r->name != NULL);
		KT_ASSERT(!r->skipped);
		if (r->skipped) {
			kprintf("    skipped: %s\n", r->name);
			continue;
		}
		KT_EQ(r->s.n, BENCH_MIN_N);
		KT_ASSERT(r->s.min <= r->s.median && r->s.median <= r->s.p99 && r->s.p99 <= r->s.max);
		KT_ASSERT(r->s.mean >= r->s.min && r->s.mean <= r->s.max);
	}
	KT_ASSERT(context_switches - sw >= 2 * BENCH_MIN_N); /* the ping-pong switched */
	KT_EQ(bench_run(10), 0); /* n is clamped up to the minimum */
	KT_EQ(bench_results[0].s.n, BENCH_MIN_N);
}
