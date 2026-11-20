// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! io_uring-style submission/completion rings. Idea: Linux io_uring
//! (Axboe, 2019), re-derived and deliberately narrow.
//!
//! The ring lives in one user-visible region shared with the kernel:
//!
//! ```text
//!   0              struct uring_ring_hdr (64 bytes)
//!   64             u32 sq_array[entries]      (indexes into sqes)
//!   sqes_off       struct uring_sqe[entries]  (64-byte aligned)
//!   cqes_off       struct uring_cqe[2*entries]
//! ```
//!
//! The user may scribble on all of it at any time, so the kernel treats
//! it as hostile input: it keeps its own copies of sq_head and cq_tail,
//! reads sq_tail once per call, reads every index once, copies every SQE
//! to a local before validating it, and bounds every index with the mask.
//!
//! Attack-surface controls (Google reported that 60% of the Linux kernel
//! exploits submitted to its kCTF VRP used io_uring - "Learnings from kCTF
//! VRP's 42 Linux kernel exploits submissions", Google Security Blog,
//! 2023-06-14, https://security.googleblog.com/2023/06/learnings-from-kctf-vrps-42-linux.html
//! - "in the past year, there has been a clear trend: 60% of the
//! submissions exploited the io_uring component"): only four ops (NOP, READ, WRITE,
//! TIMEOUT); READ/WRITE call the *same* `io_read`/`io_write` path as
//! read(2)/write(2), so Capsicum rights and user-pointer checks are shared;
//! a global kill switch (sysctl kern.ring.enable) and a per-jail
//! permission (JAIL_ALLOW_RING).

use crate::abi::ringhdr::*;
use crate::abi::*;
use crate::fs::Io;
use crate::kenv;
use crate::kernel::{FileKind, Kernel, WaitKey};
use crate::syscall::{Ctx, Ret, SysResult, val};
use crate::uaccess::*;
use alloc::vec::Vec;

pub struct Ring {
    pub owner: i32,
    pub jid: i32,
    pub uva: u64,
    pub kva: u64,
    pub npages: u64,
    pub entries: u32,
    pub cq_entries: u32,
    pub sqes_off: usize,
    pub cqes_off: usize,
    /// kernel-private copies; the shared header copies are output only
    sq_head: u32,
    cq_tail: u32,
    dropped: u32,
    overflow: u32,
    /// pending TIMEOUT ops: (due ms, user_data)
    pub timeouts: Vec<(u64, u64)>,
}

pub fn layout(entries: u32) -> (usize, usize, usize) {
    let e = entries as usize;
    let sqes = (SIZE + 4 * e).div_ceil(64) * 64;
    let cqes = (sqes + 40 * e).div_ceil(16) * 16;
    let total = cqes + 16 * 2 * e;
    (sqes, cqes, total)
}

impl Ring {
    fn rd32(&self, off: usize) -> u32 {
        // SAFETY: off < region size (callers mask indexes); kva maps the
        // whole region for the ring's lifetime.
        unsafe { core::ptr::read_volatile((self.kva as usize + off) as *const u32) }
    }
    fn wr32(&self, off: usize, v: u32) {
        unsafe { core::ptr::write_volatile((self.kva as usize + off) as *mut u32, v) }
    }
    fn read_sqe(&self, idx: u32) -> UringSqe {
        let mut s = UringSqe::zeroed();
        let base = self.kva as usize + self.sqes_off + 40 * idx as usize;
        for (i, b) in s.as_bytes_mut().iter_mut().enumerate() {
            *b = unsafe { core::ptr::read_volatile((base + i) as *const u8) };
        }
        s
    }

    /// Post a completion. Returns false (and counts an overflow) if the
    /// completion queue is full.
    pub fn post(&mut self, user_data: u64, res: i32) -> bool {
        let head = self.rd32(CQ_HEAD);
        if self.cq_tail.wrapping_sub(head) >= self.cq_entries {
            self.overflow = self.overflow.wrapping_add(1);
            self.wr32(CQ_OVERFLOW, self.overflow);
            return false;
        }
        let slot = (self.cq_tail & (self.cq_entries - 1)) as usize;
        let cqe = UringCqe { user_data, res, flags: 0 };
        let base = self.kva as usize + self.cqes_off + 16 * slot;
        for (i, b) in cqe.as_bytes().iter().enumerate() {
            unsafe { core::ptr::write_volatile((base + i) as *mut u8, *b) };
        }
        core::sync::atomic::fence(core::sync::atomic::Ordering::Release);
        self.cq_tail = self.cq_tail.wrapping_add(1);
        self.wr32(CQ_TAIL, self.cq_tail);
        true
    }

