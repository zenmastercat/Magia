// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! Magia kernel, Rust half: policy and subsystems that parse or check
//! untrusted input. `#![no_std]`, `alloc` only, zero external crates.
//!
//! C (kernel/) owns the hardware: boot, CPU tables, interrupts, paging,
//! physical memory, threads and the context switch. Rust owns the
//! process table, Capsicum rights, jails and resource accounting, the
//! system call table and argument validation, VFS (ramfs, devfs, pipes),
//! kqueue, io_uring-style rings, futex, the sysctl MIB tree and the EEVDF
//! scheduling policy. See docs/LANGUAGES.md for why the line is drawn
//! there. Everything here is written for Magia; ideas are credited in
//! module comments and docs/IDEAS.md.

#![cfg_attr(not(test), no_std)]
#![allow(clippy::new_without_default)]

extern crate alloc;

pub const RELEASE: &str = "0.2.0";
pub const VERSION: &str = "Magia 0.2.0 (experimental; adapted BSD code is listed in docs/THIRD_PARTY.md)";

#[macro_export]
macro_rules! kprint {
    ($($t:tt)*) => { $crate::kenv::print(format_args!($($t)*)) };
}

pub mod abi;
pub mod acpi;
pub mod acct;
pub mod cap;
pub mod dbg;
pub mod fs;
pub mod futex;
pub mod jail;
pub mod kenv;
pub mod kernel;
pub mod klock;
pub mod kqueue;
pub mod pipe;
pub mod proc;
pub mod sched;
pub mod syscall;
pub mod sysctl;
pub mod uaccess;
pub mod uring;
pub mod vfs;

#[cfg(not(test))]
pub mod ffi;

#[cfg(test)]
pub mod testutil;

#[cfg(not(test))]
mod rt {
    use core::alloc::{GlobalAlloc, Layout};
    use core::fmt::Write;

    unsafe extern "C" {
        fn magia_kmalloc(size: usize, align: usize) -> *mut u8;
        fn magia_kfree(p: *mut u8);
        fn magia_panic(msg: *const u8, n: usize) -> !;
    }

    struct KAlloc;

    unsafe impl GlobalAlloc for KAlloc {
        unsafe fn alloc(&self, l: Layout) -> *mut u8 {
            unsafe { magia_kmalloc(l.size().max(1), l.align()) }
        }
        unsafe fn dealloc(&self, p: *mut u8, _l: Layout) {
            unsafe { magia_kfree(p) }
        }
    }

    #[global_allocator]
    static ALLOC: KAlloc = KAlloc;

    struct Buf {
        b: [u8; 256],
        n: usize,
    }
    impl Write for Buf {
        fn write_str(&mut self, s: &str) -> core::fmt::Result {
            let room = self.b.len() - self.n;
            let k = s.len().min(room);
            self.b[self.n..self.n + k].copy_from_slice(&s.as_bytes()[..k]);
            self.n += k;
            Ok(())
        }
    }

    #[panic_handler]
    fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
        let mut b = Buf { b: [0; 256], n: 0 };
        let _ = write!(b, "rust: {}", info);
        unsafe { magia_panic(b.b.as_ptr(), b.n) }
    }
}
