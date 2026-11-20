// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! Process table: spawn, exit, wait, priorities, process listing, and
//! anonymous memory (mmap/munmap).
//!
//! A Magia process is one kernel thread plus the Rust-side state (fd
//! table, root/cwd, jail, capability mode, mappings). There is no fork and
//! no ELF loader: the C side creates the thread and calls `rs_proc_spawn`.

use crate::abi::*;
use crate::kenv;
use crate::kernel::{Kernel, Mapping, WaitKey};
use crate::sched::nice_to_weight;
use crate::syscall::{Ctx, Ret, SysResult, Timed, val};
use crate::uaccess::*;
use alloc::vec::Vec;

impl Kernel {
    /// New process `ppid` -> child running on thread `tid`. The child
    /// inherits jail, root, cwd and capability mode (a sandboxed process
    /// cannot escape by spawning) and gets a fresh fd table with 0-2 on
    /// /dev/console. Enforces pids_max along the jail hierarchy.
    pub fn spawn(&mut self, ppid: i32, tid: i32, name: &[u8]) -> KResult<i32> {
        let parent = self.proc(ppid)?;
        if parent.zombie {
            return Err(ESRCH);
        }
        let jid = parent.jid;
        self.pids_charge(jid)?;
        let pid = self.next_pid;
        self.next_pid += 1;
        self.insert_proc(pid, ppid, tid, jid, name);
        let w = self.effective_weight(pid);
        kenv::with_sched(|s| s.set_weight(tid, w));
        Ok(pid)
    }

    /// Count one more process in `jid` and every ancestor, or EAGAIN.
    pub fn pids_charge(&mut self, jid: i32) -> KResult<()> {
        let mut j = jid;
        while j >= 0 {
            let jl = &self.jails[&j];
            if jl.pids_max != 0 && jl.pids_cur >= jl.pids_max {
                return Err(EAGAIN);
            }
            j = jl.parent;
        }
        let mut j = jid;
        while j >= 0 {
            let jl = self.jails.get_mut(&j).unwrap();
            jl.pids_cur += 1;
            j = jl.parent;
        }
        Ok(())
    }

    pub fn pids_uncharge(&mut self, jid: i32) {
        let mut j = jid;
        while j >= 0 {
            let Some(jl) = self.jails.get_mut(&j) else { return };
            jl.pids_cur = jl.pids_cur.saturating_sub(1);
            j = jl.parent;
        }
    }

    /// Process exit: release everything, notify watchers, become a zombie.
    pub fn exit(&mut self, pid: i32, status: i32) -> KResult<()> {
        if pid == 0 {
            return Err(EPERM);
        }
        let p = self.proc_mut(pid)?;
        if p.zombie {
            return Err(ESRCH);
        }
        p.zombie = true;
        p.status = status;
        let tid = p.tid;
        let ppid = p.ppid;
        let fds: Vec<i64> = (0..p.fds.len()).filter(|&i| p.fds[i].is_some()).map(|i| i as i64).collect();
        let maps = core::mem::take(&mut p.maps);
        for fd in fds {
            let _ = self.fd_close(pid, fd);
        }
        for m in maps {
            let _ = kenv::uvm_free(m.jid, m.uva, m.npages);
        }
        self.waits_remove_tid(tid);
        // orphans go to process 0, which reaps zombies at once
        let kids: Vec<i32> = self.procs.values().filter(|c| c.ppid == pid).map(|c| c.pid).collect();
        for c in kids {
            self.procs.get_mut(&c).unwrap().ppid = 0;
            if self.procs[&c].zombie {
                self.reap(c);
            }
        }
        for kq in self.kqs.values_mut() {
            kq.proc_exited(pid, status);
        }
        self.wake(WaitKey::Kevent);
        self.wake(WaitKey::Child(ppid));
        if ppid == 0 {
            self.reap(pid);
        }
        kenv::thread_exit_pending(status);
        Ok(())
    }

    pub fn reap(&mut self, pid: i32) {
        if let Some(p) = self.procs.remove(&pid) {
            self.pids_uncharge(p.jid);
        }
    }

    pub fn sys_exit(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        self.exit(c.pid, a[0] as i32)?;
        val(0)
    }