    pub fn cq_ready(&self) -> u32 {
        // a hostile cq_head can only make this look larger or smaller;
        // clamp so it never exceeds the ring
        self.cq_tail.wrapping_sub(self.rd32(CQ_HEAD)).min(self.cq_entries)
    }

    pub fn fire_timeouts(&mut self, now: u64) -> bool {
        let mut fired = false;
        let mut i = 0;
        while i < self.timeouts.len() {
            if self.timeouts[i].0 <= now {
                let (_, ud) = self.timeouts.remove(i);
                self.post(ud, -ETIME);
                fired = true;
            } else {
                i += 1;
            }
        }
        fired
    }

    pub fn next_timeout(&self) -> u64 {
        self.timeouts.iter().map(|t| t.0).min().unwrap_or(u64::MAX)
    }
}

impl Kernel {
    /// uring_setup(entries, params*) -> fd
    pub fn sys_uring_setup(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        if !self.ring_enable {
            return Err(EPERM);
        }
        let entries = a[0] as u32;
        if !entries.is_power_of_two() || entries > URING_MAX_ENTRIES {
            return Err(EINVAL);
        }
        let jid = self.proc(c.pid)?.jid;
        if self.jail(jid)?.flags & JAIL_ALLOW_RING == 0 {
            return Err(EPERM);
        }
        // fail early on a bad params pointer, before allocating
        check_range(a[1], core::mem::size_of::<UringParams>() as u64)?;
        let (sqes_off, cqes_off, total) = layout(entries);
        let npages = (total as u64).div_ceil(PAGE_SIZE);
        let (uva, kva) = kenv::uvm_alloc(jid, npages, UVM_CONTIG)?;
        let id = self.new_id();
        let ring = Ring {
            owner: c.pid,
            jid,
            uva,
            kva,
            npages,
            entries,
            cq_entries: 2 * entries,
            sqes_off,
            cqes_off,
            sq_head: 0,
            cq_tail: 0,
            dropped: 0,
            overflow: 0,
            timeouts: Vec::new(),
        };
        ring.wr32(SQ_MASK, entries - 1);
        ring.wr32(SQ_ENTRIES, entries);
        ring.wr32(CQ_MASK, 2 * entries - 1);
        ring.wr32(CQ_ENTRIES, 2 * entries);
        self.rings.insert(id, ring);
        let f = self.file_new(FileKind::Ring(id), O_RDWR);
        let fd = self.fd_install_new(c.pid, f, CAP_ALL)?; // failure frees the ring
        let p = UringParams {
            sq_entries: entries,
            cq_entries: 2 * entries,
            ring_addr: uva,
            ring_size: npages * PAGE_SIZE,
            sq_array_off: SIZE as u64,
            sqes_off: sqes_off as u64,
            cqes_off: cqes_off as u64,
        };
        if let Err(e) = copyout_val(a[1], &p) {
            let _ = self.fd_close(c.pid, fd as i64);
            return Err(e);
        }
        val(fd as i64)
    }

    pub fn ring_free(&mut self, id: u32) {
        if let Some(r) = self.rings.remove(&id) {
            let _ = kenv::uvm_free(r.jid, r.uva, r.npages);
        }
    }

