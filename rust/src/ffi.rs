// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! The C -> Rust boundary (include/magia/rust.h). This is the only file
//! with global kernel state: GIANT guards the `Kernel`; the scheduler
//! (SCHED) and jail memory accounting (ACCT) have their own locks in
//! kenv.rs.
//!
//! Lock order: GIANT -> SCHED, GIANT -> ACCT. SCHED and ACCT never take
//! another lock. No Rust lock is held across magia_sleep_commit or
//! magia_yield.

use crate::abi::*;
use crate::kenv::{self, ACCT, SCHED};
use crate::kernel::Kernel;
use crate::klock::KLock;
use crate::syscall::{Ctx, Ret};

static GIANT: KLock<Option<Kernel>> = KLock::new("GIANT", None);

fn bytes<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        // SAFETY: C passes (pointer, length) pairs of kernel memory.
        unsafe { core::slice::from_raw_parts(p, n) }
    }
}

fn text<'a>(p: *const u8, n: usize) -> &'a str {
    core::str::from_utf8(bytes(p, n)).unwrap_or("")
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_init(bi: *const RsBootInfo) {
    // SAFETY: C passes a valid boot info struct.
    let bi = unsafe { &*bi };
    for (i, &(name, sz)) in ABI_SIZES.iter().enumerate() {
        if bi.abi_sizes[i] != sz as u64 {
            panic!("ABI mismatch: sizeof(struct {}) is {} in C, {} in Rust", name, bi.abi_sizes[i], sz);
        }
    }
    let tid0 = kenv::current_tid();
    let k = Kernel::new(if tid0 > 0 { tid0 } else { 1 });
    let nsys = crate::syscall::SYSCALLS.len();
    let nsysctl = k.sysctl.leaves().len();
    *GIANT.lock() = Some(k);
    crate::kprint!(
        "rust: {} syscalls, {} sysctl nodes, ramfs + devfs, jail 0 ({} pages, NX={} SMEP={} SMAP={})\n",
        nsys,
        nsysctl,
        bi.total_pages,
        bi.cpu_flags & RS_CPU_NX != 0,
        bi.cpu_flags & RS_CPU_SMEP != 0,
        bi.cpu_flags & RS_CPU_SMAP != 0
    );
}

/// System call entry for both ring 3 and kernel-thread processes.
#[unsafe(no_mangle)]
pub extern "C" fn rs_syscall(pid: i32, nr: u64, args: *const u64) -> i64 {
    let mut a = [0u64; 6];
    if !args.is_null() {
        // SAFETY: C passes an array of six u64 in kernel memory.
        unsafe { core::ptr::copy_nonoverlapping(args, a.as_mut_ptr(), 6) };
    }
    let mut ctx = Ctx::new(pid, kenv::current_tid());
    loop {
        let r = {
            let mut g = GIANT.lock();
            let Some(k) = g.as_mut() else { return -(ENOSYS as i64) };
            let r = k.syscall(&mut ctx, nr, &a);
            if let Ret::Block(t) = r {
                // still under GIANT: a waker needs GIANT, so no wakeup
                // can slip in between registration and prepare
                kenv::sleep_prepare(t);
            }
            r
        };
        match r {
            Ret::Val(v) => {
                if ctx.yield_after {
                    kenv::yield_now();
                }
                return v;
            }
            Ret::Block(_) => {
                ctx.timed_out = kenv::sleep_commit();
                ctx.retry += 1;
            }
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_proc_spawn(ppid: i32, tid: i32, name: *const u8, namelen: usize) -> i32 {
    let mut g = GIANT.lock();
    let Some(k) = g.as_mut() else { return -ENOSYS };
    match k.spawn(ppid, tid, bytes(name, namelen)) {
        Ok(pid) => pid,
        Err(e) => -e,
    }
}

// ---- EEVDF scheduler (usable before rs_init) ----

#[unsafe(no_mangle)]
pub extern "C" fn rs_sched_add(tid: i32, weight: u32) {
    SCHED.lock().add(tid, weight)
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_sched_remove(tid: i32) {
    SCHED.lock().remove(tid)
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_sched_wakeup(tid: i32, now_ns: u64) -> i32 {
    SCHED.lock().wakeup(tid, now_ns) as i32
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_sched_tick(now_ns: u64) -> i32 {
    SCHED.lock().tick(now_ns) as i32
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_sched_pick(prev_tid: i32, prev_runnable: i32, now_ns: u64) -> i32 {
    SCHED.lock().pick(prev_tid, prev_runnable != 0, now_ns)
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_sched_yield(tid: i32) {
    SCHED.lock().yield_(tid)
}

/// Timer IRQ, every ms. Skips the tick if GIANT is busy (cannot happen on
/// a uniprocessor, since GIANT disables IRQs) or before rs_init.
#[unsafe(no_mangle)]
pub extern "C" fn rs_timer_tick(now_ms: u64) {
    if let Some(mut g) = GIANT.try_lock() {
        if let Some(k) = g.as_mut() {
            k.timer_tick(now_ms);
        }
    }
}

// ---- jail memory accounting (ACCT only; called from C uvm_alloc) ----

#[unsafe(no_mangle)]
pub extern "C" fn rs_jail_mem_charge(jid: i32, bytes: u64) -> i32 {
    match ACCT.lock().charge(jid, bytes) {
        Ok(()) => 0,
        Err(_) => -ENOMEM,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_jail_mem_uncharge(jid: i32, bytes: u64) {
    ACCT.lock().uncharge(jid, bytes)
}

// ---- debug monitor ----

fn with_kernel(f: impl FnOnce(&mut Kernel)) {
    let mut g = GIANT.lock();
    match g.as_mut() {
        Some(k) => f(k),
        None => crate::kprint!("rust subsystems not initialised\n"),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_dbg_ps() {
    with_kernel(|k| k.dbg_ps())
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_dbg_jails() {
    with_kernel(|k| k.dbg_jails())
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_dbg_sysctl(name: *const u8, namelen: usize, value: *const u8, valuelen: usize) {
    with_kernel(|k| k.dbg_sysctl(text(name, namelen), text(value, valuelen)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_dbg_ls(path: *const u8, len: usize) {
    with_kernel(|k| k.dbg_ls(bytes(path, len)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_dbg_cat(path: *const u8, len: usize) {
    with_kernel(|k| k.dbg_cat(bytes(path, len)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rs_dbg_uname() {
    with_kernel(|k| k.dbg_uname())
}

// ---- ACPI (rust/src/acpi.rs; C side kernel/acpi.c) ----

unsafe extern "C" {
    fn magia_acpi_map(pa: u64, len: usize) -> *const u8;
}

struct KernelPhys;
impl crate::acpi::PhysMem for KernelPhys {
    fn read(&self, pa: u64, len: usize) -> Option<&[u8]> {
        // SAFETY: magia_acpi_map returns NULL or a direct-map pointer to
        // `len` bytes of mapped, read-only-in-practice firmware memory.
        let p = unsafe { magia_acpi_map(pa, len) };
        if p.is_null() { None } else { Some(unsafe { core::slice::from_raw_parts(p, len) }) }
    }
}

/// Parse the ACPI tables from the RSDP; 0 or -code (size mismatch: -99).
#[unsafe(no_mangle)]
pub extern "C" fn rs_acpi_parse(rsdp_pa: u64, bios: i32, out: *mut crate::acpi::CAcpiInfo, size: usize) -> i32 {
    if out.is_null() || size != core::mem::size_of::<crate::acpi::CAcpiInfo>() {
        return -99;
    }
    // SAFETY: C passes a pointer to its `struct acpi_info` of `size` bytes.
    let out = unsafe { &mut *out };
    match crate::acpi::parse_all(&KernelPhys, rsdp_pa, bios != 0, out) {
        Ok(()) => 0,
        Err(e) => -e.code(),
    }
}
