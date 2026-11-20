// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! futex: wait on / wake a 32-bit user word. Idea: Linux futex (Franke,
//! Russell, Kirkwood, 2002), re-derived.
//!
//! WAIT checks the word and queues the caller atomically with respect to
//! WAKE because both run under GIANT, so no wakeup can be lost between
//! the check and the sleep. Magia has a single address space, so the user
//! address itself is the futex key.

use crate::abi::*;
use crate::kernel::{Kernel, WaitKey};
use crate::syscall::{Ctx, Ret, SysResult, Timed, val};
use crate::uaccess::*;

impl Kernel {
    /// futex(uaddr, op, val, timeout_ms); timeout 0 = wait forever.
    pub fn sys_futex(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let uaddr = a[0];
        if uaddr % 4 != 0 {
            return Err(EINVAL);
        }
        let key = WaitKey::Futex(uaddr);
        match a[1] {
            FUTEX_WAIT => {
                if c.retry > 0 {
                    if !self.is_waiting(key, c.tid) {
                        return val(0); // woken by FUTEX_WAKE
                    }
                } else {
                    let cur: u32 = copyin_val(uaddr)?;
                    if cur != a[2] as u32 {
                        return Err(EAGAIN);
                    }
                }
                let t = if a[3] == 0 { None } else { Some(a[3]) };
                match c.timed(t) {
                    Timed::Expired => Err(ETIMEDOUT),
                    Timed::Wait(ms) => {
                        self.wait_on(key, c.tid);
                        Ok(Ret::Block(ms))
                    }
                }
            }
            FUTEX_WAKE => {
                // validate the address like WAIT does
                let _: u32 = copyin_val(uaddr)?;
                val(self.wake_n(key, a[2] as usize) as i64)
            }
            _ => Err(EINVAL),
        }
    }
}

#[cfg(test)]
mod tests {
    use crate::abi::*;
    use crate::syscall::Ctx;
    use crate::testutil::*;

    #[test]
    fn wait_value_mismatch_is_eagain() {
        let (mut k, pid) = kernel_with_proc();
        let w = umap(4);
        uwrite(w, &5u32.to_le_bytes());
        assert_eq!(sys(&mut k, pid, SYS_FUTEX, &[w, FUTEX_WAIT, 4, 0]), -EAGAIN as i64);
    }

    #[test]
    fn wait_times_out() {
        let (mut k, pid) = kernel_with_proc();
        let w = umap(4);
        assert_eq!(sys(&mut k, pid, SYS_FUTEX, &[w, FUTEX_WAIT, 0, 10]), -ETIMEDOUT as i64);
        assert!(k.waits.is_empty());
    }

    #[test]
    fn bad_args() {
        let (mut k, pid) = kernel_with_proc();
        let w = umap(8);
        assert_eq!(sys(&mut k, pid, SYS_FUTEX, &[w + 2, FUTEX_WAIT, 0, 1]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_FUTEX, &[w, 2, 0, 1]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_FUTEX, &[0x4000, FUTEX_WAIT, 0, 1]), -EFAULT as i64);
        assert_eq!(sys(&mut k, pid, SYS_FUTEX, &[0xffff_ffff_8000_0000, FUTEX_WAKE, 1, 0]), -EFAULT as i64);
    }

    #[test]
    fn wake_counts_and_fifo() {
        let (mut k, pid) = kernel_with_proc();
        let p2 = spawn(&mut k, pid, "w2");
        let p3 = spawn(&mut k, pid, "w3");
        let w = umap(4);
        let mut cs: Vec<Ctx> = [pid, p2, p3].iter().map(|&p| Ctx::new(p, -1)).collect();
        for c in cs.iter_mut() {
            assert!(is_block(&call_ctx(&mut k, c, SYS_FUTEX, &[w, FUTEX_WAIT, 0, 0])));
        }
        take_woken();
        assert_eq!(sys(&mut k, pid, SYS_FUTEX, &[w, FUTEX_WAKE, 2, 0]), 2);
        let tids: Vec<i32> = [pid, p2].iter().map(|&p| k.proc(p).unwrap().tid).collect();
        assert_eq!(take_woken(), tids, "oldest waiters first");
        // woken waiters return 0, the third is still waiting
        cs[0].retry = 1;
        assert_eq!(ret_val(call_ctx(&mut k, &mut cs[0], SYS_FUTEX, &[w, FUTEX_WAIT, 0, 0])), 0);
        assert_eq!(sys(&mut k, pid, SYS_FUTEX, &[w, FUTEX_WAKE, 10, 0]), 1);
        assert_eq!(sys(&mut k, pid, SYS_FUTEX, &[w, FUTEX_WAKE, 10, 0]), 0);
    }

    #[test]
    fn exit_removes_waiters() {
        let (mut k, pid) = kernel_with_proc();
        let p2 = spawn(&mut k, pid, "w2");
        let w = umap(4);
        assert!(is_block(&call(&mut k, p2, SYS_FUTEX, &[w, FUTEX_WAIT, 0, 0])));
        sys(&mut k, p2, SYS_EXIT, &[0]);
        assert_eq!(sys(&mut k, pid, SYS_FUTEX, &[w, FUTEX_WAKE, 10, 0]), 0);
    }
}
