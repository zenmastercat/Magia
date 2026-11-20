// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! VFS: one in-memory namespace with two file systems.
//!
//! * ramfs: directories and regular files kept in kernel memory.
//! * devfs: a read-only namespace of character devices, mounted at /dev
//!   (idea: FreeBSD devfs). Mounting is simplified: the /dev directory node
//!   itself belongs to devfs; creating or removing names inside it gives
//!   EROFS, removing /dev itself gives EBUSY.
//!
//! Path resolution enforces two policies that come from untrusted input:
//! a per-process root (jail root; ".." at the root stays there) and the
//! Capsicum "beneath" rule for capability mode: lookups are relative to a
//! directory descriptor and may never climb above it (idea: FreeBSD
//! Capsicum `openat` in capability mode).

use crate::abi::*;
use alloc::collections::BTreeMap;
use alloc::vec::Vec;

pub type Ino = u64;
pub const MAX_FILE_SIZE: u64 = 1 << 20;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Fs {
    Ram,
    Dev,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DevKind {
    Null,
    Zero,
    Console,
}

pub enum NodeKind {
    Dir { entries: BTreeMap<Vec<u8>, Ino>, parent: Ino },
    File { data: Vec<u8> },
    Dev(DevKind),
}

pub struct Node {
    pub kind: NodeKind,
    pub fs: Fs,
    pub nlink: u32,
    pub open_refs: u32,
}

impl Node {
    pub fn is_dir(&self) -> bool {
        matches!(self.kind, NodeKind::Dir { .. })
    }
}

pub struct Vfs {
    nodes: BTreeMap<Ino, Node>,
    next_ino: Ino,
    pub root: Ino,
    pub dev: Ino,
    pub console: Ino,
}

/// Result of resolving a path: the directory that holds the last
/// component, its name (empty when the path names a directory itself,
/// e.g. "/" or ".."), and the node it names if it exists.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Walk {
    pub parent: Ino,
    pub name: Vec<u8>,
    pub target: Option<Ino>,
}

impl Default for Vfs {
    fn default() -> Self {
        Self::new()
    }
}

impl Vfs {
    pub fn new() -> Self {
        let mut v = Vfs { nodes: BTreeMap::new(), next_ino: 1, root: 0, dev: 0, console: 0 };
        let root = v.alloc(NodeKind::Dir { entries: BTreeMap::new(), parent: 0 }, Fs::Ram);
        v.root = root;
        if let NodeKind::Dir { parent, .. } = &mut v.nodes.get_mut(&root).unwrap().kind {
            *parent = root;
        }
        let dev = v.alloc(NodeKind::Dir { entries: BTreeMap::new(), parent: root }, Fs::Dev);
        v.link(root, b"dev", dev);
        v.dev = dev;
        for (name, k) in [(&b"null"[..], DevKind::Null), (b"zero", DevKind::Zero), (b"console", DevKind::Console)] {
            let ino = v.alloc(NodeKind::Dev(k), Fs::Dev);
            v.link(dev, name, ino);
            if k == DevKind::Console {
                v.console = ino;
            }
        }
        let tmp = v.mkdir(root, b"tmp").unwrap();
        let _ = tmp;
        let etc = v.mkdir(root, b"etc").unwrap();
        let motd = v.create(etc, b"motd").unwrap();
        v.write(motd, 0, b"Welcome to Magia, an experimental kernel. Linux ideas only; BSD code listed in THIRD_PARTY.md.\n")
            .unwrap();
        v
    }

    fn alloc(&mut self, kind: NodeKind, fs: Fs) -> Ino {
        let ino = self.next_ino;
        self.next_ino += 1;
        self.nodes.insert(ino, Node { kind, fs, nlink: 0, open_refs: 0 });
        ino
    }

    fn link(&mut self, dir: Ino, name: &[u8], ino: Ino) {
        if let NodeKind::Dir { entries, .. } = &mut self.nodes.get_mut(&dir).unwrap().kind {
            entries.insert(name.to_vec(), ino);
        }
        self.nodes.get_mut(&ino).unwrap().nlink += 1;
    }

