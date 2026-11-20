// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! Text output for the `magia>` serial debug monitor (kernel/monitor.c).
//! The monitor acts as the kernel itself (pid 0, jail 0).

use crate::abi::*;
use crate::kernel::{FileKind, Kernel};
use crate::vfs::NodeKind;
use alloc::string::String;

fn s(b: &[u8]) -> String {
    String::from_utf8_lossy(b).into_owned()
}

impl Kernel {
    pub fn dbg_ps(&self) {
        kprint!("  PID  PPID  JID   TID  ST  NICE  WEIGHT  CAP   RUNTIME(us)  MEM(KiB)  NAME\n");
        for &pid in self.procs.keys() {
            let Ok(i) = self.proc_info(pid) else { continue };
            let st = ["R", "S", "Z"][i.state.min(2) as usize];
            kprint!(
                "{:5} {:5} {:4} {:5}  {:2}  {:4}  {:6}  {:3}  {:12}  {:8}  {}\n",
                i.pid, i.ppid, i.jid, i.tid, st, i.nice, i.weight,
                if i.cap_mode != 0 { "yes" } else { "no" },
                i.runtime_ns / 1000, i.mem_bytes / 1024, s(cstr_bytes(&i.name))
            );
        }
    }

    pub fn dbg_jails(&self) {
        kprint!("  JID  PARENT  DEPTH  PROCS  CPU_W  MEM_CUR(KiB)  MEM_MAX(KiB)  PIDS_MAX  RING  HOSTNAME  PATH\n");
        for &jid in self.jails.keys() {
            let Ok(i) = self.jail_info(jid) else { continue };
            let ring = self.jails[&jid].flags & JAIL_ALLOW_RING != 0;
            kprint!(
                "{:5}  {:6}  {:5}  {:5}  {:5}  {:12}  {:12}  {:8}  {:4}  {}  {}\n",
                i.jid, i.parent, i.depth, i.nprocs, i.cpu_weight, i.mem_cur / 1024,
                if i.mem_max == 0 { String::from("-") } else { alloc::format!("{}", i.mem_max / 1024) },
                if i.pids_max == 0 { String::from("-") } else { alloc::format!("{}", i.pids_max) },
                if ring { "yes" } else { "no" },
                s(cstr_bytes(&i.hostname)), s(cstr_bytes(&i.path))
            );
        }
    }

    pub fn dbg_sysctl(&mut self, name: &str, value: &str) {
        if name.is_empty() {
            for n in self.sysctl.leaves() {
                let full = self.sysctl.full_name(n);
                match self.sysctl_get(0, &full) {
                    Ok(v) => kprint!("{} = {}\n", full, v.show()),
                    Err(e) => kprint!("{}: {}\n", full, errno_name(e)),
                }
            }
            return;
        }
        if !value.is_empty() {
            let old = self.sysctl_get(0, name).map(|v| v.show());
            match self.sysctl_set_text(0, name, value) {
                Ok(()) => kprint!("{}: {} -> {}\n", name, old.unwrap_or_default(), value),
                Err(e) => kprint!("sysctl {}: {}\n", name, errno_name(e)),
            }
            return;
        }
        match self.sysctl.find(name) {
            Some(n) if self.sysctl.nodes[n].leaf.is_none() => {
                // interior node: list the subtree
                for l in self.sysctl.leaves() {
                    let full = self.sysctl.full_name(l);
                    if full.starts_with(name) && full.as_bytes().get(name.len()) == Some(&b'.') {
                        if let Ok(v) = self.sysctl_get(0, &full) {
                            kprint!("{} = {}\n", full, v.show());
                        }
                    }
                }
            }
            Some(n) => {
                let l = self.sysctl.nodes[n].leaf.clone().unwrap();
                match self.sysctl_get(0, name) {
                    Ok(v) => kprint!("{} = {}    [oid {}, {}{}] {}\n", name, v.show(), self.sysctl.oid(n),
                        if l.writable { "rw" } else { "ro" }, if l.caprd { ", caprd" } else { "" }, l.descr),
                    Err(e) => kprint!("sysctl {}: {}\n", name, errno_name(e)),
                }
            }
            None => kprint!("sysctl {}: ENOENT\n", name),
        }
    }

