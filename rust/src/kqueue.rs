// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! kqueue/kevent. Idea: FreeBSD kqueue (Lemon, 2001), re-derived.
//!
//! A kqueue holds knotes keyed by (ident, filter). `kevent` first applies
//! a change list, then returns ready events. Readiness of descriptor
//! filters is computed on demand from the pipe/file state (level
//! triggered; EV_CLEAR makes a note report only when its data changed
//! since the last delivery). Timer and process notes are driven by
//! `fire_timers` (timer tick) and `proc_exited`.
//!
//! Waiting: every state change that could make a note ready wakes all
//! kevent sleepers (`WaitKey::Kevent`); they recheck. Simple and correct
//! for a uniprocessor kernel with few waiters.

use crate::abi::*;
use crate::kenv;
use crate::kernel::{FileKind, Kernel, WaitKey};
use crate::syscall::{Ctx, Ret, SysResult, Timed, val};
use crate::uaccess::*;
use alloc::vec::Vec;

#[derive(Clone, Debug)]
pub struct Knote {
    pub ident: u64,
    pub filter: i16,
    pub flags: u16,
    pub fflags: u32,
    pub udata: u64,
    pub enabled: bool,
    /// open file for READ/WRITE notes
    pub file: u32,
    /// TIMER: period, next expiry, pending expirations
    pub period_ms: u64,
    pub next_ms: u64,
    pub count: i64,
    /// USER: triggered; PROC: exited
    pub fired: bool,
    pub status: i64,
    /// EV_CLEAR on fd filters: data at the last delivery
    pub last: Option<i64>,
}

/// Most notes one kqueue may hold (security review F2): every note is kernel
/// heap that is not charged to a jail, so the count is bounded.
pub const KQ_NOTES_MAX: usize = 1024;

/// Largest ident accepted for EVFILT_TIMER and EVFILT_USER notes (these
/// idents name nothing in the kernel; keep them small, like descriptors).
pub const KQ_IDENT_MAX: u64 = i32::MAX as u64;

#[derive(Default)]
pub struct Kqueue {
    pub notes: Vec<Knote>,
}

impl Kqueue {
    pub fn find(&self, ident: u64, filter: i16) -> Option<usize> {
        self.notes.iter().position(|n| n.ident == ident && n.filter == filter)
    }

    /// Account timer expirations up to `now`. True if any fired.
    pub fn fire_timers(&mut self, now: u64) -> bool {
        let mut any = false;
        for n in self.notes.iter_mut().filter(|n| n.filter == EVFILT_TIMER && n.enabled) {
            if n.next_ms != u64::MAX && now >= n.next_ms {
                let k = if n.flags & EV_ONESHOT != 0 { 1 } else { (now - n.next_ms) / n.period_ms + 1 };
                n.count += k as i64;
                n.next_ms = if n.flags & EV_ONESHOT != 0 { u64::MAX } else { n.next_ms + k * n.period_ms };
                any = true;
            }
        }
        any
    }

    pub fn next_timer(&self) -> u64 {
        self.notes
            .iter()
            .filter(|n| n.filter == EVFILT_TIMER && n.enabled)
            .map(|n| n.next_ms)
            .min()
            .unwrap_or(u64::MAX)
    }

    pub fn proc_exited(&mut self, pid: i32, status: i32) {
        for n in self.notes.iter_mut().filter(|n| n.filter == EVFILT_PROC && n.ident == pid as u64) {
            n.fired = true;
            n.status = status as i64;
        }
    }
}

fn err_event(ch: &KEvent, e: Errno) -> KEvent {
    KEvent { flags: ch.flags | EV_ERROR, data: e as i64, ..*ch }
}

impl Kernel {
    pub fn sys_kqueue(&mut self, c: &mut Ctx, _a: &[u64; 6]) -> SysResult {
        let id = self.new_id();
        self.kqs.insert(id, Kqueue::default());
        let f = self.file_new(FileKind::Kq(id), O_RDWR);
        val(self.fd_install_new(c.pid, f, CAP_ALL)? as i64)
    }