    /// Execute one SQE (already copied out of shared memory).
    fn ring_exec(&mut self, pid: i32, id: u32, sqe: &UringSqe) -> Option<i32> {
        let r = |x: KResult<Io>| -> i32 {
            match x {
                Ok(Io::Done(n)) => n as i32,
                Ok(Io::WouldBlock(_)) => -EAGAIN,
                Err(e) => -e,
            }
        };
        if sqe.flags != 0 {
            return Some(-EINVAL);
        }
        let off = if sqe.off == URING_OFF_CURRENT { None } else { Some(sqe.off) };
        match sqe.opcode {
            URING_OP_NOP => Some(0),
            URING_OP_READ => Some(r(self.io_read(pid, sqe.fd as i64, sqe.addr, sqe.len as u64, off))),
            URING_OP_WRITE => Some(r(self.io_write(pid, sqe.fd as i64, sqe.addr, sqe.len as u64, off))),
            URING_OP_TIMEOUT => {
                if sqe.off == 0 {
                    return Some(-ETIME);
                }
                if sqe.off > 86_400_000 {
                    return Some(-EINVAL);
                }
                // Bound kernel memory: at most `entries` pending timeouts per
                // ring (security review F1); the rest complete with -EAGAIN.
                let ring = self.rings.get_mut(&id).unwrap();
                if ring.timeouts.len() >= ring.entries as usize {
                    return Some(-EAGAIN);
                }
                let due = kenv::now_ms() + sqe.off;
                ring.timeouts.push((due, sqe.user_data));
                self.arm_timer(due);
                None // completes later
            }
            _ => Some(-EINVAL),
        }
    }

    /// uring_enter(fd, to_submit, min_complete, flags) -> SQEs consumed
    pub fn sys_uring_enter(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        if !self.ring_enable {
            return Err(EPERM);
        }
        let e = self.fd_get(c.pid, a[0] as i64, CAP_URING)?;
        let id = match self.files[&e.file].kind {
            FileKind::Ring(id) => id,
            _ => return Err(EINVAL),
        };
        let (to_submit, min_complete) = (a[1] as u32, a[2] as u32);
        if c.phase == 0 {
            c.phase = 1;
            let ring = &self.rings[&id];
            if min_complete > ring.cq_entries {
                return Err(EINVAL);
            }
            let tail = ring.rd32(SQ_TAIL); // read once
            let mut head = ring.sq_head;
            let pending = tail.wrapping_sub(head);
            let mut n = to_submit.min(pending);
            if pending > ring.entries {
                // corrupted tail: consume at most one ring's worth
                n = n.min(ring.entries);
            }
            let mut consumed = 0;
            for _ in 0..n {
                let ring = &self.rings[&id];
                let mask = ring.entries - 1;
                let idx = ring.rd32(SQ_ARRAY + 4 * (head & mask) as usize); // read once
                head = head.wrapping_add(1);
                consumed += 1;
                if idx >= ring.entries {
                    let r = self.rings.get_mut(&id).unwrap();
                    r.dropped = r.dropped.wrapping_add(1);
                    r.wr32(SQ_DROPPED, r.dropped);
                    continue;
                }
                let sqe = ring.read_sqe(idx); // private copy, then validate
                if let Some(res) = self.ring_exec(c.pid, id, &sqe) {
                    self.rings.get_mut(&id).unwrap().post(sqe.user_data, res);
                }
            }
            let r = self.rings.get_mut(&id).unwrap();
            r.sq_head = head;
            r.wr32(SQ_HEAD, head);
            c.saved = consumed;
        }
        if min_complete == 0 || self.rings[&id].cq_ready() >= min_complete {
            return val(c.saved);
        }
        if self.rings[&id].timeouts.is_empty() {
            // nothing in flight can ever complete: do not sleep forever
            return val(c.saved);
        }
        self.wait_on(WaitKey::Ring(id), c.tid);
        Ok(Ret::Block(0))
    }
}

