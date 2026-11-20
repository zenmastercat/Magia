// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! Capsicum-style capabilities. Idea: FreeBSD Capsicum (Watson et al.,
//! 2010), re-derived and simplified to one 64-bit rights mask per
//! descriptor.
//!
//! * `cap_enter` puts the process in capability mode for good: global
//!   namespaces (AT_FDCWD, absolute paths, process list, jails, hostname)
//!   are gone; only descriptors it already holds give access.
//! * `cap_rights_limit` can only shrink the rights of a descriptor.
//! * Each descriptor operation checks its right (see `Kernel::fd_get`).
//!   Rights checks in fs.rs, kqueue.rs and uring.rs use the same helper.

use crate::abi::*;
use crate::kernel::Kernel;
use crate::syscall::{Ctx, SysResult, val};
use crate::uaccess::*;

impl Kernel {
    pub fn sys_cap_enter(&mut self, c: &mut Ctx, _a: &[u64; 6]) -> SysResult {
        self.proc_mut(c.pid)?.cap_mode = true;
        val(0)
    }

    pub fn sys_cap_getmode(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let m = self.proc(c.pid)?.cap_mode as u32;
        copyout_val(a[0], &m)?;
        val(0)
    }

    pub fn sys_cap_rights_limit(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let rights = a[1];
        if rights & !CAP_ALL != 0 {
            return Err(EINVAL);
        }
        let e = self.fd_get(c.pid, a[0] as i64, 0)?;
        if rights & !e.rights != 0 {
            return Err(ENOTCAPABLE); // rights can never grow back
        }
        self.proc_mut(c.pid)?.fds[a[0] as usize].as_mut().unwrap().rights = rights;
        val(0)
    }

    pub fn sys_cap_rights_get(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let e = self.fd_get(c.pid, a[0] as i64, 0)?;
        copyout_val(a[1], &e.rights)?;
        val(0)
    }
}

#[cfg(test)]
mod tests {
    use crate::abi::*;
    use crate::testutil::*;

    fn enter(k: &mut crate::kernel::Kernel, pid: i32) {
        assert_eq!(sys(k, pid, SYS_CAP_ENTER, &[]), 0);
    }

    #[test]
    fn cap_enter_is_sticky_and_reported() {
        let (mut k, pid) = kernel_with_proc();
        let m = umap(4);
        sys(&mut k, pid, SYS_CAP_GETMODE, &[m]);
        assert_eq!(uget::<u32>(m), 0);
        enter(&mut k, pid);
        enter(&mut k, pid); // idempotent
        sys(&mut k, pid, SYS_CAP_GETMODE, &[m]);
        assert_eq!(uget::<u32>(m), 1);
    }

