// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 zenmastercat and Magia contributors
//! Mirror of `include/magia/abi.h` (system call ABI) and
//! `include/magia/sysctl.h`. Keep both sides in sync: `rs_init` compares
//! the C `sizeof()` of every ABI struct with the Rust layout at boot.

#![allow(dead_code)]

use core::mem::size_of;

// ---- system call numbers ----
pub const SYS_EXIT: u64 = 1;
pub const SYS_GETPID: u64 = 2;
pub const SYS_GETPPID: u64 = 3;
pub const SYS_READ: u64 = 4;
pub const SYS_WRITE: u64 = 5;
pub const SYS_OPENAT: u64 = 6;
pub const SYS_CLOSE: u64 = 7;
pub const SYS_LSEEK: u64 = 8;
pub const SYS_MKDIRAT: u64 = 9;
pub const SYS_UNLINKAT: u64 = 10;
pub const SYS_GETDENTS: u64 = 11;
pub const SYS_FSTAT: u64 = 12;
pub const SYS_DUP: u64 = 13;
pub const SYS_PIPE: u64 = 14;
pub const SYS_SCHED_YIELD: u64 = 20;
pub const SYS_NANOSLEEP: u64 = 21;
pub const SYS_SCHED_SETWEIGHT: u64 = 22;
pub const SYS_SETPRIORITY: u64 = 23;
pub const SYS_PROCINFO: u64 = 24;
pub const SYS_PSLIST: u64 = 25;
pub const SYS_WAITPID: u64 = 26;
pub const SYS_CAP_ENTER: u64 = 30;
pub const SYS_CAP_GETMODE: u64 = 31;
pub const SYS_CAP_RIGHTS_LIMIT: u64 = 32;
pub const SYS_CAP_RIGHTS_GET: u64 = 33;
pub const SYS_JAIL_CREATE: u64 = 40;
pub const SYS_JAIL_ATTACH: u64 = 41;
pub const SYS_JAIL_GET: u64 = 42;
pub const SYS_GETHOSTNAME: u64 = 43;
pub const SYS_SETHOSTNAME: u64 = 44;
pub const SYS_JAIL_SET: u64 = 45;
pub const SYS_KQUEUE: u64 = 50;
pub const SYS_KEVENT: u64 = 51;
pub const SYS_URING_SETUP: u64 = 60;
pub const SYS_URING_ENTER: u64 = 61;
pub const SYS_FUTEX: u64 = 70;
pub const SYS_SYSCTL: u64 = 80;
pub const SYS_UNAME: u64 = 81;
pub const SYS_MMAP: u64 = 90;
pub const SYS_MUNMAP: u64 = 91;
pub const SYS_MAX: u64 = 92;

// ---- errno (positive values; syscalls return -errno) ----
pub type Errno = i32;
pub type KResult<T> = Result<T, Errno>;

pub const EPERM: Errno = 1;
pub const ENOENT: Errno = 2;
pub const ESRCH: Errno = 3;
pub const EINTR: Errno = 4;
pub const EIO: Errno = 5;
pub const E2BIG: Errno = 7;
pub const EBADF: Errno = 9;
pub const ECHILD: Errno = 10;
pub const EAGAIN: Errno = 11;
pub const ENOMEM: Errno = 12;
pub const EACCES: Errno = 13;
pub const EFAULT: Errno = 14;
pub const EBUSY: Errno = 16;
pub const EEXIST: Errno = 17;
pub const ENOTDIR: Errno = 20;
pub const EISDIR: Errno = 21;
pub const EINVAL: Errno = 22;
pub const EMFILE: Errno = 24;
pub const EFBIG: Errno = 27;
pub const ENOSPC: Errno = 28;
pub const ESPIPE: Errno = 29;
pub const EROFS: Errno = 30;
pub const EPIPE: Errno = 32;
pub const ERANGE: Errno = 34;
pub const ENAMETOOLONG: Errno = 36;
pub const ENOSYS: Errno = 38;
pub const ENOTEMPTY: Errno = 39;
pub const ETIME: Errno = 62;
pub const ENOTCAPABLE: Errno = 93;
pub const ECAPMODE: Errno = 94;
pub const ETIMEDOUT: Errno = 110;

