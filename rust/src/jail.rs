// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! Jails: FreeBSD jails (a file-system root, a hostname, process
//! visibility) with Linux namespace and cgroup ideas folded in.
//!
//! One hierarchy for everything (idea: cgroup v2's single unified
//! hierarchy): the jail tree *is* the resource-accounting tree. Memory
//! (acct.rs), process counts and CPU weight are all charged or scaled
//! along the same parent chain, so there are no separate controllers that
//! could disagree about where a process belongs.
//!
//! Rules:
//! * A jail is created as a child of the caller's jail; its limits may not
//!   exceed the parent's (0 = inherit).
//! * A process may attach only to its own jail or a descendant, never to
//!   an ancestor or sibling (no way out).
//! * Limits of a jail may only be changed from a strict ancestor; a
//!   jailed process cannot raise its own limits.
//! * Privileges (JAIL_ALLOW_*) are inherited only if the parent jail has
//!   them and the creator asks for them.

use crate::abi::*;
use crate::kenv;
use crate::kernel::{Jail, Kernel};
use crate::syscall::{Ctx, SysResult, val};
use crate::uaccess::*;

pub const DEFAULT_CPU_WEIGHT: u32 = 100;
pub const MAX_CPU_WEIGHT: u32 = 10000;

impl Kernel {
    pub fn jail(&self, jid: i32) -> KResult<&Jail> {
        self.jails.get(&jid).ok_or(ESRCH)
    }

    pub fn caller_jail(&self, pid: i32) -> KResult<i32> {
        Ok(self.proc(pid)?.jid)
    }

    pub fn sys_jail_create(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let jp: JailParams = copyin_val(a[0])?;
        let parent_jid = self.caller_jail(c.pid)?;
        if jp.flags & !JAIL_FLAGS_VALID != 0 {
            return Err(EINVAL);
        }
        let path = copyin_str(jp.path, JAIL_PATH_MAX)?;
        let host = copyin_str(jp.hostname, JAIL_HOSTNAME_MAX)?;
        let cpu_weight = if jp.cpu_weight == 0 { DEFAULT_CPU_WEIGHT } else { jp.cpu_weight };
        if !(1..=MAX_CPU_WEIGHT).contains(&cpu_weight) {
            return Err(EINVAL);
        }
        let parent = self.jail(parent_jid)?;
        let mem_max = Self::inherit_limit(jp.mem_max, parent.mem_max)?;
        let pids_max = Self::inherit_limit(jp.pids_max as u64, parent.pids_max as u64)? as u32;
        if parent.depth + 1 > JAIL_MAX_DEPTH {
            return Err(EAGAIN);
        }
        // children_max is enforced along the whole ancestor chain
        let mut j = parent_jid;
        while j >= 0 {
            let jl = &self.jails[&j];
            if jl.children_cur >= jl.children_max {
                return Err(EAGAIN);
            }
            j = jl.parent;
        }
        let flags = jp.flags & parent.flags;
        if jp.flags & !parent.flags != 0 {
            return Err(EPERM); // cannot grant a privilege the parent lacks
        }
        let p = self.proc(c.pid)?;
        let w = self.vfs.walk(p.root, p.cwd, &path, p.cap_mode)?;
        let root = w.target.ok_or(ENOENT)?;
        if !self.vfs.is_dir(root) {
            return Err(ENOTDIR);
        }
        let depth = parent.depth + 1;
        let jid = self.next_jid;
        self.next_jid += 1;
        let mut j = parent_jid;
        while j >= 0 {
            let jl = self.jails.get_mut(&j).unwrap();
            jl.children_cur += 1;
            j = jl.parent;
        }
        let abs = self.vfs.path_of(root);
        self.jails.insert(
            jid,
            Jail {
                jid,
                parent: parent_jid,
                depth,
                hostname: host,
                path: abs,
                root,
                cpu_weight,
                mem_max,
                pids_max,
                pids_cur: 0,
                children_max: jp.children_max,
                children_cur: 0,
                flags,
            },
        );
        kenv::with_acct(|ac| ac.add_jail(jid, parent_jid, mem_max));
        val(jid as i64)
    }