    #[test]
    fn rights_only_shrink() {
        let (mut k, pid) = kernel_with_proc();
        let f = open(&mut k, pid, "/etc/motd", O_RDONLY);
        assert_eq!(sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[f as u64, CAP_READ | CAP_SEEK]), 0);
        assert_eq!(sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[f as u64, CAP_READ | CAP_FSTAT]), -ENOTCAPABLE as i64);
        assert_eq!(sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[f as u64, CAP_READ]), 0);
        assert_eq!(sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[f as u64, CAP_ALL]), -ENOTCAPABLE as i64);
        assert_eq!(sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[f as u64, 1 << 40]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[40, CAP_READ]), -EBADF as i64);
    }

    #[test]
    fn each_operation_checks_its_right() {
        let (mut k, pid) = kernel_with_proc();
        let f = open(&mut k, pid, "/tmp/r", O_CREAT | O_RDWR);
        sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[f as u64, CAP_FSTAT]);
        assert_eq!(read_vec(&mut k, pid, f, 1), Err(-ENOTCAPABLE as i64));
        assert_eq!(write_str(&mut k, pid, f, "x"), -ENOTCAPABLE as i64);
        assert_eq!(sys(&mut k, pid, SYS_LSEEK, &[f as u64, 0, 0]), -ENOTCAPABLE as i64);
        let st = umap(32);
        assert_eq!(sys(&mut k, pid, SYS_FSTAT, &[f as u64, st]), 0);
        sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[f as u64, 0]);
        assert_eq!(sys(&mut k, pid, SYS_FSTAT, &[f as u64, st]), -ENOTCAPABLE as i64);
        // close and dup need no right
        assert!(sys(&mut k, pid, SYS_DUP, &[f as u64]) > 0);
        assert_eq!(sys(&mut k, pid, SYS_CLOSE, &[f as u64]), 0);
    }

    #[test]
    fn dirfd_rights_for_mkdir_unlink_getdents() {
        let (mut k, pid) = kernel_with_proc();
        let d = open(&mut k, pid, "/tmp", O_RDONLY | O_DIRECTORY);
        sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[d as u64, CAP_LOOKUP]);
        let n = ustr("sub");
        assert_eq!(sys(&mut k, pid, SYS_MKDIRAT, &[d as u64, n]), -ENOTCAPABLE as i64);
        assert_eq!(sys(&mut k, pid, SYS_UNLINKAT, &[d as u64, n, 0]), -ENOTCAPABLE as i64);
        let b = umap(64);
        assert_eq!(sys(&mut k, pid, SYS_GETDENTS, &[d as u64, b, 64]), -ENOTCAPABLE as i64);
        let d2 = open(&mut k, pid, "/tmp", O_RDONLY | O_DIRECTORY);
        sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[d2 as u64, CAP_MKDIRAT]);
        assert_eq!(sys(&mut k, pid, SYS_MKDIRAT, &[d2 as u64, n]), -ENOTCAPABLE as i64, "CAP_LOOKUP needed too");
    }

    #[test]
    fn capability_mode_blocks_global_namespaces() {
        let (mut k, pid) = kernel_with_proc();
        let d = open(&mut k, pid, "/tmp", O_RDONLY | O_DIRECTORY);
        enter(&mut k, pid);
        assert_eq!(open(&mut k, pid, "/etc/motd", O_RDONLY), -ECAPMODE as i64);
        assert_eq!(openat(&mut k, pid, d, "/etc/motd", O_RDONLY), -ENOTCAPABLE as i64);
        assert_eq!(openat(&mut k, pid, d, "../etc/motd", O_RDONLY), -ENOTCAPABLE as i64);
        let f = openat(&mut k, pid, d, "inside", O_CREAT | O_RDWR);
        assert!(f >= 0, "relative create beneath a dirfd is allowed: {f}");
        let j = umap(40);
        assert_eq!(sys(&mut k, pid, SYS_JAIL_CREATE, &[j]), -ECAPMODE as i64);
        assert_eq!(sys(&mut k, pid, SYS_JAIL_ATTACH, &[0]), -ECAPMODE as i64);
        let h = ustr("x");
        assert_eq!(sys(&mut k, pid, SYS_SETHOSTNAME, &[h, 1]), -ECAPMODE as i64);
        assert_eq!(sys(&mut k, pid, SYS_PSLIST, &[0, 0]), -ECAPMODE as i64);
        let d2 = ustr("dir");
        assert_eq!(sys(&mut k, pid, SYS_MKDIRAT, &[AT_FDCWD as u64, d2]), -ECAPMODE as i64);
        // still allowed
        assert_eq!(sys(&mut k, pid, SYS_GETPID, &[]), pid as i64);
        assert!(sys(&mut k, pid, SYS_KQUEUE, &[]) >= 0);
        let fds = umap(8);
        assert_eq!(sys(&mut k, pid, SYS_PIPE, &[fds]), 0);
        assert_eq!(sys(&mut k, pid, SYS_SETPRIORITY, &[0, 1]), 0);
        let other = spawn(&mut k, 0, "other");
        assert_eq!(sys(&mut k, pid, SYS_SETPRIORITY, &[other as u64, 1]), -ECAPMODE as i64);
        assert_eq!(sys(&mut k, pid, SYS_SCHED_SETWEIGHT, &[other as u64, 1]), -ECAPMODE as i64);
    }

    #[test]
    fn capability_mode_is_inherited_by_children() {
        let (mut k, pid) = kernel_with_proc();
        enter(&mut k, pid);
        let child = spawn(&mut k, pid, "c");
        assert!(k.proc(child).unwrap().cap_mode);
        assert_eq!(open(&mut k, child, "/etc/motd", O_RDONLY), -ECAPMODE as i64);
    }
}
