/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * bench.c - the kernel-side benchmark harness (`bench` in the monitor,
 * `ktest bench` on the command line), for the paper's comparison with
 * Linux. Every primitive is timed with the TSC the way Intel's "How to
 * Benchmark Code Execution Times" white paper recommends: lfence; rdtsc;
 * lfence before, rdtscp; lfence after (lfence; rdtsc without RDTSCP), one
 * sample per operation, n >= 1000 samples after a warm-up of n/10 (at
 * least 100), then min, median, p99, max and mean in cycles and ns.
 * The empty measurement is reported too (tsc_overhead), so it can be
 * subtracted. Lines are machine readable:
 *   BENCH-INFO key=value ...
 *   BENCH name=<primitive> n=<samples> median_cycles=... median_ns=... ...
 *   BENCH-END
 * Primitives (what exists in Magia today):
 *   null_syscall_ring3   `syscall` getpid from ring 3, timed in user mode
 *   null_syscall_kernel  ksyscall(getpid): the Rust dispatcher without the
 *                        ring change
 *   ctx_switch           two kernel threads handing the CPU to each other
 *                        with sleep/wakeup: half of one round trip (one
 *                        switch with its wakeup and sleep)
 *   irq_to_thread        timer interrupt to the woken thread running:
 *                        from the TSC deadline that fired (TSC-deadline
 *                        mode) or the handler's entry (other modes)
 *   page_map, page_unmap one 4 KiB kernel mapping (vmm_map / vmm_unmap,
 *                        with the TLB flush)
 *   kmalloc_free_64      kmalloc(64) + kfree
 *   page_alloc_free      page_alloc(0) (zeroes 4 KiB) + page_free
 * The invariant TSC (CPUID 0x80000007 EDX[8]) is reported; without it
 * cycles are still counted but ns depend on the clock not changing.
 * Numbers are Magia-internal: the Linux comparison needs the user-space
 * suite of stage 3 that builds unchanged for both systems.
 */
#include <magia/abi.h>
#include <magia/apic.h>
#include <magia/bench.h>
#include <magia/cpu.h>
#include <magia/mm.h>
#include <magia/syscall.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

struct bench_result bench_results[BENCH_COUNT];
static bool has_rdtscp;
static uint64_t *samples;

static inline uint64_t t_begin(void)
{
	uint32_t lo, hi;
	__asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi)::"memory");
	return (uint64_t)hi << 32 | lo;
}

static inline uint64_t t_end(void)
{
	uint32_t lo, hi;
	if (has_rdtscp)
		__asm__ volatile("rdtscp; lfence" : "=a"(lo), "=d"(hi)::"rcx", "memory");
	else
		__asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi)::"memory");
	return (uint64_t)hi << 32 | lo;
}

/* ---------------------------------------------------------------- primitives */

static int b_overhead(uint64_t *v, unsigned n, unsigned warm)
{
	for (unsigned i = 0; i < warm + n; i++) {
		uint64_t t0 = t_begin(), t1 = t_end();
		if (i >= warm)
			v[i - warm] = t1 - t0;
	}
	return (int)n;
}

static int b_kcall(uint64_t *v, unsigned n, unsigned warm)
{
	for (unsigned i = 0; i < warm + n; i++) {
		uint64_t t0 = t_begin();
		ksys0(SYS_getpid);
		uint64_t t1 = t_end();
		if (i >= warm)
			v[i - warm] = t1 - t0;
	}
	return (int)n;
}

extern const char ublob_bench_getpid[], ublob_bench_getpid_end[];

struct ring3_args {
	uint64_t rip, rsp, data, n, warm;
};

static void ring3_child(void *arg)
{
	struct ring3_args *a = arg;
	enter_user(a->rip, a->rsp, a->data, a->n, a->warm);
}

static int b_ring3(uint64_t *v, unsigned n, unsigned warm)
{
	if (!has_rdtscp)
		return -ENOSYS; /* the blob uses rdtscp */
	uint64_t npages = ALIGN_UP((uint64_t)n * 4, PAGE_SIZE) / PAGE_SIZE;
	uint64_t code = 0, code_kva = 0, stack = 0, data = 0, data_kva = 0, st = 0;
	int r = -ENOMEM;
	if (uvm_alloc(0, 1, UVM_EXEC | UVM_CONTIG, &code, &code_kva) != 0)
		return r;
	if (uvm_alloc(0, 2, 0, &stack, NULL) != 0)
		goto free_code;
	if (uvm_alloc(0, npages, UVM_CONTIG, &data, &data_kva) != 0)
		goto free_stack;
	if (uvm_alloc(0, 1, 0, &st, NULL) != 0)
		goto free_data;
	memcpy((void *)code_kva, ublob_bench_getpid, (size_t)(ublob_bench_getpid_end - ublob_bench_getpid));
	struct ring3_args a = { code, stack + 2 * PAGE_SIZE - 64, data, n, warm };
	int pid = kproc_spawn("bench-ring3", ring3_child, &a);
	r = pid;
	if (pid > 0) {
		int64_t w = ksys3(SYS_waitpid, pid, st, 0);
		int32_t status = -1;
		if (w >= 0 && copyin(&status, st, sizeof(status)) == 0 && status == 0) {
			const uint32_t *d = (const uint32_t *)data_kva;
			for (unsigned i = 0; i < n; i++)
				v[i] = d[i];
			r = (int)n;
		} else {
			r = -EIO;
		}
	}
	uvm_free(0, st, 1);
free_data:
	uvm_free(0, data, npages);
free_stack:
	uvm_free(0, stack, 2);
free_code:
	uvm_free(0, code, 1);
	return r;
}

