// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! File system calls: openat, read, write, close, lseek, mkdirat,
//! unlinkat, getdents, fstat, dup, pipe.
//!
//! Every descriptor operation checks Capsicum rights first (ENOTCAPABLE),
//! then the open mode (EBADF). `io_read`/`io_write` are the single I/O
//! path: read(2), write(2) and ring READ/WRITE ops all go through them,
//! so rights checks and user-pointer validation cannot drift apart.

use crate::abi::*;
use crate::kenv;
use crate::kernel::{FileKind, Kernel, WaitKey};
use crate::pipe::Pipe;
use crate::syscall::{Ctx, Ret, SysResult, val};
use crate::uaccess::*;
use crate::vfs::{DevKind, Ino, NodeKind};
use alloc::vec::Vec;

pub enum Io {
    Done(i64),
    WouldBlock(WaitKey),
}

impl Kernel {
    /// Shared read path. `off` = Some(pos) is a positioned read (needs
    /// CAP_SEEK as well, like FreeBSD's CAP_PREAD).
    pub fn io_read(&mut self, pid: i32, fd: i64, uaddr: u64, len: u64, off: Option<u64>) -> KResult<Io> {
        let need = CAP_READ | if off.is_some() { CAP_SEEK } else { 0 };
        let e = self.fd_get(pid, fd, need)?;
        check_range(uaddr, len)?;
        let f = &self.files[&e.file];
        if !f.readable() {
            return Err(EBADF);
        }
        let (kind, offset) = (f.kind, f.offset);
        match kind {
            FileKind::Vnode(ino) => {
                let n = match &self.vfs.node(ino)?.kind {
                    NodeKind::Dir { .. } => return Err(EISDIR),
                    NodeKind::Dev(DevKind::Null) | NodeKind::Dev(DevKind::Console) => 0,
                    NodeKind::Dev(DevKind::Zero) => {
                        let zeros = [0u8; 256];
                        let mut done = 0u64;
                        while done < len {
                            let n = (len - done).min(256);
                            copyout(uaddr + done, &zeros[..n as usize])?;
                            done += n;
                        }
                        len
                    }
                    NodeKind::File { data } => {
                        let pos = off.unwrap_or(offset);
                        let size = data.len() as u64;
                        let n = if pos >= size { 0 } else { len.min(size - pos) };
                        copyout(uaddr, &data[pos as usize..(pos + n) as usize])?;
                        if off.is_none() {
                            self.files.get_mut(&e.file).unwrap().offset = pos + n;
                        }
                        n
                    }
                };
                Ok(Io::Done(n as i64))
            }
            FileKind::PipeR(p) => {
                if off.is_some() {
                    return Err(ESPIPE);
                }
                let pipe = self.pipes.get_mut(&p).ok_or(EBADF)?;
                if len == 0 {
                    return Ok(Io::Done(0));
                }
                if pipe.avail() == 0 {
                    return if pipe.writers == 0 { Ok(Io::Done(0)) } else { Ok(Io::WouldBlock(WaitKey::Pipe(p))) };
                }
                let n = (len as usize).min(pipe.avail());
                let s = pipe.buf.make_contiguous();
                copyout(uaddr, &s[..n])?;
                pipe.buf.drain(..n);
                self.wake(WaitKey::Pipe(p));
                self.wake(WaitKey::Kevent);
                Ok(Io::Done(n as i64))
            }
            _ => Err(EINVAL),
        }
    }

