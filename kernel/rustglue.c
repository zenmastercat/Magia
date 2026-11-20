/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * rustglue.c - the kernel services Rust calls (magia_* in
 * include/magia/rust.h) and the boot-time hand-off to Rust (rs_init).
 */
#include <magia/abi.h>
#include <magia/cpu.h>
#include <magia/linker_set.h>
#include <magia/mm.h>
#include <magia/rust.h>
#include <magia/sysctl.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

extern bool rust_ready;

void magia_console_write(const char *s, size_t n) { console_write(s, n); }

void magia_panic(const char *msg, size_t n) { panic("rust: %.*s", (int)n, msg); }

void *magia_kmalloc(size_t size, size_t align) { return kmalloc_aligned(size, align); }
void magia_kfree(void *p) { kfree(p); }
uint64_t magia_now_ns(void) { return now_ns(); }
uint64_t magia_ticks(void) { return ticks; }
int32_t magia_current_tid(void) { return curthread()->tid; }
uint64_t magia_irq_save(void) { return irq_save(); }
void magia_irq_restore(uint64_t flags) { irq_restore(flags); }
void magia_sleep_prepare(uint64_t timeout_ms) { sleep_prepare(timeout_ms); }
int32_t magia_sleep_commit(void) { return sleep_commit() ? 1 : 0; }
void magia_wake(int32_t tid) { thread_wake(tid); }
void magia_yield(void) { sched_yield(); }

void magia_thread_exit_pending(int32_t status)
{
	struct thread *t = curthread();
	t->exit_pending = true;
	t->exit_status = status;
}

int32_t magia_copyin(void *kdst, uint64_t usrc, size_t n) { return copyin(kdst, usrc, n); }
int32_t magia_copyout(uint64_t udst, const void *ksrc, size_t n) { return copyout(udst, ksrc, n); }

int32_t magia_uvm_alloc(int32_t jid, uint64_t npages, int32_t flags, uint64_t *uva, uint64_t *kva)
{
	return uvm_alloc(jid, npages, flags, uva, kva);
}

int32_t magia_uvm_free(int32_t jid, uint64_t uva, uint64_t npages) { return uvm_free(jid, uva, npages); }
uint64_t magia_free_pages(void) { return pmm_free_pages(); }
uint64_t magia_total_pages(void) { return pmm_total_pages(); }
uint64_t magia_context_switches(void) { return context_switches; }

uint64_t magia_kmem_inuse(void)
{
	struct kmem_stats s;
	kmem_get_stats(&s);
	return s.bytes_inuse;
}

const struct c_sysctl *const *magia_sysctl_c_set(size_t *count)
{
	*count = MAGIA_SET_COUNT(sysctl);
	return __start_set_sysctl;
}

static const char kern_build[] = MAGIA_NAME " " MAGIA_RELEASE " built " __DATE__ " " __TIME__;
SYSCTL_STR_RO(kern_build, "kern.build", kern_build, "build stamp");

static void rust_sysinit(void)
{
	struct rs_boot_info bi = {
		.abi_sizes = { sizeof(struct m_stat), sizeof(struct m_dirent), sizeof(struct jail_params),
			       sizeof(struct jail_info), sizeof(struct proc_info), sizeof(struct utsname),
			       sizeof(struct kevent), sizeof(struct uring_sqe) },
		.total_pages = pmm_total_pages(),
		.hz = HZ,
		.cpu_flags = (cpu_feat.nx ? RS_CPU_NX : 0) | (smep_enabled ? RS_CPU_SMEP : 0) |
			     (smap_enabled ? RS_CPU_SMAP : 0),
	};
	rs_init(&bi);
	rust_ready = true;
}
SYSINIT(rust, SI_ORDER_RUST, rust_sysinit);