    pub fn dbg_ls(&self, path: &[u8]) {
        let path: &[u8] = if path.is_empty() { b"/" } else { path };
        let root = self.vfs.root;
        let w = match self.vfs.walk(root, root, path, false) {
            Ok(w) => w,
            Err(e) => return kprint!("ls: {}: {}\n", s(path), errno_name(e)),
        };
        let Some(ino) = w.target else { return kprint!("ls: {}: ENOENT\n", s(path)) };
        if !self.vfs.is_dir(ino) {
            if let Ok(st) = self.vfs.stat(ino) {
                kprint!("{:o} {:8} {}\n", st.mode, st.size, s(path));
            }
            return;
        }
        let mut pos = 2;
        while let Ok(Some((cino, typ, name))) = self.vfs.readdir(ino, pos) {
            let size = self.vfs.size(cino);
            let t = match typ {
                DT_DIR => 'd',
                DT_CHR => 'c',
                _ => '-',
            };
            kprint!("{} {:8} {}{}\n", t, size, s(&name), if typ == DT_DIR { "/" } else { "" });
            pos += 1;
        }
    }

    pub fn dbg_cat(&self, path: &[u8]) {
        let root = self.vfs.root;
        let ino = match self.vfs.walk(root, root, path, false) {
            Ok(w) => match w.target {
                Some(i) => i,
                None => return kprint!("cat: {}: ENOENT\n", s(path)),
            },
            Err(e) => return kprint!("cat: {}: {}\n", s(path), errno_name(e)),
        };
        match &self.vfs.node(ino).map(|n| &n.kind) {
            Ok(NodeKind::File { data }) => {
                crate::kenv::console_write(&data[..data.len().min(64 * 1024)]);
                if data.last().is_some_and(|&b| b != b'\n') {
                    kprint!("\n");
                }
            }
            Ok(NodeKind::Dir { .. }) => kprint!("cat: {}: EISDIR\n", s(path)),
            Ok(NodeKind::Dev(_)) => kprint!("cat: {}: device\n", s(path)),
            Err(e) => kprint!("cat: {}: {}\n", s(path), errno_name(*e)),
        }
    }

    pub fn dbg_uname(&self) {
        kprint!(
            "Magia {} {} x86_64 ({} processes, {} jails, {} open files, {} pipes, {} kqueues, {} rings, {} syscalls)\n",
            self.jails[&0].hostname.escape_ascii(),
            crate::VERSION,
            self.procs.len(),
            self.jails.len(),
            self.files.len(),
            self.pipes.len(),
            self.files.values().filter(|f| matches!(f.kind, FileKind::Kq(_))).count(),
            self.rings.len(),
            self.syscalls
        );
    }
}

#[cfg(test)]
mod tests {
    use crate::testutil::*;

    #[test]
    fn monitor_output() {
        let (mut k, pid) = kernel_with_proc();
        make_jail(&mut k, pid, "/tmp", "boxed", 1 << 20, 3);
        take_console();
        k.dbg_ps();
        let out = take_console();
        assert!(out.contains("kernel") && out.contains("test"), "{out}");
        k.dbg_jails();
        let out = take_console();
        assert!(out.contains("boxed") && out.contains("/tmp") && out.contains("1024"), "{out}");
        k.dbg_ls(b"/dev");
        let out = take_console();
        assert!(out.contains("console") && out.contains("null"), "{out}");
        k.dbg_cat(b"/etc/motd");
        assert!(take_console().contains("Welcome to Magia"));
        k.dbg_cat(b"/nope");
        assert!(take_console().contains("ENOENT"));
        k.dbg_sysctl("", "");
        let out = take_console();
        assert!(out.contains("kern.ostype = Magia") && out.contains("hw.pagesize = 4096"), "{out}");
        k.dbg_sysctl("kern.ostype", "");
        assert!(take_console().contains("oid 1.1"));
        k.dbg_sysctl("kern.sched", "");
        assert!(take_console().contains("kern.sched.base_slice_ns = 3000000"));
        k.dbg_sysctl("debug.syscall_trace", "1");
        assert!(take_console().contains("0 -> 1"));
        k.dbg_sysctl("hw.ncpu", "4");
        assert!(take_console().contains("EPERM"));
        k.dbg_uname();
        assert!(take_console().contains("Magia magia"));
    }
}
