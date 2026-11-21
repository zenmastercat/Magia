/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * bench_stats.c - statistics for the benchmark harness (kernel/bench.c):
 * heapsort (no recursion, O(n log n) worst case), lower median, p99
 * (the ceil(0.99 n)-th smallest), mean, and cycles -> tenths of ns. No
 * floating point (the kernel is built with -mgeneral-regs-only) and no
 * kernel dependencies: `make host-test` builds this file with
 * -DBENCH_STATS_TEST and runs its test program on the host.
 *
 * First draft by the local model (qwen3.6:27b); reviewed and fixed: a
 * dead first copy in bench_fmt_ns10 removed, the struct taken from
 * <magia/bench.h>.
 */
#include <magia/bench.h>

/*
 * bench_sort: Heapsort in-place, iterative, no recursion.
 * Sorts v[0..n) ascending.
 */
void bench_sort(uint64_t *v, size_t n) {
	if (n <= 1) return;

	/* Build heap */
	/* Start from the last non-leaf node */
	size_t start = (n - 1) / 2;
	for (size_t i = start; ; ) {
		/* Sift down v[i] */
		size_t root = i;
		while (1) {
			size_t child = 2 * root + 1;
			if (child >= n) break;
			size_t swap = root;
			if (v[swap] < v[child]) swap = child;
			if (child + 1 < n && v[swap] < v[child + 1]) swap = child + 1;
			if (swap == root) break;
			uint64_t tmp = v[root];
			v[root] = v[swap];
			v[swap] = tmp;
			root = swap;
		}
		if (i == 0) break;
		i--;
	}

	/* Extract elements one by one */
	for (size_t end = n - 1; end > 0; end--) {
		/* Move current root to end */
		uint64_t tmp = v[0];
		v[0] = v[end];
		v[end] = tmp;
		/* Sift down new root */
		size_t root = 0;
		while (1) {
			size_t child = 2 * root + 1;
			if (child >= end) break;
			size_t swap = root;
			if (v[swap] < v[child]) swap = child;
			if (child + 1 < end && v[swap] < v[child + 1]) swap = child + 1;
			if (swap == root) break;
			tmp = v[root];
			v[root] = v[swap];
			v[swap] = tmp;
			root = swap;
		}
	}
}

/*
 * bench_compute: sorts v, then fills out stats.
 */
void bench_compute(uint64_t *v, size_t n, struct bench_stats *out) {
	if (out == NULL) return;

	if (n == 0) {
		out->n = 0;
		out->min = 0;
		out->max = 0;
		out->median = 0;
		out->p99 = 0;
		out->mean = 0;
		return;
	}

	bench_sort(v, n);

	out->n = n;
	out->min = v[0];
	out->max = v[n - 1];

	/* Median: lower median index (n-1)/2 */
	size_t med_idx = (n - 1) / 2;
	out->median = v[med_idx];

	/* P99: idx = ceil(0.99*n) - 1 = (99*n + 99)/100 - 1, clamped to [0, n-1] */
	/* Use unsigned __int128 to avoid overflow for large n */
	unsigned __int128 n128 = (unsigned __int128)n;
	unsigned __int128 p99_raw = (99 * n128 + 99) / 100 - 1;
	size_t p99_idx = (size_t)p99_raw;
	if (p99_idx >= n) p99_idx = n - 1;
	out->p99 = v[p99_idx];

	/* Mean: sum / n, sum in unsigned __int128 */
	unsigned __int128 sum = 0;
	for (size_t i = 0; i < n; i++) {
		sum += v[i];
	}
	out->mean = (uint64_t)(sum / n);
}

/*
 * bench_cycles_to_ns10: convert cycles to tenths of nanoseconds.
 * Formula: (cycles * 10^10 + tsc_hz/2) / tsc_hz
 * Returns 0 if tsc_hz is 0. Saturates at UINT64_MAX.
 */
uint64_t bench_cycles_to_ns10(uint64_t cycles, uint64_t tsc_hz) {
	if (tsc_hz == 0) return 0;

	unsigned __int128 c128 = (unsigned __int128)cycles;
	unsigned __int128 hz128 = (unsigned __int128)tsc_hz;
	unsigned __int128 ten_pow_10 = 10000000000ULL; /* 10^10 */

	unsigned __int128 num = c128 * ten_pow_10 + (hz128 / 2);
	unsigned __int128 result = num / hz128;

	if (result > UINT64_MAX) {
		return UINT64_MAX;
	}
	return (uint64_t)result;
}

/*
 * bench_fmt_ns10: format ns10 as decimal with one digit after point.
 * e.g. 1234 -> "123.4", 5 -> "0.5", 0 -> "0.0"
 * Returns length of full string (like snprintf).
 * Never writes more than size bytes (including NUL).
 */
