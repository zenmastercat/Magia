// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! Kernel environment: the only path from Rust to C kernel services.
//!
//! In the kernel build every function calls the `magia_*` glue in
//! kernel/rustglue.c. In host tests (`cargo test`) a per-thread mock stands
//! in: a fake clock, a console capture buffer, fake user memory at
//! 0x1_0000_0000, and the real jail accounting and scheduler policy objects.
//! Policy code never calls `magia_*` directly, so all of it runs on the host.

use crate::abi::*;
use crate::acct::Acct;
use crate::sched::Sched;
use alloc::vec::Vec;
use core::fmt;

pub use imp::*;

/// Milliseconds since boot (timer ticks at HZ=1000).
pub fn now_ms() -> u64 {
    ticks_ms()
}

struct ConsoleWriter;
impl fmt::Write for ConsoleWriter {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        console_write(s.as_bytes());
        Ok(())
    }
}

pub fn print(args: fmt::Arguments<'_>) {
    let _ = fmt::write(&mut ConsoleWriter, args);
}

// ------------------------------------------------------------------------
#[cfg(not(test))]
mod imp {
    use super::*;
    use crate::klock::KLock;

    unsafe extern "C" {
        fn magia_console_write(s: *const u8, n: usize);
        fn magia_now_ns() -> u64;
        fn magia_ticks() -> u64;
        fn magia_current_tid() -> i32;
        fn magia_irq_save() -> u64;
        fn magia_irq_restore(flags: u64);
        fn magia_sleep_prepare(timeout_ms: u64);
        fn magia_sleep_commit() -> i32;
        fn magia_wake(tid: i32);
        fn magia_yield();
        fn magia_thread_exit_pending(status: i32);
        fn magia_copyin(kdst: *mut u8, usrc: u64, n: usize) -> i32;
        fn magia_copyout(udst: u64, ksrc: *const u8, n: usize) -> i32;
        fn magia_uvm_alloc(jid: i32, npages: u64, flags: i32, uva: *mut u64, kva: *mut u64) -> i32;
        fn magia_uvm_free(jid: i32, uva: u64, npages: u64) -> i32;
        fn magia_free_pages() -> u64;
        fn magia_total_pages() -> u64;
        fn magia_context_switches() -> u64;
        fn magia_kmem_inuse() -> u64;
        fn magia_sysctl_c_set(count: *mut usize) -> *const *const CSysctl;
    }

    /// Jail memory accounting. Taken alone by rs_jail_mem_charge (called
    /// from C uvm_alloc), or after GIANT; never before GIANT.
    pub static ACCT: KLock<Acct> = KLock::new("ACCT", Acct::new());
    /// EEVDF run queue. Taken from IRQ context and the C scheduler; never
    /// takes GIANT.
    pub static SCHED: KLock<Sched> = KLock::new("SCHED", Sched::new());

    pub fn console_write(s: &[u8]) {
        unsafe { magia_console_write(s.as_ptr(), s.len()) }
    }
    pub fn now_ns() -> u64 {
        unsafe { magia_now_ns() }
    }
    pub fn ticks_ms() -> u64 {
        unsafe { magia_ticks() }
    }
    pub fn current_tid() -> i32 {
        unsafe { magia_current_tid() }
    }
    pub fn irq_save() -> u64 {
        unsafe { magia_irq_save() }
    }
    pub fn irq_restore(f: u64) {
        unsafe { magia_irq_restore(f) }
    }
    pub fn sleep_prepare(timeout_ms: u64) {
        unsafe { magia_sleep_prepare(timeout_ms) }
    }
    /// Returns true when the sleep ended because of its timeout.
    pub fn sleep_commit() -> bool {
        unsafe { magia_sleep_commit() != 0 }
    }
    pub fn wake(tid: i32) {
        unsafe { magia_wake(tid) }
    }
    pub fn yield_now() {
        unsafe { magia_yield() }
    }
    pub fn thread_exit_pending(status: i32) {
        unsafe { magia_thread_exit_pending(status) }
    }
    pub fn copyin(dst: &mut [u8], uaddr: u64) -> KResult<()> {
        if dst.is_empty() {
            return Ok(());
        }
        match unsafe { magia_copyin(dst.as_mut_ptr(), uaddr, dst.len()) } {
            0 => Ok(()),
            _ => Err(EFAULT),
        }
    }
    pub fn copyout(uaddr: u64, src: &[u8]) -> KResult<()> {
        if src.is_empty() {
            return Ok(());
        }
        match unsafe { magia_copyout(uaddr, src.as_ptr(), src.len()) } {
            0 => Ok(()),
            _ => Err(EFAULT),
        }
    }
    /// Map `npages` fresh zeroed user pages charged to `jid`.
    /// Returns (user address, kernel alias address).
    pub fn uvm_alloc(jid: i32, npages: u64, flags: i32) -> KResult<(u64, u64)> {
        let (mut uva, mut kva) = (0u64, 0u64);
        let r = unsafe { magia_uvm_alloc(jid, npages, flags, &mut uva, &mut kva) };
        if r < 0 { Err(-r) } else { Ok((uva, kva)) }
    }
    pub fn uvm_free(jid: i32, uva: u64, npages: u64) -> KResult<()> {
        let r = unsafe { magia_uvm_free(jid, uva, npages) };
        if r < 0 { Err(-r) } else { Ok(()) }
    }
    pub fn free_pages() -> u64 {
        unsafe { magia_free_pages() }
    }
    pub fn total_pages() -> u64 {
        unsafe { magia_total_pages() }
    }
    pub fn context_switches() -> u64 {
        unsafe { magia_context_switches() }
    }
    pub fn kmem_inuse() -> u64 {
        unsafe { magia_kmem_inuse() }
    }
    pub fn c_sysctls() -> Vec<*const CSysctl> {
        let mut n = 0usize;
        let p = unsafe { magia_sysctl_c_set(&mut n) };
        let mut v = Vec::new();
        if !p.is_null() {
            for i in 0..n {
                v.push(unsafe { *p.add(i) });
            }
        }
        v
    }
    pub fn with_acct<R>(f: impl FnOnce(&mut Acct) -> R) -> R {
        f(&mut ACCT.lock())
    }
    pub fn with_sched<R>(f: impl FnOnce(&mut Sched) -> R) -> R {
        f(&mut SCHED.lock())
    }
}

