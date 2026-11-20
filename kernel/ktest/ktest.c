/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * ktest.c - the in-kernel test runner.
 *
 * Tests self-register with KTEST() into the set_ktest linker set (FreeBSD
 * linker-set idea); the runner walks the set in sort-key order. Each test
 * runs in a fresh process (kproc_spawn: own fd table, jail 0, no
 * capability mode) so tests cannot leak state into each other through the
 * process. The output format is parsed by tests/run-ktest.sh:
 *   KTEST BEGIN <n>
 *   KTEST RUN <key>.<name>
 *   KTEST PASS|FAIL|TIMEOUT <name> checks=<n>
 *   KTEST SUMMARY total=<n> passed=<n> failed=<n> checks=<n>
 */
#include <magia/ktest.h>
#include <magia/mm.h>
#include <magia/syscall.h>
#include <magia/thread.h>
#include <magia/trap.h>

#define KTEST_TIMEOUT_MS 15000

volatile int ktest_failures;
volatile uint64_t ktest_checks;
static volatile int test_fail_count; /* failures in the current test */

struct run_ctx {
	const struct ktest *t;
	volatile bool done;
	int runner_tid;
};

void ktest_fail(const char *file, int line, const char *expr)
{
	test_fail_count++;
	kprintf("    check failed: %s at %s:%d\n", expr, file, line);
}

void ktest_fail_eq(const char *file, int line, const char *a, const char *b, long va, long vb)
{
	test_fail_count++;
	kprintf("    check failed: %s == %s (%ld != %ld, 0x%lx != 0x%lx) at %s:%d\n", a, b, va, vb, va, vb,
		file, line);
}

static void test_proc(void *arg)
{
	struct run_ctx *c = arg;
	c->t->fn();
	c->done = true;
	thread_wake(c->runner_tid);
}

/* The sort key is the suffix of the set section; recover it from the
 * pointer's position is not possible, so print the index instead. */
int ktest_run_all(void)
{
	size_t n = MAGIA_SET_COUNT(ktest);
	int passed = 0, failed = 0;
	kprintf("KTEST BEGIN %zu\n", n);
	const struct ktest *const *tp;
	MAGIA_SET_FOREACH(tp, ktest) {
		const struct ktest *t = *tp;
		struct run_ctx c = { .t = t, .done = false, .runner_tid = curthread()->tid };
		uint64_t checks0 = ktest_checks;
		test_fail_count = 0;
		kprintf("KTEST RUN %s (%s)\n", t->name, t->file);
		int pid = kproc_spawn(t->name, test_proc, &c);
		if (pid < 0) {
			kprintf("KTEST FAIL %s checks=0 (spawn failed: %d)\n", t->name, pid);
			failed++;
			continue;
		}
		uint64_t deadline = ticks + KTEST_TIMEOUT_MS;
		while (!c.done && ticks < deadline) {
			sleep_prepare(100);
			if (c.done) {
				sleep_cancel();
				break;
			}
			sleep_commit();
		}
		if (!c.done) {
			kprintf("KTEST TIMEOUT %s checks=%lu\n", t->name, ktest_checks - checks0);
			failed++;
			ktest_failures++;
			break; /* the stuck process still references our stack */
		}
		thread_sleep_ms(1); /* let the test process finish exiting */
		if (test_fail_count) {
			failed++;
			ktest_failures++;
			kprintf("KTEST FAIL %s checks=%lu\n", t->name, ktest_checks - checks0);
		} else {
			passed++;
			kprintf("KTEST PASS %s checks=%lu\n", t->name, ktest_checks - checks0);
		}
	}
	kprintf("KTEST SUMMARY total=%zu passed=%d failed=%d checks=%lu\n", n, passed, failed,
		(uint64_t)ktest_checks);
	return failed;
}

/* ---------------- helpers for tests acting as processes ---------------- */

uint64_t kt_umem(size_t len)
{
	int64_t r = ksys4(SYS_mmap, 0, len, PROT_READ | PROT_WRITE, 0);
	KT_ASSERT(r > 0);
	return r > 0 ? (uint64_t)r : 0;
}

void kt_uput(uint64_t uva, const void *src, size_t n)
{
	int r = copyout(uva, src, n);
	KT_EQ(r, 0);
}

void kt_uget(void *dst, uint64_t uva, size_t n)
{
	int r = copyin(dst, uva, n);
	KT_EQ(r, 0);
}

uint64_t kt_ustr(uint64_t uva, const char *s)
{
	kt_uput(uva, s, strlen(s) + 1);
	return uva;
}

int kt_spawn(const char *name, void (*fn)(void *), void *arg) { return kproc_spawn(name, fn, arg); }

int kt_wait(int pid)
{
	uint64_t st = kt_umem(sizeof(int32_t));
	int64_t r = ksys3(SYS_waitpid, pid, st, 0);
	if (r < 0)
		return (int)r;
	int32_t status = -1;
	kt_uget(&status, st, sizeof(status));
	ksys2(SYS_munmap, st, PAGE_SIZE);
	return status;
}