    pub fn sys_sched_yield(&mut self, c: &mut Ctx, _a: &[u64; 6]) -> SysResult {
        let tid = c.tid;
        kenv::with_sched(|s| s.yield_(tid));
        c.yield_after = true;
        val(0)
    }

    pub fn sys_nanosleep(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let ms = a[0].div_ceil(1_000_000);
        if ms == 0 {
            return val(0);
        }
        match c.timed(Some(ms)) {
            Timed::Expired => val(0),
            Timed::Wait(t) => Ok(Ret::Block(t)),
        }
    }

    /// Target of setpriority/sched_setweight: 0 = caller; other processes
    /// must be visible; capability mode may only change itself.
    fn prio_target(&self, c: &Ctx, pid: i64) -> KResult<i32> {
        let pid = pid as i32;
        if pid == 0 || pid == c.pid {
            return Ok(c.pid);
        }
        if self.proc(c.pid)?.cap_mode {
            return Err(ECAPMODE);
        }
        if !self.visible(c.pid, pid) || self.procs[&pid].zombie {
            return Err(ESRCH);
        }
        Ok(pid)
    }

    /// Weight 1..10000 (1024 = nice 0). Inside a jail a process may only
    /// lower weights (FreeBSD: raising priority needs privilege).
    pub fn sys_sched_setweight(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let target = self.prio_target(c, a[0] as i64)?;
        let w = a[1] as u32;
        if self.proc(c.pid)?.jid != 0 && w > self.procs[&target].weight {
            return Err(EPERM);
        }
        self.proc_mut(target)?.weight = w;
        self.update_sched_weight(target);
        val(0)
    }

    pub fn sys_setpriority(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let target = self.prio_target(c, a[0] as i64)?;
        let nice = a[1] as i64 as i32;
        if self.proc(c.pid)?.jid != 0 && nice < self.procs[&target].nice {
            return Err(EPERM);
        }
        let p = self.proc_mut(target)?;
        p.nice = nice;
        p.weight = nice_to_weight(nice);
        self.update_sched_weight(target);
        val(0)
    }

    pub fn proc_mem_bytes(&self, pid: i32) -> u64 {
        let Some(p) = self.procs.get(&pid) else { return 0 };
        let maps: u64 = p.maps.iter().map(|m| m.npages * PAGE_SIZE).sum();
        let rings: u64 = self.rings.values().filter(|r| r.owner == pid).map(|r| r.npages * PAGE_SIZE).sum();
        maps + rings
    }

    pub fn proc_info(&self, pid: i32) -> KResult<ProcInfo> {
        let p = self.proc(pid)?;
        let tid = p.tid;
        let mut info = ProcInfo::zeroed();
        info.pid = p.pid;
        info.ppid = p.ppid;
        info.jid = p.jid;
        info.tid = p.tid;
        info.state = if p.zombie { 2 } else if self.is_blocked(tid) { 1 } else { 0 };
        info.nice = p.nice;
        info.weight = self.effective_weight(pid);
        info.cap_mode = p.cap_mode as u32;
        info.runtime_ns = kenv::with_sched(|s| s.runtime_ns(tid));
        info.mem_bytes = self.proc_mem_bytes(pid);
        put_cstr(&mut info.name, &p.name);
        Ok(info)
    }

    pub fn sys_procinfo(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let pid = if a[0] == 0 { c.pid } else { a[0] as i32 };
        if !self.visible(c.pid, pid) {
            return Err(ESRCH);
        }
        let info = self.proc_info(pid)?;
        copyout_val(a[1], &info)?;
        val(0)
    }

    pub fn visible_pids(&self, caller: i32) -> Vec<i32> {
        self.procs.keys().copied().filter(|&p| self.visible(caller, p)).collect()
    }

    pub fn sys_pslist(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let pids = self.visible_pids(c.pid);
        let n = pids.len().min(a[1] as usize);
        if n > 0 {
            if a[0] == 0 {
                return Err(EFAULT);
            }
            let mut buf = Vec::with_capacity(n * 4);
            for p in &pids[..n] {
                buf.extend_from_slice(&p.to_le_bytes());
            }
            copyout(a[0], &buf)?;
        }
        val(pids.len() as i64)
    }

