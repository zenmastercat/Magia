/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * rust.h - the C <-> Rust boundary.
 *
 * Direction C -> Rust: policy and subsystems implemented in rust/src
 * (scheduler decisions, syscall dispatch, accounting, debugger helpers).
 * Direction Rust -> C: the magia_* functions at the bottom, implemented in
 * kernel/rustglue.c. Keep both lists in sync with rust/src/ffi.rs.
 *
 * Rules: all pointers passed across are kernel pointers unless named
 * "uaddr"; strings are (pointer, length) pairs, never assumed
 * NUL-terminated by Rust; Rust never unwinds (panic = abort -> magia_panic).
 */
#ifndef MAGIA_RUST_H
#define MAGIA_RUST_H

#include <stddef.h>
#include <stdint.h>

struct c_sysctl;

/* ---------- implemented in Rust ---------- */

/* abi_sizes[] order: m_stat, m_dirent, jail_params, jail_info, proc_info,
 * utsname, kevent, uring_sqe. Rust panics at boot on any mismatch. */
struct rs_boot_info {
	uint64_t abi_sizes[8];   /* sizeof() of ABI structs, checked by Rust */
	uint64_t total_pages;
	uint64_t hz;
	uint32_t cpu_flags;      /* RS_CPU_* */
	uint32_t _pad;
};
#define RS_CPU_NX   1
#define RS_CPU_SMEP 2
#define RS_CPU_SMAP 4

/* Bring up Rust subsystems: jail0, pid 0, VFS (ramfs + devfs), sysctl. */
void rs_init(const struct rs_boot_info *bi);

/* System call dispatcher: validates and executes syscall `nr` for process
 * `pid`. Returns the result or -errno. */
int64_t rs_syscall(int32_t pid, uint64_t nr, const uint64_t args[6]);

/* Process table */
int32_t rs_proc_spawn(int32_t ppid, int32_t tid, const char *name, size_t namelen);

/* EEVDF scheduler policy */
void rs_sched_add(int32_t tid, uint32_t weight);
void rs_sched_remove(int32_t tid);
int32_t rs_sched_wakeup(int32_t tid, uint64_t now_ns);   /* returns 1 to preempt */
int32_t rs_sched_tick(uint64_t now_ns);                  /* returns 1 to resched */
int32_t rs_sched_pick(int32_t prev_tid, int32_t prev_runnable, uint64_t now_ns);
void rs_sched_yield(int32_t tid);

/* Timer callout: kqueue timers, io_uring timeouts. Called from the tick. */
void rs_timer_tick(uint64_t now_ms);

/* Jail resource accounting hook used by the C page allocator. */
int32_t rs_jail_mem_charge(int32_t jid, uint64_t bytes); /* 0 or -ENOMEM */
void rs_jail_mem_uncharge(int32_t jid, uint64_t bytes);

/* Kernel debug monitor helpers (print to the console). */
void rs_dbg_ps(void);
void rs_dbg_jails(void);
void rs_dbg_sysctl(const char *name, size_t namelen, const char *value, size_t valuelen);
void rs_dbg_ls(const char *path, size_t len);
void rs_dbg_cat(const char *path, size_t len);
void rs_dbg_uname(void);

/* ---------- implemented in C (kernel/rustglue.c), called by Rust ---------- */

void magia_console_write(const char *s, size_t n);
__attribute__((noreturn)) void magia_panic(const char *msg, size_t n);
void *magia_kmalloc(size_t size, size_t align);
void magia_kfree(void *p);
uint64_t magia_now_ns(void);
uint64_t magia_ticks(void);
int32_t magia_current_tid(void);
uint64_t magia_irq_save(void);
void magia_irq_restore(uint64_t flags);
void magia_sleep_prepare(uint64_t timeout_ms);
int32_t magia_sleep_commit(void);          /* 1 if timed out */
void magia_wake(int32_t tid);
void magia_yield(void);
void magia_thread_exit_pending(int32_t status);
int32_t magia_copyin(void *kdst, uint64_t usrc, size_t n);
int32_t magia_copyout(uint64_t udst, const void *ksrc, size_t n);
int32_t magia_uvm_alloc(int32_t jid, uint64_t npages, int32_t flags, uint64_t *uva, uint64_t *kva);
int32_t magia_uvm_free(int32_t jid, uint64_t uva, uint64_t npages);
uint64_t magia_free_pages(void);
uint64_t magia_total_pages(void);
uint64_t magia_context_switches(void);
uint64_t magia_kmem_inuse(void);
/* C sysctl nodes from the set_sysctl linker set (include/magia/sysctl.h). */
const struct c_sysctl *const *magia_sysctl_c_set(size_t *count);

#endif