    /// Shared write path (write(2) and ring WRITE ops).
    pub fn io_write(&mut self, pid: i32, fd: i64, uaddr: u64, len: u64, off: Option<u64>) -> KResult<Io> {
        let need = CAP_WRITE | if off.is_some() { CAP_SEEK } else { 0 };
        let e = self.fd_get(pid, fd, need)?;
        check_range(uaddr, len)?;
        let f = &self.files[&e.file];
        if !f.writable() {
            return Err(EBADF);
        }
        let (kind, offset, append) = (f.kind, f.offset, f.append);
        match kind {
            FileKind::Vnode(ino) => {
                let dev = match &self.vfs.node(ino)?.kind {
                    NodeKind::Dir { .. } => return Err(EISDIR),
                    NodeKind::Dev(d) => Some(*d),
                    NodeKind::File { .. } => None,
                };
                match dev {
                    Some(DevKind::Console) => {
                        // bounded kernel buffer: copy and print in chunks
                        let mut chunk = [0u8; 256];
                        let mut done = 0u64;
                        while done < len {
                            let n = (len - done).min(256) as usize;
                            copyin(&mut chunk[..n], uaddr + done)?;
                            kenv::console_write(&chunk[..n]);
                            done += n as u64;
                        }
                        Ok(Io::Done(len as i64))
                    }
                    Some(_) => Ok(Io::Done(len as i64)),
                    None => {
                        if len == 0 {
                            return Ok(Io::Done(0));
                        }
                        let pos = match off {
                            Some(p) => p,
                            None if append => self.vfs.size(ino),
                            None => offset,
                        };
                        let old = self.vfs.size(ino) as usize;
                        let data = self.vfs.file_data_for_write(ino, pos, len)?;
                        if let Err(e) = copyin(&mut data[pos as usize..(pos + len) as usize], uaddr) {
                            data.truncate(old.max(data.len().min(old)));
                            data.truncate(old);
                            return Err(e);
                        }
                        if off.is_none() {
                            self.files.get_mut(&e.file).unwrap().offset = pos + len;
                        }
                        Ok(Io::Done(len as i64))
                    }
                }
            }
            FileKind::PipeW(p) => {
                if off.is_some() {
                    return Err(ESPIPE);
                }
                let pipe = self.pipes.get_mut(&p).ok_or(EBADF)?;
                if pipe.readers == 0 {
                    return Err(EPIPE);
                }
                if len == 0 {
                    return Ok(Io::Done(0));
                }
                if pipe.space() == 0 {
                    return Ok(Io::WouldBlock(WaitKey::Pipe(p)));
                }
                let n = (len as usize).min(pipe.space());
                let tmp = copyin_vec(uaddr, n)?;
                let pipe = self.pipes.get_mut(&p).unwrap();
                pipe.buf.extend(tmp.iter());
                self.wake(WaitKey::Pipe(p));
                self.wake(WaitKey::Kevent);
                Ok(Io::Done(n as i64))
            }
            _ => Err(EINVAL),
        }
    }

    fn io_block(&mut self, c: &Ctx, r: KResult<Io>) -> SysResult {
        match r? {
            Io::Done(n) => val(n),
            Io::WouldBlock(k) => {
                self.wait_on(k, c.tid);
                Ok(Ret::Block(0))
            }
        }
    }

    pub fn sys_read(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let r = self.io_read(c.pid, a[0] as i64, a[1], a[2], None);
        self.io_block(c, r)
    }

    pub fn sys_write(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let r = self.io_write(c.pid, a[0] as i64, a[1], a[2], None);
        self.io_block(c, r)
    }

    /// Where a *at() lookup starts: (root, start dir, beneath, dirfd rights).
    /// AT_FDCWD is refused in capability mode (ECAPMODE); a dirfd needs
    /// CAP_LOOKUP plus `need`.
    fn at_start(&self, pid: i32, dirfd: i64, need: u64) -> KResult<(Ino, Ino, bool, u64)> {
        let p = self.proc(pid)?;
        if dirfd == AT_FDCWD {
            if p.cap_mode {
                return Err(ECAPMODE);
            }
            return Ok((p.root, p.cwd, false, CAP_ALL));
        }
        let e = self.fd_get(pid, dirfd, CAP_LOOKUP | need)?;
        match self.files[&e.file].kind {
            FileKind::Vnode(ino) if self.vfs.is_dir(ino) => Ok((p.root, ino, p.cap_mode, e.rights)),
            _ => Err(ENOTDIR),
        }
    }