    /// waitpid(pid or -1, status*, WNOHANG). The status is the raw exit
    /// value (no W* encoding).
    pub fn sys_waitpid(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let want = a[0] as i64 as i32;
        if want == 0 {
            return Err(EINVAL);
        }
        let me = c.pid;
        let kids: Vec<(i32, bool, i32)> = self
            .procs
            .values()
            .filter(|p| p.ppid == me && p.pid != me && (want == -1 || p.pid == want))
            .map(|p| (p.pid, p.zombie, p.status))
            .collect();
        if kids.is_empty() {
            return Err(ECHILD);
        }
        if let Some(&(pid, _, status)) = kids.iter().find(|k| k.1) {
            if a[1] != 0 {
                copyout_val(a[1], &status)?;
            }
            self.reap(pid);
            return val(pid as i64);
        }
        if a[2] & WNOHANG != 0 {
            return val(0);
        }
        self.wait_on(WaitKey::Child(me), c.tid);
        Ok(Ret::Block(0))
    }

    /// Anonymous memory: mmap(0, len, prot). PROT_EXEC is refused with
    /// EACCES (W^X: user code is only mapped by the kernel). All mappings
    /// are currently read-write; `prot` is validated but not narrowed.
    pub fn sys_mmap(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        if a[0] != 0 {
            return Err(EINVAL);
        }
        if a[2] & PROT_EXEC != 0 {
            return Err(EACCES);
        }
        let npages = a[1].div_ceil(PAGE_SIZE);
        let jid = self.proc(c.pid)?.jid;
        let (uva, _kva) = kenv::uvm_alloc(jid, npages, 0)?;
        self.proc_mut(c.pid)?.maps.push(Mapping { uva, npages, jid });
        val(uva as i64)
    }

    /// munmap must name exactly one earlier mapping.
    pub fn sys_munmap(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let npages = a[1].div_ceil(PAGE_SIZE);
        let p = self.proc_mut(c.pid)?;
        let i = p.maps.iter().position(|m| m.uva == a[0] && m.npages == npages).ok_or(EINVAL)?;
        let m = p.maps.remove(i);
        kenv::uvm_free(m.jid, m.uva, m.npages)?;
        val(0)
    }
}

#[cfg(test)]
mod tests {
    use crate::abi::*;
    use crate::testutil::*;

    #[test]
    fn spawn_inherits_and_gets_console_fds() {
        let (mut k, pid) = kernel_with_proc();
        let child = spawn(&mut k, pid, "c");
        let p = k.proc(child).unwrap();
        assert_eq!(p.ppid, pid);
        assert_eq!(p.jid, 0);
        assert!(p.fds[0].is_some() && p.fds[1].is_some() && p.fds[2].is_some() && p.fds[3].is_none());
        assert_eq!(k.spawn(999, 5, b"x"), Err(ESRCH));
    }

    #[test]
    fn exit_wait_status() {
        let (mut k, pid) = kernel_with_proc();
        let child = spawn(&mut k, pid, "c");
        let st = umap(4);
        assert_eq!(sys(&mut k, pid, SYS_WAITPID, &[child as u64, st, WNOHANG]), 0);
        assert_eq!(sys(&mut k, child, SYS_EXIT, &[7]), 0);
        assert_eq!(crate::kenv::mock::with(|m| m.exit_pending), Some(7));
        assert_eq!(sys(&mut k, pid, SYS_WAITPID, &[(-1i64) as u64, st, 0]), child as i64);
        assert_eq!(uget::<i32>(st), 7);
        assert_eq!(sys(&mut k, pid, SYS_WAITPID, &[(-1i64) as u64, 0, 0]), -ECHILD as i64);
        assert!(k.proc(child).is_err());
    }

    #[test]
    fn waitpid_blocks_then_wakes_on_exit() {
        let (mut k, pid) = kernel_with_proc();
        let child = spawn(&mut k, pid, "c");
        let r = call(&mut k, pid, SYS_WAITPID, &[(-1i64) as u64, 0, 0]);
        assert!(matches!(r, crate::syscall::Ret::Block(0)));
        take_woken();
        sys(&mut k, child, SYS_EXIT, &[0]);
        assert_eq!(take_woken(), [k.proc(pid).unwrap().tid]);
        assert_eq!(sys(&mut k, pid, SYS_WAITPID, &[(-1i64) as u64, 0, 0]), child as i64);
    }

