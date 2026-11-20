/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * thread.h - kernel threads and the C half of the scheduler.
 *
 * C owns thread objects, stacks, sleep/wakeup and the context switch.
 * The Rust EEVDF policy (rust/src/sched.rs) decides who runs next.
 */
#ifndef MAGIA_THREAD_H
#define MAGIA_THREAD_H

#include <magia/kernel.h>

#define MAX_THREADS 128
#define KSTACK_ORDER 2 /* 16 KiB kernel stacks */
#define KSTACK_SIZE (PAGE_SIZE << KSTACK_ORDER)
#define STACK_CANARY 0x4d61676961537421ULL /* "Magia" stack bottom marker */

enum thread_state { TS_FREE = 0, TS_EMBRYO, TS_RUNNABLE, TS_SLEEPING, TS_DEAD };

struct thread {
	uint64_t rsp; /* saved stack pointer; offset 0 is used by switch.S */
	int tid;
	int pid;
	enum thread_state state;
	char name[16];
	uint8_t *stack;
	void (*fn)(void *);
	void *arg;
	uint64_t wake_tick;   /* 0 = no timeout */
	bool timed_out;
	bool on_rq;           /* known to the Rust run queue as runnable */
	bool exit_pending;    /* set by SYS_exit, honoured on syscall return */
	bool user_mode_used;  /* thread entered ring 3 at least once */
	bool sleep_intent;    /* between sleep_prepare and sleep_commit */
	int exit_status;
	uint64_t switches;
	struct kjmp *fault_jmp; /* armed expected-fault recovery (ktest) */
	uint64_t fault_addr;    /* CR2 of the last recovered fault */
	int fault_vector;
};

extern struct thread *curthread_ptr;
static inline struct thread *curthread(void) { return curthread_ptr; }

void sched_init(void);
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg, int pid);
void thread_start(struct thread *t);
__noreturn void thread_exit(int status);
struct thread *thread_by_tid(int tid);
void schedule(void);
void sched_yield(void);
void sched_tick(void);        /* timer IRQ */
void preempt_check(void);     /* called on IRQ exit */
extern volatile bool need_resched;
extern uint64_t context_switches;

/* Sleep/wakeup protocol (no lost wakeups):
 *   sleep_prepare(timeout); <drop locks>; timed_out = sleep_commit();
 * A thread_wake() between prepare and commit makes commit return at once. */
void sleep_prepare(uint64_t timeout_ms); /* 0 = no timeout */
bool sleep_commit(void);
void sleep_cancel(void);                 /* undo sleep_prepare without sleeping */
void thread_wake(int tid);
void thread_sleep_ms(uint64_t ms);

/* Create a process (Rust proc table entry) backed by a new kernel thread,
 * as a child of the calling thread's process. Returns pid or -errno. */
int kproc_spawn(const char *name, void (*fn)(void *), void *arg);
void thread_list(void);

/* Enter ring 3 at rip with stack rsp; a0..a2 land in rdi, rsi, rdx. */
__noreturn void enter_user(uint64_t rip, uint64_t rsp, uint64_t a0, uint64_t a1, uint64_t a2);

#endif
