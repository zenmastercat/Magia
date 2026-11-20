// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! The system call table and the generic validation that runs before any
//! handler.
//!
//! Each entry declares its arguments with typed descriptors (fd, fd or
//! AT_FDCWD, integer range, flag mask, user pointer with a size taken from
//! a constant or another argument). `dispatch` checks, in order:
//! unknown number (ENOSYS), Capsicum capability mode (ECAPMODE), integer
//! ranges and flag masks (EINVAL), descriptor ranges (EBADF), user pointer
//! ranges (EFAULT). Handlers then copy user data in once and apply their
//! own policy. (Idea: FreeBSD's `sysent` table with per-call capability
//! annotations; Linux-style negative errno returns.)
//!
//! Blocking: a handler that must wait registers the caller on a wait key
//! and returns `Ret::Block(timeout_ms)`. The caller of `syscall()` sleeps
//! with GIANT dropped and calls again with `ctx.retry` incremented and
//! `ctx.timed_out` set when the sleep timed out. Handlers must therefore
//! be restartable: anything done before blocking is recorded in `Ctx`.

use crate::abi::*;
use crate::kenv;
use crate::kernel::Kernel;
use crate::uaccess::check_range;

pub enum Ret {
    Val(i64),
    /// Sleep, then call again. Timeout in ms, 0 = no timeout.
    Block(u64),
}

pub type SysResult = Result<Ret, Errno>;

pub fn val(v: i64) -> SysResult {
    Ok(Ret::Val(v))
}

/// Per-call state that survives sleeps.
#[derive(Debug, Default, Clone)]
pub struct Ctx {
    pub pid: i32,
    pub tid: i32,
    pub retry: u32,
    pub timed_out: bool,
    pub deadline_ms: Option<u64>,
    /// handler-defined progress (e.g. SQEs already consumed)
    pub saved: i64,
    pub phase: u32,
    /// sched_yield: the caller must yield after dropping GIANT
    pub yield_after: bool,
}

pub enum Timed {
    Expired,
    /// sleep this long (ms, 0 = forever)
    Wait(u64),
}

impl Ctx {
    pub fn new(pid: i32, tid: i32) -> Ctx {
        Ctx { pid, tid, ..Default::default() }
    }