// ------------------------------------------------------------------------
#[cfg(test)]
mod imp {
    pub use super::mock::*;
}

#[cfg(test)]
pub mod mock {
    //! Per-thread mock kernel (each `#[test]` runs on its own thread).
    use super::*;
    use core::cell::RefCell;

    pub const UMEM_BASE: u64 = 0x1_0000_0000;
    pub const UMEM_SIZE: usize = 4 << 20;

    struct UMem {
        ptr: *mut u8,
    }
    impl Drop for UMem {
        fn drop(&mut self) {
            // SAFETY: allocated in Mock::new with the same layout.
            unsafe {
                drop(Vec::from_raw_parts(self.ptr, UMEM_SIZE, UMEM_SIZE));
            }
        }
    }

    pub struct Mock {
        pub now_ns: u64,
        pub console: Vec<u8>,
        umem: UMem,
        brk: u64,
        ranges: Vec<(u64, u64, i32)>, // (uva, bytes, charged jid)
        pub woken: Vec<i32>,
        pub sleeps: Vec<u64>,
        pub exit_pending: Option<i32>,
        pub yields: u32,
        pub csysctls: Vec<*const CSysctl>,
    }

    impl Mock {
        fn new() -> Self {
            let mut v = alloc::vec![0u8; UMEM_SIZE];
            let ptr = v.as_mut_ptr();
            core::mem::forget(v);
            Mock {
                now_ns: 0,
                console: Vec::new(),
                umem: UMem { ptr },
                brk: UMEM_BASE,
                ranges: Vec::new(),
                woken: Vec::new(),
                sleeps: Vec::new(),
                exit_pending: None,
                yields: 0,
                csysctls: Vec::new(),
            }
        }
        fn mapped(&self, uaddr: u64, len: usize) -> Option<*mut u8> {
            let end = uaddr.checked_add(len as u64)?;
            if end > USER_TOP {
                return None;
            }
            for &(b, n, _) in &self.ranges {
                if uaddr >= b && end <= b + n {
                    return Some(unsafe { self.umem.ptr.add((uaddr - UMEM_BASE) as usize) });
                }
            }
            None
        }
    }

    thread_local! {
        static M: RefCell<Mock> = RefCell::new(Mock::new());
        static ACCT_T: RefCell<Acct> = const { RefCell::new(Acct::new()) };
        static SCHED_T: RefCell<Sched> = const { RefCell::new(Sched::new()) };
    }

    pub fn with<R>(f: impl FnOnce(&mut Mock) -> R) -> R {
        M.with(|m| f(&mut m.borrow_mut()))
    }