    pub fn sys_openat(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let flags = a[2];
        let acc = flags & O_ACCMODE;
        if acc == O_ACCMODE {
            return Err(EINVAL);
        }
        let mut need = 0;
        if acc != O_WRONLY {
            need |= CAP_READ;
        }
        if acc != O_RDONLY {
            need |= CAP_WRITE;
        }
        if flags & O_CREAT != 0 {
            need |= CAP_CREATE;
        }
        if flags & O_TRUNC != 0 {
            need |= CAP_FTRUNCATE;
        }
        let path = copyin_str(a[1], PATH_MAX)?;
        let (root, start, beneath, rights) = self.at_start(c.pid, a[0] as i64, need)?;
        let w = self.vfs.walk(root, start, &path, beneath)?;
        let target = match w.target {
            Some(t) => {
                if flags & O_CREAT != 0 && flags & O_EXCL != 0 {
                    return Err(EEXIST);
                }
                t
            }
            None => {
                if flags & O_CREAT == 0 {
                    return Err(ENOENT);
                }
                self.vfs.create(w.parent, &w.name)?
            }
        };
        let is_dir = self.vfs.is_dir(target);
        if is_dir && (acc != O_RDONLY || flags & O_TRUNC != 0) {
            return Err(EISDIR);
        }
        if flags & O_DIRECTORY != 0 && !is_dir {
            return Err(ENOTDIR);
        }
        if flags & O_TRUNC != 0 {
            self.vfs.truncate(target)?;
        }
        let id = self.file_new(FileKind::Vnode(target), acc);
        self.files.get_mut(&id).unwrap().append = flags & O_APPEND != 0;
        val(self.fd_install_new(c.pid, id, rights)? as i64)
    }

    pub fn sys_close(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        self.fd_close(c.pid, a[0] as i64)?;
        val(0)
    }

    pub fn sys_lseek(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let e = self.fd_get(c.pid, a[0] as i64, CAP_SEEK)?;
        let f = &self.files[&e.file];
        let ino = match f.kind {
            FileKind::Vnode(ino) => ino,
            _ => return Err(ESPIPE),
        };
        if matches!(self.vfs.node(ino)?.kind, NodeKind::Dev(_)) {
            return val(0);
        }
        let base = match a[2] as i64 {
            SEEK_SET => 0,
            SEEK_CUR => f.offset as i64,
            _ => self.vfs.size(ino) as i64,
        };
        let new = base.checked_add(a[1] as i64).ok_or(EINVAL)?;
        if new < 0 {
            return Err(EINVAL);
        }
        self.files.get_mut(&e.file).unwrap().offset = new as u64;
        val(new)
    }

    pub fn sys_mkdirat(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let path = copyin_str(a[1], PATH_MAX)?;
        let (root, start, beneath, _) = self.at_start(c.pid, a[0] as i64, CAP_MKDIRAT)?;
        let w = self.vfs.walk(root, start, &path, beneath)?;
        if w.target.is_some() || w.name.is_empty() {
            return Err(EEXIST);
        }
        self.vfs.mkdir(w.parent, &w.name)?;
        val(0)
    }

    pub fn sys_unlinkat(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let path = copyin_str(a[1], PATH_MAX)?;
        let (root, start, beneath, _) = self.at_start(c.pid, a[0] as i64, CAP_UNLINKAT)?;
        let w = self.vfs.walk(root, start, &path, beneath)?;
        self.vfs.unlink(w.parent, &w.name, a[2] & AT_REMOVEDIR != 0)?;
        val(0)
    }

    pub fn sys_getdents(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let e = self.fd_get(c.pid, a[0] as i64, CAP_READ)?;
        let f = &self.files[&e.file];
        let ino = match f.kind {
            FileKind::Vnode(ino) if self.vfs.is_dir(ino) => ino,
            _ => return Err(ENOTDIR),
        };
        let sz = core::mem::size_of::<MDirent>();
        if (a[2] as usize) < sz {
            return Err(EINVAL);
        }
        let mut pos = f.offset;
        let mut out: Vec<u8> = Vec::new();
        while out.len() + sz <= a[2] as usize {
            let Some((dino, typ, name)) = self.vfs.readdir(ino, pos)? else { break };
            let mut d = MDirent { ino: dino, typ, namelen: name.len() as u8, name: [0; DIRENT_NAME_MAX] };
            put_cstr(&mut d.name, &name);
            out.extend_from_slice(d.as_bytes());
            pos += 1;
        }
        copyout(a[1], &out)?;
        self.files.get_mut(&e.file).unwrap().offset = pos;
        val(out.len() as i64)
    }

    pub fn sys_fstat(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let e = self.fd_get(c.pid, a[0] as i64, CAP_FSTAT)?;
        let st = match self.files[&e.file].kind {
            FileKind::Vnode(ino) => self.vfs.stat(ino)?,
            FileKind::PipeR(p) | FileKind::PipeW(p) => MStat {
                ino: p as u64,
                size: self.pipes.get(&p).map_or(0, |x| x.avail() as u64),
                mode: S_IFIFO | 0o600,
                nlink: 1,
                dev: 2,
                _pad: 0,
            },
            FileKind::Kq(id) | FileKind::Ring(id) => MStat { ino: id as u64, size: 0, mode: 0o600, nlink: 1, dev: 3, _pad: 0 },
        };
        copyout_val(a[1], &st)?;
        val(0)
    }

