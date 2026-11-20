// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! sysctl: a MIB tree of named kernel variables. Idea: FreeBSD sysctl
//! (BSD 4.4), re-derived.
//!
//! Nodes have dotted names ("kern.ostype") and numeric OIDs ("1.1"),
//! assigned in registration order among siblings. Leaves are either
//! built in (computed from kernel state, some caller-relative such as
//! security.jail.jailed) or exported by C code through the set_sysctl
//! linker set (include/magia/sysctl.h), the way FreeBSD's SYSCTL_* macros
//! self-register.
//!
//! Policy: writes only from jail 0 (except kern.hostname, which follows
//! the sethostname rules); in capability mode only nodes flagged CAPRD
//! can be read and nothing can be written.

use crate::abi::*;
use crate::kenv;
use crate::kernel::Kernel;
use crate::syscall::{Ctx, SysResult, val};
use crate::uaccess::*;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Id {
    Ostype,
    Osrelease,
    Version,
    Hostname,
    BaseSlice,
    Machine,
    Ncpu,
    Pagesize,
    Physmem,
    FreePages,
    KmemInuse,
    CtxSwitches,
    Jailed,
    CapMode,
    Nprocs,
    Syscalls,
    SyscallTrace,
    RingEnable,
    /// index into Sysctl::c_nodes
    C(usize),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Ty {
    U64,
    Str,
}

#[derive(Clone, Debug)]
pub struct Leaf {
    pub id: Id,
    pub ty: Ty,
    pub writable: bool,
    pub caprd: bool,
    pub min: u64,
    pub max: u64,
    pub descr: String,
}

pub struct MibNode {
    pub name: String,
    pub num: u32,
    pub parent: usize,
    pub children: Vec<usize>,
    pub leaf: Option<Leaf>,
}

#[derive(Clone, Copy)]
pub struct CNode {
    pub kind: u32,
    pub ptr: *mut u8,
}
// SAFETY: C sysctl pointers refer to static kernel variables.
unsafe impl Send for CNode {}

pub struct Sysctl {
    pub nodes: Vec<MibNode>,
    pub c_nodes: Vec<CNode>,
}

pub enum Value {
    U64(u64),
    Str(Vec<u8>),
}

impl Value {
    pub fn bytes(&self) -> Vec<u8> {
        match self {
            Value::U64(v) => v.to_le_bytes().to_vec(),
            Value::Str(s) => {
                let mut b = s.clone();
                b.push(0);
                b
            }
        }
    }
    pub fn show(&self) -> String {
        match self {
            Value::U64(v) => format!("{v}"),
            Value::Str(s) => String::from_utf8_lossy(s).into_owned(),
        }
    }
}

impl Default for Sysctl {
    fn default() -> Self {
        Self::new()
    }
}

impl Sysctl {
    pub fn new() -> Self {
        Sysctl {
            nodes: alloc::vec![MibNode { name: String::new(), num: 0, parent: 0, children: Vec::new(), leaf: None }],
            c_nodes: Vec::new(),
        }
    }

    /// Add a leaf; creates interior nodes. Fails if the name is taken or
    /// passes through an existing leaf.
    pub fn add(&mut self, name: &str, leaf: Leaf) -> Result<(), ()> {
        if name.is_empty() || name.split('.').any(|c| c.is_empty()) {
            return Err(());
        }
        let comps: Vec<&str> = name.split('.').collect();
        let mut cur = 0usize;
        for (i, c) in comps.iter().enumerate() {
            let last = i + 1 == comps.len();
            let found = self.nodes[cur].children.iter().copied().find(|&n| self.nodes[n].name == *c);
            match found {
                Some(n) => {
                    if last || self.nodes[n].leaf.is_some() {
                        return Err(());
                    }
                    cur = n;
                }
                None => {
                    let num = self.nodes[cur].children.len() as u32 + 1;
                    let idx = self.nodes.len();
                    self.nodes.push(MibNode {
                        name: String::from(*c),
                        num,
                        parent: cur,
                        children: Vec::new(),
                        leaf: if last { Some(leaf.clone()) } else { None },
                    });
                    self.nodes[cur].children.push(idx);
                    cur = idx;
                }
            }
        }
        Ok(())
    }

    pub fn find(&self, name: &str) -> Option<usize> {
        let mut cur = 0usize;
        for c in name.split('.') {
            cur = self.nodes[cur].children.iter().copied().find(|&n| self.nodes[n].name == c)?;
        }
        Some(cur)
    }

    pub fn full_name(&self, mut n: usize) -> String {
        let mut parts = Vec::new();
        while n != 0 {
            parts.push(self.nodes[n].name.as_str());
            n = self.nodes[n].parent;
        }
        parts.reverse();
        parts.join(".")
    }

    pub fn oid(&self, mut n: usize) -> String {
        let mut parts = Vec::new();
        while n != 0 {
            parts.push(format!("{}", self.nodes[n].num));
            n = self.nodes[n].parent;
        }
        parts.reverse();
        parts.join(".")
    }

    /// All leaves in tree order.
    pub fn leaves(&self) -> Vec<usize> {
        let mut out = Vec::new();
        let mut stack = alloc::vec![0usize];
        while let Some(n) = stack.pop() {
            if self.nodes[n].leaf.is_some() {
                out.push(n);
            }
            for &c in self.nodes[n].children.iter().rev() {
                stack.push(c);
            }
        }
        out
    }
}

fn leaf(id: Id, ty: Ty, writable: bool, caprd: bool, descr: &str) -> Leaf {
    Leaf { id, ty, writable, caprd, min: 0, max: u64::MAX, descr: String::from(descr) }
}

impl Kernel {
    pub fn sysctl_init(&mut self) {
        use Id::*;
        use Ty::*;
        let s = &mut self.sysctl;
        let mut add = |n: &str, l: Leaf| {
            let _ = s.add(n, l);
        };
        add("kern.ostype", leaf(Ostype, Str, false, true, "operating system type"));
        add("kern.osrelease", leaf(Osrelease, Str, false, true, "release"));
        add("kern.version", leaf(Version, Str, false, true, "version string"));
        add("kern.hostname", leaf(Hostname, Str, true, true, "hostname of the caller's jail"));
        add("kern.sched.base_slice_ns", Leaf {
            min: crate::sched::MIN_BASE_SLICE_NS,
            max: crate::sched::MAX_BASE_SLICE_NS,
            ..leaf(BaseSlice, U64, true, true, "EEVDF base slice")
        });
        add("kern.sched.context_switches", leaf(CtxSwitches, U64, false, true, "context switches"));
        add("kern.proc.nprocs", leaf(Nprocs, U64, false, false, "processes"));
        add("kern.syscalls", leaf(Syscalls, U64, false, true, "system calls made"));
        add("kern.ring.enable", Leaf { min: 0, max: 1, ..leaf(RingEnable, U64, true, true, "io_uring-style rings enabled") });
        add("hw.machine", leaf(Machine, Str, false, true, "machine class"));
        add("hw.ncpu", leaf(Ncpu, U64, false, true, "CPUs"));
        add("hw.pagesize", leaf(Pagesize, U64, false, true, "page size"));
        add("hw.physmem", leaf(Physmem, U64, false, true, "physical memory bytes"));
        add("vm.stats.free_pages", leaf(FreePages, U64, false, true, "free physical pages"));
        add("vm.kmem_inuse", leaf(KmemInuse, U64, false, true, "kmalloc bytes in use"));
        add("security.jail.jailed", leaf(Jailed, U64, false, true, "caller is jailed"));
        add("security.capability_mode", leaf(CapMode, U64, false, true, "caller is in capability mode"));
        add("debug.syscall_trace", Leaf { min: 0, max: 1, ..leaf(SyscallTrace, U64, true, false, "trace system calls") });
        for p in kenv::c_sysctls() {
            if p.is_null() {
                continue;
            }
            // SAFETY: entries of the C linker set are static and valid.
            let cs = unsafe { *p };
            let Some(name) = (unsafe { c_str(cs.name) }) else { continue };
            let descr = unsafe { c_str(cs.descr) }.unwrap_or_default();
            let ty = match cs.kind {
                CSYSCTL_U64_RO | CSYSCTL_U64_RW => U64,
                CSYSCTL_STR_RO => Str,
                _ => continue,
            };
            if cs.ptr.is_null() {
                continue;
            }
            let idx = self.sysctl.c_nodes.len();
            let l = Leaf {
                id: C(idx),
                ty,
                writable: cs.kind == CSYSCTL_U64_RW,
                caprd: cs.flags & CSYSCTL_F_CAPRD != 0,
                min: if cs.kind == CSYSCTL_U64_RW { cs.min } else { 0 },
                max: if cs.kind == CSYSCTL_U64_RW { cs.max } else { u64::MAX },
                descr,
            };
            if self.sysctl.add(&name, l).is_ok() {
                self.sysctl.c_nodes.push(CNode { kind: cs.kind, ptr: cs.ptr });
            }
        }
    }

    fn sysctl_read(&self, pid: i32, id: Id) -> Value {
        use Value::*;
        let p = self.procs.get(&pid);
        let jid = p.map_or(0, |p| p.jid);
        match id {
            Id::Ostype => Str(b"Magia".to_vec()),
            Id::Osrelease => Str(crate::RELEASE.as_bytes().to_vec()),
            Id::Version => Str(crate::VERSION.as_bytes().to_vec()),
            Id::Hostname => Str(self.jails.get(&jid).map_or(Vec::new(), |j| j.hostname.clone())),
            Id::BaseSlice => U64(kenv::with_sched(|s| s.base_slice())),
            Id::Machine => Str(b"x86_64".to_vec()),
            Id::Ncpu => U64(1),
            Id::Pagesize => U64(PAGE_SIZE),
            Id::Physmem => U64(kenv::total_pages() * PAGE_SIZE),
            Id::FreePages => U64(kenv::free_pages()),
            Id::KmemInuse => U64(kenv::kmem_inuse()),
            Id::CtxSwitches => U64(kenv::context_switches()),
            Id::Jailed => U64((jid != 0) as u64),
            Id::CapMode => U64(p.is_some_and(|p| p.cap_mode) as u64),
            Id::Nprocs => U64(self.visible_pids(pid).len() as u64),
            Id::Syscalls => U64(self.syscalls),
            Id::SyscallTrace => U64(self.trace as u64),
            Id::RingEnable => U64(self.ring_enable as u64),
            Id::C(i) => {
                let n = self.sysctl.c_nodes[i];
                if n.kind == CSYSCTL_STR_RO {
                    Str(unsafe { c_str(n.ptr) }.unwrap_or_default().into_bytes())
                } else {
                    // SAFETY: C promised a u64 variable.
                    U64(unsafe { core::ptr::read_volatile(n.ptr as *const u64) })
                }
            }
        }
    }

    fn sysctl_write(&mut self, pid: i32, l: &Leaf, new: &[u8]) -> KResult<()> {
        if l.id == Id::Hostname {
            let n = new.iter().position(|&b| b == 0).unwrap_or(new.len());
            if new[n..].iter().any(|&b| b != 0) {
                return Err(EINVAL);
            }
            return self.set_hostname(pid, &new[..n]);
        }
        if self.proc(pid)?.jid != 0 {
            return Err(EPERM);
        }
        if l.ty != Ty::U64 || new.len() != 8 {
            return Err(EINVAL);
        }
        let v = u64::from_le_bytes(new.try_into().unwrap());
        if v < l.min || v > l.max {
            return Err(EINVAL);
        }
        match l.id {
            Id::BaseSlice => {
                if !kenv::with_sched(|s| s.set_base_slice(v)) {
                    return Err(EINVAL);
                }
            }
            Id::SyscallTrace => self.trace = v != 0,
            Id::RingEnable => self.ring_enable = v != 0,
            Id::C(i) => {
                let n = self.sysctl.c_nodes[i];
                // SAFETY: C promised a writable u64 variable.
                unsafe { core::ptr::write_volatile(n.ptr as *mut u64, v) };
            }
            _ => return Err(EPERM),
        }
        Ok(())
    }

    /// Look up a node by name for `pid`, applying the capability-mode
    /// read rule. Returns the leaf.
    pub fn sysctl_leaf(&self, pid: i32, name: &str) -> KResult<Leaf> {
        let n = self.sysctl.find(name).ok_or(ENOENT)?;
        let l = self.sysctl.nodes[n].leaf.clone().ok_or(EISDIR)?;
        if self.proc(pid)?.cap_mode && !l.caprd {
            return Err(ECAPMODE);
        }
        Ok(l)
    }

    pub fn sysctl_get(&self, pid: i32, name: &str) -> KResult<Value> {
        let l = self.sysctl_leaf(pid, name)?;
        Ok(self.sysctl_read(pid, l.id))
    }

    /// Set from a text value (debug monitor): u64 in decimal or 0x hex.
    pub fn sysctl_set_text(&mut self, pid: i32, name: &str, text: &str) -> KResult<()> {
        let l = self.sysctl_leaf(pid, name)?;
        if !l.writable {
            return Err(EPERM);
        }
        let bytes = match l.ty {
            Ty::Str => text.as_bytes().to_vec(),
            Ty::U64 => {
                let t = text.trim();
                let v = if let Some(h) = t.strip_prefix("0x") { u64::from_str_radix(h, 16) } else { t.parse() };
                v.map_err(|_| EINVAL)?.to_le_bytes().to_vec()
            }
        };
        self.sysctl_write(pid, &l, &bytes)
    }

    /// sysctl(name, namelen, oldp, oldlenp, newp, newlen)
    pub fn sys_sysctl(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let raw = copyin_vec(a[0], a[1] as usize)?;
        let n = raw.iter().position(|&b| b == 0).unwrap_or(raw.len());
        let name = core::str::from_utf8(&raw[..n]).map_err(|_| ENOENT)?;
        let l = self.sysctl_leaf(c.pid, name)?;
        let (oldp, oldlenp, newp, newlen) = (a[2], a[3], a[4], a[5]);
        if newp != 0 {
            if self.proc(c.pid)?.cap_mode {
                return Err(ECAPMODE);
            }
            if !l.writable {
                return Err(EPERM);
            }
        }
        if oldlenp != 0 {
            let v = self.sysctl_read(c.pid, l.id).bytes();
            let need = v.len() as u64;
            if oldp != 0 {
                let have: u64 = copyin_val(oldlenp)?;
                if have < need {
                    copyout_val(oldlenp, &need)?;
                    return Err(ENOMEM);
                }
                copyout(oldp, &v)?;
            }
            copyout_val(oldlenp, &need)?;
        } else if oldp != 0 {
            return Err(EINVAL);
        }
        if newp != 0 {
            let new = copyin_vec(newp, newlen as usize)?;
            self.sysctl_write(c.pid, &l, &new)?;
        }
        val(0)
    }
}

/// # Safety
/// `p` must be null or a NUL-terminated string.
unsafe fn c_str(p: *const u8) -> Option<String> {
    if p.is_null() {
        return None;
    }
    let mut n = 0;
    while unsafe { *p.add(n) } != 0 && n < 256 {
        n += 1;
    }
    let s = unsafe { core::slice::from_raw_parts(p, n) };
    Some(String::from_utf8_lossy(s).into_owned())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::*;

    /// sysctl read helper: returns (result, bytes, reported len)
    fn get(k: &mut Kernel, pid: i32, name: &str, buflen: u64) -> (i64, Vec<u8>, u64) {
        let n = ustr(name);
        let old = umap(256);
        let lenp = umap(8);
        uwrite(lenp, &buflen.to_le_bytes());
        let r = sys(k, pid, SYS_SYSCTL, &[n, name.len() as u64, old, lenp, 0, 0]);
        let len = uget::<u64>(lenp);
        (r, uread(old, len.min(256) as usize), len)
    }

    fn set_u64(k: &mut Kernel, pid: i32, name: &str, v: u64) -> i64 {
        let n = ustr(name);
        let newp = umap(8);
        uwrite(newp, &v.to_le_bytes());
        sys(k, pid, SYS_SYSCTL, &[n, name.len() as u64, 0, 0, newp, 8])
    }

    #[test]
    fn tree_names_and_oids() {
        let k = Kernel::new(1);
        let s = &k.sysctl;
        let n = s.find("kern.ostype").unwrap();
        assert_eq!(s.full_name(n), "kern.ostype");
        assert_eq!(s.oid(n), "1.1");
        assert!(s.find("kern").unwrap() > 0);
        assert!(s.find("kern.nope").is_none());
        let mut t = Sysctl::new();
        let l = leaf(Id::Ncpu, Ty::U64, false, false, "");
        assert!(t.add("a.b", l.clone()).is_ok());
        assert!(t.add("a.b", l.clone()).is_err(), "duplicate");
        assert!(t.add("a.b.c", l.clone()).is_err(), "through a leaf");
        assert!(t.add("a..c", l.clone()).is_err());
        assert!(t.add("", l).is_err());
    }

    #[test]
    fn read_strings_and_numbers() {
        let (mut k, pid) = kernel_with_proc();
        let (r, v, len) = get(&mut k, pid, "kern.ostype", 64);
        assert_eq!((r, len), (0, 6));
        assert_eq!(v, b"Magia\0");
        let (r, v, len) = get(&mut k, pid, "hw.pagesize", 8);
        assert_eq!((r, len), (0, 8));
        assert_eq!(u64::from_le_bytes(v[..8].try_into().unwrap()), 4096);
        let (_, v, _) = get(&mut k, pid, "hw.physmem", 8);
        assert_eq!(u64::from_le_bytes(v[..8].try_into().unwrap()), 65536 * 4096);
    }

    #[test]
    fn buffer_too_small_reports_size() {
        let (mut k, pid) = kernel_with_proc();
        let (r, _, len) = get(&mut k, pid, "kern.version", 4);
        assert_eq!(r, -ENOMEM as i64);
        assert_eq!(len, crate::VERSION.len() as u64 + 1);
        // size query with oldp = 0
        let n = ustr("kern.ostype");
        let lenp = umap(8);
        assert_eq!(sys(&mut k, pid, SYS_SYSCTL, &[n, 11, 0, lenp, 0, 0]), 0);
        assert_eq!(uget::<u64>(lenp), 6);
        let old = umap(8);
        assert_eq!(sys(&mut k, pid, SYS_SYSCTL, &[n, 11, old, 0, 0, 0]), -EINVAL as i64);
    }

    #[test]
    fn errors() {
        let (mut k, pid) = kernel_with_proc();
        assert_eq!(get(&mut k, pid, "kern.nonexistent", 8).0, -ENOENT as i64);
        assert_eq!(get(&mut k, pid, "kern", 8).0, -EISDIR as i64);
        assert_eq!(set_u64(&mut k, pid, "hw.ncpu", 4), -EPERM as i64);
        assert_eq!(set_u64(&mut k, pid, "kern.sched.base_slice_ns", 5), -EINVAL as i64);
        assert_eq!(set_u64(&mut k, pid, "debug.syscall_trace", 2), -EINVAL as i64);
        let n = ustr("debug.syscall_trace");
        let newp = umap(8);
        assert_eq!(sys(&mut k, pid, SYS_SYSCTL, &[n, 19, 0, 0, newp, 4]), -EINVAL as i64, "wrong size");
        assert_eq!(sys(&mut k, pid, SYS_SYSCTL, &[n, 0, 0, 0, 0, 0]), -EINVAL as i64, "namelen 0");
    }

    #[test]
    fn writes_take_effect() {
        let (mut k, pid) = kernel_with_proc();
        assert_eq!(set_u64(&mut k, pid, "kern.sched.base_slice_ns", 2_000_000), 0);
        assert_eq!(kenv::with_sched(|s| s.base_slice()), 2_000_000);
        assert_eq!(set_u64(&mut k, pid, "debug.syscall_trace", 1), 0);
        assert!(k.trace);
        assert_eq!(set_u64(&mut k, pid, "kern.ring.enable", 0), 0);
        assert!(!k.ring_enable);
        let pp = umap(48);
        assert_eq!(sys(&mut k, pid, SYS_URING_SETUP, &[4, pp]), -EPERM as i64);
    }

    #[test]
    fn jailed_writes_and_relative_values() {
        let (mut k, pid) = kernel_with_proc();
        let j = try_jail(&mut k, pid, "/tmp", "jh", 0, 0, 100, JAIL_ALLOW_SET_HOSTNAME) as i32;
        let p = jailed_proc(&mut k, pid, j);
        assert_eq!(set_u64(&mut k, p, "kern.sched.base_slice_ns", 2_000_000), -EPERM as i64);
        assert_eq!(set_u64(&mut k, p, "kern.ring.enable", 0), -EPERM as i64);
        let (_, v, _) = get(&mut k, p, "security.jail.jailed", 8);
        assert_eq!(v[0], 1);
        let (_, v, _) = get(&mut k, pid, "security.jail.jailed", 8);
        assert_eq!(v[0], 0);
        let (_, v, _) = get(&mut k, p, "kern.hostname", 64);
        assert_eq!(v, b"jh\0");
        // hostname write follows sethostname rules
        let n = ustr("kern.hostname");
        let h = ustr("viasysctl");
        assert_eq!(sys(&mut k, p, SYS_SYSCTL, &[n, 13, 0, 0, h, 9]), 0);
        assert_eq!(k.jails[&j].hostname, b"viasysctl");
    }

    #[test]
    fn capability_mode_reads_only_caprd() {
        let (mut k, pid) = kernel_with_proc();
        sys(&mut k, pid, SYS_CAP_ENTER, &[]);
        assert_eq!(get(&mut k, pid, "kern.ostype", 64).0, 0);
        assert_eq!(get(&mut k, pid, "kern.proc.nprocs", 8).0, -ECAPMODE as i64);
        assert_eq!(get(&mut k, pid, "security.capability_mode", 8).1[0], 1);
        assert_eq!(set_u64(&mut k, pid, "debug.syscall_trace", 1), -ECAPMODE as i64);
    }

    static mut C_TICKS: u64 = 1234;
    static mut C_KNOB: u64 = 5;

    #[test]
    fn c_linker_set_nodes() {
        let nodes: Vec<CSysctl> = {
            alloc::vec![
                CSysctl { name: c"kern.ticks".as_ptr() as *const u8, kind: CSYSCTL_U64_RO, flags: CSYSCTL_F_CAPRD, ptr: &raw mut C_TICKS as *mut u8, descr: c"ticks".as_ptr() as *const u8, min: 0, max: 0 },
                CSysctl { name: c"debug.knob".as_ptr() as *const u8, kind: CSYSCTL_U64_RW, flags: 0, ptr: &raw mut C_KNOB as *mut u8, descr: core::ptr::null(), min: 1, max: 10 },
                CSysctl { name: c"kern.compiler".as_ptr() as *const u8, kind: CSYSCTL_STR_RO, flags: 0, ptr: c"gcc".as_ptr() as *mut u8, descr: core::ptr::null(), min: 0, max: 0 },
                CSysctl { name: c"kern.ostype".as_ptr() as *const u8, kind: CSYSCTL_STR_RO, flags: 0, ptr: c"dup".as_ptr() as *mut u8, descr: core::ptr::null(), min: 0, max: 0 },
            ]
        };
        let leaked: &'static [CSysctl] = Vec::leak(nodes);
        with(|m| m.csysctls = leaked.iter().map(|c| c as *const CSysctl).collect());
        let mut k = Kernel::new(1);
        let pid = spawn(&mut k, 0, "t");
        let (_, v, _) = get(&mut k, pid, "kern.ticks", 8);
        assert_eq!(u64::from_le_bytes(v[..8].try_into().unwrap()), 1234);
        assert_eq!(get(&mut k, pid, "kern.compiler", 16).1, b"gcc\0");
        assert_eq!(get(&mut k, pid, "kern.ostype", 16).1, b"Magia\0", "duplicate C node ignored");
        assert_eq!(set_u64(&mut k, pid, "debug.knob", 11), -EINVAL as i64);
        assert_eq!(set_u64(&mut k, pid, "debug.knob", 7), 0);
        assert_eq!(unsafe { core::ptr::read_volatile(&raw const C_KNOB) }, 7);
        assert_eq!(set_u64(&mut k, pid, "kern.ticks", 7), -EPERM as i64);
    }

    #[test]
    fn text_interface_for_monitor() {
        let mut k = Kernel::new(1);
        assert_eq!(k.sysctl_get(0, "hw.ncpu").unwrap().show(), "1");
        k.sysctl_set_text(0, "kern.sched.base_slice_ns", "0x1e8480").unwrap();
        assert_eq!(kenv::with_sched(|s| s.base_slice()), 2_000_000);
        assert_eq!(k.sysctl_set_text(0, "kern.sched.base_slice_ns", "abc"), Err(EINVAL));
        assert_eq!(k.sysctl_set_text(0, "hw.ncpu", "2"), Err(EPERM));
        k.sysctl_set_text(0, "kern.hostname", "box").unwrap();
        assert_eq!(k.sysctl_get(0, "kern.hostname").unwrap().show(), "box");
        assert!(k.sysctl.leaves().len() >= 18);
    }
}
