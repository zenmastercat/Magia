// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! The Rust kernel state: processes, jails, open files, VFS, pipes,
//! kqueues, rings, wait queues and the sysctl tree.
//!
//! One `Kernel` value holds everything. In the kernel it lives behind the
//! GIANT KLock (ffi.rs); host tests create a fresh one per test.

use crate::abi::*;
use crate::kenv;
use crate::kqueue::Kqueue;
use crate::pipe::Pipe;
use crate::sysctl::Sysctl;
use crate::uring::Ring;
use crate::vfs::{Ino, Vfs};
use alloc::collections::BTreeMap;
use alloc::vec::Vec;

pub const NFDS: usize = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FdEnt {
    pub file: u32,
    pub rights: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FileKind {
    Vnode(Ino),
    PipeR(u32),
    PipeW(u32),
    Kq(u32),
    Ring(u32),
}

/// An open file description (shared by dup'ed descriptors).
pub struct OpenFile {
    pub kind: FileKind,
    pub accmode: u64,
    pub append: bool,
    pub offset: u64,
    pub refs: u32,
}

impl OpenFile {
    pub fn readable(&self) -> bool {
        self.accmode == O_RDONLY || self.accmode == O_RDWR
    }
    pub fn writable(&self) -> bool {
        self.accmode == O_WRONLY || self.accmode == O_RDWR
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Mapping {
    pub uva: u64,
    pub npages: u64,
    /// The jail the pages were charged to (uncharged there on unmap even
    /// if the process moved to another jail meanwhile).
    pub jid: i32,
}

pub struct Proc {
    pub pid: i32,
    pub ppid: i32,
    pub tid: i32,
    pub jid: i32,
    pub name: Vec<u8>,
    pub zombie: bool,
    pub status: i32,
    pub nice: i32,
    pub weight: u32,
    pub cap_mode: bool,
    pub fds: [Option<FdEnt>; NFDS],
    pub root: Ino,
    pub cwd: Ino,
    pub maps: Vec<Mapping>,
}

pub struct Jail {
    pub jid: i32,
    pub parent: i32,
    pub depth: u32,
    pub hostname: Vec<u8>,
    pub path: Vec<u8>,
    pub root: Ino,
    pub cpu_weight: u32,
    pub mem_max: u64,
    pub pids_max: u32,
    /// processes in this jail and all descendants
    pub pids_cur: u32,
    pub children_max: u32,
    /// jails below this one (all levels)
    pub children_cur: u32,
    pub flags: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WaitKey {
    Pipe(u32),
    Kevent,
    Child(i32),
    Futex(u64),
    Ring(u32),
}

pub struct Kernel {
    pub procs: BTreeMap<i32, Proc>,
    pub next_pid: i32,
    pub jails: BTreeMap<i32, Jail>,
    pub next_jid: i32,
    pub vfs: Vfs,
    pub files: BTreeMap<u32, OpenFile>,
    pub next_id: u32,
    pub pipes: BTreeMap<u32, Pipe>,
    pub kqs: BTreeMap<u32, Kqueue>,
    pub rings: BTreeMap<u32, Ring>,
    /// (what, tid) in registration order (FIFO for futex wakeups)
    pub waits: Vec<(WaitKey, i32)>,
    pub sysctl: Sysctl,
    pub trace: bool,
    pub ring_enable: bool,
    /// Earliest kqueue timer / ring timeout in ms, u64::MAX if none.
    pub next_timer_ms: u64,
    pub syscalls: u64,
}

impl Kernel {
    /// Boot state: jail 0, process 0 ("kernel", thread `tid0`), the VFS
    /// and the sysctl tree.
    pub fn new(tid0: i32) -> Kernel {
        let vfs = Vfs::new();
        let root = vfs.root;
        let mut k = Kernel {
            procs: BTreeMap::new(),
            next_pid: 1,
            jails: BTreeMap::new(),
            next_jid: 1,
            vfs,
            files: BTreeMap::new(),
            next_id: 1,
            pipes: BTreeMap::new(),
            kqs: BTreeMap::new(),
            rings: BTreeMap::new(),
            waits: Vec::new(),
            sysctl: Sysctl::new(),
            trace: false,
            ring_enable: true,
            next_timer_ms: u64::MAX,
            syscalls: 0,
        };
        k.jails.insert(
            0,
            Jail {
                jid: 0,
                parent: -1,
                depth: 0,
                hostname: b"magia".to_vec(),
                path: b"/".to_vec(),
                root,
                cpu_weight: 100,
                mem_max: 0,
                pids_max: 0,
                pids_cur: 0,
                children_max: u32::MAX,
                children_cur: 0,
                flags: JAIL_ALLOW_SET_HOSTNAME | JAIL_ALLOW_RING,
            },
        );
        kenv::with_acct(|a| a.add_jail(0, -1, 0));
        k.insert_proc(0, -1, tid0, 0, b"kernel");
        k.jails.get_mut(&0).unwrap().pids_cur = 1;
        k.sysctl_init();
        k
    }

    pub fn new_id(&mut self) -> u32 {
        let id = self.next_id;
        self.next_id += 1;
        id
    }

    pub(crate) fn insert_proc(&mut self, pid: i32, ppid: i32, tid: i32, jid: i32, name: &[u8]) {
        let (root, cwd, cap) = match self.procs.get(&ppid) {
            Some(p) => (p.root, p.cwd, p.cap_mode),
            None => (self.vfs.root, self.vfs.root, false),
        };
        let mut p = Proc {
            pid,
            ppid,
            tid,
            jid,
            name: name[..name.len().min(PROC_NAME_MAX - 1)].to_vec(),
            zombie: false,
            status: 0,
            nice: 0,
            weight: crate::sched::NICE_0_WEIGHT as u32,
            cap_mode: cap,
            fds: [None; NFDS],
            root,
            cwd,
            maps: Vec::new(),
        };
        // fds 0, 1, 2 on /dev/console with all rights
        let console = self.vfs.console;
        for fd in 0..3 {
            let id = self.new_id();
            self.vfs.open_ref(console);
            self.files.insert(
                id,
                OpenFile { kind: FileKind::Vnode(console), accmode: O_RDWR, append: false, offset: 0, refs: 1 },
            );
            p.fds[fd] = Some(FdEnt { file: id, rights: CAP_ALL });
        }
        self.procs.insert(pid, p);
    }

    // ---------------------------------------------------------------- procs

    pub fn proc(&self, pid: i32) -> KResult<&Proc> {
        self.procs.get(&pid).ok_or(ESRCH)
    }

    pub fn proc_mut(&mut self, pid: i32) -> KResult<&mut Proc> {
        self.procs.get_mut(&pid).ok_or(ESRCH)
    }

    // ---------------------------------------------------------------- jails

    /// Is `anc` equal to `jid` or one of its ancestors?
    pub fn jail_contains(&self, anc: i32, jid: i32) -> bool {
        let mut j = jid;
        let mut guard = 0;
        while j >= 0 && guard <= JAIL_MAX_DEPTH + 1 {
            if j == anc {
                return true;
            }
            j = self.jails.get(&j).map_or(-1, |x| x.parent);
            guard += 1;
        }
        false
    }

    /// Process visibility: a process sees processes in its own jail and in
    /// jails below it (idea: FreeBSD jail process visibility, Linux PID
    /// namespaces).
    pub fn visible(&self, caller: i32, target: i32) -> bool {
        match (self.procs.get(&caller), self.procs.get(&target)) {
            (Some(c), Some(t)) => self.jail_contains(c.jid, t.jid),
            _ => false,
        }
    }

    /// Effective scheduler weight: the process weight scaled by the
    /// cpu_weight/100 of its jail and every ancestor jail (idea: Linux
    /// cgroup v2 cpu.weight, applied along the single jail hierarchy).
    pub fn effective_weight(&self, pid: i32) -> u32 {
        let Some(p) = self.procs.get(&pid) else { return 1024 };
        let mut w = p.weight as u64;
        let mut j = p.jid;
        while j > 0 {
            let Some(jl) = self.jails.get(&j) else { break };
            w = (w * jl.cpu_weight as u64 / 100).clamp(1, crate::sched::MAX_WEIGHT as u64);
            j = jl.parent;
        }
        w as u32
    }

    pub fn update_sched_weight(&self, pid: i32) {
        if let Some(p) = self.procs.get(&pid) {
            let w = self.effective_weight(pid);
            let tid = p.tid;
            kenv::with_sched(|s| s.set_weight(tid, w));
        }
    }

    // ---------------------------------------------------------------- files

    pub fn file_new(&mut self, kind: FileKind, accmode: u64) -> u32 {
        let id = self.new_id();
        if let FileKind::Vnode(ino) = kind {
            self.vfs.open_ref(ino);
        }
        self.files.insert(id, OpenFile { kind, accmode, append: false, offset: 0, refs: 0 });
        id
    }

    /// Install `file` in the lowest free descriptor of `pid`.
    pub fn fd_install(&mut self, pid: i32, file: u32, rights: u64) -> KResult<i32> {
        let p = self.procs.get_mut(&pid).ok_or(ESRCH)?;
        let slot = p.fds.iter().position(|f| f.is_none()).ok_or(EMFILE)?;
        p.fds[slot] = Some(FdEnt { file, rights });
        self.files.get_mut(&file).unwrap().refs += 1;
        Ok(slot as i32)
    }

    /// Install a freshly created file, releasing it if no fd is free.
    pub fn fd_install_new(&mut self, pid: i32, file: u32, rights: u64) -> KResult<i32> {
        match self.fd_install(pid, file, rights) {
            Ok(fd) => Ok(fd),
            Err(e) => {
                self.files.get_mut(&file).unwrap().refs = 1;
                self.file_unref(file);
                Err(e)
            }
        }
    }

    /// Look up `fd` and check that it holds all of `need` (Capsicum).
    pub fn fd_get(&self, pid: i32, fd: i64, need: u64) -> KResult<FdEnt> {
        if !(0..NFDS as i64).contains(&fd) {
            return Err(EBADF);
        }
        let e = self.proc(pid)?.fds[fd as usize].ok_or(EBADF)?;
        if e.rights & need != need {
            return Err(ENOTCAPABLE);
        }
        Ok(e)
    }

    pub fn fd_close(&mut self, pid: i32, fd: i64) -> KResult<()> {
        if !(0..NFDS as i64).contains(&fd) {
            return Err(EBADF);
        }
        let e = self.proc_mut(pid)?.fds[fd as usize].take().ok_or(EBADF)?;
        self.file_unref(e.file);
        Ok(())
    }

    pub fn file_unref(&mut self, id: u32) {
        let Some(f) = self.files.get_mut(&id) else { return };
        f.refs = f.refs.saturating_sub(1);
        if f.refs > 0 {
            return;
        }
        let f = self.files.remove(&id).unwrap();
        match f.kind {
            FileKind::Vnode(ino) => self.vfs.close_ref(ino),
            FileKind::PipeR(p) | FileKind::PipeW(p) => {
                if let Some(pp) = self.pipes.get_mut(&p) {
                    if matches!(f.kind, FileKind::PipeR(_)) {
                        pp.readers -= 1;
                    } else {
                        pp.writers -= 1;
                    }
                    if pp.readers == 0 && pp.writers == 0 {
                        self.pipes.remove(&p);
                    }
                }
                self.wake(WaitKey::Pipe(p));
                self.wake(WaitKey::Kevent);
            }
            FileKind::Kq(q) => {
                self.kqs.remove(&q);
            }
            FileKind::Ring(r) => self.ring_free(r),
        }
    }

    // ---------------------------------------------------------------- waits

    pub fn wait_on(&mut self, key: WaitKey, tid: i32) {
        if !self.waits.iter().any(|&(k, t)| k == key && t == tid) {
            self.waits.push((key, tid));
        }
    }

    pub fn is_waiting(&self, key: WaitKey, tid: i32) -> bool {
        self.waits.iter().any(|&(k, t)| k == key && t == tid)
    }

    /// Wake every thread waiting for `key`.
    pub fn wake(&mut self, key: WaitKey) -> usize {
        self.wake_n(key, usize::MAX)
    }

    /// Wake up to `n` waiters for `key`, oldest first.
    pub fn wake_n(&mut self, key: WaitKey, n: usize) -> usize {
        let mut woken = 0;
        let mut i = 0;
        while i < self.waits.len() && woken < n {
            if self.waits[i].0 == key {
                let (_, tid) = self.waits.remove(i);
                kenv::wake(tid);
                woken += 1;
            } else {
                i += 1;
            }
        }
        woken
    }

    pub fn waits_remove_tid(&mut self, tid: i32) {
        self.waits.retain(|&(_, t)| t != tid);
    }

    pub fn is_blocked(&self, tid: i32) -> bool {
        self.waits.iter().any(|&(_, t)| t == tid)
    }

    // ---------------------------------------------------------------- timers

    /// Called every timer tick (ms). Cheap when nothing is due.
    pub fn timer_tick(&mut self, now_ms: u64) {
        if now_ms < self.next_timer_ms {
            return;
        }
        let mut next = u64::MAX;
        let mut kq_fired = false;
        for kq in self.kqs.values_mut() {
            kq_fired |= kq.fire_timers(now_ms);
            next = next.min(kq.next_timer());
        }
        let ids: Vec<u32> = self.rings.keys().copied().collect();
        for id in ids {
            let fired = self.rings.get_mut(&id).unwrap().fire_timeouts(now_ms);
            if fired {
                self.wake(WaitKey::Ring(id));
            }
            next = next.min(self.rings[&id].next_timeout());
        }
        self.next_timer_ms = next;
        if kq_fired {
            self.wake(WaitKey::Kevent);
        }
    }

    pub fn arm_timer(&mut self, at_ms: u64) {
        self.next_timer_ms = self.next_timer_ms.min(at_ms);
    }
}