    /// dup: lowest free descriptor, same open file, same rights.
    pub fn sys_dup(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let e = self.fd_get(c.pid, a[0] as i64, 0)?;
        val(self.fd_install(c.pid, e.file, e.rights)? as i64)
    }

    pub fn sys_pipe(&mut self, c: &mut Ctx, a: &[u64; 6]) -> SysResult {
        let id = self.new_id();
        self.pipes.insert(id, Pipe::new());
        let r = self.file_new(FileKind::PipeR(id), O_RDONLY);
        let w = self.file_new(FileKind::PipeW(id), O_WRONLY);
        let rfd = match self.fd_install_new(c.pid, r, CAP_ALL) {
            Ok(fd) => fd,
            Err(e) => {
                self.files.get_mut(&w).unwrap().refs = 1;
                self.file_unref(w);
                return Err(e);
            }
        };
        let wfd = match self.fd_install_new(c.pid, w, CAP_ALL) {
            Ok(fd) => fd,
            Err(e) => {
                let _ = self.fd_close(c.pid, rfd as i64);
                return Err(e);
            }
        };
        let mut out = [0u8; 8];
        out[..4].copy_from_slice(&rfd.to_le_bytes());
        out[4..].copy_from_slice(&wfd.to_le_bytes());
        if let Err(e) = copyout(a[0], &out) {
            let _ = self.fd_close(c.pid, rfd as i64);
            let _ = self.fd_close(c.pid, wfd as i64);
            return Err(e);
        }
        val(0)
    }
}

#[cfg(test)]
mod tests {
    use crate::abi::*;
    use crate::testutil::*;

    #[test]
    fn create_write_read_back() {
        let (mut k, pid) = kernel_with_proc();
        let fd = open(&mut k, pid, "/tmp/a", O_CREAT | O_RDWR);
        assert_eq!(fd, 3);
        assert_eq!(write_str(&mut k, pid, fd, "hello world"), 11);
        assert_eq!(sys(&mut k, pid, SYS_LSEEK, &[fd as u64, 6, SEEK_SET as u64]), 6);
        assert_eq!(read_vec(&mut k, pid, fd, 100).unwrap(), b"world");
        assert_eq!(read_vec(&mut k, pid, fd, 100).unwrap(), b"");
        assert_eq!(sys(&mut k, pid, SYS_CLOSE, &[fd as u64]), 0);
        assert_eq!(sys(&mut k, pid, SYS_CLOSE, &[fd as u64]), -EBADF as i64);
    }

    #[test]
    fn open_errors() {
        let (mut k, pid) = kernel_with_proc();
        assert_eq!(open(&mut k, pid, "/tmp/missing", O_RDONLY), -ENOENT as i64);
        assert_eq!(open(&mut k, pid, "/etc/motd", O_CREAT | O_EXCL | O_RDWR), -EEXIST as i64);
        assert_eq!(open(&mut k, pid, "/tmp", O_RDWR), -EISDIR as i64);
        assert_eq!(open(&mut k, pid, "/etc/motd", O_RDONLY | O_DIRECTORY), -ENOTDIR as i64);
        assert_eq!(open(&mut k, pid, "/etc/motd", 3), -EINVAL as i64);
        assert_eq!(open(&mut k, pid, "/dev/newdev", O_CREAT | O_RDWR), -EROFS as i64);
        let long = alloc::format!("/tmp/{}", "z".repeat(300));
        assert_eq!(open(&mut k, pid, &long, O_RDONLY), -ENAMETOOLONG as i64);
        let longc = alloc::format!("/tmp/{}", "z".repeat(54));
        assert_eq!(open(&mut k, pid, &longc, O_CREAT | O_RDWR), -ENAMETOOLONG as i64);
    }

    #[test]
    fn open_mode_is_enforced() {
        let (mut k, pid) = kernel_with_proc();
        let fd = open(&mut k, pid, "/etc/motd", O_RDONLY);
        assert_eq!(write_str(&mut k, pid, fd, "x"), -EBADF as i64);
        let wfd = open(&mut k, pid, "/tmp/w", O_CREAT | O_WRONLY);
        assert_eq!(read_vec(&mut k, pid, wfd, 4), Err(-EBADF as i64));
    }