    pub fn node(&self, ino: Ino) -> KResult<&Node> {
        self.nodes.get(&ino).ok_or(ENOENT)
    }

    pub fn node_mut(&mut self, ino: Ino) -> KResult<&mut Node> {
        self.nodes.get_mut(&ino).ok_or(ENOENT)
    }

    pub fn exists(&self, ino: Ino) -> bool {
        self.nodes.contains_key(&ino)
    }

    pub fn is_dir(&self, ino: Ino) -> bool {
        self.nodes.get(&ino).is_some_and(|n| n.is_dir())
    }

    pub fn lookup(&self, dir: Ino, name: &[u8]) -> KResult<Option<Ino>> {
        match &self.node(dir)?.kind {
            NodeKind::Dir { entries, .. } => Ok(entries.get(name).copied()),
            _ => Err(ENOTDIR),
        }
    }

    fn parent_of(&self, dir: Ino) -> KResult<Ino> {
        match &self.node(dir)?.kind {
            NodeKind::Dir { parent, .. } => Ok(*parent),
            _ => Err(ENOTDIR),
        }
    }

    /// Resolve `path`.
    /// * `root`: the process root (jail root); absolute paths start here
    ///   and ".." never climbs above it.
    /// * `start`: directory for relative paths (cwd or a dirfd).
    /// * `beneath`: capability mode: absolute paths and any ".." that
    ///   would leave `start` fail with ENOTCAPABLE.
    pub fn walk(&self, root: Ino, start: Ino, path: &[u8], beneath: bool) -> KResult<Walk> {
        if path.is_empty() {
            return Err(ENOENT);
        }
        if path.len() >= PATH_MAX {
            return Err(ENAMETOOLONG);
        }
        let mut cur = if path[0] == b'/' {
            if beneath {
                return Err(ENOTCAPABLE);
            }
            root
        } else {
            start
        };
        if !self.is_dir(cur) {
            return Err(if self.exists(cur) { ENOTDIR } else { ENOENT });
        }
        let comps: Vec<&[u8]> = path.split(|&b| b == b'/').filter(|c| !c.is_empty() && *c != b".").collect();
        for c in &comps {
            if c.len() > NAME_MAX {
                return Err(ENAMETOOLONG);
            }
        }
        let mut depth: usize = 0;
        let n = comps.len();
        for (i, c) in comps.iter().enumerate() {
            let last = i + 1 == n;
            if !self.is_dir(cur) {
                return Err(ENOTDIR);
            }
            if *c == b".." {
                if beneath {
                    if depth == 0 {
                        return Err(ENOTCAPABLE);
                    }
                    depth -= 1;
                }
                if cur != root {
                    cur = self.parent_of(cur)?;
                    if !self.exists(cur) {
                        return Err(ENOENT);
                    }
                }
                continue;
            }
            if last {
                let target = self.lookup(cur, c)?;
                return Ok(Walk { parent: cur, name: c.to_vec(), target });
            }
            cur = self.lookup(cur, c)?.ok_or(ENOENT)?;
            depth += 1;
        }
        Ok(Walk { parent: cur, name: Vec::new(), target: Some(cur) })
    }

    fn check_writable_dir(&self, dir: Ino) -> KResult<()> {
        let d = self.node(dir)?;
        if !d.is_dir() {
            return Err(ENOTDIR);
        }
        if d.fs != Fs::Ram {
            return Err(EROFS);
        }
        Ok(())
    }

    pub fn create(&mut self, dir: Ino, name: &[u8]) -> KResult<Ino> {
        self.check_writable_dir(dir)?;
        if name.is_empty() || self.lookup(dir, name)?.is_some() {
            return Err(EEXIST);
        }
        let ino = self.alloc(NodeKind::File { data: Vec::new() }, Fs::Ram);
        self.link(dir, name, ino);
        Ok(ino)
    }

    pub fn mkdir(&mut self, dir: Ino, name: &[u8]) -> KResult<Ino> {
        self.check_writable_dir(dir)?;
        if name.is_empty() || self.lookup(dir, name)?.is_some() {
            return Err(EEXIST);
        }
        let ino = self.alloc(NodeKind::Dir { entries: BTreeMap::new(), parent: dir }, Fs::Ram);
        self.link(dir, name, ino);
        Ok(ino)
    }