    /// 0 = inherit the parent's limit; a non-zero child limit must not
    /// exceed a non-zero parent limit.
    fn inherit_limit(child: u64, parent: u64) -> KResult<u64> {
        match (child, parent) {
            (0, p) => Ok(p),
            (c, 0) => Ok(c),
            (c, p) if c <= p => Ok(c),
            _ => Err(EINVAL),
        }
    }

    pub fn sys_jail_attach(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let jid = a[0] as i32;
        let cur = self.caller_jail(c.pid)?;
        let root = self.jail(jid)?.root;
        if !self.jail_contains(cur, jid) {
            return Err(EPERM);
        }
        if jid == cur {
            return val(0);
        }
        self.pids_charge(jid)?;
        self.pids_uncharge(cur);
        let p = self.proc_mut(c.pid)?;
        p.jid = jid;
        p.root = root;
        p.cwd = root;
        self.update_sched_weight(c.pid);
        val(0)
    }

    pub fn jail_info(&self, jid: i32) -> KResult<JailInfo> {
        let j = self.jail(jid)?;
        let mut info = JailInfo::zeroed();
        info.jid = j.jid;
        info.parent = j.parent;
        info.cpu_weight = j.cpu_weight;
        info.nprocs = j.pids_cur;
        info.mem_max = j.mem_max;
        info.mem_cur = kenv::with_acct(|a| a.get(jid).map_or(0, |x| x.mem_cur));
        info.children_max = j.children_max;
        info.children_cur = j.children_cur;
        info.pids_max = j.pids_max;
        info.depth = j.depth;
        put_cstr(&mut info.hostname, &j.hostname);
        put_cstr(&mut info.path, &j.path);
        Ok(info)
    }

    pub fn sys_jail_get(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let jid = a[0] as i32;
        let cur = self.caller_jail(c.pid)?;
        if !self.jails.contains_key(&jid) || !self.jail_contains(cur, jid) {
            return Err(ESRCH);
        }
        let info = self.jail_info(jid)?;
        copyout_val(a[1], &info)?;
        val(0)
    }

    pub fn sys_jail_set(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let jid = a[0] as i32;
        let cur = self.caller_jail(c.pid)?;
        if !self.jails.contains_key(&jid) || !self.jail_contains(cur, jid) {
            return Err(ESRCH);
        }
        if jid == cur {
            return Err(EPERM); // only an ancestor may change limits
        }
        let parent = self.jails[&jid].parent;
        let (pmem, ppids) = (self.jails[&parent].mem_max, self.jails[&parent].pids_max);
        let v = a[2];
        match a[1] {
            JAIL_SET_CPU_WEIGHT => {
                if !(1..=MAX_CPU_WEIGHT as u64).contains(&v) {
                    return Err(EINVAL);
                }
                self.jails.get_mut(&jid).unwrap().cpu_weight = v as u32;
                let pids: alloc::vec::Vec<i32> = self
                    .procs
                    .values()
                    .filter(|p| self.jail_contains(jid, p.jid))
                    .map(|p| p.pid)
                    .collect();
                for p in pids {
                    self.update_sched_weight(p);
                }
            }
            JAIL_SET_MEM_MAX => {
                let m = Self::inherit_limit(v, pmem)?;
                self.jails.get_mut(&jid).unwrap().mem_max = m;
                kenv::with_acct(|ac| ac.set_max(jid, m));
            }
            JAIL_SET_PIDS_MAX => {
                if v > u32::MAX as u64 {
                    return Err(EINVAL);
                }
                let m = Self::inherit_limit(v, ppids as u64)? as u32;
                self.jails.get_mut(&jid).unwrap().pids_max = m;
            }
            _ => return Err(EINVAL),
        }
        val(0)
    }

    pub fn sys_gethostname(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let jid = self.caller_jail(c.pid)?;
        let h = &self.jail(jid)?.hostname;
        if h.len() + 1 > a[1] as usize {
            return Err(ENAMETOOLONG);
        }
        let mut buf = h.clone();
        buf.push(0);
        copyout(a[0], &buf)?;
        val(0)
    }