    #[test]
    fn trunc_and_append() {
        let (mut k, pid) = kernel_with_proc();
        let fd = open(&mut k, pid, "/tmp/t", O_CREAT | O_WRONLY);
        write_str(&mut k, pid, fd, "abcdef");
        let fd2 = open(&mut k, pid, "/tmp/t", O_WRONLY | O_TRUNC);
        write_str(&mut k, pid, fd2, "xy");
        let fd3 = open(&mut k, pid, "/tmp/t", O_WRONLY | O_APPEND);
        write_str(&mut k, pid, fd3, "zz");
        let r = open(&mut k, pid, "/tmp/t", O_RDONLY);
        assert_eq!(read_vec(&mut k, pid, r, 64).unwrap(), b"xyzz");
    }

    #[test]
    fn write_faulting_buffer_leaves_file_unchanged() {
        let (mut k, pid) = kernel_with_proc();
        let fd = open(&mut k, pid, "/tmp/f", O_CREAT | O_RDWR);
        write_str(&mut k, pid, fd, "abc");
        assert_eq!(sys(&mut k, pid, SYS_WRITE, &[fd as u64, 0x3000, 10]), -EFAULT as i64);
        assert_eq!(k.vfs.size(k.vfs.walk(k.vfs.root, k.vfs.root, b"/tmp/f", false).unwrap().target.unwrap()), 3);
    }

    #[test]
    fn efbig() {
        let (mut k, pid) = kernel_with_proc();
        let fd = open(&mut k, pid, "/tmp/big", O_CREAT | O_RDWR);
        sys(&mut k, pid, SYS_LSEEK, &[fd as u64, crate::vfs::MAX_FILE_SIZE, SEEK_SET as u64]);
        assert_eq!(write_str(&mut k, pid, fd, "x"), -EFBIG as i64);
    }

    #[test]
    fn lseek_rules() {
        let (mut k, pid) = kernel_with_proc();
        let fd = open(&mut k, pid, "/etc/motd", O_RDONLY);
        let end = sys(&mut k, pid, SYS_LSEEK, &[fd as u64, 0, SEEK_END as u64]);
        assert!(end > 10);
        assert_eq!(sys(&mut k, pid, SYS_LSEEK, &[fd as u64, (-1i64) as u64, SEEK_CUR as u64]), end - 1);
        assert_eq!(sys(&mut k, pid, SYS_LSEEK, &[fd as u64, (-1000i64) as u64, SEEK_SET as u64]), -EINVAL as i64);
        let (r, _w) = make_pipe(&mut k, pid);
        assert_eq!(sys(&mut k, pid, SYS_LSEEK, &[r as u64, 0, 0]), -ESPIPE as i64);
        assert_eq!(sys(&mut k, pid, SYS_LSEEK, &[1, 0, 0]), 0); // console
    }

    #[test]
    fn devices() {
        let (mut k, pid) = kernel_with_proc();
        let z = open(&mut k, pid, "/dev/zero", O_RDONLY);
        let b = umap(600);
        uwrite(b, &[7u8; 600]);
        assert_eq!(sys(&mut k, pid, SYS_READ, &[z as u64, b, 600]), 600);
        assert!(uread(b, 600).iter().all(|&x| x == 0));
        let n = open(&mut k, pid, "/dev/null", O_RDWR);
        assert_eq!(write_str(&mut k, pid, n, "gone"), 4);
        assert_eq!(read_vec(&mut k, pid, n, 10).unwrap(), b"");
        take_console();
        assert_eq!(write_str(&mut k, pid, 1, "to console"), 10);
        assert_eq!(take_console(), "to console");
    }