    #[test]
    fn waitpid_errors() {
        let (mut k, pid) = kernel_with_proc();
        assert_eq!(sys(&mut k, pid, SYS_WAITPID, &[0, 0, 0]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_WAITPID, &[1, 0, 0]), -ECHILD as i64);
        assert_eq!(sys(&mut k, pid, SYS_WAITPID, &[(-1i64) as u64, 0, 2]), -EINVAL as i64);
    }

    #[test]
    fn children_of_exited_process_are_reparented() {
        let (mut k, pid) = kernel_with_proc();
        let child = spawn(&mut k, pid, "c");
        let grandchild = spawn(&mut k, child, "g");
        sys(&mut k, child, SYS_EXIT, &[0]);
        assert_eq!(k.proc(grandchild).unwrap().ppid, 0);
        sys(&mut k, grandchild, SYS_EXIT, &[0]);
        assert!(k.proc(grandchild).is_err(), "zombie of pid 0 reaped at once");
    }

    #[test]
    fn exit_closes_fds_and_frees_memory() {
        let (mut k, pid) = kernel_with_proc();
        let child = spawn(&mut k, pid, "c");
        let fds = umap(8);
        sys(&mut k, child, SYS_PIPE, &[fds]);
        let rfd = uget::<i32>(fds);
        let addr = sys(&mut k, child, SYS_MMAP, &[0, 8192, PROT_READ | PROT_WRITE]);
        assert!(addr > 0);
        let before = crate::kenv::with_acct(|a| a.get(0).unwrap().mem_cur);
        assert_eq!(before, 8192);
        let files_before = k.files.len();
        sys(&mut k, child, SYS_EXIT, &[0]);
        assert_eq!(crate::kenv::with_acct(|a| a.get(0).unwrap().mem_cur), 0);
        assert!(k.files.len() < files_before);
        assert!(k.pipes.is_empty());
        let _ = rfd;
    }

    #[test]
    fn exit_of_pid0_is_refused() {
        let mut k = crate::kernel::Kernel::new(1);
        assert_eq!(k.exit(0, 0), Err(EPERM));
    }

    #[test]
    fn mmap_rules() {
        let (mut k, pid) = kernel_with_proc();
        assert_eq!(sys(&mut k, pid, SYS_MMAP, &[0x1000, 4096, PROT_READ]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_MMAP, &[0, 0, PROT_READ]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_MMAP, &[0, MMAP_MAX_BYTES + 1, PROT_READ]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_MMAP, &[0, 4096, PROT_EXEC | PROT_READ]), -EACCES as i64);
        assert_eq!(sys(&mut k, pid, SYS_MMAP, &[0, 4096, 8]), -EINVAL as i64);
        let a = sys(&mut k, pid, SYS_MMAP, &[0, 100, PROT_READ | PROT_WRITE]) as u64;
        uwrite(a, b"data");
        assert_eq!(sys(&mut k, pid, SYS_MUNMAP, &[a, 8192]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_MUNMAP, &[a + 4096, 4096]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_MUNMAP, &[a, 4096]), 0);
        assert_eq!(sys(&mut k, pid, SYS_MUNMAP, &[a, 4096]), -EINVAL as i64);
    }

    #[test]
    fn procinfo_and_visibility() {
        let (mut k, pid) = kernel_with_proc();
        let buf = umap(80);
        assert_eq!(sys(&mut k, pid, SYS_PROCINFO, &[0, buf]), 0);
        let info: ProcInfo = uget(buf);
        assert_eq!(info.pid, pid);
        assert_eq!(cstr_bytes(&info.name), b"test");
        assert_eq!(info.weight, 1024);
        assert_eq!(sys(&mut k, pid, SYS_PROCINFO, &[9999, buf]), -ESRCH as i64);
        // a jailed process cannot see its parent's jail
        let jid = make_jail(&mut k, pid, "/tmp", "j1", 0, 0);
        let jailed = spawn(&mut k, pid, "jailed");
        assert_eq!(sys(&mut k, jailed, SYS_JAIL_ATTACH, &[jid as u64]), 0);
        assert_eq!(sys(&mut k, jailed, SYS_PROCINFO, &[pid as u64, buf]), -ESRCH as i64);
        assert_eq!(sys(&mut k, pid, SYS_PROCINFO, &[jailed as u64, buf]), 0);
    }