    /// unlink (rmdir = false) or rmdir (rmdir = true) `name` in `dir`.
    pub fn unlink(&mut self, dir: Ino, name: &[u8], rmdir: bool) -> KResult<()> {
        if name.is_empty() {
            return Err(EBUSY); // the path names a directory itself, e.g. "/"
        }
        let dnode = self.node(dir)?;
        if !dnode.is_dir() {
            return Err(ENOTDIR);
        }
        if dnode.fs != Fs::Ram {
            return Err(EROFS);
        }
        let ino = self.lookup(dir, name)?.ok_or(ENOENT)?;
        let n = self.node(ino)?;
        if n.fs != Fs::Ram {
            return Err(EBUSY); // mount point (/dev)
        }
        match &n.kind {
            NodeKind::Dir { entries, .. } => {
                if !rmdir {
                    return Err(EISDIR);
                }
                if !entries.is_empty() {
                    return Err(ENOTEMPTY);
                }
            }
            _ => {
                if rmdir {
                    return Err(ENOTDIR);
                }
            }
        }
        if let NodeKind::Dir { entries, .. } = &mut self.nodes.get_mut(&dir).unwrap().kind {
            entries.remove(name);
        }
        let n = self.nodes.get_mut(&ino).unwrap();
        n.nlink -= 1;
        self.maybe_free(ino);
        Ok(())
    }

    fn maybe_free(&mut self, ino: Ino) {
        if let Some(n) = self.nodes.get(&ino) {
            if n.nlink == 0 && n.open_refs == 0 && ino != self.root {
                self.nodes.remove(&ino);
            }
        }
    }

    pub fn open_ref(&mut self, ino: Ino) {
        if let Some(n) = self.nodes.get_mut(&ino) {
            n.open_refs += 1;
        }
    }

    pub fn close_ref(&mut self, ino: Ino) {
        if let Some(n) = self.nodes.get_mut(&ino) {
            n.open_refs = n.open_refs.saturating_sub(1);
        }
        self.maybe_free(ino);
    }

    pub fn size(&self, ino: Ino) -> u64 {
        match self.nodes.get(&ino).map(|n| &n.kind) {
            Some(NodeKind::File { data }) => data.len() as u64,
            Some(NodeKind::Dir { entries, .. }) => entries.len() as u64,
            _ => 0,
        }
    }

    pub fn truncate(&mut self, ino: Ino) -> KResult<()> {
        if let NodeKind::File { data } = &mut self.node_mut(ino)?.kind {
            data.clear();
        }
        Ok(())
    }

    /// Kernel-side write (used at boot and by tests).
    pub fn write(&mut self, ino: Ino, off: u64, buf: &[u8]) -> KResult<usize> {
        let data = self.file_data_for_write(ino, off, buf.len() as u64)?;
        data[off as usize..off as usize + buf.len()].copy_from_slice(buf);
        Ok(buf.len())
    }

    /// Grow the file to cover `off..off+len` (zero fill) and return its
    /// storage, or EFBIG beyond MAX_FILE_SIZE.
    pub fn file_data_for_write(&mut self, ino: Ino, off: u64, len: u64) -> KResult<&mut Vec<u8>> {
        let end = off.checked_add(len).ok_or(EFBIG)?;
        if end > MAX_FILE_SIZE {
            return Err(EFBIG);
        }
        match &mut self.node_mut(ino)?.kind {
            NodeKind::File { data } => {
                if data.len() < end as usize {
                    data.resize(end as usize, 0);
                }
                Ok(data)
            }
            NodeKind::Dir { .. } => Err(EISDIR),
            NodeKind::Dev(_) => Err(EINVAL),
        }
    }

    pub fn file_data(&self, ino: Ino) -> KResult<&[u8]> {
        match &self.node(ino)?.kind {
            NodeKind::File { data } => Ok(data),
            NodeKind::Dir { .. } => Err(EISDIR),
            NodeKind::Dev(_) => Err(EINVAL),
        }
    }