    #[test]
    fn mkdir_unlink_rmdir() {
        let (mut k, pid) = kernel_with_proc();
        let d = ustr("/tmp/d");
        assert_eq!(sys(&mut k, pid, SYS_MKDIRAT, &[AT_FDCWD as u64, d]), 0);
        assert_eq!(sys(&mut k, pid, SYS_MKDIRAT, &[AT_FDCWD as u64, d]), -EEXIST as i64);
        let f = open(&mut k, pid, "/tmp/d/f", O_CREAT | O_RDWR);
        assert!(f >= 0);
        assert_eq!(sys(&mut k, pid, SYS_UNLINKAT, &[AT_FDCWD as u64, d, AT_REMOVEDIR]), -ENOTEMPTY as i64);
        assert_eq!(sys(&mut k, pid, SYS_UNLINKAT, &[AT_FDCWD as u64, d, 0]), -EISDIR as i64);
        let fp = ustr("/tmp/d/f");
        assert_eq!(sys(&mut k, pid, SYS_UNLINKAT, &[AT_FDCWD as u64, fp, 0]), 0);
        // the open fd still works after unlink
        assert_eq!(write_str(&mut k, pid, f, "ok"), 2);
        assert_eq!(sys(&mut k, pid, SYS_UNLINKAT, &[AT_FDCWD as u64, d, AT_REMOVEDIR]), 0);
        let root = ustr("/");
        assert_eq!(sys(&mut k, pid, SYS_UNLINKAT, &[AT_FDCWD as u64, root, AT_REMOVEDIR]), -EBUSY as i64);
        let dev = ustr("/dev");
        assert_eq!(sys(&mut k, pid, SYS_UNLINKAT, &[AT_FDCWD as u64, dev, AT_REMOVEDIR]), -EBUSY as i64);
        let null = ustr("/dev/null");
        assert_eq!(sys(&mut k, pid, SYS_UNLINKAT, &[AT_FDCWD as u64, null, 0]), -EROFS as i64);
    }

    #[test]
    fn getdents_lists_entries() {
        let (mut k, pid) = kernel_with_proc();
        let fd = open(&mut k, pid, "/dev", O_RDONLY | O_DIRECTORY);
        let b = umap(64 * 8);
        let n = sys(&mut k, pid, SYS_GETDENTS, &[fd as u64, b, 64 * 8]);
        assert_eq!(n, 5 * 64);
        let raw = uread(b, n as usize);
        let names: Vec<&[u8]> = raw.chunks(64).map(|r| cstr_bytes(&r[10..])).collect();
        assert_eq!(names, [&b"."[..], b"..", b"console", b"null", b"zero"]);
        assert_eq!(raw[64 * 2 + 8], DT_CHR);
        assert_eq!(sys(&mut k, pid, SYS_GETDENTS, &[fd as u64, b, 64]), 0); // at end
        assert_eq!(sys(&mut k, pid, SYS_GETDENTS, &[fd as u64, b, 63]), -EINVAL as i64);
        let f = open(&mut k, pid, "/etc/motd", O_RDONLY);
        assert_eq!(sys(&mut k, pid, SYS_GETDENTS, &[f as u64, b, 64]), -ENOTDIR as i64);
        assert_eq!(read_vec(&mut k, pid, fd, 4), Err(-EISDIR as i64));
    }

    #[test]
    fn fstat_kinds() {
        let (mut k, pid) = kernel_with_proc();
        let st = umap(32);
        let f = open(&mut k, pid, "/etc/motd", O_RDONLY);
        assert_eq!(sys(&mut k, pid, SYS_FSTAT, &[f as u64, st]), 0);
        let s: MStat = uget(st);
        assert_eq!(s.mode & S_IFMT, S_IFREG);
        assert!(s.size > 0);
        assert_eq!(sys(&mut k, pid, SYS_FSTAT, &[0, st]), 0);
        assert_eq!(uget::<MStat>(st).mode & S_IFMT, S_IFCHR);
        let (r, _) = make_pipe(&mut k, pid);
        sys(&mut k, pid, SYS_FSTAT, &[r as u64, st]);
        assert_eq!(uget::<MStat>(st).mode & S_IFMT, S_IFIFO);
    }

