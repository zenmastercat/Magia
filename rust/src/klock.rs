// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! KLock: an IRQ-disabling spinlock for Rust kernel state.
//!
//! Magia is uniprocessor. With interrupts disabled nobody else can run, so
//! finding the lock already taken means the current CPU took it before:
//! a recursive acquire or an IRQ handler re-entering. Spinning would hang
//! forever, so `lock()` panics with the lock name instead. `try_lock()` is
//! for IRQ context (the timer tick), which skips work when the lock is busy.
//! Once SMP arrives `lock()` must spin with a holder check instead.

use crate::kenv;
use core::cell::UnsafeCell;
use core::ops::{Deref, DerefMut};
use core::sync::atomic::{AtomicBool, Ordering};

pub struct KLock<T> {
    name: &'static str,
    locked: AtomicBool,
    data: UnsafeCell<T>,
}

// SAFETY: access to `data` is serialised by `locked` with IRQs disabled.
unsafe impl<T> Sync for KLock<T> {}
unsafe impl<T> Send for KLock<T> {}

pub struct KGuard<'a, T> {
    lock: &'a KLock<T>,
    flags: u64,
}

impl<T> KLock<T> {
    pub const fn new(name: &'static str, v: T) -> Self {
        KLock { name, locked: AtomicBool::new(false), data: UnsafeCell::new(v) }
    }

    pub fn lock(&self) -> KGuard<'_, T> {
        let flags = kenv::irq_save();
        if self.locked.swap(true, Ordering::Acquire) {
            kenv::irq_restore(flags);
            panic!("KLock {}: recursive acquire (uniprocessor, IRQs off)", self.name);
        }
        KGuard { lock: self, flags }
    }

    pub fn try_lock(&self) -> Option<KGuard<'_, T>> {
        let flags = kenv::irq_save();
        if self.locked.swap(true, Ordering::Acquire) {
            kenv::irq_restore(flags);
            return None;
        }
        Some(KGuard { lock: self, flags })
    }

    pub fn is_locked(&self) -> bool {
        self.locked.load(Ordering::Relaxed)
    }
}

impl<T> Drop for KGuard<'_, T> {
    fn drop(&mut self) {
        self.lock.locked.store(false, Ordering::Release);
        kenv::irq_restore(self.flags);
    }
}

impl<T> Deref for KGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: the guard proves exclusive access.
        unsafe { &*self.lock.data.get() }
    }
}

impl<T> DerefMut for KGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: the guard proves exclusive access.
        unsafe { &mut *self.lock.data.get() }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lock_unlock_and_try_lock() {
        let l = KLock::new("t", 5);
        {
            let mut g = l.lock();
            *g += 1;
            assert!(l.is_locked());
            assert!(l.try_lock().is_none());
        }
        assert!(!l.is_locked());
        assert_eq!(*l.try_lock().unwrap(), 6);
    }

    #[test]
    #[should_panic(expected = "recursive acquire")]
    fn recursive_lock_panics() {
        let l = KLock::new("rec", ());
        let _a = l.lock();
        let _b = l.lock();
    }
}
