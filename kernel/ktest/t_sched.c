/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* t_sched.c - ktests for kernel threads, sleep/wakeup, preemption (2xx). */
#include <magia/ktest.h>
#include <magia/mm.h>
#include <magia/rust.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

struct flag_ctx {
	volatile int value;
	int waiter;
};

static void set_flag(void *arg)
{
	struct flag_ctx *c = arg;
	c->value = 1;
	thread_wake(c->waiter);
}

KTEST(200, thread_create_runs_and_exits)
{
	struct flag_ctx c = { 0, curthread()->tid };
	struct thread *t = thread_create("kt-flag", set_flag, &c, 0);
	KT_ASSERT(t != NULL);
	if (!t)
		return;
	uint64_t created = *(volatile uint64_t *)&context_switches;
	thread_start(t);
	for (int i = 0; i < 100 && !c.value; i++)
		thread_sleep_ms(1);
	KT_EQ(c.value, 1);
	KT_ASSERT(context_switches > created);
}

KTEST(201, sleep_times_out)
{
	uint64_t t0 = ticks;
	sleep_prepare(30);
	bool to = sleep_commit();
	KT_ASSERT(to);
	KT_ASSERT(ticks - t0 >= 30);
	KT_ASSERT(ticks - t0 < 300);
}

KTEST(202, wakeup_before_commit_is_not_lost)
{
	uint64_t t0 = ticks;
	sleep_prepare(2000);
	thread_wake(curthread()->tid); /* e.g. an IRQ handler waking us early */
	bool to = sleep_commit();
	KT_ASSERT(!to);
	KT_ASSERT(ticks - t0 < 100); /* returned at once, did not wait 2 s */
	/* cancel path */
	sleep_prepare(2000);
	sleep_cancel();
	KT_ASSERT(!sleep_commit());
	KT_ASSERT(ticks - t0 < 200);
}

struct spin_ctx {
	volatile bool stop;
	volatile uint64_t count;
	volatile bool done;
};

static void spinner(void *arg)
{
	struct spin_ctx *s = arg;
	while (!s->stop)
		s->count++; /* never yields: only timer preemption can switch away */
	s->done = true;
}

static void run_spinners(struct spin_ctx *a, struct spin_ctx *b, uint32_t wa, uint32_t wb, uint64_t ms)
{
	struct thread *ta = thread_create("kt-spin-a", spinner, a, 0);
	struct thread *tb = thread_create("kt-spin-b", spinner, b, 0);
	if (!ta || !tb)
		panic("ktest: thread_create failed");
	thread_start_weighted(ta, wa);
	thread_start_weighted(tb, wb);
	thread_sleep_ms(ms);
	a->stop = b->stop = true;
	for (int i = 0; i < 1000 && !(a->done && b->done); i++)
		thread_sleep_ms(1);
}

KTEST(203, timer_preempts_cpu_bound_threads)
{
	struct spin_ctx a = { 0 }, b = { 0 };
	uint64_t sw0 = context_switches;
	run_spinners(&a, &b, 1024, 1024, 150);
	KT_ASSERT(a.done && b.done);
	KT_ASSERT(a.count > 0 && b.count > 0);
	KT_ASSERT(context_switches - sw0 >= 10); /* many slices, not one */
	/* equal weights: neither gets more than ~2x the other */
	KT_ASSERT(a.count * 2 > b.count && b.count * 2 > a.count);
}

KTEST(204, eevdf_weights_share_cpu_proportionally)
{
	struct spin_ctx a = { 0 }, b = { 0 };
	run_spinners(&a, &b, 2048, 1024, 300);
	KT_ASSERT(a.done && b.done);
	/* weight 2048 vs 1024: expect ~2x; accept 1.4x .. 2.8x (emulated CPU noise) */
	KT_ASSERT(a.count * 10 >= b.count * 14);
	KT_ASSERT(a.count * 10 <= b.count * 28);
	kprintf("    spin counts: w2048=%lu w1024=%lu\n", (uint64_t)a.count, (uint64_t)b.count);
}

KTEST(205, yield_lets_another_thread_run)
{
	struct flag_ctx c = { 0, -1 };
	struct thread *t = thread_create("kt-yield", set_flag, &c, 0);
	KT_ASSERT(t != NULL);
	if (!t)
		return;
	thread_start(t);
	for (int i = 0; i < 1000 && !c.value; i++)
		sched_yield();
	KT_EQ(c.value, 1);
}

KTEST(206, stack_canaries_intact)
{
	struct thread *me = curthread();
	KT_EQ(*(uint64_t *)me->stack, STACK_CANARY);
	struct thread *boot = thread_by_tid(1);
	KT_ASSERT(boot != NULL);
	KT_EQ(thread_by_tid(MAX_THREADS), NULL); /* negative: out of range */
	KT_EQ(thread_by_tid(-1), NULL);
}

KTEST(207, many_threads_create_and_reap)
{
	uint64_t before = 0;
	for (int round = 0; round < 3; round++) {
		struct flag_ctx c[20];
		for (int i = 0; i < 20; i++) {
			c[i].value = 0;
			c[i].waiter = -1;
			struct thread *t = thread_create("kt-many", set_flag, &c[i], 0);
			KT_ASSERT(t != NULL);
			if (t)
				thread_start(t);
		}
		for (int i = 0; i < 20; i++)
			for (int k = 0; k < 200 && !c[i].value; k++)
				thread_sleep_ms(1);
		thread_sleep_ms(5); /* let the dead threads be reaped */
		uint64_t now = pmm_free_pages();
		if (round == 0)
			before = now;
		else /* stacks (4 pages each) are freed; a kmalloc slab page may come or go */
			KT_ASSERT(now + 2 >= before && now <= before + 2);
	}
}