    /// Track an overall deadline across retries. `timeout_ms` None means
    /// wait forever, Some(0) means do not wait at all.
    pub fn timed(&mut self, timeout_ms: Option<u64>) -> Timed {
        let Some(t) = timeout_ms else {
            return Timed::Wait(0);
        };
        let now = kenv::now_ms();
        let dl = *self.deadline_ms.get_or_insert(now.saturating_add(t));
        if self.timed_out || now >= dl {
            Timed::Expired
        } else {
            Timed::Wait(dl - now)
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub enum Sz {
    Fixed(u64),
    /// size in bytes is argument i
    Len(usize),
    /// size is argument i times a constant element size
    Mul(usize, u64),
    /// NUL-terminated string: only the start address is checked here
    Str,
}

#[derive(Clone, Copy, Debug)]
pub enum Arg {
    /// any value
    Any,
    /// signed integer range (inclusive)
    Int(i64, i64),
    /// file descriptor 0..NFDS
    Fd,
    /// file descriptor or AT_FDCWD
    FdCwd,
    /// bit mask: unknown bits -> EINVAL
    Flags(u64),
    /// user pointer, must be valid
    Ptr(Sz),
    /// user pointer, may be 0
    OptPtr(Sz),
}

pub type Handler = fn(&mut Kernel, &mut Ctx, &[u64; 6]) -> SysResult;

pub struct SysEnt {
    pub nr: u64,
    pub name: &'static str,
    pub args: &'static [Arg],
    /// allowed in Capsicum capability mode
    pub cap: bool,
    pub h: Handler,
}

use Arg::*;
use Sz::*;

const IO_MAX: i64 = 1 << 30;

pub static SYSCALLS: &[SysEnt] = &[
    SysEnt { nr: SYS_EXIT, name: "exit", args: &[Any], cap: true, h: Kernel::sys_exit },
    SysEnt { nr: SYS_GETPID, name: "getpid", args: &[], cap: true, h: Kernel::sys_getpid },
    SysEnt { nr: SYS_GETPPID, name: "getppid", args: &[], cap: true, h: Kernel::sys_getppid },
    SysEnt { nr: SYS_READ, name: "read", args: &[Fd, Ptr(Len(2)), Int(0, IO_MAX)], cap: true, h: Kernel::sys_read },
    SysEnt { nr: SYS_WRITE, name: "write", args: &[Fd, Ptr(Len(2)), Int(0, IO_MAX)], cap: true, h: Kernel::sys_write },
    SysEnt { nr: SYS_OPENAT, name: "openat", args: &[FdCwd, Ptr(Str), Flags(O_VALID)], cap: true, h: Kernel::sys_openat },
    SysEnt { nr: SYS_CLOSE, name: "close", args: &[Fd], cap: true, h: Kernel::sys_close },
    SysEnt { nr: SYS_LSEEK, name: "lseek", args: &[Fd, Any, Int(0, 2)], cap: true, h: Kernel::sys_lseek },
    SysEnt { nr: SYS_MKDIRAT, name: "mkdirat", args: &[FdCwd, Ptr(Str)], cap: true, h: Kernel::sys_mkdirat },
    SysEnt { nr: SYS_UNLINKAT, name: "unlinkat", args: &[FdCwd, Ptr(Str), Flags(AT_REMOVEDIR)], cap: true, h: Kernel::sys_unlinkat },
    SysEnt { nr: SYS_GETDENTS, name: "getdents", args: &[Fd, Ptr(Len(2)), Int(0, IO_MAX)], cap: true, h: Kernel::sys_getdents },
    SysEnt { nr: SYS_FSTAT, name: "fstat", args: &[Fd, Ptr(Fixed(32))], cap: true, h: Kernel::sys_fstat },
    SysEnt { nr: SYS_DUP, name: "dup", args: &[Fd], cap: true, h: Kernel::sys_dup },
    SysEnt { nr: SYS_PIPE, name: "pipe", args: &[Ptr(Fixed(8))], cap: true, h: Kernel::sys_pipe },
    SysEnt { nr: SYS_SCHED_YIELD, name: "sched_yield", args: &[], cap: true, h: Kernel::sys_sched_yield },
    SysEnt { nr: SYS_NANOSLEEP, name: "nanosleep", args: &[Int(0, i64::MAX)], cap: true, h: Kernel::sys_nanosleep },
    SysEnt { nr: SYS_SCHED_SETWEIGHT, name: "sched_setweight", args: &[Int(0, i32::MAX as i64), Int(1, 10000)], cap: true, h: Kernel::sys_sched_setweight },
    SysEnt { nr: SYS_SETPRIORITY, name: "setpriority", args: &[Int(0, i32::MAX as i64), Int(-20, 19)], cap: true, h: Kernel::sys_setpriority },
    SysEnt { nr: SYS_PROCINFO, name: "procinfo", args: &[Int(0, i32::MAX as i64), Ptr(Fixed(80))], cap: true, h: Kernel::sys_procinfo },
    SysEnt { nr: SYS_PSLIST, name: "pslist", args: &[OptPtr(Mul(1, 4)), Int(0, 4096)], cap: false, h: Kernel::sys_pslist },
    SysEnt { nr: SYS_WAITPID, name: "waitpid", args: &[Int(-1, i32::MAX as i64), OptPtr(Fixed(4)), Flags(WNOHANG)], cap: true, h: Kernel::sys_waitpid },
    SysEnt { nr: SYS_CAP_ENTER, name: "cap_enter", args: &[], cap: true, h: Kernel::sys_cap_enter },
    SysEnt { nr: SYS_CAP_GETMODE, name: "cap_getmode", args: &[Ptr(Fixed(4))], cap: true, h: Kernel::sys_cap_getmode },
    SysEnt { nr: SYS_CAP_RIGHTS_LIMIT, name: "cap_rights_limit", args: &[Fd, Any], cap: true, h: Kernel::sys_cap_rights_limit },
    SysEnt { nr: SYS_CAP_RIGHTS_GET, name: "cap_rights_get", args: &[Fd, Ptr(Fixed(8))], cap: true, h: Kernel::sys_cap_rights_get },
    SysEnt { nr: SYS_JAIL_CREATE, name: "jail_create", args: &[Ptr(Fixed(40))], cap: false, h: Kernel::sys_jail_create },
    SysEnt { nr: SYS_JAIL_ATTACH, name: "jail_attach", args: &[Int(0, i32::MAX as i64)], cap: false, h: Kernel::sys_jail_attach },
    SysEnt { nr: SYS_JAIL_GET, name: "jail_get", args: &[Int(0, i32::MAX as i64), Ptr(Fixed(240))], cap: false, h: Kernel::sys_jail_get },
    SysEnt { nr: SYS_GETHOSTNAME, name: "gethostname", args: &[Ptr(Len(1)), Int(1, 4096)], cap: true, h: Kernel::sys_gethostname },
    SysEnt { nr: SYS_SETHOSTNAME, name: "sethostname", args: &[Ptr(Len(1)), Int(0, 4096)], cap: false, h: Kernel::sys_sethostname },
    SysEnt { nr: SYS_JAIL_SET, name: "jail_set", args: &[Int(0, i32::MAX as i64), Int(1, 3), Any], cap: false, h: Kernel::sys_jail_set },
    SysEnt { nr: SYS_KQUEUE, name: "kqueue", args: &[], cap: true, h: Kernel::sys_kqueue },
    SysEnt { nr: SYS_KEVENT, name: "kevent", args: &[Fd, OptPtr(Mul(2, 32)), Int(0, KEVENT_MAX), OptPtr(Mul(4, 32)), Int(0, KEVENT_MAX), Int(-1, i64::MAX)], cap: true, h: Kernel::sys_kevent },
    SysEnt { nr: SYS_URING_SETUP, name: "uring_setup", args: &[Int(1, URING_MAX_ENTRIES as i64), Ptr(Fixed(48))], cap: true, h: Kernel::sys_uring_setup },
    SysEnt { nr: SYS_URING_ENTER, name: "uring_enter", args: &[Fd, Int(0, URING_MAX_ENTRIES as i64), Int(0, 2 * URING_MAX_ENTRIES as i64), Flags(0)], cap: true, h: Kernel::sys_uring_enter },
    SysEnt { nr: SYS_FUTEX, name: "futex", args: &[Ptr(Fixed(4)), Int(0, 1), Int(0, u32::MAX as i64), Int(0, i64::MAX)], cap: true, h: Kernel::sys_futex },
    SysEnt { nr: SYS_SYSCTL, name: "sysctl", args: &[Ptr(Len(1)), Int(1, 128), OptPtr(Fixed(0)), OptPtr(Fixed(8)), OptPtr(Len(5)), Int(0, 4096)], cap: true, h: Kernel::sys_sysctl },
    SysEnt { nr: SYS_UNAME, name: "uname", args: &[Ptr(Fixed(224))], cap: true, h: Kernel::sys_uname },
    SysEnt { nr: SYS_MMAP, name: "mmap", args: &[Any, Int(1, MMAP_MAX_BYTES as i64), Flags(PROT_READ | PROT_WRITE | PROT_EXEC)], cap: true, h: Kernel::sys_mmap },
    SysEnt { nr: SYS_MUNMAP, name: "munmap", args: &[Any, Int(1, MMAP_MAX_BYTES as i64)], cap: true, h: Kernel::sys_munmap },
];

pub fn lookup(nr: u64) -> Option<&'static SysEnt> {
    SYSCALLS.iter().find(|e| e.nr == nr)
}

/// Generic argument validation (see the module comment for the order).
pub fn validate(ent: &SysEnt, a: &[u64; 6]) -> KResult<()> {
    for (i, d) in ent.args.iter().enumerate() {
        let v = a[i];
        match *d {
            Int(lo, hi) => {
                if !(lo..=hi).contains(&(v as i64)) {
                    return Err(EINVAL);
                }
            }
            Flags(mask) => {
                if v & !mask != 0 {
                    return Err(EINVAL);
                }
            }
            _ => {}
        }
    }
    for (i, d) in ent.args.iter().enumerate() {
        let v = a[i] as i64;
        match *d {
            Fd if !(0..crate::kernel::NFDS as i64).contains(&v) => return Err(EBADF),
            FdCwd if v != AT_FDCWD && !(0..crate::kernel::NFDS as i64).contains(&v) => return Err(EBADF),
            _ => {}
        }
    }
    for (i, d) in ent.args.iter().enumerate() {
        let p = a[i];
        let (sz, opt) = match *d {
            Ptr(s) => (s, false),
            OptPtr(s) => (s, true),
            _ => continue,
        };
        if opt && p == 0 {
            continue;
        }
        let len = match sz {
            Fixed(n) => n,
            Len(j) => a[j],
            Mul(j, m) => a[j].checked_mul(m).ok_or(EFAULT)?,
            Str => 1,
        };
        if len == 0 && !opt && matches!(sz, Len(_) | Mul(..)) {
            continue; // zero-length buffer: pointer is not used
        }
        if p == 0 {
            return Err(EFAULT);
        }
        check_range(p, len.max(1))?;
    }
    Ok(())
}

impl Kernel {
    /// Run one attempt of system call `nr` for the process in `ctx`.
    pub fn syscall(&mut self, ctx: &mut Ctx, nr: u64, a: &[u64; 6]) -> Ret {
        if ctx.retry == 0 {
            self.syscalls += 1;
        }
        let r = match self.dispatch(ctx, nr, a) {
            Ok(r) => r,
            Err(e) => Ret::Val(-(e as i64)),
        };
        if let Ret::Val(v) = r {
            self.waits_remove_tid(ctx.tid);
            if self.trace {
                let name = lookup(nr).map_or("?", |e| e.name);
                if v < 0 {
                    crate::kprint!("[trace] pid {} {}({:#x}, {:#x}, {:#x}) = -{}\n", ctx.pid, name, a[0], a[1], a[2], errno_name(-v as i32));
                } else {
                    crate::kprint!("[trace] pid {} {}({:#x}, {:#x}, {:#x}) = {}\n", ctx.pid, name, a[0], a[1], a[2], v);
                }
            }
        }
        r
    }

    fn dispatch(&mut self, ctx: &mut Ctx, nr: u64, a: &[u64; 6]) -> SysResult {
        let ent = lookup(nr).ok_or(ENOSYS)?;
        let p = self.proc(ctx.pid)?;
        if p.zombie {
            return Err(ESRCH);
        }
        ctx.tid = p.tid;
        if p.cap_mode && !ent.cap {
            return Err(ECAPMODE);
        }
        validate(ent, a)?;
        (ent.h)(self, ctx, a)
    }

    /// Complete a call, sleeping through the kenv (host tests: the mock
    /// clock advances and every sleep times out).
    pub fn syscall_blocking(&mut self, pid: i32, nr: u64, a: [u64; 6]) -> i64 {
        let mut ctx = Ctx::new(pid, -1);
        loop {
            match self.syscall(&mut ctx, nr, &a) {
                Ret::Val(v) => return v,
                Ret::Block(t) => {
                    kenv::sleep_prepare(t);
                    ctx.timed_out = kenv::sleep_commit();
                    ctx.retry += 1;
                }
            }
        }
    }

    pub fn sys_getpid(&mut self, c: &mut Ctx, _a: &[u64; 6]) -> SysResult {
        val(c.pid as i64)
    }

    pub fn sys_getppid(&mut self, c: &mut Ctx, _a: &[u64; 6]) -> SysResult {
        val(self.proc(c.pid)?.ppid as i64)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::*;

    #[test]
    fn table_is_complete_and_unique() {
        let all = [
            SYS_EXIT, SYS_GETPID, SYS_GETPPID, SYS_READ, SYS_WRITE, SYS_OPENAT, SYS_CLOSE, SYS_LSEEK,
            SYS_MKDIRAT, SYS_UNLINKAT, SYS_GETDENTS, SYS_FSTAT, SYS_DUP, SYS_PIPE, SYS_SCHED_YIELD,
            SYS_NANOSLEEP, SYS_SCHED_SETWEIGHT, SYS_SETPRIORITY, SYS_PROCINFO, SYS_PSLIST, SYS_WAITPID,
            SYS_CAP_ENTER, SYS_CAP_GETMODE, SYS_CAP_RIGHTS_LIMIT, SYS_CAP_RIGHTS_GET, SYS_JAIL_CREATE,
            SYS_JAIL_ATTACH, SYS_JAIL_GET, SYS_GETHOSTNAME, SYS_SETHOSTNAME, SYS_JAIL_SET, SYS_KQUEUE,
            SYS_KEVENT, SYS_URING_SETUP, SYS_URING_ENTER, SYS_FUTEX, SYS_SYSCTL, SYS_UNAME, SYS_MMAP,
            SYS_MUNMAP,
        ];
        assert_eq!(SYSCALLS.len(), all.len());
        for nr in all {
            assert!(lookup(nr).is_some(), "missing {nr}");
            assert!(nr < SYS_MAX);
        }
        for (i, e) in SYSCALLS.iter().enumerate() {
            assert!(e.args.len() <= 6);
            for f in &SYSCALLS[i + 1..] {
                assert_ne!(e.nr, f.nr);
                assert_ne!(e.name, f.name);
            }
        }
    }

    #[test]
    fn unknown_numbers_are_enosys() {
        let (mut k, pid) = kernel_with_proc();
        for nr in [0, 15, 19, 29, 92, 1000, u64::MAX] {
            assert_eq!(sys(&mut k, pid, nr, &[]), -ENOSYS as i64, "nr {nr}");
        }
    }

    #[test]
    fn getpid_getppid() {
        let (mut k, pid) = kernel_with_proc();
        assert_eq!(sys(&mut k, pid, SYS_GETPID, &[]), pid as i64);
        assert_eq!(sys(&mut k, pid, SYS_GETPPID, &[]), 0);
    }

    #[test]
    fn fd_range_is_ebadf() {
        let (mut k, pid) = kernel_with_proc();
        let b = umap(16);
        assert_eq!(sys(&mut k, pid, SYS_READ, &[64, b, 1]), -EBADF as i64);
        assert_eq!(sys(&mut k, pid, SYS_READ, &[(-1i64) as u64, b, 1]), -EBADF as i64);
        assert_eq!(sys(&mut k, pid, SYS_READ, &[10, b, 1]), -EBADF as i64);
        assert_eq!(sys(&mut k, pid, SYS_CLOSE, &[63]), -EBADF as i64);
    }

    #[test]
    fn pointers_are_checked() {
        let (mut k, pid) = kernel_with_proc();
        // kernel address
        assert_eq!(sys(&mut k, pid, SYS_WRITE, &[1, 0xffff_ffff_8000_0000, 4]), -EFAULT as i64);
        // null
        assert_eq!(sys(&mut k, pid, SYS_WRITE, &[1, 0, 4]), -EFAULT as i64);
        // wraps
        assert_eq!(sys(&mut k, pid, SYS_FSTAT, &[1, u64::MAX - 8]), -EFAULT as i64);
        // straddles USER_TOP
        assert_eq!(sys(&mut k, pid, SYS_WRITE, &[1, USER_TOP - 2, 4]), -EFAULT as i64);
        // below USER_TOP but unmapped: copyin fails
        assert_eq!(sys(&mut k, pid, SYS_WRITE, &[1, 0x2000, 4]), -EFAULT as i64);
        // zero length with null pointer is fine
        assert_eq!(sys(&mut k, pid, SYS_WRITE, &[1, 0, 0]), 0);
    }

    #[test]
    fn ints_and_flags_are_einval() {
        let (mut k, pid) = kernel_with_proc();
        let b = umap(16);
        assert_eq!(sys(&mut k, pid, SYS_READ, &[0, b, (-1i64) as u64]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_LSEEK, &[0, 0, 3]), -EINVAL as i64);
        let p = ustr("/tmp/x");
        assert_eq!(sys(&mut k, pid, SYS_OPENAT, &[AT_FDCWD as u64, p, 0x4000_0000]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_UNLINKAT, &[AT_FDCWD as u64, p, 1]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_SETPRIORITY, &[0, 20]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_SETPRIORITY, &[0, (-21i64) as u64]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_URING_ENTER, &[3, 0, 0, 1]), -EINVAL as i64);
    }

    #[test]
    fn validation_order_einval_before_ebadf_before_efault() {
        let (mut k, pid) = kernel_with_proc();
        assert_eq!(sys(&mut k, pid, SYS_READ, &[99, 0, (-5i64) as u64]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_READ, &[99, 0, 5]), -EBADF as i64);
        assert_eq!(sys(&mut k, pid, SYS_READ, &[0, 0, 5]), -EFAULT as i64);
    }

    #[test]
    fn trace_prints_calls() {
        let (mut k, pid) = kernel_with_proc();
        k.trace = true;
        take_console();
        sys(&mut k, pid, SYS_GETPID, &[]);
        sys(&mut k, pid, SYS_CLOSE, &[50]);
        let out = take_console();
        assert!(out.contains("getpid(") && out.contains(&alloc::format!("= {pid}")), "{out}");
        assert!(out.contains("close(0x32") && out.contains("-EBADF"), "{out}");
    }

    #[test]
    fn zombie_and_unknown_pid() {
        let (mut k, pid) = kernel_with_proc();
        assert_eq!(sys(&mut k, 999, SYS_GETPID, &[]), -ESRCH as i64);
        let child = spawn(&mut k, pid, "child");
        sys(&mut k, child, SYS_EXIT, &[3]);
        assert_eq!(sys(&mut k, child, SYS_GETPID, &[]), -ESRCH as i64);
    }
}