pub fn errno_name(e: Errno) -> &'static str {
    match e {
        EPERM => "EPERM",
        ENOENT => "ENOENT",
        ESRCH => "ESRCH",
        EINTR => "EINTR",
        EIO => "EIO",
        E2BIG => "E2BIG",
        EBADF => "EBADF",
        ECHILD => "ECHILD",
        EAGAIN => "EAGAIN",
        ENOMEM => "ENOMEM",
        EACCES => "EACCES",
        EFAULT => "EFAULT",
        EBUSY => "EBUSY",
        EEXIST => "EEXIST",
        ENOTDIR => "ENOTDIR",
        EISDIR => "EISDIR",
        EINVAL => "EINVAL",
        EMFILE => "EMFILE",
        EFBIG => "EFBIG",
        ENOSPC => "ENOSPC",
        ESPIPE => "ESPIPE",
        EROFS => "EROFS",
        EPIPE => "EPIPE",
        ERANGE => "ERANGE",
        ENAMETOOLONG => "ENAMETOOLONG",
        ENOSYS => "ENOSYS",
        ENOTEMPTY => "ENOTEMPTY",
        ETIME => "ETIME",
        ENOTCAPABLE => "ENOTCAPABLE",
        ECAPMODE => "ECAPMODE",
        ETIMEDOUT => "ETIMEDOUT",
        _ => "E?",
    }
}

// ---- open(2) ----
pub const O_RDONLY: u64 = 0x0000;
pub const O_WRONLY: u64 = 0x0001;
pub const O_RDWR: u64 = 0x0002;
pub const O_ACCMODE: u64 = 0x0003;
pub const O_APPEND: u64 = 0x0008;
pub const O_CREAT: u64 = 0x0200;
pub const O_TRUNC: u64 = 0x0400;
pub const O_EXCL: u64 = 0x0800;
pub const O_DIRECTORY: u64 = 0x20000;
pub const O_VALID: u64 = O_ACCMODE | O_APPEND | O_CREAT | O_TRUNC | O_EXCL | O_DIRECTORY;
pub const AT_FDCWD: i64 = -100;
pub const AT_REMOVEDIR: u64 = 0x800;
pub const SEEK_SET: i64 = 0;
pub const SEEK_CUR: i64 = 1;
pub const SEEK_END: i64 = 2;

pub const S_IFMT: u32 = 0o170000;
pub const S_IFIFO: u32 = 0o010000;
pub const S_IFCHR: u32 = 0o020000;
pub const S_IFDIR: u32 = 0o040000;
pub const S_IFREG: u32 = 0o100000;

pub const DT_FIFO: u8 = 1;
pub const DT_CHR: u8 = 2;
pub const DT_DIR: u8 = 4;
pub const DT_REG: u8 = 8;
pub const DIRENT_NAME_MAX: usize = 54;
/// Longest path component: 53 bytes, so `m_dirent.name` is always
/// NUL-terminated.
pub const NAME_MAX: usize = DIRENT_NAME_MAX - 1;
/// Longest path including the terminating NUL.
pub const PATH_MAX: usize = 256;

// ---- Capsicum rights ----
pub const CAP_READ: u64 = 1 << 0;
pub const CAP_WRITE: u64 = 1 << 1;
pub const CAP_SEEK: u64 = 1 << 2;
pub const CAP_MMAP: u64 = 1 << 3;
pub const CAP_EVENT: u64 = 1 << 4;
pub const CAP_FSTAT: u64 = 1 << 5;
pub const CAP_LOOKUP: u64 = 1 << 6;
pub const CAP_CREATE: u64 = 1 << 7;
pub const CAP_MKDIRAT: u64 = 1 << 8;
pub const CAP_UNLINKAT: u64 = 1 << 9;
pub const CAP_KQUEUE_CHANGE: u64 = 1 << 10;
pub const CAP_KQUEUE_EVENT: u64 = 1 << 11;
pub const CAP_URING: u64 = 1 << 12;
pub const CAP_FTRUNCATE: u64 = 1 << 13;
pub const CAP_ALL: u64 = (1 << 14) - 1;
pub const CAP_PREAD: u64 = CAP_READ | CAP_SEEK;
pub const CAP_PWRITE: u64 = CAP_WRITE | CAP_SEEK;

// ---- jails ----
pub const JAIL_ALLOW_SET_HOSTNAME: u32 = 0x1;
pub const JAIL_ALLOW_RING: u32 = 0x2;
pub const JAIL_FLAGS_VALID: u32 = JAIL_ALLOW_SET_HOSTNAME | JAIL_ALLOW_RING;
pub const JAIL_HOSTNAME_MAX: usize = 64;
pub const JAIL_PATH_MAX: usize = 128;
pub const JAIL_SET_CPU_WEIGHT: u64 = 1;
pub const JAIL_SET_MEM_MAX: u64 = 2;
pub const JAIL_SET_PIDS_MAX: u64 = 3;
pub const JAIL_MAX_DEPTH: u32 = 8;

// ---- processes ----
pub const PROC_NAME_MAX: usize = 32;
pub const WNOHANG: u64 = 1;