    fn kq_of(&self, pid: i32, fd: i64, need: u64) -> KResult<u32> {
        let e = self.fd_get(pid, fd, need)?;
        match self.files[&e.file].kind {
            FileKind::Kq(q) => Ok(q),
            _ => Err(EBADF),
        }
    }

    /// Apply one change to kqueue `q` for process `pid`.
    pub fn kq_change(&mut self, pid: i32, q: u32, ch: &KEvent) -> KResult<()> {
        if ch.flags & !EV_CHANGE_VALID != 0 {
            return Err(EINVAL);
        }
        let idx = self.kqs[&q].find(ch.ident, ch.filter);
        if ch.flags & EV_DELETE != 0 {
            let i = idx.ok_or(ENOENT)?;
            self.kqs.get_mut(&q).unwrap().notes.remove(i);
            return Ok(());
        }
        if ch.flags & EV_ADD == 0 {
            // ENABLE/DISABLE/trigger on an existing note
            let i = idx.ok_or(ENOENT)?;
            let now = kenv::now_ms();
            let n = &mut self.kqs.get_mut(&q).unwrap().notes[i];
            if ch.flags & EV_DISABLE != 0 {
                n.enabled = false;
            }
            if ch.flags & EV_ENABLE != 0 {
                n.enabled = true;
                if n.filter == EVFILT_TIMER && n.next_ms == u64::MAX && n.flags & EV_ONESHOT == 0 {
                    n.next_ms = now + n.period_ms;
                }
            }
            if n.filter == EVFILT_USER && ch.fflags & NOTE_TRIGGER != 0 {
                n.fired = true;
            }
            let at = n.next_ms;
            if n.filter == EVFILT_TIMER {
                self.arm_timer(at);
            }
            self.wake(WaitKey::Kevent);
            return Ok(());
        }
        let mut note = Knote {
            ident: ch.ident,
            filter: ch.filter,
            flags: ch.flags & (EV_ONESHOT | EV_CLEAR),
            fflags: ch.fflags,
            udata: ch.udata,
            enabled: ch.flags & EV_DISABLE == 0,
            file: 0,
            period_ms: 0,
            next_ms: u64::MAX,
            count: 0,
            fired: false,
            status: 0,
            last: None,
        };
        match ch.filter {
            EVFILT_READ | EVFILT_WRITE => {
                if ch.ident > i32::MAX as u64 {
                    return Err(EBADF);
                }
                let e = self.fd_get(pid, ch.ident as i64, CAP_EVENT)?;
                match self.files[&e.file].kind {
                    FileKind::Vnode(_) | FileKind::PipeR(_) | FileKind::PipeW(_) => {}
                    _ => return Err(EINVAL),
                }
                note.file = e.file;
            }
            EVFILT_PROC => {
                if ch.fflags & !NOTE_EXIT != 0 {
                    return Err(EINVAL);
                }
                let target = ch.ident as i32;
                if ch.ident > i32::MAX as u64 || !self.visible(pid, target) {
                    return Err(ESRCH);
                }
                let t = &self.procs[&target];
                if t.zombie {
                    note.fired = true;
                    note.status = t.status as i64;
                }
            }
            EVFILT_TIMER => {
                if ch.ident > KQ_IDENT_MAX {
                    return Err(EINVAL);
                }
                if ch.data <= 0 || ch.data > 86_400_000 {
                    return Err(EINVAL);
                }
                note.period_ms = ch.data as u64;
                if note.enabled {
                    note.next_ms = kenv::now_ms() + note.period_ms;
                }
            }
            EVFILT_USER => {
                if ch.ident > KQ_IDENT_MAX {
                    return Err(EINVAL);
                }
                note.fired = ch.fflags & NOTE_TRIGGER != 0;
            }
            _ => return Err(EINVAL),
        }
        let at = note.next_ms;
        let kq = self.kqs.get_mut(&q).unwrap();
        match idx {
            Some(i) => {
                // re-adding updates the note (keeping a pending USER trigger)
                let old = &kq.notes[i];
                if note.filter == EVFILT_USER {
                    note.fired |= old.fired;
                }
                kq.notes[i] = note;
            }
            None => {
                if kq.notes.len() >= KQ_NOTES_MAX {
                    return Err(ENOMEM);
                }
                kq.notes.push(note)
            }
        }
        self.arm_timer(at);
        self.wake(WaitKey::Kevent);
        Ok(())
    }