    pub fn set_hostname(&mut self, pid: i32, name: &[u8]) -> KResult<()> {
        let jid = self.caller_jail(pid)?;
        if name.len() >= JAIL_HOSTNAME_MAX {
            return Err(ENAMETOOLONG);
        }
        if name.contains(&0) {
            return Err(EINVAL);
        }
        let j = self.jails.get_mut(&jid).ok_or(ESRCH)?;
        if jid != 0 && j.flags & JAIL_ALLOW_SET_HOSTNAME == 0 {
            return Err(EPERM);
        }
        j.hostname = name.to_vec();
        Ok(())
    }

    /// sethostname(buf, len): `len` bytes, no NUL needed.
    pub fn sys_sethostname(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        if a[1] as usize >= JAIL_HOSTNAME_MAX {
            return Err(ENAMETOOLONG);
        }
        let name = copyin_vec(a[0], a[1] as usize)?;
        self.set_hostname(c.pid, &name)?;
        val(0)
    }

    pub fn sys_uname(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let jid = self.caller_jail(c.pid)?;
        let mut u = Utsname::zeroed();
        put_cstr(&mut u.sysname, b"Magia");
        put_cstr(&mut u.nodename, &self.jail(jid)?.hostname);
        put_cstr(&mut u.release, crate::RELEASE.as_bytes());
        put_cstr(&mut u.version, crate::VERSION.as_bytes());
        put_cstr(&mut u.machine, b"x86_64");
        copyout_val(a[0], &u)?;
        val(0)
    }
}

#[cfg(test)]
mod tests {
    use crate::abi::*;
    use crate::testutil::*;

    #[test]
    fn create_and_get() {
        let (mut k, pid) = kernel_with_proc();
        let jid = make_jail(&mut k, pid, "/tmp", "box", 1 << 20, 5);
        assert_eq!(jid, 1);
        let b = umap(240);
        assert_eq!(sys(&mut k, pid, SYS_JAIL_GET, &[jid as u64, b]), 0);
        let info: JailInfo = uget(b);
        assert_eq!(info.parent, 0);
        assert_eq!(info.depth, 1);
        assert_eq!(info.cpu_weight, 100);
        assert_eq!(info.mem_max, 1 << 20);
        assert_eq!(info.pids_max, 5);
        assert_eq!(cstr_bytes(&info.hostname), b"box");
        assert_eq!(cstr_bytes(&info.path), b"/tmp");
        assert_eq!(sys(&mut k, pid, SYS_JAIL_GET, &[77, b]), -ESRCH as i64);
    }

    #[test]
    fn create_validation() {
        let (mut k, pid) = kernel_with_proc();
        assert_eq!(try_jail(&mut k, pid, "/nope", "h", 0, 0, 100, 0), -ENOENT as i64);
        assert_eq!(try_jail(&mut k, pid, "/etc/motd", "h", 0, 0, 100, 0), -ENOTDIR as i64);
        let long = "h".repeat(64);
        assert_eq!(try_jail(&mut k, pid, "/tmp", &long, 0, 0, 100, 0), -ENAMETOOLONG as i64);
        assert_eq!(try_jail(&mut k, pid, "/tmp", "h", 0, 0, 10001, 0), -EINVAL as i64);
        assert_eq!(try_jail(&mut k, pid, "/tmp", "h", 0, 0, 100, 0x80), -EINVAL as i64);
        assert!(try_jail(&mut k, pid, "/tmp", &"h".repeat(63), 0, 0, 0, 0) > 0);
        assert_eq!(sys(&mut k, pid, SYS_JAIL_CREATE, &[0x7000]), -EFAULT as i64);
    }

    #[test]
    fn child_limits_cannot_exceed_parent() {
        let (mut k, pid) = kernel_with_proc();
        let outer = make_jail(&mut k, pid, "/tmp", "o", 1 << 20, 10);
        let p = jailed_proc(&mut k, pid, outer);
        assert_eq!(try_jail(&mut k, p, "/", "i", 2 << 20, 0, 100, 0), -EINVAL as i64);
        assert_eq!(try_jail(&mut k, p, "/", "i", 0, 11, 100, 0), -EINVAL as i64);
        let inner = try_jail(&mut k, p, "/", "i", 0, 0, 100, 0) as i32;
        assert!(inner > 0);
        assert_eq!(k.jails[&inner].mem_max, 1 << 20, "0 inherits");
        assert_eq!(k.jails[&inner].pids_max, 10);
        assert_eq!(k.jails[&inner].depth, 2);
    }