    /// Directory entry number `pos`: 0 is ".", 1 is "..", then the names.
    pub fn readdir(&self, dir: Ino, pos: u64) -> KResult<Option<(Ino, u8, Vec<u8>)>> {
        let NodeKind::Dir { entries, parent } = &self.node(dir)?.kind else { return Err(ENOTDIR) };
        match pos {
            0 => Ok(Some((dir, DT_DIR, b".".to_vec()))),
            1 => Ok(Some((*parent, DT_DIR, b"..".to_vec()))),
            _ => Ok(entries.iter().nth((pos - 2) as usize).map(|(name, &ino)| {
                let t = match self.nodes.get(&ino).map(|n| &n.kind) {
                    Some(NodeKind::Dir { .. }) => DT_DIR,
                    Some(NodeKind::Dev(_)) => DT_CHR,
                    _ => DT_REG,
                };
                (ino, t, name.clone())
            })),
        }
    }

    pub fn stat(&self, ino: Ino) -> KResult<MStat> {
        let n = self.node(ino)?;
        let mode = match n.kind {
            NodeKind::Dir { .. } => S_IFDIR | 0o755,
            NodeKind::File { .. } => S_IFREG | 0o644,
            NodeKind::Dev(_) => S_IFCHR | 0o666,
        };
        Ok(MStat {
            ino,
            size: self.size(ino),
            mode,
            nlink: n.nlink,
            dev: if n.fs == Fs::Ram { 0 } else { 1 },
            _pad: 0,
        })
    }