// ---- kqueue ----
pub const EVFILT_READ: i16 = -1;
pub const EVFILT_WRITE: i16 = -2;
pub const EVFILT_PROC: i16 = -5;
pub const EVFILT_TIMER: i16 = -7;
pub const EVFILT_USER: i16 = -11;
pub const EV_ADD: u16 = 0x0001;
pub const EV_DELETE: u16 = 0x0002;
pub const EV_ENABLE: u16 = 0x0004;
pub const EV_DISABLE: u16 = 0x0008;
pub const EV_ONESHOT: u16 = 0x0010;
pub const EV_CLEAR: u16 = 0x0020;
pub const EV_RECEIPT: u16 = 0x0040;
pub const EV_ERROR: u16 = 0x4000;
pub const EV_EOF: u16 = 0x8000;
pub const EV_CHANGE_VALID: u16 =
    EV_ADD | EV_DELETE | EV_ENABLE | EV_DISABLE | EV_ONESHOT | EV_CLEAR | EV_RECEIPT;
pub const NOTE_TRIGGER: u32 = 0x0100_0000;
pub const NOTE_EXIT: u32 = 0x8000_0000;
pub const KEVENT_MAX: i64 = 64;

// ---- io_uring-style rings ----
pub const URING_OP_NOP: u8 = 0;
pub const URING_OP_READ: u8 = 1;
pub const URING_OP_WRITE: u8 = 2;
pub const URING_OP_TIMEOUT: u8 = 3;
pub const URING_MAX_ENTRIES: u32 = 256;
pub const URING_OFF_CURRENT: u64 = 0xffff_ffff_ffff_ffff;

// ---- mmap ----
pub const PROT_READ: u64 = 0x1;
pub const PROT_WRITE: u64 = 0x2;
pub const PROT_EXEC: u64 = 0x4;
pub const MMAP_MAX_BYTES: u64 = 16 << 20;

// ---- futex ----
pub const FUTEX_WAIT: u64 = 0;
pub const FUTEX_WAKE: u64 = 1;

// ---- memory layout ----
pub const USER_TOP: u64 = 0x0000_8000_0000_0000;
pub const PAGE_SIZE: u64 = 4096;
/// uvm_alloc flags (include/magia/mm.h)
pub const UVM_EXEC: i32 = 0x1;
pub const UVM_CONTIG: i32 = 0x2;

/// Plain-old-data ABI structs: any bit pattern is valid, no implicit
/// padding, so they can be copied to/from user memory as bytes.
///
/// # Safety
/// Implement only for `#[repr(C)]` types without padding whose fields
/// are integers or arrays of integers.
pub unsafe trait Pod: Copy + 'static {
    fn zeroed() -> Self {
        // SAFETY: Pod types accept the all-zero bit pattern.
        unsafe { core::mem::zeroed() }
    }
    fn as_bytes(&self) -> &[u8] {
        // SAFETY: Pod types have no padding.
        unsafe { core::slice::from_raw_parts(self as *const Self as *const u8, size_of::<Self>()) }
    }
    fn as_bytes_mut(&mut self) -> &mut [u8] {
        // SAFETY: Pod types accept any bit pattern.
        unsafe { core::slice::from_raw_parts_mut(self as *mut Self as *mut u8, size_of::<Self>()) }
    }
}