    #[test]
    fn jailed_root_is_the_jail_path() {
        let (mut k, pid) = kernel_with_proc();
        let d = ustr("/tmp/jroot");
        sys(&mut k, pid, SYS_MKDIRAT, &[AT_FDCWD as u64, d]);
        let f = open(&mut k, pid, "/tmp/jroot/inside", O_CREAT | O_RDWR);
        write_str(&mut k, pid, f, "jailed data");
        let jid = make_jail(&mut k, pid, "/tmp/jroot", "j", 0, 0);
        let p = jailed_proc(&mut k, pid, jid);
        let fd = open(&mut k, p, "/inside", O_RDONLY);
        assert_eq!(read_vec(&mut k, p, fd, 64).unwrap(), b"jailed data");
        assert_eq!(open(&mut k, p, "/etc/motd", O_RDONLY), -ENOENT as i64);
        assert_eq!(open(&mut k, p, "/../../etc/motd", O_RDONLY), -ENOENT as i64, ".. stays at jail root");
        assert!(open(&mut k, p, "../inside", O_RDONLY) >= 0);
    }

    #[test]
    fn attach_rules() {
        let (mut k, pid) = kernel_with_proc();
        let a = make_jail(&mut k, pid, "/tmp", "a", 0, 0);
        let b = make_jail(&mut k, pid, "/tmp", "b", 0, 0);
        let p = jailed_proc(&mut k, pid, a);
        assert_eq!(sys(&mut k, p, SYS_JAIL_ATTACH, &[0]), -EPERM as i64, "no way back up");
        assert_eq!(sys(&mut k, p, SYS_JAIL_ATTACH, &[b as u64]), -EPERM as i64, "no sideways");
        assert_eq!(sys(&mut k, p, SYS_JAIL_ATTACH, &[99]), -ESRCH as i64);
        assert_eq!(sys(&mut k, p, SYS_JAIL_ATTACH, &[a as u64]), 0, "own jail is a no-op");
        assert_eq!(k.jails[&a].pids_cur, 1);
        assert_eq!(k.jails[&0].pids_cur, 3);
    }

    #[test]
    fn jail_get_visibility() {
        let (mut k, pid) = kernel_with_proc();
        let a = make_jail(&mut k, pid, "/tmp", "a", 0, 0);
        let b = make_jail(&mut k, pid, "/tmp", "b", 0, 0);
        let p = jailed_proc(&mut k, pid, a);
        let buf = umap(240);
        assert_eq!(sys(&mut k, p, SYS_JAIL_GET, &[a as u64, buf]), 0);
        assert_eq!(sys(&mut k, p, SYS_JAIL_GET, &[b as u64, buf]), -ESRCH as i64);
        assert_eq!(sys(&mut k, p, SYS_JAIL_GET, &[0, buf]), -ESRCH as i64);
    }