    /// (ready, data, extra flags) of a descriptor note.
    fn fd_ready(&self, n: &Knote) -> Option<(bool, i64, u16)> {
        let f = self.files.get(&n.file)?;
        Some(match (n.filter, f.kind) {
            (EVFILT_READ, FileKind::PipeR(p)) => {
                let pp = self.pipes.get(&p)?;
                let eof = pp.writers == 0;
                (pp.avail() > 0 || eof, pp.avail() as i64, if eof { EV_EOF } else { 0 })
            }
            (EVFILT_WRITE, FileKind::PipeW(p)) => {
                let pp = self.pipes.get(&p)?;
                let eof = pp.readers == 0;
                (pp.space() > 0 || eof, pp.space() as i64, if eof { EV_EOF } else { 0 })
            }
            (EVFILT_READ, FileKind::Vnode(ino)) => {
                let left = self.vfs.size(ino).saturating_sub(f.offset) as i64;
                (left > 0, left, 0)
            }
            (EVFILT_WRITE, FileKind::Vnode(_)) => (true, 0, 0),
            _ => (false, 0, 0),
        })
    }

    /// Collect up to `max` ready events from kqueue `q`.
    pub fn kq_collect(&mut self, q: u32, max: usize) -> Vec<KEvent> {
        let mut out = Vec::new();
        let mut i = 0;
        while out.len() < max && i < self.kqs[&q].notes.len() {
            let n = self.kqs[&q].notes[i].clone();
            if !n.enabled {
                i += 1;
                continue;
            }
            let mut ev = KEvent { ident: n.ident, filter: n.filter, flags: 0, fflags: 0, data: 0, udata: n.udata };
            let mut remove = n.flags & EV_ONESHOT != 0;
            let ready = match n.filter {
                EVFILT_READ | EVFILT_WRITE => match self.fd_ready(&n) {
                    None => {
                        // descriptor closed: the note goes away
                        self.kqs.get_mut(&q).unwrap().notes.remove(i);
                        continue;
                    }
                    Some((r, data, fl)) => {
                        ev.data = data;
                        ev.flags |= fl;
                        let fresh = n.flags & EV_CLEAR == 0 || n.last != Some(data);
                        if r && fresh && n.flags & EV_CLEAR != 0 {
                            self.kqs.get_mut(&q).unwrap().notes[i].last = Some(data);
                        }
                        r && fresh
                    }
                },
                EVFILT_TIMER => {
                    ev.data = n.count;
                    if n.count > 0 {
                        self.kqs.get_mut(&q).unwrap().notes[i].count = 0;
                    }
                    n.count > 0
                }
                EVFILT_PROC => {
                    ev.data = n.status;
                    ev.fflags = NOTE_EXIT;
                    ev.flags |= EV_EOF;
                    remove |= n.fired;
                    n.fired
                }
                EVFILT_USER => {
                    ev.fflags = n.fflags & !NOTE_TRIGGER;
                    if n.fired && n.flags & EV_CLEAR != 0 {
                        self.kqs.get_mut(&q).unwrap().notes[i].fired = false;
                    }
                    n.fired
                }
                _ => false,
            };
            if ready {
                out.push(ev);
                if remove {
                    self.kqs.get_mut(&q).unwrap().notes.remove(i);
                    continue;
                }
            }
            i += 1;
        }
        out
    }

