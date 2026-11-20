/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * sched.c - kernel threads: creation, context switch, sleep/wakeup,
 * timer preemption. The *policy* (who runs next) is the Rust EEVDF
 * scheduler in rust/src/sched.rs; this file is the mechanism.
 *
 * Locking: Magia is uniprocessor, so "interrupts disabled" is the
 * scheduler lock. Every function that touches thread state runs with
 * irq_save()/irq_restore() around it. Kernel code is fully preemptible
 * whenever interrupts are enabled; the timer IRQ sets need_resched and the
 * IRQ exit path calls schedule().
 *
 * Thread ids are slot indexes: tid 0 is the idle thread (never known to
 * the Rust run queue), tid 1 is the boot thread (kmain, process 0).
 */
#include <magia/abi.h>
#include <magia/cpu.h>
#include <magia/linker_set.h>
#include <magia/mm.h>
#include <magia/rust.h>
#include <magia/syscall.h>
#include <magia/sysctl.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
void thread_start_asm(void);
void thread_entry_c(struct thread *t);
extern uint8_t boot_kstack[], boot_kstack_top[];

static struct thread threads[MAX_THREADS];
struct thread *curthread_ptr;
static struct thread *idle_thread;
volatile bool need_resched;
uint64_t context_switches;
static volatile int dead_pending;
static bool sched_ready;
bool rust_ready;
static uint64_t nthreads_created;

SYSCTL_U64_RO(kern_ctxsw, "kern.sched.context_switches_c", &context_switches, "context switches");
SYSCTL_U64_RO(kern_nthreads, "kern.threads_created", &nthreads_created, "kernel threads created since boot");

static uint64_t stack_top(const struct thread *t)
{
	if (t->tid == 1)
		return (uint64_t)boot_kstack_top;
	return (uint64_t)t->stack + KSTACK_SIZE;
}

static void check_canary(const struct thread *t)
{
	if (*(const uint64_t *)t->stack != STACK_CANARY)
		panic("kernel stack overflow: thread %d (%s) canary smashed", t->tid, t->name);
}

struct thread *thread_by_tid(int tid)
{
	if (tid < 0 || tid >= MAX_THREADS || threads[tid].state == TS_FREE)
		return NULL;
	return &threads[tid];
}

static void build_frame(struct thread *t)
{
	uint64_t *sp = (uint64_t *)((uint64_t)t->stack + KSTACK_SIZE);
	*--sp = (uint64_t)thread_start_asm; /* return address of switch_context */
	*--sp = 0;                          /* rbp */
	*--sp = 0;                          /* rbx */
	*--sp = (uint64_t)t;                /* r12 */
	*--sp = 0;                          /* r13 */
	*--sp = 0;                          /* r14 */
	*--sp = 0;                          /* r15 */
	t->rsp = (uint64_t)sp;
}

static struct thread *thread_alloc_slot(int slot, const char *name, void (*fn)(void *), void *arg, int pid)
{
	struct thread *t = NULL;
	uint64_t f = irq_save();
	if (slot >= 0) {
		t = &threads[slot];
	} else {
		for (int i = 2; i < MAX_THREADS; i++)
			if (threads[i].state == TS_FREE) {
				t = &threads[i];
				break;
			}
	}
	if (t) {
		int tid = (int)(t - threads);
		memset(t, 0, sizeof(*t));
		t->tid = tid;
		t->state = TS_EMBRYO;
	}
	irq_restore(f);
	if (!t)
		return NULL;
	t->stack = page_alloc(KSTACK_ORDER);
	if (!t->stack) {
		t->state = TS_FREE;
		return NULL;
	}
	*(uint64_t *)t->stack = STACK_CANARY;
	strlcpy(t->name, name, sizeof(t->name));
	t->fn = fn;
	t->arg = arg;
	t->pid = pid;
	build_frame(t);
	nthreads_created++;
	return t;
}

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg, int pid)
{
	return thread_alloc_slot(-1, name, fn, arg, pid);
}

static void thread_free(struct thread *t)
{
	page_free(t->stack, KSTACK_ORDER);
	t->stack = NULL;
	t->state = TS_FREE;
}

void thread_start(struct thread *t) { thread_start_weighted(t, 1024); }

void thread_start_weighted(struct thread *t, uint32_t weight)
{
	uint64_t f = irq_save();
	t->state = TS_RUNNABLE;
	t->on_rq = true;
	rs_sched_add(t->tid, weight);
	if (curthread_ptr == idle_thread)
		need_resched = true;
	irq_restore(f);
}

/* Free the stacks of threads that died (never our own). IRQs are off. */
static void sched_post_switch(void)
{
	if (!dead_pending)
		return;
	for (int i = 2; i < MAX_THREADS; i++) {
		struct thread *t = &threads[i];
		if (t->state == TS_DEAD && t != curthread_ptr) {
			thread_free(t);
			dead_pending--;
		}
	}
}

void schedule(void)
{
	uint64_t f = irq_save();
	struct thread *prev = curthread_ptr;
	need_resched = false;
	bool prev_run = prev != idle_thread && prev->state == TS_RUNNABLE;
	int tid = rs_sched_pick(prev == idle_thread ? -1 : prev->tid, prev_run, now_ns());
	struct thread *next = idle_thread;
	if (tid > 0 && tid < MAX_THREADS)
		next = &threads[tid];
	if (next != idle_thread && next->state != TS_RUNNABLE)
		panic("schedule: policy picked tid %d in state %d", tid, next->state);
	if (next != prev) {
		check_canary(prev);
		check_canary(next);
		context_switches++;
		next->switches++;
		tss_set_rsp0(stack_top(next));
		curthread_ptr = next;
		cpu0.curthread = next;
		switch_context(&prev->rsp, next->rsp);
		sched_post_switch();
	}
	irq_restore(f);
}