/* Context switch: two kernel threads hand the CPU to each other with
 * sleep/wakeup (as LMbench's lat_ctx does with a pipe): A wakes B and
 * sleeps, B wakes A and sleeps. One round trip is exactly two switches
 * (checked with the switch counter); a sample is half of it. */
static volatile int pp_turn; /* 1: B's turn, 0: A's turn */
static volatile bool pp_stop;
static int pp_a;

static void pp_wait(int want)
{
	for (;;) {
		sleep_prepare(0);
		if (pp_turn == want) {
			sleep_cancel();
			return;
		}
		sleep_commit(); /* a wake between prepare and commit returns at once */
	}
}

static void pp_partner(void *arg)
{
	(void)arg;
	for (;;) {
		pp_wait(1);
		if (pp_stop)
			return;
		pp_turn = 0;
		thread_wake(pp_a);
	}
}

static int b_ctxsw(uint64_t *v, unsigned n, unsigned warm)
{
	pp_stop = false;
	pp_turn = 0;
	pp_a = curthread()->tid;
	struct thread *t = thread_create("bench-pong", pp_partner, NULL, 0);
	if (!t)
		return -ENOMEM;
	int tid_b = t->tid;
	thread_start(t);
	thread_sleep_ms(1); /* let it reach its first sleep */
	uint64_t sw0 = context_switches;
	for (unsigned i = 0; i < warm + n; i++) {
		uint64_t t0 = t_begin();
		pp_turn = 1;
		thread_wake(tid_b);
		pp_wait(0);
		uint64_t t1 = t_end();
		if (i >= warm)
			v[i - warm] = (t1 - t0) / 2;
	}
	uint64_t switches = context_switches - sw0;
	pp_stop = true;
	pp_turn = 1;
	thread_wake(tid_b);
	thread_sleep_ms(2); /* let it see the flag and exit */
	/* exactly two switches per round trip, or something else ran */
	return switches >= 2ULL * (warm + n) && switches <= 2ULL * (warm + n) + 8 ? (int)n : -EAGAIN;
}

static int b_irq_thread(uint64_t *v, unsigned n, unsigned warm)
{
	for (unsigned i = 0; i < warm + n; i++) {
		thread_sleep_ms(1); /* woken by a timer tick */
		uint64_t now = t_end();
		uint64_t from = timer_deadline_tsc ? timer_deadline_tsc : timer_irq_tsc;
		if (i >= warm)
			v[i - warm] = now > from ? now - from : 0;
	}
	return (int)n;
}

#define SCRATCH_VA 0x00007e0000000000ULL /* lower half, far from user windows */

static int b_map(uint64_t *v, unsigned n, unsigned warm, bool unmap_side)
{
	void *page = page_alloc(0);
	if (!page)
		return -ENOMEM;
	uint64_t pa = virt_to_phys(page), old;
	int r = (int)n;
	for (unsigned i = 0; i < warm + n; i++) {
		uint64_t t0 = t_begin();
		int e1 = vmm_map(SCRATCH_VA, pa, PTE_P | PTE_W | PTE_NX);
		uint64_t t1 = t_end();
		int e2 = vmm_unmap(SCRATCH_VA, &old);
		uint64_t t2 = t_end();
		if (e1 || e2 || old != pa) {
			r = -EIO;
			break;
		}
		if (i >= warm)
			v[i - warm] = unmap_side ? t2 - t1 : t1 - t0;
	}
	page_free(page, 0);
	return r;
}

static int b_map_only(uint64_t *v, unsigned n, unsigned warm) { return b_map(v, n, warm, false); }
static int b_unmap(uint64_t *v, unsigned n, unsigned warm) { return b_map(v, n, warm, true); }