    /// Absolute path of a directory (for the jail info), best effort.
    pub fn path_of(&self, mut ino: Ino) -> Vec<u8> {
        let mut parts: Vec<Vec<u8>> = Vec::new();
        let mut guard = 0;
        while ino != self.root && guard < 64 {
            guard += 1;
            let Ok(p) = self.parent_of(ino) else { break };
            if let Ok(NodeKind::Dir { entries, .. }) = self.node(p).map(|n| &n.kind) {
                if let Some((name, _)) = entries.iter().find(|&(_, &i)| i == ino) {
                    parts.push(name.clone());
                }
            }
            ino = p;
        }
        let mut out = Vec::new();
        for p in parts.iter().rev() {
            out.push(b'/');
            out.extend_from_slice(p);
        }
        if out.is_empty() {
            out.push(b'/');
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn w(v: &Vfs, path: &str) -> KResult<Walk> {
        v.walk(v.root, v.root, path.as_bytes(), false)
    }

    #[test]
    fn boot_layout() {
        let v = Vfs::new();
        assert!(w(&v, "/dev/null").unwrap().target.is_some());
        assert!(w(&v, "/dev/console").unwrap().target == Some(v.console));
        assert!(v.is_dir(w(&v, "/tmp").unwrap().target.unwrap()));
        let motd = w(&v, "/etc/motd").unwrap().target.unwrap();
        assert!(v.file_data(motd).unwrap().starts_with(b"Welcome to Magia"));
    }

    #[test]
    fn walk_basic_and_missing() {
        let v = Vfs::new();
        let r = w(&v, "/tmp/newfile").unwrap();
        assert_eq!(r.name, b"newfile");
        assert_eq!(r.target, None);
        assert_eq!(w(&v, "/nope/x"), Err(ENOENT));
        assert_eq!(w(&v, ""), Err(ENOENT));
        assert_eq!(w(&v, "/etc/motd/x"), Err(ENOTDIR));
        assert_eq!(w(&v, "/").unwrap().target, Some(v.root));
        assert_eq!(w(&v, "//./tmp/").unwrap().name, b"tmp");
    }

    #[test]
    fn dotdot_stops_at_root() {
        let v = Vfs::new();
        let tmp = w(&v, "/tmp").unwrap().target.unwrap();
        assert_eq!(v.walk(tmp, tmp, b"../../..", false).unwrap().target, Some(tmp));
        // jailed at /tmp: "/../etc" is /tmp/etc, which does not exist
        assert_eq!(v.walk(tmp, tmp, b"/../etc", false).unwrap().target, None);
        assert_eq!(v.walk(tmp, tmp, b"/../etc/motd", false), Err(ENOENT));
        assert_eq!(w(&v, "/../../etc/motd").unwrap().name, b"motd");
    }

    #[test]
    fn beneath_rules() {
        let mut v = Vfs::new();
        let tmp = w(&v, "/tmp").unwrap().target.unwrap();
        let a = v.mkdir(tmp, b"a").unwrap();
        v.mkdir(a, b"b").unwrap();
        assert_eq!(v.walk(v.root, tmp, b"/etc", true), Err(ENOTCAPABLE));
        assert_eq!(v.walk(v.root, tmp, b"..", true), Err(ENOTCAPABLE));
        assert_eq!(v.walk(v.root, tmp, b"a/../../etc", true), Err(ENOTCAPABLE));
        assert_eq!(v.walk(v.root, tmp, b"a/b/../..", true).unwrap().target, Some(tmp));
        assert_eq!(v.walk(v.root, tmp, b"a/./b", true).unwrap().name, b"b");
    }

    #[test]
    fn name_limits() {
        let v = Vfs::new();
        let long = "x".repeat(NAME_MAX + 1);
        assert_eq!(w(&v, &alloc::format!("/tmp/{long}")), Err(ENAMETOOLONG));
        let ok = "y".repeat(NAME_MAX);
        assert!(w(&v, &alloc::format!("/tmp/{ok}")).is_ok());
        let path = "/a".repeat(PATH_MAX / 2);
        assert_eq!(w(&v, &path), Err(ENAMETOOLONG));
    }

    #[test]
    fn devfs_is_read_only() {
        let mut v = Vfs::new();
        let dev = v.dev;
        assert_eq!(v.create(dev, b"x"), Err(EROFS));
        assert_eq!(v.mkdir(dev, b"x"), Err(EROFS));
        assert_eq!(v.unlink(dev, b"null", false), Err(EROFS));
        let root = v.root;
        assert_eq!(v.unlink(root, b"dev", true), Err(EBUSY));
    }

    #[test]
    fn unlink_rules() {
        let mut v = Vfs::new();
        let tmp = w(&v, "/tmp").unwrap().target.unwrap();
        let d = v.mkdir(tmp, b"d").unwrap();
        v.create(d, b"f").unwrap();
        assert_eq!(v.unlink(tmp, b"d", false), Err(EISDIR));
        assert_eq!(v.unlink(tmp, b"d", true), Err(ENOTEMPTY));
        assert_eq!(v.unlink(d, b"f", true), Err(ENOTDIR));
        v.unlink(d, b"f", false).unwrap();
        v.unlink(tmp, b"d", true).unwrap();
        assert_eq!(v.unlink(tmp, b"d", true), Err(ENOENT));
        assert_eq!(v.mkdir(tmp, b""), Err(EEXIST));
    }

    #[test]
    fn open_file_survives_unlink() {
        let mut v = Vfs::new();
        let tmp = w(&v, "/tmp").unwrap().target.unwrap();
        let f = v.create(tmp, b"f").unwrap();
        v.write(f, 0, b"hi").unwrap();
        v.open_ref(f);
        v.unlink(tmp, b"f", false).unwrap();
        assert_eq!(v.file_data(f).unwrap(), b"hi");
        v.close_ref(f);
        assert!(!v.exists(f));
    }

    #[test]
    fn file_size_limit() {
        let mut v = Vfs::new();
        let tmp = w(&v, "/tmp").unwrap().target.unwrap();
        let f = v.create(tmp, b"big").unwrap();
        assert_eq!(v.write(f, MAX_FILE_SIZE, b"x"), Err(EFBIG));
        assert_eq!(v.write(f, 10, b"x").unwrap(), 1);
        assert_eq!(v.size(f), 11);
    }

    #[test]
    fn readdir_lists_dot_entries_first() {
        let v = Vfs::new();
        let names: Vec<Vec<u8>> = (0..10).map_while(|i| v.readdir(v.dev, i).unwrap()).map(|e| e.2).collect();
        assert_eq!(names, [&b"."[..], b"..", b"console", b"null", b"zero"]);
        assert_eq!(v.path_of(v.dev), b"/dev");
    }
}