    #[test]
    fn dup_shares_offset_and_rights() {
        let (mut k, pid) = kernel_with_proc();
        let f = open(&mut k, pid, "/etc/motd", O_RDONLY);
        assert_eq!(sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[f as u64, CAP_READ]), 0);
        let d = sys(&mut k, pid, SYS_DUP, &[f as u64]);
        assert_eq!(d, f + 1);
        read_vec(&mut k, pid, f, 3).unwrap();
        let r = umap(8);
        sys(&mut k, pid, SYS_CAP_RIGHTS_GET, &[d as u64, r]);
        assert_eq!(uget::<u64>(r), CAP_READ);
        // shared offset: the dup continues where f stopped
        assert_eq!(read_vec(&mut k, pid, d, 4).unwrap(), b"come");
    }

    #[test]
    fn fd_table_exhaustion() {
        let (mut k, pid) = kernel_with_proc();
        for _ in 3..64 {
            assert!(sys(&mut k, pid, SYS_DUP, &[0]) > 0);
        }
        assert_eq!(sys(&mut k, pid, SYS_DUP, &[0]), -EMFILE as i64);
        assert_eq!(open(&mut k, pid, "/etc/motd", O_RDONLY), -EMFILE as i64);
        let fds = umap(8);
        assert_eq!(sys(&mut k, pid, SYS_PIPE, &[fds]), -EMFILE as i64);
        assert!(k.pipes.is_empty(), "failed pipe() leaks nothing");
    }

    #[test]
    fn pipe_basic_and_eof() {
        let (mut k, pid) = kernel_with_proc();
        let (r, w) = make_pipe(&mut k, pid);
        assert_eq!(write_str(&mut k, pid, w, "ping"), 4);
        assert_eq!(read_vec(&mut k, pid, r, 2).unwrap(), b"pi");
        assert_eq!(read_vec(&mut k, pid, r, 10).unwrap(), b"ng");
        sys(&mut k, pid, SYS_CLOSE, &[w as u64]);
        assert_eq!(read_vec(&mut k, pid, r, 10).unwrap(), b""); // EOF
        assert_eq!(read_vec(&mut k, pid, w, 1), Err(-EBADF as i64));
    }

    #[test]
    fn pipe_epipe_when_no_readers() {
        let (mut k, pid) = kernel_with_proc();
        let (r, w) = make_pipe(&mut k, pid);
        sys(&mut k, pid, SYS_CLOSE, &[r as u64]);
        assert_eq!(write_str(&mut k, pid, w, "x"), -EPIPE as i64);
    }

    #[test]
    fn pipe_read_blocks_and_writer_wakes() {
        let (mut k, pid) = kernel_with_proc();
        let (r, w) = make_pipe(&mut k, pid);
        let b = umap(16);
        let mut c = crate::syscall::Ctx::new(pid, -1);
        assert!(is_block(&call_ctx(&mut k, &mut c, SYS_READ, &[r as u64, b, 16])));
        take_woken();
        write_str(&mut k, pid, w, "hey");
        assert!(take_woken().contains(&k.proc(pid).unwrap().tid));
        c.retry = 1;
        assert_eq!(ret_val(call_ctx(&mut k, &mut c, SYS_READ, &[r as u64, b, 16])), 3);
        assert!(k.waits.is_empty());
    }

    #[test]
    fn pipe_write_partial_then_blocks() {
        let (mut k, pid) = kernel_with_proc();
        let (_r, w) = make_pipe(&mut k, pid);
        let b = umap(5000);
        assert_eq!(ret_val(call(&mut k, pid, SYS_WRITE, &[w as u64, b, 5000])), 4096);
        assert!(is_block(&call(&mut k, pid, SYS_WRITE, &[w as u64, b, 1])));
    }

    #[test]
    fn dirfd_relative_open_inherits_rights() {
        let (mut k, pid) = kernel_with_proc();
        let d = open(&mut k, pid, "/etc", O_RDONLY | O_DIRECTORY);
        assert_eq!(sys(&mut k, pid, SYS_CAP_RIGHTS_LIMIT, &[d as u64, CAP_LOOKUP | CAP_READ | CAP_FSTAT]), 0);
        let f = openat(&mut k, pid, d, "motd", O_RDONLY);
        assert!(f >= 0);
        let r = umap(8);
        sys(&mut k, pid, SYS_CAP_RIGHTS_GET, &[f as u64, r]);
        assert_eq!(uget::<u64>(r), CAP_LOOKUP | CAP_READ | CAP_FSTAT);
        assert_eq!(openat(&mut k, pid, d, "motd", O_RDWR), -ENOTCAPABLE as i64);
        assert_eq!(openat(&mut k, pid, d, "new", O_CREAT | O_RDONLY), -ENOTCAPABLE as i64);
        let notdir = open(&mut k, pid, "/etc/motd", O_RDONLY);
        assert_eq!(openat(&mut k, pid, notdir, "x", O_RDONLY), -ENOTDIR as i64);
    }
}