unsafe impl Pod for u8 {}
unsafe impl Pod for u32 {}
unsafe impl Pod for i32 {}
unsafe impl Pod for u64 {}
unsafe impl Pod for i64 {}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct MStat {
    pub ino: u64,
    pub size: u64,
    pub mode: u32,
    pub nlink: u32,
    pub dev: u32,
    pub _pad: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MDirent {
    pub ino: u64,
    pub typ: u8,
    pub namelen: u8,
    pub name: [u8; DIRENT_NAME_MAX],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct JailParams {
    pub path: u64,
    pub hostname: u64,
    pub mem_max: u64,
    pub cpu_weight: u32,
    pub children_max: u32,
    pub pids_max: u32,
    pub flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct JailInfo {
    pub jid: i32,
    pub parent: i32,
    pub cpu_weight: u32,
    pub nprocs: u32,
    pub mem_max: u64,
    pub mem_cur: u64,
    pub children_max: u32,
    pub children_cur: u32,
    pub pids_max: u32,
    pub depth: u32,
    pub hostname: [u8; JAIL_HOSTNAME_MAX],
    pub path: [u8; JAIL_PATH_MAX],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProcInfo {
    pub pid: i32,
    pub ppid: i32,
    pub jid: i32,
    pub tid: i32,
    pub state: u32,
    pub nice: i32,
    pub weight: u32,
    pub cap_mode: u32,
    pub runtime_ns: u64,
    pub mem_bytes: u64,
    pub name: [u8; PROC_NAME_MAX],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Utsname {
    pub sysname: [u8; 32],
    pub nodename: [u8; 64],
    pub release: [u8; 32],
    pub version: [u8; 64],
    pub machine: [u8; 32],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KEvent {
    pub ident: u64,
    pub filter: i16,
    pub flags: u16,
    pub fflags: u32,
    pub data: i64,
    pub udata: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UringSqe {
    pub opcode: u8,
    pub flags: u8,
    pub _pad: u16,
    pub fd: i32,
    pub off: u64,
    pub addr: u64,
    pub len: u32,
    pub _pad2: u32,
    pub user_data: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct UringCqe {
    pub user_data: u64,
    pub res: i32,
    pub flags: u32,
}

/// Byte offsets of the shared ring header fields (struct uring_ring_hdr).
pub mod ringhdr {
    pub const SQ_HEAD: usize = 0;
    pub const SQ_TAIL: usize = 4;
    pub const SQ_MASK: usize = 8;
    pub const SQ_ENTRIES: usize = 12;
    pub const SQ_DROPPED: usize = 16;
    pub const SQ_FLAGS: usize = 20;
    pub const CQ_HEAD: usize = 32;
    pub const CQ_TAIL: usize = 36;
    pub const CQ_MASK: usize = 40;
    pub const CQ_ENTRIES: usize = 44;
    pub const CQ_OVERFLOW: usize = 48;
    pub const SIZE: usize = 64;
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UringParams {
    pub sq_entries: u32,
    pub cq_entries: u32,
    pub ring_addr: u64,
    pub ring_size: u64,
    pub sq_array_off: u64,
    pub sqes_off: u64,
    pub cqes_off: u64,
}

unsafe impl Pod for MStat {}
unsafe impl Pod for MDirent {}
unsafe impl Pod for JailParams {}
unsafe impl Pod for JailInfo {}
unsafe impl Pod for ProcInfo {}
unsafe impl Pod for Utsname {}
unsafe impl Pod for KEvent {}
unsafe impl Pod for UringSqe {}
unsafe impl Pod for UringCqe {}
unsafe impl Pod for UringParams {}

/// struct c_sysctl from include/magia/sysctl.h
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CSysctl {
    pub name: *const u8,
    pub kind: u32,
    pub flags: u32,
    pub ptr: *mut u8,
    pub descr: *const u8,
    pub min: u64,
    pub max: u64,
}
pub const CSYSCTL_U64_RO: u32 = 1;
pub const CSYSCTL_STR_RO: u32 = 2;
pub const CSYSCTL_U64_RW: u32 = 3;
pub const CSYSCTL_F_CAPRD: u32 = 0x1;

/// struct rs_boot_info from include/magia/rust.h
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RsBootInfo {
    pub abi_sizes: [u64; 8],
    pub total_pages: u64,
    pub hz: u64,
    pub cpu_flags: u32,
    pub _pad: u32,
}
pub const RS_CPU_NX: u32 = 1;
pub const RS_CPU_SMEP: u32 = 2;
pub const RS_CPU_SMAP: u32 = 4;

/// Rust-side sizes in the order of `rs_boot_info.abi_sizes`.
pub const ABI_SIZES: [(&str, usize); 8] = [
    ("m_stat", size_of::<MStat>()),
    ("m_dirent", size_of::<MDirent>()),
    ("jail_params", size_of::<JailParams>()),
    ("jail_info", size_of::<JailInfo>()),
    ("proc_info", size_of::<ProcInfo>()),
    ("utsname", size_of::<Utsname>()),
    ("kevent", size_of::<KEvent>()),
    ("uring_sqe", size_of::<UringSqe>()),
];

const _: () = {
    assert!(size_of::<MStat>() == 32);
    assert!(size_of::<MDirent>() == 64);
    assert!(size_of::<JailParams>() == 40);
    assert!(size_of::<JailInfo>() == 240);
    assert!(size_of::<ProcInfo>() == 80);
    assert!(size_of::<Utsname>() == 224);
    assert!(size_of::<KEvent>() == 32);
    assert!(size_of::<UringSqe>() == 40);
    assert!(size_of::<UringCqe>() == 16);
    assert!(size_of::<UringParams>() == 48);
    assert!(size_of::<CSysctl>() == 48);
};

/// Copy `src` into a fixed C char array, always NUL-terminated.
pub fn put_cstr(dst: &mut [u8], src: &[u8]) {
    let n = src.len().min(dst.len().saturating_sub(1));
    dst[..n].copy_from_slice(&src[..n]);
    for b in dst[n..].iter_mut() {
        *b = 0;
    }
}

/// Bytes of a NUL-terminated C array.
pub fn cstr_bytes(src: &[u8]) -> &[u8] {
    let n = src.iter().position(|&b| b == 0).unwrap_or(src.len());
    &src[..n]
}