    #[test]
    fn jail_set_only_from_strict_ancestor() {
        let (mut k, pid) = kernel_with_proc();
        let a = make_jail(&mut k, pid, "/tmp", "a", 1 << 20, 0);
        let p = jailed_proc(&mut k, pid, a);
        assert_eq!(sys(&mut k, p, SYS_JAIL_SET, &[a as u64, JAIL_SET_MEM_MAX, 1 << 30]), -EPERM as i64);
        assert_eq!(sys(&mut k, pid, SYS_JAIL_SET, &[a as u64, JAIL_SET_MEM_MAX, 1 << 30]), 0);
        assert_eq!(sys(&mut k, pid, SYS_JAIL_SET, &[a as u64, JAIL_SET_CPU_WEIGHT, 0]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_JAIL_SET, &[a as u64, JAIL_SET_CPU_WEIGHT, 200]), 0);
        assert_eq!(sys(&mut k, pid, SYS_JAIL_SET, &[a as u64, 4, 1]), -EINVAL as i64);
        assert_eq!(sys(&mut k, pid, SYS_JAIL_SET, &[0, JAIL_SET_PIDS_MAX, 1]), -EPERM as i64);
        assert_eq!(sys(&mut k, pid, SYS_JAIL_SET, &[42, JAIL_SET_PIDS_MAX, 1]), -ESRCH as i64);
        // inner jail limited by outer
        let inner = try_jail(&mut k, p, "/", "i", 0, 0, 100, 0) as i32;
        sys(&mut k, pid, SYS_JAIL_SET, &[a as u64, JAIL_SET_PIDS_MAX, 4]);
        assert_eq!(sys(&mut k, p, SYS_JAIL_SET, &[inner as u64, JAIL_SET_PIDS_MAX, 5]), -EINVAL as i64);
        assert_eq!(sys(&mut k, p, SYS_JAIL_SET, &[inner as u64, JAIL_SET_PIDS_MAX, 3]), 0);
    }

    #[test]
    fn cpu_weight_scales_scheduler_weight_hierarchically() {
        let (mut k, pid) = kernel_with_proc();
        let a = try_jail(&mut k, pid, "/tmp", "a", 0, 0, 200, 0) as i32;
        let p = jailed_proc(&mut k, pid, a);
        assert_eq!(k.effective_weight(p), 2048);
        let tid = k.proc(p).unwrap().tid;
        assert_eq!(crate::kenv::with_sched(|s| s.weight(tid)), Some(2048));
        let inner = try_jail(&mut k, p, "/", "i", 0, 0, 50, 0) as i32;
        let q = jailed_proc(&mut k, p, inner);
        assert_eq!(k.effective_weight(q), 1024);
        sys(&mut k, pid, SYS_JAIL_SET, &[a as u64, JAIL_SET_CPU_WEIGHT, 100]);
        assert_eq!(k.effective_weight(q), 512);
    }

    #[test]
    fn mem_limit_hits_mmap() {
        let (mut k, pid) = kernel_with_proc();
        let a = make_jail(&mut k, pid, "/tmp", "a", 3 * 4096, 0);
        let p = jailed_proc(&mut k, pid, a);
        assert!(sys(&mut k, p, SYS_MMAP, &[0, 8192, PROT_READ | PROT_WRITE]) > 0);
        assert_eq!(sys(&mut k, p, SYS_MMAP, &[0, 8192, PROT_READ | PROT_WRITE]), -ENOMEM as i64);
        let one = sys(&mut k, p, SYS_MMAP, &[0, 4096, PROT_READ | PROT_WRITE]);
        assert!(one > 0);
        let b = umap(240);
        sys(&mut k, pid, SYS_JAIL_GET, &[a as u64, b]);
        assert_eq!(uget::<JailInfo>(b).mem_cur, 3 * 4096);
        assert_eq!(sys(&mut k, p, SYS_MUNMAP, &[one as u64, 4096]), 0);
        sys(&mut k, pid, SYS_JAIL_GET, &[a as u64, b]);
        assert_eq!(uget::<JailInfo>(b).mem_cur, 2 * 4096);
    }

    #[test]
    fn mapping_uncharged_from_original_jail_after_attach() {
        let (mut k, pid) = kernel_with_proc();
        let a = make_jail(&mut k, pid, "/tmp", "a", 0, 0);
        let p = spawn(&mut k, pid, "p");
        let m = sys(&mut k, p, SYS_MMAP, &[0, 4096, PROT_READ]);
        sys(&mut k, p, SYS_JAIL_ATTACH, &[a as u64]);
        sys(&mut k, p, SYS_MUNMAP, &[m as u64, 4096]);
        assert_eq!(crate::kenv::with_acct(|x| x.get(0).unwrap().mem_cur), 0);
        assert_eq!(crate::kenv::with_acct(|x| x.get(a).unwrap().mem_cur), 0);
    }