    pub fn console_write(s: &[u8]) {
        with(|m| m.console.extend_from_slice(s))
    }
    pub fn now_ns() -> u64 {
        with(|m| m.now_ns)
    }
    pub fn ticks_ms() -> u64 {
        with(|m| m.now_ns / 1_000_000)
    }
    pub fn current_tid() -> i32 {
        -1
    }
    pub fn irq_save() -> u64 {
        0
    }
    pub fn irq_restore(_f: u64) {}
    pub fn sleep_prepare(timeout_ms: u64) {
        with(|m| m.sleeps.push(timeout_ms))
    }
    /// The mock never really sleeps: it advances the clock by the timeout
    /// and reports a timeout. An infinite sleep would hang the test.
    pub fn sleep_commit() -> bool {
        with(|m| {
            let t = *m.sleeps.last().expect("sleep_commit without prepare");
            assert!(t != 0, "host test would block forever");
            m.now_ns += t * 1_000_000;
            true
        })
    }
    pub fn wake(tid: i32) {
        with(|m| m.woken.push(tid))
    }
    pub fn yield_now() {
        with(|m| m.yields += 1)
    }
    pub fn thread_exit_pending(status: i32) {
        with(|m| m.exit_pending = Some(status))
    }
    pub fn copyin(dst: &mut [u8], uaddr: u64) -> KResult<()> {
        if dst.is_empty() {
            return Ok(());
        }
        let p = with(|m| m.mapped(uaddr, dst.len())).ok_or(EFAULT)?;
        unsafe { core::ptr::copy_nonoverlapping(p, dst.as_mut_ptr(), dst.len()) };
        Ok(())
    }
    pub fn copyout(uaddr: u64, src: &[u8]) -> KResult<()> {
        if src.is_empty() {
            return Ok(());
        }
        let p = with(|m| m.mapped(uaddr, src.len())).ok_or(EFAULT)?;
        unsafe { core::ptr::copy_nonoverlapping(src.as_ptr(), p, src.len()) };
        Ok(())
    }
    pub fn uvm_alloc(jid: i32, npages: u64, _flags: i32) -> KResult<(u64, u64)> {
        let bytes = npages * PAGE_SIZE;
        if jid >= 0 {
            with_acct(|a| a.charge(jid, bytes))?;
        }
        let r = with(|m| {
            if m.brk + bytes > UMEM_BASE + UMEM_SIZE as u64 {
                return Err(ENOMEM);
            }
            let uva = m.brk;
            m.brk += bytes;
            m.ranges.push((uva, bytes, jid));
            let kva = unsafe { m.umem.ptr.add((uva - UMEM_BASE) as usize) } as u64;
            unsafe { core::ptr::write_bytes(kva as *mut u8, 0, bytes as usize) };
            Ok((uva, kva))
        });
        if r.is_err() && jid >= 0 {
            with_acct(|a| a.uncharge(jid, bytes));
        }
        r
    }
    pub fn uvm_free(jid: i32, uva: u64, npages: u64) -> KResult<()> {
        let bytes = npages * PAGE_SIZE;
        with(|m| {
            let i = m.ranges.iter().position(|r| r.0 == uva && r.1 == bytes).ok_or(EINVAL)?;
            m.ranges.remove(i);
            Ok::<(), Errno>(())
        })?;
        if jid >= 0 {
            with_acct(|a| a.uncharge(jid, bytes));
        }
        Ok(())
    }
    pub fn free_pages() -> u64 {
        1000
    }
    pub fn total_pages() -> u64 {
        65536
    }
    pub fn context_switches() -> u64 {
        42
    }
    pub fn kmem_inuse() -> u64 {
        12345
    }
    pub fn c_sysctls() -> Vec<*const CSysctl> {
        with(|m| m.csysctls.clone())
    }
    pub fn with_acct<R>(f: impl FnOnce(&mut Acct) -> R) -> R {
        ACCT_T.with(|a| f(&mut a.borrow_mut()))
    }
    pub fn with_sched<R>(f: impl FnOnce(&mut Sched) -> R) -> R {
        SCHED_T.with(|s| f(&mut s.borrow_mut()))
    }

    // ---- helpers for tests ----

    /// Map `len` bytes of uncharged user memory, return its user address.
    pub fn umap(len: usize) -> u64 {
        let pages = (len as u64).div_ceil(PAGE_SIZE).max(1);
        uvm_alloc(-1, pages, 0).unwrap().0
    }
    pub fn uwrite(uva: u64, data: &[u8]) {
        copyout(uva, data).expect("uwrite outside mapped user memory")
    }
    pub fn uread(uva: u64, len: usize) -> Vec<u8> {
        let mut v = alloc::vec![0u8; len];
        copyin(&mut v, uva).expect("uread outside mapped user memory");
        v
    }
    pub fn ustr(s: &str) -> u64 {
        let u = umap(s.len() + 1);
        uwrite(u, s.as_bytes());
        uwrite(u + s.len() as u64, &[0]);
        u
    }
    pub fn uput<T: Pod>(v: &T) -> u64 {
        let u = umap(core::mem::size_of::<T>());
        uwrite(u, v.as_bytes());
        u
    }
    pub fn uget<T: Pod>(uva: u64) -> T {
        let mut v = T::zeroed();
        copyin(v.as_bytes_mut(), uva).unwrap();
        v
    }
    pub fn take_console() -> alloc::string::String {
        with(|m| alloc::string::String::from_utf8_lossy(&core::mem::take(&mut m.console)).into_owned())
    }
    pub fn advance_ms(ms: u64) {
        with(|m| m.now_ns += ms * 1_000_000)
    }
    pub fn take_woken() -> Vec<i32> {
        with(|m| core::mem::take(&mut m.woken))
    }
}