void sched_yield(void)
{
	uint64_t f = irq_save();
	if (curthread_ptr != idle_thread)
		rs_sched_yield(curthread_ptr->tid);
	schedule();
	irq_restore(f);
}

void preempt_check(void)
{
	if (need_resched && sched_ready)
		schedule();
}

void thread_exit(int status)
{
	cli();
	struct thread *t = curthread_ptr;
	if (t->tid <= 1)
		panic("thread_exit: the %s thread cannot exit", t->name);
	t->exit_status = status;
	t->state = TS_DEAD;
	t->on_rq = false;
	rs_sched_remove(t->tid);
	dead_pending++;
	schedule();
	panic("thread_exit: dead thread %d was scheduled", t->tid);
}

void thread_entry_c(struct thread *t)
{
	sched_post_switch();
	sti();
	t->fn(t->arg);
	if (t->pid > 0)
		ksys1(SYS_exit, 0); /* through Rust: closes fds, wakes the parent */
	thread_exit(0);
}

/* ---------------- sleep / wakeup ---------------- */

void sleep_prepare(uint64_t timeout_ms)
{
	uint64_t f = irq_save();
	struct thread *t = curthread_ptr;
	t->sleep_intent = true;
	t->timed_out = false;
	t->wake_tick = timeout_ms ? ticks + timeout_ms : 0;
	irq_restore(f);
}

void sleep_cancel(void)
{
	uint64_t f = irq_save();
	curthread_ptr->sleep_intent = false;
	curthread_ptr->wake_tick = 0;
	irq_restore(f);
}

bool sleep_commit(void)
{
	uint64_t f = irq_save();
	struct thread *t = curthread_ptr;
	if (t->sleep_intent) {
		t->state = TS_SLEEPING;
		schedule();
	}
	t->sleep_intent = false;
	t->wake_tick = 0;
	bool to = t->timed_out;
	t->timed_out = false;
	irq_restore(f);
	return to;
}

static void wake_locked(struct thread *t, bool timeout)
{
	if (t->state == TS_SLEEPING) {
		t->sleep_intent = false;
		t->timed_out = timeout;
		t->state = TS_RUNNABLE;
		if (rs_sched_wakeup(t->tid, now_ns()) || curthread_ptr == idle_thread)
			need_resched = true;
	} else if (t->sleep_intent) {
		/* woken between prepare and commit: commit will not sleep */
		t->sleep_intent = false;
		t->timed_out = timeout;
	}
}

void thread_wake(int tid)
{
	if (tid < 0 || tid >= MAX_THREADS)
		return;
	uint64_t f = irq_save();
	wake_locked(&threads[tid], false);
	irq_restore(f);
}

void thread_sleep_ms(uint64_t ms)
{
	sleep_prepare(ms ? ms : 1);
	sleep_commit();
}

/* Timer IRQ (interrupts off). */
void sched_tick(void)
{
	if (!sched_ready)
		return;
	for (int i = 1; i < MAX_THREADS; i++) {
		struct thread *t = &threads[i];
		if (t->wake_tick && ticks >= t->wake_tick &&
		    (t->state == TS_SLEEPING || t->sleep_intent)) {
			t->wake_tick = 0;
			wake_locked(t, true);
		}
	}
	if (rust_ready)
		rs_timer_tick(ticks);
	if (curthread_ptr != idle_thread && rs_sched_tick(now_ns()))
		need_resched = true;
}

/* ---------------- processes ---------------- */

int kproc_spawn(const char *name, void (*fn)(void *), void *arg)
{
	struct thread *t = thread_create(name, fn, arg, 0);
	if (!t)
		return -ENOMEM;
	int pid = rs_proc_spawn(curthread_ptr->pid, t->tid, name, strlen(name));
	if (pid < 0) {
		thread_free(t);
		return pid;
	}
	t->pid = pid;
	thread_start(t);
	return pid;
}

void thread_list(void)
{
	static const char *const st[] = { "free", "embryo", "run", "sleep", "dead" };
	kprintf("  TID   PID  STATE  SWITCHES  NAME\n");
	for (int i = 0; i < MAX_THREADS; i++) {
		struct thread *t = &threads[i];
		if (t->state == TS_FREE)
			continue;
		kprintf("%5d %5d  %-5s %9lu  %s%s\n", t->tid, t->pid, st[t->state], t->switches, t->name,
			t == curthread_ptr ? " (current)" : "");
	}
}

/* ---------------- init ---------------- */

static void idle_main(void *arg)
{
	(void)arg;
	for (;;) {
		cli();
		if (need_resched) {
			sti();
			schedule();
			continue;
		}
		__asm__ volatile("sti; hlt" ::: "memory"); /* sti shadow: no lost wakeup */
	}
}

void sched_init(void)
{
	struct thread *boot = &threads[1];
	memset(boot, 0, sizeof(*boot));
	boot->tid = 1;
	boot->pid = 0;
	boot->state = TS_RUNNABLE;
	boot->on_rq = true;
	boot->stack = boot_kstack;
	*(uint64_t *)boot_kstack = STACK_CANARY;
	strlcpy(boot->name, "kmain", sizeof(boot->name));
	curthread_ptr = boot;
	cpu0.curthread = boot;
	tss_set_rsp0((uint64_t)boot_kstack_top);

	idle_thread = thread_alloc_slot(0, "idle", idle_main, NULL, 0);
	if (!idle_thread)
		panic("sched: cannot create the idle thread");
	idle_thread->state = TS_RUNNABLE;

	rs_sched_add(1, 1024);
	sched_ready = true;
}

static void sched_sysinit(void) { sched_init(); }
SYSINIT(sched, SI_ORDER_SCHED, sched_sysinit);