    #[test]
    fn children_max_and_depth() {
        let (mut k, pid) = kernel_with_proc();
        let a = try_jail_children(&mut k, pid, "/tmp", "a", 0, 0, 100, 0, 1) as i32;
        let p = jailed_proc(&mut k, pid, a);
        assert!(try_jail(&mut k, p, "/", "c1", 0, 0, 100, 0) > 0);
        assert_eq!(try_jail(&mut k, p, "/", "c2", 0, 0, 100, 0), -EAGAIN as i64);
        // depth: chain of jails until depth 8
        let mut cur = pid;
        let mut created = 0;
        loop {
            let r = try_jail_children(&mut k, cur, "/", "d", 0, 0, 100, 0, 10);
            if r < 0 {
                assert_eq!(r, -EAGAIN as i64);
                break;
            }
            cur = jailed_proc(&mut k, cur, r as i32);
            created += 1;
        }
        assert_eq!(created, 8);
    }

    #[test]
    fn hostname_per_jail() {
        let (mut k, pid) = kernel_with_proc();
        let buf = umap(64);
        assert_eq!(sys(&mut k, pid, SYS_GETHOSTNAME, &[buf, 64]), 0);
        assert_eq!(cstr_bytes(&uread(buf, 64)), b"magia");
        assert_eq!(sys(&mut k, pid, SYS_GETHOSTNAME, &[buf, 5]), -ENAMETOOLONG as i64);
        let locked = make_jail(&mut k, pid, "/tmp", "locked", 0, 0);
        let open_j = try_jail(&mut k, pid, "/tmp", "open", 0, 0, 100, JAIL_ALLOW_SET_HOSTNAME) as i32;
        let p1 = jailed_proc(&mut k, pid, locked);
        let p2 = jailed_proc(&mut k, pid, open_j);
        let h = ustr("newname");
        assert_eq!(sys(&mut k, p1, SYS_SETHOSTNAME, &[h, 7]), -EPERM as i64);
        assert_eq!(sys(&mut k, p2, SYS_SETHOSTNAME, &[h, 7]), 0);
        sys(&mut k, p2, SYS_GETHOSTNAME, &[buf, 64]);
        assert_eq!(cstr_bytes(&uread(buf, 64)), b"newname");
        sys(&mut k, pid, SYS_GETHOSTNAME, &[buf, 64]);
        assert_eq!(cstr_bytes(&uread(buf, 64)), b"magia", "host jail unaffected");
        assert_eq!(sys(&mut k, pid, SYS_SETHOSTNAME, &[h, 64]), -ENAMETOOLONG as i64);
        let u = umap(224);
        sys(&mut k, p2, SYS_UNAME, &[u]);
        let un: Utsname = uget(u);
        assert_eq!(cstr_bytes(&un.nodename), b"newname");
        assert_eq!(cstr_bytes(&un.sysname), b"Magia");
        assert_eq!(cstr_bytes(&un.machine), b"x86_64");
    }

    #[test]
    fn privileges_are_not_amplified() {
        let (mut k, pid) = kernel_with_proc();
        let a = make_jail(&mut k, pid, "/tmp", "a", 0, 0); // no flags
        let p = jailed_proc(&mut k, pid, a);
        assert_eq!(try_jail(&mut k, p, "/", "c", 0, 0, 100, JAIL_ALLOW_SET_HOSTNAME), -EPERM as i64);
        assert_eq!(try_jail(&mut k, p, "/", "c", 0, 0, 100, JAIL_ALLOW_RING), -EPERM as i64);
    }

    #[test]
    fn jailed_pslist_sees_only_its_subtree() {
        let (mut k, pid) = kernel_with_proc();
        let a = make_jail(&mut k, pid, "/tmp", "a", 0, 0);
        let p = jailed_proc(&mut k, pid, a);
        let buf = umap(64);
        assert_eq!(sys(&mut k, p, SYS_PSLIST, &[buf, 16]), 1);
        assert_eq!(uget::<i32>(buf), p);
        assert_eq!(sys(&mut k, p, SYS_SETPRIORITY, &[pid as u64, 5]), -ESRCH as i64);
        assert_eq!(sys(&mut k, p, SYS_WAITPID, &[pid as u64, 0, WNOHANG]), -ECHILD as i64);
    }
}