const SQ_ARRAY: usize = SIZE;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::*;

    struct U {
        fd: i64,
        p: UringParams,
    }

    fn setup(k: &mut Kernel, pid: i32, entries: u64) -> U {
        let pp = umap(48);
        let fd = sys(k, pid, SYS_URING_SETUP, &[entries, pp]);
        assert!(fd >= 0, "setup failed {fd}");
        U { fd, p: uget(pp) }
    }

    fn hdr(u: &U, off: usize) -> u32 {
        u32::from_le_bytes(uread(u.p.ring_addr + off as u64, 4).try_into().unwrap())
    }
    fn set_hdr(u: &U, off: usize, v: u32) {
        uwrite(u.p.ring_addr + off as u64, &v.to_le_bytes());
    }

    /// Queue SQEs at the current tail (identity index mapping).
    fn push(u: &U, sqes: &[UringSqe]) {
        let mut tail = hdr(u, SQ_TAIL);
        for s in sqes {
            let slot = tail & (u.p.sq_entries - 1);
            uwrite(u.p.ring_addr + u.p.sqes_off + 40 * slot as u64, s.as_bytes());
            uwrite(u.p.ring_addr + u.p.sq_array_off + 4 * slot as u64, &slot.to_le_bytes());
            tail += 1;
        }
        set_hdr(u, SQ_TAIL, tail);
    }

    fn cqes(u: &U) -> Vec<UringCqe> {
        let mut head = hdr(u, CQ_HEAD);
        let tail = hdr(u, CQ_TAIL);
        let mut v = Vec::new();
        while head != tail {
            let slot = head & (u.p.cq_entries - 1);
            v.push(uget::<UringCqe>(u.p.ring_addr + u.p.cqes_off + 16 * slot as u64));
            head += 1;
        }
        set_hdr(u, CQ_HEAD, head);
        v
    }

    fn sqe(op: u8, fd: i32, off: u64, addr: u64, len: u32, ud: u64) -> UringSqe {
        UringSqe { opcode: op, flags: 0, _pad: 0, fd, off, addr, len, _pad2: 0, user_data: ud }
    }

    #[test]
    fn setup_validation() {
        let (mut k, pid) = kernel_with_proc();
        let pp = umap(48);
        for bad in [0u64, 3, 512, 257] {
            assert_eq!(sys(&mut k, pid, SYS_URING_SETUP, &[bad, pp]), -EINVAL as i64, "entries {bad}");
        }
        assert_eq!(sys(&mut k, pid, SYS_URING_SETUP, &[8, 0]), -EFAULT as i64);
        assert_eq!(sys(&mut k, pid, SYS_URING_SETUP, &[8, 0x9000]), -EFAULT as i64);
        let u = setup(&mut k, pid, 8);
        assert_eq!((u.p.sq_entries, u.p.cq_entries), (8, 16));
        assert_eq!(hdr(&u, SQ_MASK), 7);
        assert_eq!(hdr(&u, CQ_ENTRIES), 16);
        assert_eq!(u.p.sqes_off % 64, 0);
    }

    #[test]
    fn nop_and_write_read_through_ring() {
        let (mut k, pid) = kernel_with_proc();
        let u = setup(&mut k, pid, 4);
        let f = open(&mut k, pid, "/tmp/ring", O_CREAT | O_RDWR);
        let buf = umap(32);
        uwrite(buf, b"ring data");
        push(&u, &[sqe(URING_OP_NOP, 0, 0, 0, 0, 1), sqe(URING_OP_WRITE, f as i32, 0, buf, 9, 2)]);
        assert_eq!(sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 2, 2, 0]), 2);
        let c = cqes(&u);
        assert_eq!(c, [UringCqe { user_data: 1, res: 0, flags: 0 }, UringCqe { user_data: 2, res: 9, flags: 0 }]);
        let out = umap(32);
        push(&u, &[sqe(URING_OP_READ, f as i32, 5, out, 32, 3)]);
        assert_eq!(sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 1, 1, 0]), 1);
        assert_eq!(cqes(&u)[0].res, 4);
        assert_eq!(uread(out, 4), b"data");
        assert_eq!(hdr(&u, SQ_HEAD), 3);
    }

    #[test]
    fn ring_ops_use_the_same_rights_and_pointer_checks() {
        let (mut k, pid) = kernel_with_proc();
        let u = setup(&mut k, pid, 8);
        let f = open(&mut k, pid, "/etc/motd", O_RDONLY);
        sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[f as u64, CAP_READ]);
        let buf = umap(16);
        push(
            &u,
            &[
                sqe(URING_OP_READ, f as i32, 0, buf, 4, 1),                      // needs CAP_SEEK too
                sqe(URING_OP_READ, f as i32, URING_OFF_CURRENT, buf, 4, 2),       // ok
                sqe(URING_OP_WRITE, f as i32, URING_OFF_CURRENT, buf, 4, 3),      // ENOTCAPABLE
                sqe(URING_OP_READ, f as i32, URING_OFF_CURRENT, 0xffff_8000_0000_0000, 4, 4), // EFAULT
                sqe(URING_OP_READ, 50, URING_OFF_CURRENT, buf, 4, 5),             // EBADF
                sqe(URING_OP_READ, f as i32, URING_OFF_CURRENT, 0x7000, 4, 6),    // unmapped
            ],
        );
        sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 6, 0, 0]);
        let res: Vec<i32> = cqes(&u).iter().map(|c| c.res).collect();
        assert_eq!(res, [-ENOTCAPABLE, 4, -ENOTCAPABLE, -EFAULT, -EBADF, -EFAULT]);
    }

    #[test]
    fn bad_opcode_flags_and_index() {
        let (mut k, pid) = kernel_with_proc();
        let u = setup(&mut k, pid, 4);
        let mut s = sqe(URING_OP_NOP, 0, 0, 0, 0, 7);
        s.flags = 1;
        push(&u, &[sqe(99, 0, 0, 0, 0, 6), s]);
        sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 2, 0, 0]);
        let res: Vec<i32> = cqes(&u).iter().map(|c| c.res).collect();
        assert_eq!(res, [-EINVAL, -EINVAL]);
        // corrupt the index array: index 1000 is dropped, not executed
        push(&u, &[sqe(URING_OP_NOP, 0, 0, 0, 0, 8)]);
        let slot = (hdr(&u, SQ_TAIL) - 1) & 3;
        uwrite(u.p.ring_addr + u.p.sq_array_off + 4 * slot as u64, &1000u32.to_le_bytes());
        assert_eq!(sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 1, 0, 0]), 1);
        assert_eq!(hdr(&u, SQ_DROPPED), 1);
        assert!(cqes(&u).is_empty());
    }

    #[test]
    fn corrupted_tail_is_bounded() {
        let (mut k, pid) = kernel_with_proc();
        let u = setup(&mut k, pid, 4);
        set_hdr(&u, SQ_TAIL, 0xdead_beef);
        let n = sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 256, 0, 0]);
        assert!((0..=4).contains(&n), "consumed {n}");
        // a hostile cq_head cannot make the kernel write outside the ring
        set_hdr(&u, CQ_HEAD, 0x8000_0000);
        push(&u, &[sqe(URING_OP_NOP, 0, 0, 0, 0, 1)]);
        sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 1, 0, 0]);
    }

    #[test]
    fn cq_overflow_is_counted() {
        let (mut k, pid) = kernel_with_proc();
        let u = setup(&mut k, pid, 1); // cq holds 2
        for i in 0..3 {
            push(&u, &[sqe(URING_OP_NOP, 0, 0, 0, 0, i)]);
            sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 1, 0, 0]);
        }
        assert_eq!(hdr(&u, CQ_OVERFLOW), 1);
        assert_eq!(cqes(&u).len(), 2);
    }

    #[test]
    fn timeout_completes_from_timer_tick() {
        let (mut k, pid) = kernel_with_proc();
        let u = setup(&mut k, pid, 4);
        push(&u, &[sqe(URING_OP_TIMEOUT, 0, 10, 0, 0, 77)]);
        let r = call(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 1, 1, 0]);
        assert!(is_block(&r));
        advance_ms(9);
        k.timer_tick(kenv::now_ms());
        assert!(cqes(&u).is_empty());
        take_woken();
        advance_ms(1);
        k.timer_tick(kenv::now_ms());
        assert!(!take_woken().is_empty());
        assert_eq!(cqes(&u), [UringCqe { user_data: 77, res: -ETIME, flags: 0 }]);
        // full blocking call through the mock (sleep advances the clock)
        push(&u, &[sqe(URING_OP_TIMEOUT, 0, 5, 0, 0, 78)]);
        let mut c = Ctx::new(pid, -1);
        assert!(is_block(&call_ctx(&mut k, &mut c, SYS_URING_ENTER, &[u.fd as u64, 1, 1, 0])));
        advance_ms(5);
        k.timer_tick(kenv::now_ms());
        c.retry = 1;
        assert_eq!(ret_val(call_ctx(&mut k, &mut c, SYS_URING_ENTER, &[u.fd as u64, 1, 1, 0])), 1);
    }

    #[test]
    fn pending_timeouts_are_capped_at_ring_entries() {
        let (mut k, pid) = kernel_with_proc();
        let u = setup(&mut k, pid, 4);
        // 4 long timeouts fill the per-ring budget...
        let long: Vec<UringSqe> = (0..4).map(|i| sqe(URING_OP_TIMEOUT, 0, 86_400_000, 0, 0, i)).collect();
        push(&u, &long);
        assert_eq!(sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 4, 0, 0]), 4);
        assert!(cqes(&u).is_empty());
        assert_eq!(k.rings.values().next().unwrap().timeouts.len(), 4);
        // ...every further TIMEOUT completes at once with -EAGAIN, nothing queued
        for round in 0..3u64 {
            let more: Vec<UringSqe> = (0..4).map(|i| sqe(URING_OP_TIMEOUT, 0, 86_400_000, 0, 0, 100 + 4 * round + i)).collect();
            push(&u, &more);
            assert_eq!(sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 4, 0, 0]), 4);
            let res: Vec<i32> = cqes(&u).iter().map(|c| c.res).collect();
            assert_eq!(res, [-EAGAIN; 4]);
        }
        assert_eq!(k.rings.values().next().unwrap().timeouts.len(), 4);
        // other ops still work while the timeout budget is full
        push(&u, &[sqe(URING_OP_NOP, 0, 0, 0, 0, 9)]);
        assert_eq!(sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 1, 1, 0]), 1);
        assert_eq!(cqes(&u)[0].res, 0);
    }

    #[test]
    fn min_complete_without_inflight_does_not_hang() {
        let (mut k, pid) = kernel_with_proc();
        let u = setup(&mut k, pid, 4);
        assert_eq!(ret_val(call(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 0, 3, 0])), 0);
        assert_eq!(sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 0, 9, 0]), -EINVAL as i64);
    }

    #[test]
    fn enter_needs_cap_uring_and_a_ring_fd() {
        let (mut k, pid) = kernel_with_proc();
        let u = setup(&mut k, pid, 4);
        assert_eq!(sys(&mut k, pid, SYS_URING_ENTER, &[0, 0, 0, 0]), -EINVAL as i64);
        sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[u.fd as u64, CAP_READ]);
        assert_eq!(sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 0, 0, 0]), -ENOTCAPABLE as i64);
    }

    #[test]
    fn kill_switch() {
        let (mut k, pid) = kernel_with_proc();
        let u = setup(&mut k, pid, 4);
        k.ring_enable = false;
        let pp = umap(48);
        assert_eq!(sys(&mut k, pid, SYS_URING_SETUP, &[4, pp]), -EPERM as i64);
        assert_eq!(sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 0, 0, 0]), -EPERM as i64);
        k.ring_enable = true;
        assert_eq!(sys(&mut k, pid, SYS_URING_ENTER, &[u.fd as u64, 0, 0, 0]), 0);
    }

    #[test]
    fn jail_ring_permission() {
        let (mut k, pid) = kernel_with_proc();
        let pp = umap(48);
        let no = make_jail(&mut k, pid, "/tmp", "noring", 0, 0);
        let p = jailed_proc(&mut k, pid, no);
        assert_eq!(sys(&mut k, p, SYS_URING_SETUP, &[4, pp]), -EPERM as i64);
        let yes = try_jail(&mut k, pid, "/tmp", "ring", 0, 0, 100, JAIL_ALLOW_RING) as i32;
        let q = jailed_proc(&mut k, pid, yes);
        assert!(sys(&mut k, q, SYS_URING_SETUP, &[4, pp]) >= 0);
        // the ring region is charged to the jail
        assert!(kenv::with_acct(|a| a.get(yes).unwrap().mem_cur) >= 4096);
    }

    #[test]
    fn close_frees_region_and_charge() {
        let (mut k, pid) = kernel_with_proc();
        let u = setup(&mut k, pid, 8);
        assert!(kenv::with_acct(|a| a.get(0).unwrap().mem_cur) > 0);
        sys(&mut k, pid, SYS_CLOSE, &[u.fd as u64]);
        assert!(k.rings.is_empty());
        assert_eq!(kenv::with_acct(|a| a.get(0).unwrap().mem_cur), 0);
    }

    #[test]
    fn ring_memory_limit() {
        let (mut k, pid) = kernel_with_proc();
        let j = try_jail(&mut k, pid, "/tmp", "r", 4096, 0, 100, JAIL_ALLOW_RING) as i32;
        let p = jailed_proc(&mut k, pid, j);
        let pp = umap(48);
        assert_eq!(sys(&mut k, p, SYS_URING_SETUP, &[256, pp]), -ENOMEM as i64);
        assert!(sys(&mut k, p, SYS_URING_SETUP, &[4, pp]) >= 0);
    }
}