    #[test]
    fn pslist_counts_visible() {
        let (mut k, pid) = kernel_with_proc();
        let buf = umap(64);
        let n = sys(&mut k, pid, SYS_PSLIST, &[buf, 16]);
        assert_eq!(n, 2); // pid 0 and the test process
        assert_eq!(sys(&mut k, pid, SYS_PSLIST, &[0, 0]), 2);
        assert_eq!(sys(&mut k, pid, SYS_PSLIST, &[buf, 1]), 2);
        assert_eq!(uget::<i32>(buf), 0);
    }

    #[test]
    fn priorities() {
        let (mut k, pid) = kernel_with_proc();
        assert_eq!(sys(&mut k, pid, SYS_SETPRIORITY, &[0, 5]), 0);
        assert_eq!(k.proc(pid).unwrap().weight, nice_w(5));
        assert_eq!(sys(&mut k, pid, SYS_SCHED_SETWEIGHT, &[0, 2048]), 0);
        assert_eq!(crate::kenv::with_sched(|s| s.weight(k.proc(pid).unwrap().tid)), Some(2048));
        assert_eq!(sys(&mut k, pid, SYS_SCHED_SETWEIGHT, &[0, 0]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_SCHED_SETWEIGHT, &[0, 10001]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_SETPRIORITY, &[4242, 0]), -ESRCH as i64);
    }

    fn nice_w(n: i32) -> u32 {
        crate::sched::nice_to_weight(n)
    }

    #[test]
    fn jailed_process_cannot_raise_priority() {
        let (mut k, pid) = kernel_with_proc();
        let jid = make_jail(&mut k, pid, "/tmp", "j", 0, 0);
        let j = spawn(&mut k, pid, "j");
        sys(&mut k, j, SYS_JAIL_ATTACH, &[jid as u64]);
        assert_eq!(sys(&mut k, j, SYS_SETPRIORITY, &[0, 3]), 0);
        assert_eq!(sys(&mut k, j, SYS_SETPRIORITY, &[0, 1]), -EPERM as i64);
        assert_eq!(sys(&mut k, j, SYS_SCHED_SETWEIGHT, &[0, 5000]), -EPERM as i64);
        assert_eq!(sys(&mut k, j, SYS_SCHED_SETWEIGHT, &[0, 100]), 0);
    }

    #[test]
    fn nanosleep_times_out_and_zero_returns() {
        let (mut k, pid) = kernel_with_proc();
        assert_eq!(sys(&mut k, pid, SYS_NANOSLEEP, &[0]), 0);
        let t0 = crate::kenv::now_ms();
        assert_eq!(sys(&mut k, pid, SYS_NANOSLEEP, &[5_000_000]), 0);
        assert!(crate::kenv::now_ms() >= t0 + 5);
    }

    #[test]
    fn sched_yield_requests_yield() {
        let (mut k, pid) = kernel_with_proc();
        let mut c = crate::syscall::Ctx::new(pid, -1);
        k.syscall(&mut c, SYS_SCHED_YIELD, &[0; 6]);
        assert!(c.yield_after);
    }

    #[test]
    fn pids_max_is_hierarchical() {
        let (mut k, pid) = kernel_with_proc();
        let outer = make_jail(&mut k, pid, "/tmp", "o", 0, 2);
        let p1 = spawn(&mut k, pid, "p1");
        assert_eq!(sys(&mut k, p1, SYS_JAIL_ATTACH, &[outer as u64]), 0);
        let p2 = spawn(&mut k, p1, "p2"); // second process in outer
        assert!(p2 > 0);
        assert_eq!(k.spawn(p1, 900, b"p3"), Err(EAGAIN));
        sys(&mut k, p2, SYS_EXIT, &[0]);
        sys(&mut k, p1, SYS_WAITPID, &[p2 as u64, 0, 0]);
        assert!(k.spawn(p1, 901, b"p3").is_ok());
    }
}