static int b_kmalloc(uint64_t *v, unsigned n, unsigned warm)
{
	for (unsigned i = 0; i < warm + n; i++) {
		uint64_t t0 = t_begin();
		void *p = kmalloc(64);
		kfree(p);
		uint64_t t1 = t_end();
		if (!p)
			return -ENOMEM;
		if (i >= warm)
			v[i - warm] = t1 - t0;
	}
	return (int)n;
}

static int b_pages(uint64_t *v, unsigned n, unsigned warm)
{
	for (unsigned i = 0; i < warm + n; i++) {
		uint64_t t0 = t_begin();
		void *p = page_alloc(0);
		if (p)
			page_free(p, 0);
		uint64_t t1 = t_end();
		if (!p)
			return -ENOMEM;
		if (i >= warm)
			v[i - warm] = t1 - t0;
	}
	return (int)n;
}

static const struct {
	const char *name;
	int (*fn)(uint64_t *v, unsigned n, unsigned warm);
} benches[BENCH_COUNT] = {
	{ "tsc_overhead", b_overhead },     { "null_syscall_ring3", b_ring3 }, { "null_syscall_kernel", b_kcall },
	{ "ctx_switch", b_ctxsw },    { "irq_to_thread", b_irq_thread }, { "page_map", b_map_only },
	{ "page_unmap", b_unmap },          { "kmalloc_free_64", b_kmalloc },  { "page_alloc_free", b_pages },
};

/* ---------------------------------------------------------------- driver */

static void print_ns(const char *key, uint64_t cycles)
{
	char b[32];
	bench_fmt_ns10(b, sizeof(b), bench_cycles_to_ns10(cycles, tsc_hz()));
	kprintf(" %s=%s", key, b);
}

static const char *const timer_names[] = { "pit", "lapic_periodic", "tsc_deadline" };

struct run_ctx {
	unsigned n, warm;
	volatile bool done;
	int waiter;
};

static void bench_proc(void *arg)
{
	struct run_ctx *c = arg;
	for (int i = 0; i < BENCH_COUNT; i++) {
		struct bench_result *r = &bench_results[i];
		r->name = benches[i].name;
		memset(&r->s, 0, sizeof(r->s));
		int got = benches[i].fn(samples, c->n, c->warm);
		r->skipped = got <= 0;
		if (r->skipped) {
			kprintf("BENCH name=%s skipped=%d\n", r->name, got);
			continue;
		}
		bench_compute(samples, (size_t)got, &r->s);
		kprintf("BENCH name=%s n=%lu median_cycles=%lu p99_cycles=%lu min_cycles=%lu max_cycles=%lu "
			"mean_cycles=%lu",
			r->name, r->s.n, r->s.median, r->s.p99, r->s.min, r->s.max, r->s.mean);
		print_ns("median_ns", r->s.median);
		print_ns("p99_ns", r->s.p99);
		print_ns("min_ns", r->s.min);
		kprintf("\n");
	}
	c->done = true;
	thread_wake(c->waiter);
}

int bench_run(unsigned n)
{
	uint32_t a, b, c, d;
	cpuid(0x80000001, 0, &a, &b, &c, &d);
	has_rdtscp = (d >> 27) & 1;
	n = MAX(BENCH_MIN_N, MIN(n, BENCH_MAX_N));
	if (!samples)
		samples = page_alloc(5); /* 32 pages: BENCH_MAX_N u64 */
	if (!samples)
		return -ENOMEM;
	static struct run_ctx ctx; /* static: a timed-out bench process may still use it */
	if (ctx.waiter && !ctx.done)
		return -EBUSY;
	ctx = (struct run_ctx){ n, MAX(100u, n / 10), false, curthread()->tid };
	kprintf("BENCH-INFO kernel=Magia-" MAGIA_RELEASE " cpu=\"%s\" tsc_hz=%lu tsc_source=%s invariant_tsc=%d "
		"rdtscp=%d timer=%s hz=%d n=%u warmup=%u\n",
		cpu_feat.brand, tsc_hz(), tsc_source_name(), cpu_feat.invariant_tsc, has_rdtscp,
		timer_names[timer_mode], HZ, ctx.n, ctx.warm);
	/* a process of its own: the ring-3 benchmark spawns and waits for a child */
	int pid = kproc_spawn("bench", bench_proc, &ctx);
	if (pid < 0)
		return pid;
	uint64_t deadline = ticks + 300000; /* 5 minutes, even under TCG */
	while (!ctx.done && ticks < deadline) {
		sleep_prepare(100);
		if (ctx.done) {
			sleep_cancel();
			break;
		}
		sleep_commit();
	}
	if (!ctx.done) {
		kprintf("BENCH-END timeout\n");
		return -ETIMEDOUT;
	}
	thread_sleep_ms(1); /* let the bench process finish exiting */
	kprintf("BENCH-END\n");
	return 0;
}