size_t bench_fmt_ns10(char *buf, size_t size, uint64_t ns10) {
	/* Compute integer part and fractional digit */
	uint64_t int_part = ns10 / 10;
	uint64_t frac_digit = ns10 % 10;

	/* Convert int_part to decimal string in reverse */
	/* We need to know the length first to handle buffer constraints properly */
	/* Let's compute the digits and store them in a temporary buffer (stack) */
	/* Max digits for uint64_t is 20, plus dot and frac digit and NUL = 23 chars max */
	char tmp[24];
	size_t len = 0;

	if (int_part == 0) {
		tmp[len++] = '0';
	} else {
		/* Extract digits in reverse order */
		uint64_t ip = int_part;
		size_t dlen = 0;
		char digits[21];
		while (ip > 0) {
			digits[dlen++] = (char)('0' + (ip % 10));
			ip /= 10;
		}
		/* Reverse into tmp */
		for (size_t i = 0; i < dlen; i++) {
			tmp[len++] = digits[dlen - 1 - i];
		}
	}

	/* Add dot and fractional digit */
	tmp[len++] = '.';
	tmp[len++] = (char)('0' + frac_digit);
	tmp[len] = '\0';

	/* copy what fits, always NUL-terminated (snprintf rules); len
	 * excludes the NUL, tmp[len] is it */

	if (size == 0) {
		return len;
	}

	if (size > len) {
		for (size_t i = 0; i <= len; i++) {
			buf[i] = tmp[i];
		}
	} else {
		/* Write size-1 characters, then NUL */
		for (size_t i = 0; i < size - 1; i++) {
			buf[i] = tmp[i];
		}
		buf[size - 1] = '\0';
	}

	return len;
}

#ifdef BENCH_STATS_TEST
#include <stdio.h>
#include <string.h>

/* Simple xorshift64 generator for testing */
static uint64_t xorshift64_state = 1234567890123456789ULL;

static uint64_t xorshift64(void) {
	uint64_t x = xorshift64_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	xorshift64_state = x;
	return x;
}

static int test_sort_empty(void) {
	uint64_t v[1] = {0};
	bench_sort(v, 0);
	return 0; /* No assertion needed, just ensure no crash */
}

static int test_sort_one(void) {
	uint64_t v[1] = {42};
	bench_sort(v, 1);
	if (v[0] != 42) return 1;
	return 0;
}

static int test_sort_already_sorted(void) {
	uint64_t v[] = {1, 2, 3, 4, 5};
	bench_sort(v, 5);
	for (size_t i = 0; i < 5; i++) {
		if (v[i] != i + 1) return 1;
	}
	return 0;
}

static int test_sort_reverse(void) {
	uint64_t v[] = {5, 4, 3, 2, 1};
	bench_sort(v, 5);
	for (size_t i = 0; i < 5; i++) {
		if (v[i] != i + 1) return 1;
	}
	return 0;
}

static int test_sort_all_equal(void) {
	uint64_t v[] = {7, 7, 7, 7, 7};
	bench_sort(v, 5);
	for (size_t i = 0; i < 5; i++) {
		if (v[i] != 7) return 1;
	}
	return 0;
}

static int test_sort_1000_xorshift(void) {
	size_t n = 1000;
	uint64_t v[1000];
	unsigned __int128 sum_before = 0;
	for (size_t i = 0; i < n; i++) {
		v[i] = xorshift64();
		sum_before += v[i];
	}

	bench_sort(v, n);

	/* Check sorted */
	for (size_t i = 1; i < n; i++) {
		if (v[i] < v[i - 1]) return 1;
	}

	/* Check sum unchanged */
	unsigned __int128 sum_after = 0;
	for (size_t i = 0; i < n; i++) {
		sum_after += v[i];
	}
	if (sum_before != sum_after) return 1;

	return 0;
}

static int test_compute_1_to_100(void) {
	uint64_t v[100];
	for (size_t i = 0; i < 100; i++) {
		v[i] = i + 1;
	}
	struct bench_stats stats;
	bench_compute(v, 100, &stats);

	if (stats.n != 100) return 1;
	if (stats.min != 1) return 1;
	if (stats.max != 100) return 1;
	/* Median: (100-1)/2 = 49, v[49] = 50 */
	if (stats.median != 50) return 1;
	/* P99: (99*100 + 99)/100 - 1 = (9900+99)/100 - 1 = 9999/100 - 1 = 99 - 1 = 98. v[98] = 99 */
	if (stats.p99 != 99) return 1;
	/* Mean: sum(1..100) = 5050, 5050/100 = 50 */
	if (stats.mean != 50) return 1;

	return 0;
}

static int test_compute_1_to_1000(void) {
	uint64_t v[1000];
	for (size_t i = 0; i < 1000; i++) {
		v[i] = i + 1;
	}
	struct bench_stats stats;
	bench_compute(v, 1000, &stats);

	if (stats.n != 1000) return 1;
	if (stats.min != 1) return 1;
	if (stats.max != 1000) return 1;
	/* Median: (1000-1)/2 = 499, v[499] = 500 */
	if (stats.median != 500) return 1;
	/* P99: (99*1000 + 99)/100 - 1 = (99000+99)/100 - 1 = 99099/100 - 1 = 990 - 1 = 989. v[989] = 990 */
	if (stats.p99 != 990) return 1;

	return 0;
}

