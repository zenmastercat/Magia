// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! Helpers for host tests.

pub use crate::kenv::mock::*;
use crate::abi::*;
use crate::kernel::Kernel;
use crate::syscall::{Ctx, Ret};

/// A kernel with one ordinary process ("test", child of pid 0, jail 0).
pub fn kernel_with_proc() -> (Kernel, i32) {
    let mut k = Kernel::new(1);
    let pid = spawn(&mut k, 0, "test");
    (k, pid)
}

pub fn spawn(k: &mut Kernel, ppid: i32, name: &str) -> i32 {
    let tid = 100 + k.next_pid;
    let pid = k.spawn(ppid, tid, name.as_bytes()).expect("spawn");
    crate::kenv::with_sched(|s| s.add(tid, 1024));
    pid
}

fn args(a: &[u64]) -> [u64; 6] {
    let mut v = [0u64; 6];
    v[..a.len()].copy_from_slice(a);
    v
}

/// Full call, sleeping (= timing out) through the mock as needed.
pub fn sys(k: &mut Kernel, pid: i32, nr: u64, a: &[u64]) -> i64 {
    k.syscall_blocking(pid, nr, args(a))
}

/// One attempt; returns Block instead of sleeping.
pub fn call(k: &mut Kernel, pid: i32, nr: u64, a: &[u64]) -> Ret {
    let mut c = Ctx::new(pid, -1);
    k.syscall(&mut c, nr, &args(a))
}

pub fn call_ctx(k: &mut Kernel, c: &mut Ctx, nr: u64, a: &[u64]) -> Ret {
    k.syscall(c, nr, &args(a))
}

pub fn is_block(r: &Ret) -> bool {
    matches!(r, Ret::Block(_))
}

pub fn ret_val(r: Ret) -> i64 {
    match r {
        Ret::Val(v) => v,
        Ret::Block(_) => panic!("unexpected block"),
    }
}

pub fn open(k: &mut Kernel, pid: i32, path: &str, flags: u64) -> i64 {
    let p = ustr(path);
    sys(k, pid, SYS_OPENAT, &[AT_FDCWD as u64, p, flags])
}

pub fn openat(k: &mut Kernel, pid: i32, dirfd: i64, path: &str, flags: u64) -> i64 {
    let p = ustr(path);
    sys(k, pid, SYS_OPENAT, &[dirfd as u64, p, flags])
}

pub fn write_str(k: &mut Kernel, pid: i32, fd: i64, s: &str) -> i64 {
    let b = umap(s.len().max(1));
    uwrite(b, s.as_bytes());
    sys(k, pid, SYS_WRITE, &[fd as u64, b, s.len() as u64])
}

pub fn read_vec(k: &mut Kernel, pid: i32, fd: i64, len: usize) -> Result<std::vec::Vec<u8>, i64> {
    let b = umap(len.max(1));
    let n = sys(k, pid, SYS_READ, &[fd as u64, b, len as u64]);
    if n < 0 { Err(n) } else { Ok(uread(b, n as usize)) }
}

pub fn make_pipe(k: &mut Kernel, pid: i32) -> (i64, i64) {
    let fds = umap(8);
    assert_eq!(sys(k, pid, SYS_PIPE, &[fds]), 0);
    let v = uread(fds, 8);
    (
        i32::from_le_bytes(v[0..4].try_into().unwrap()) as i64,
        i32::from_le_bytes(v[4..8].try_into().unwrap()) as i64,
    )
}

/// jail_create from `pid`; returns the jid (asserting success).
pub fn make_jail(k: &mut Kernel, pid: i32, path: &str, host: &str, mem_max: u64, pids_max: u32) -> i32 {
    let r = try_jail(k, pid, path, host, mem_max, pids_max, 100, 0);
    assert!(r > 0, "jail_create failed: {r}");
    r as i32
}

#[allow(clippy::too_many_arguments)]
pub fn try_jail(k: &mut Kernel, pid: i32, path: &str, host: &str, mem_max: u64, pids_max: u32, cpu_weight: u32, flags: u32) -> i64 {
    try_jail_children(k, pid, path, host, mem_max, pids_max, cpu_weight, flags, 4)
}

#[allow(clippy::too_many_arguments)]
pub fn try_jail_children(k: &mut Kernel, pid: i32, path: &str, host: &str, mem_max: u64, pids_max: u32, cpu_weight: u32, flags: u32, children_max: u32) -> i64 {
    let jp = JailParams {
        path: ustr(path),
        hostname: ustr(host),
        mem_max,
        cpu_weight,
        children_max,
        pids_max,
        flags,
    };
    let u = uput(&jp);
    sys(k, pid, SYS_JAIL_CREATE, &[u])
}

/// Spawn a process from `parent` and move it into `jid`.
pub fn jailed_proc(k: &mut Kernel, parent: i32, jid: i32) -> i32 {
    let p = spawn(k, parent, "jailed");
    assert_eq!(sys(k, p, SYS_JAIL_ATTACH, &[jid as u64]), 0);
    p
}