    /// kevent(kq, changes, nchanges, events, nevents, timeout_ms)
    pub fn sys_kevent(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let (nch, nev) = (a[2] as usize, a[4] as usize);
        let need = if nch > 0 { CAP_KQUEUE_CHANGE } else { 0 } | if nev > 0 { CAP_KQUEUE_EVENT } else { 0 };
        let q = self.kq_of(c.pid, a[0] as i64, need)?;
        let mut out: Vec<KEvent> = Vec::new();
        if c.phase == 0 {
            c.phase = 1; // changes are applied once, even if we sleep
            if nch > 0 {
                let raw = copyin_vec(a[1], nch * 32)?;
                for chunk in raw.chunks(32) {
                    let mut ch = KEvent::zeroed();
                    ch.as_bytes_mut().copy_from_slice(chunk);
                    let r = self.kq_change(c.pid, q, &ch);
                    match r {
                        Err(e) if out.len() < nev => out.push(err_event(&ch, e)),
                        Err(e) => return Err(e),
                        Ok(()) if ch.flags & EV_RECEIPT != 0 && out.len() < nev => out.push(err_event(&ch, 0)),
                        Ok(()) => {}
                    }
                }
            }
        }
        if out.is_empty() && nev > 0 {
            out = self.kq_collect(q, nev);
        }
        if !out.is_empty() || nev == 0 {
            let mut bytes = Vec::with_capacity(out.len() * 32);
            for e in &out {
                bytes.extend_from_slice(e.as_bytes());
            }
            copyout(a[3], &bytes)?;
            return val(out.len() as i64);
        }
        let timeout = a[5] as i64;
        match c.timed(if timeout < 0 { None } else { Some(timeout as u64) }) {
            Timed::Expired => val(0),
            Timed::Wait(t) => {
                self.wait_on(WaitKey::Kevent, c.tid);
                Ok(Ret::Block(t))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use crate::abi::*;
    use crate::kernel::Kernel;
    use crate::testutil::*;

    fn kev(ident: u64, filter: i16, flags: u16, fflags: u32, data: i64) -> KEvent {
        KEvent { ident, filter, flags, fflags, data, udata: 0xabc }
    }

    /// kevent with the given changes; returns (result, events).
    fn kevent(k: &mut Kernel, pid: i32, kq: i64, ch: &[KEvent], nev: usize, timeout: i64) -> (i64, Vec<KEvent>) {
        let cp = umap(32 * ch.len().max(1));
        for (i, c) in ch.iter().enumerate() {
            uwrite(cp + 32 * i as u64, c.as_bytes());
        }
        let ep = umap(32 * nev.max(1));
        let r = sys(k, pid, SYS_KEVENT, &[kq as u64, cp, ch.len() as u64, ep, nev as u64, timeout as u64]);
        let evs = (0..r.max(0) as u64).map(|i| uget::<KEvent>(ep + 32 * i)).collect();
        (r, evs)
    }

    #[test]
    fn pipe_read_readiness_and_eof() {
        let (mut k, pid) = kernel_with_proc();
        let kq = sys(&mut k, pid, SYS_KQUEUE, &[]);
        let (r, w) = make_pipe(&mut k, pid);
        let (n, _) = kevent(&mut k, pid, kq, &[kev(r as u64, EVFILT_READ, EV_ADD, 0, 0)], 4, 0);
        assert_eq!(n, 0, "empty pipe not readable");
        write_str(&mut k, pid, w, "abc");
        let (n, ev) = kevent(&mut k, pid, kq, &[], 4, 0);
        assert_eq!(n, 1);
        assert_eq!((ev[0].ident, ev[0].filter, ev[0].data, ev[0].udata), (r as u64, EVFILT_READ, 3, 0xabc));
        sys(&mut k, pid, SYS_CLOSE, &[w as u64]);
        let (_, ev) = kevent(&mut k, pid, kq, &[], 4, 0);
        assert!(ev[0].flags & EV_EOF != 0);
    }

    #[test]
    fn pipe_write_readiness() {
        let (mut k, pid) = kernel_with_proc();
        let kq = sys(&mut k, pid, SYS_KQUEUE, &[]);
        let (r, w) = make_pipe(&mut k, pid);
        let (n, ev) = kevent(&mut k, pid, kq, &[kev(w as u64, EVFILT_WRITE, EV_ADD, 0, 0)], 4, 0);
        assert_eq!((n, ev[0].data), (1, 4096));
        sys(&mut k, pid, SYS_CLOSE, &[r as u64]);
        let (_, ev) = kevent(&mut k, pid, kq, &[], 4, 0);
        assert!(ev[0].flags & EV_EOF != 0);
    }

    #[test]
    fn ev_clear_reports_changes_only() {
        let (mut k, pid) = kernel_with_proc();
        let kq = sys(&mut k, pid, SYS_KQUEUE, &[]);
        let (r, w) = make_pipe(&mut k, pid);
        write_str(&mut k, pid, w, "a");
        let (n, _) = kevent(&mut k, pid, kq, &[kev(r as u64, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0)], 4, 0);
        assert_eq!(n, 1);
        assert_eq!(kevent(&mut k, pid, kq, &[], 4, 0).0, 0);
        write_str(&mut k, pid, w, "b");
        assert_eq!(kevent(&mut k, pid, kq, &[], 4, 0).0, 1);
    }

    #[test]
    fn timer_periodic_and_oneshot() {
        let (mut k, pid) = kernel_with_proc();
        let kq = sys(&mut k, pid, SYS_KQUEUE, &[]);
        kevent(&mut k, pid, kq, &[kev(1, EVFILT_TIMER, EV_ADD, 0, 10), kev(2, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 5)], 0, 0);
        advance_ms(4);
        k.timer_tick(crate::kenv::now_ms());
        assert_eq!(kevent(&mut k, pid, kq, &[], 4, 0).0, 0);
        advance_ms(31);
        k.timer_tick(crate::kenv::now_ms());
        let (n, ev) = kevent(&mut k, pid, kq, &[], 4, 0);
        assert_eq!(n, 2);
        let t1 = ev.iter().find(|e| e.ident == 1).unwrap();
        assert_eq!(t1.data, 3, "35 ms / 10 ms = 3 expirations");
        assert_eq!(ev.iter().find(|e| e.ident == 2).unwrap().data, 1);
        assert!(k.kqs.values().next().unwrap().find(2, EVFILT_TIMER).is_none(), "oneshot removed");
    }

    #[test]
    fn kevent_blocks_until_timer_fires() {
        let (mut k, pid) = kernel_with_proc();
        let kq = sys(&mut k, pid, SYS_KQUEUE, &[]);
        kevent(&mut k, pid, kq, &[kev(9, EVFILT_TIMER, EV_ADD, 0, 20)], 0, 0);
        let ep = umap(32);
        let r = call(&mut k, pid, SYS_KEVENT, &[kq as u64, 0, 0, ep, 1, (-1i64) as u64]);
        assert!(is_block(&r));
        take_woken();
        advance_ms(20);
        k.timer_tick(crate::kenv::now_ms());
        assert!(!take_woken().is_empty(), "timer wakes kevent sleepers");
        assert_eq!(ret_val(call(&mut k, pid, SYS_KEVENT, &[kq as u64, 0, 0, ep, 1, (-1i64) as u64])), 1);
    }

    #[test]
    fn kevent_timeout_returns_zero() {
        let (mut k, pid) = kernel_with_proc();
        let kq = sys(&mut k, pid, SYS_KQUEUE, &[]);
        let (n, _) = kevent(&mut k, pid, kq, &[], 4, 15);
        assert_eq!(n, 0);
        assert!(crate::kenv::now_ms() >= 15);
    }

    #[test]
    fn user_events() {
        let (mut k, pid) = kernel_with_proc();
        let kq = sys(&mut k, pid, SYS_KQUEUE, &[]);
        kevent(&mut k, pid, kq, &[kev(5, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0)], 0, 0);
        assert_eq!(kevent(&mut k, pid, kq, &[], 4, 0).0, 0);
        kevent(&mut k, pid, kq, &[kev(5, EVFILT_USER, 0, NOTE_TRIGGER, 0)], 0, 0);
        let (n, ev) = kevent(&mut k, pid, kq, &[], 4, 0);
        assert_eq!((n, ev[0].ident, ev[0].filter), (1, 5, EVFILT_USER));
        assert_eq!(kevent(&mut k, pid, kq, &[], 4, 0).0, 0, "EV_CLEAR resets the trigger");
        // disable: trigger is kept but not reported
        kevent(&mut k, pid, kq, &[kev(5, EVFILT_USER, EV_DISABLE, NOTE_TRIGGER, 0)], 0, 0);
        assert_eq!(kevent(&mut k, pid, kq, &[], 4, 0).0, 0);
        kevent(&mut k, pid, kq, &[kev(5, EVFILT_USER, EV_ENABLE, 0, 0)], 0, 0);
        assert_eq!(kevent(&mut k, pid, kq, &[], 4, 0).0, 1);
    }

    #[test]
    fn proc_exit_note() {
        let (mut k, pid) = kernel_with_proc();
        let kq = sys(&mut k, pid, SYS_KQUEUE, &[]);
        let child = spawn(&mut k, pid, "c");
        kevent(&mut k, pid, kq, &[kev(child as u64, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0)], 0, 0);
        assert_eq!(kevent(&mut k, pid, kq, &[], 4, 0).0, 0);
        sys(&mut k, child, SYS_EXIT, &[42]);
        let (n, ev) = kevent(&mut k, pid, kq, &[], 4, 0);
        assert_eq!(n, 1);
        assert_eq!((ev[0].data, ev[0].fflags), (42, NOTE_EXIT));
        assert_eq!(kevent(&mut k, pid, kq, &[], 4, 0).0, 0, "delivered once");
    }

    #[test]
    fn change_errors_become_events_or_return() {
        let (mut k, pid) = kernel_with_proc();
        let kq = sys(&mut k, pid, SYS_KQUEUE, &[]);
        let bad = [
            kev(1, EVFILT_TIMER, EV_ADD, 0, 0),         // EINVAL: period 0
            kev(9999, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0), // ESRCH
            kev(40, EVFILT_READ, EV_ADD, 0, 0),         // EBADF
            kev(1, -99, EV_ADD, 0, 0),                  // EINVAL: filter
            kev(3, EVFILT_USER, EV_DELETE, 0, 0),       // ENOENT
            kev(1, EVFILT_USER, 0x1000 | EV_ADD, 0, 0), // EINVAL: flag
        ];
        let (n, ev) = kevent(&mut k, pid, kq, &bad, 8, 0);
        assert_eq!(n, 6);
        let errs: Vec<i64> = ev.iter().map(|e| e.data).collect();
        assert_eq!(errs, [EINVAL as i64, ESRCH as i64, EBADF as i64, EINVAL as i64, ENOENT as i64, EINVAL as i64]);
        assert!(ev.iter().all(|e| e.flags & EV_ERROR != 0));
        // no room for the error: returned directly
        let (n, _) = kevent(&mut k, pid, kq, &bad[..1], 0, 0);
        assert_eq!(n, -EINVAL as i64);
    }

    #[test]
    fn receipt_and_limits() {
        let (mut k, pid) = kernel_with_proc();
        let kq = sys(&mut k, pid, SYS_KQUEUE, &[]);
        let (n, ev) = kevent(&mut k, pid, kq, &[kev(1, EVFILT_USER, EV_ADD | EV_RECEIPT, 0, 0)], 2, 0);
        assert_eq!((n, ev[0].data), (1, 0));
        assert!(ev[0].flags & EV_ERROR != 0);
        let p = umap(32);
        assert_eq!(sys(&mut k, pid, SYS_KEVENT, &[kq as u64, p, 65, p, 0, 0]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_KEVENT, &[kq as u64, p, 0, p, 1, (-2i64) as u64]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_KEVENT, &[1, 0, 0, p, 1, 0]), -EBADF as i64, "not a kqueue");
        assert_eq!(sys(&mut k, pid, SYS_KEVENT, &[kq as u64, 0, 1, p, 1, 0]), -EFAULT as i64);
    }

    #[test]
    fn notes_are_capped_and_idents_validated() {
        let (mut k, pid) = kernel_with_proc();
        let kq = sys(&mut k, pid, SYS_KQUEUE, &[]);
        // fill the kqueue with USER notes, 64 changes per call
        let mut added = 0u64;
        while added < super::KQ_NOTES_MAX as u64 {
            let ch: Vec<KEvent> = (0..64).map(|i| kev(added + i, EVFILT_USER, EV_ADD, 0, 0)).collect();
            assert_eq!(kevent(&mut k, pid, kq, &ch, 0, 0).0, 0);
            added += 64;
        }
        assert_eq!(k.kqs.values().next().unwrap().notes.len(), super::KQ_NOTES_MAX);
        // one more distinct note: ENOMEM (reported as an EV_ERROR event)
        let (n, ev) = kevent(&mut k, pid, kq, &[kev(5000, EVFILT_TIMER, EV_ADD, 0, 10)], 1, 0);
        assert_eq!((n, ev[0].flags & EV_ERROR != 0, ev[0].data), (1, true, ENOMEM as i64));
        // updating an existing note still works at the cap
        assert_eq!(kevent(&mut k, pid, kq, &[kev(3, EVFILT_USER, EV_ADD, NOTE_TRIGGER, 0)], 0, 0).0, 0);
        // deleting frees a slot
        assert_eq!(kevent(&mut k, pid, kq, &[kev(3, EVFILT_USER, EV_DELETE, 0, 0)], 0, 0).0, 0);
        assert_eq!(kevent(&mut k, pid, kq, &[kev(5000, EVFILT_USER, EV_ADD, 0, 0)], 0, 0).0, 0);
        // idents for TIMER/USER must be small
        let kq2 = sys(&mut k, pid, SYS_KQUEUE, &[]);
        let big = super::KQ_IDENT_MAX + 1;
        for f in [EVFILT_USER, EVFILT_TIMER] {
            let (n, ev) = kevent(&mut k, pid, kq2, &[kev(big, f, EV_ADD, 0, 10)], 1, 0);
            assert_eq!((n, ev[0].data), (1, EINVAL as i64));
        }
    }

    #[test]
    fn kqueue_rights() {
        let (mut k, pid) = kernel_with_proc();
        let kq = sys(&mut k, pid, SYS_KQUEUE, &[]);
        let (r, _w) = make_pipe(&mut k, pid);
        sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[r as u64, CAP_READ]);
        let (n, ev) = kevent(&mut k, pid, kq, &[kev(r as u64, EVFILT_READ, EV_ADD, 0, 0)], 1, 0);
        assert_eq!((n, ev[0].data), (1, ENOTCAPABLE as i64), "CAP_EVENT needed on the fd");
        sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[kq as u64, CAP_KQUEUE_EVENT]);
        let (n, _) = kevent(&mut k, pid, kq, &[kev(1, EVFILT_USER, EV_ADD, 0, 0)], 1, 0);
        assert_eq!(n, -ENOTCAPABLE as i64);
        assert_eq!(kevent(&mut k, pid, kq, &[], 1, 0).0, 0);
    }

    #[test]
    fn closed_fd_note_disappears() {
        let (mut k, pid) = kernel_with_proc();
        let kq = sys(&mut k, pid, SYS_KQUEUE, &[]);
        let (r, w) = make_pipe(&mut k, pid);
        kevent(&mut k, pid, kq, &[kev(r as u64, EVFILT_READ, EV_ADD, 0, 0)], 0, 0);
        sys(&mut k, pid, SYS_CLOSE, &[r as u64]);
        write_str(&mut k, pid, w, "x");
        assert_eq!(kevent(&mut k, pid, kq, &[], 4, 0).0, 0);
        assert!(k.kqs.values().next().unwrap().notes.is_empty());
    }
}