static int test_compute_n1(void) {
	uint64_t v[1] = {42};
	struct bench_stats stats;
	bench_compute(v, 1, &stats);

	if (stats.n != 1) return 1;
	if (stats.min != 42) return 1;
	if (stats.max != 42) return 1;
	if (stats.median != 42) return 1;
	if (stats.p99 != 42) return 1;
	if (stats.mean != 42) return 1;

	return 0;
}

static int test_compute_n0(void) {
	uint64_t v[1] = {0};
	struct bench_stats stats;
	bench_compute(v, 0, &stats);

	if (stats.n != 0) return 1;
	if (stats.min != 0) return 1;
	if (stats.max != 0) return 1;
	if (stats.median != 0) return 1;
	if (stats.p99 != 0) return 1;
	if (stats.mean != 0) return 1;

	return 0;
}

static int test_cycles_to_ns10(void) {
	/* 3000 cycles at 3 GHz -> 10000 tenths of ns */
	/* 3 GHz = 3*10^9 Hz. 3000 cycles = 3000 / 3*10^9 s = 10^-6 s = 1000 ns = 10000 tenths of ns */
	uint64_t r1 = bench_cycles_to_ns10(3000, 3000000000ULL);
	if (r1 != 10000) return 1;

	/* 1 cycle at 3 GHz -> 3.333... tenths of ns, rounded to nearest -> 3 */
	uint64_t r2 = bench_cycles_to_ns10(1, 3000000000ULL);
	if (r2 != 3) return 1;

	/* tsc_hz 0 -> 0 */
	uint64_t r3 = bench_cycles_to_ns10(100, 0);
	if (r3 != 0) return 1;

	/* Huge cycles -> saturates or exact. Let's test with max cycles and 1 Hz */
	/* cycles = UINT64_MAX, tsc_hz = 1. Result should be UINT64_MAX (saturated) or close. */
	/* Formula: (UINT64_MAX * 10^10 + 0) / 1. This is huge, definitely > UINT64_MAX. */
	uint64_t r4 = bench_cycles_to_ns10(UINT64_MAX, 1);
	if (r4 != UINT64_MAX) return 1;

	return 0;
}

static int test_fmt_ns10(void) {
	char buf[64];
	size_t len;

	/* 1234 -> "123.4" */
	len = bench_fmt_ns10(buf, sizeof(buf), 1234);
	if (len != 5) return 1;
	if (strcmp(buf, "123.4") != 0) return 1;

	/* 5 -> "0.5" */
	len = bench_fmt_ns10(buf, sizeof(buf), 5);
	if (len != 3) return 1;
	if (strcmp(buf, "0.5") != 0) return 1;

	/* 0 -> "0.0" */
	len = bench_fmt_ns10(buf, sizeof(buf), 0);
	if (len != 3) return 1;
	if (strcmp(buf, "0.0") != 0) return 1;

	/* UINT64_MAX -> "1844674407370955161.5" (21 characters) */
	len = bench_fmt_ns10(buf, sizeof(buf), UINT64_MAX);
	if (len != 21) return 1;
	if (strcmp(buf, "1844674407370955161.5") != 0) return 1;

	/* Buffer size 4 for "123.4": the first 3 characters and a NUL, return 5 */
	len = bench_fmt_ns10(buf, 4, 1234);
	if (len != 5) return 1;
	if (strcmp(buf, "123") != 0) return 1;
	/* size 0 writes nothing */
	buf[0] = (char)0x7f;
	if (bench_fmt_ns10(buf, 0, 1234) != 5 || buf[0] != (char)0x7f) return 1;

	return 0;
}

int main(void) {
	int failed = 0;

	if (test_sort_empty()) { printf("FAIL: sort_empty\n"); failed = 1; }
	if (test_sort_one()) { printf("FAIL: sort_one\n"); failed = 1; }
	if (test_sort_already_sorted()) { printf("FAIL: sort_already_sorted\n"); failed = 1; }
	if (test_sort_reverse()) { printf("FAIL: sort_reverse\n"); failed = 1; }
	if (test_sort_all_equal()) { printf("FAIL: sort_all_equal\n"); failed = 1; }
	if (test_sort_1000_xorshift()) { printf("FAIL: sort_1000_xorshift\n"); failed = 1; }
	if (test_compute_1_to_100()) { printf("FAIL: compute_1_to_100\n"); failed = 1; }
	if (test_compute_1_to_1000()) { printf("FAIL: compute_1_to_1000\n"); failed = 1; }
	if (test_compute_n1()) { printf("FAIL: compute_n1\n"); failed = 1; }
	if (test_compute_n0()) { printf("FAIL: compute_n0\n"); failed = 1; }
	if (test_cycles_to_ns10()) { printf("FAIL: cycles_to_ns10\n"); failed = 1; }
	if (test_fmt_ns10()) { printf("FAIL: fmt_ns10\n"); failed = 1; }

	if (!failed) {
		printf("OK\n");
		return 0;
	} else {
		return 1;
	}
}
#endif
