# Magia system calls

Magia is an experimental research kernel. Its system call layer is written
from scratch in Rust (`rust/src/`), using public descriptions of the ideas
(papers, man pages, design articles). No code is copied from Linux or
FreeBSD. Where an idea comes from one of them, the table says so.

Constants and structures are in `include/magia/abi.h`, mirrored in
`rust/src/abi.rs`. At boot `rs_init` compares the C `sizeof()` of every ABI
struct with the Rust layout and panics if any differ.

## Conventions

- **Entry.** Ring 3 uses `syscall`: `rax` = number, arguments in `rdi, rsi,
  rdx, r10, r8, r9`, result in `rax`, and `rcx`/`r11` are clobbered.
  Kernel threads that act as processes (ktest) call `ksyscall()`. Both
  paths reach the same dispatcher, `rs_syscall(pid, nr, args)`.
- **Errors.** A call returns `-errno`, Linux style. The errno values are
  POSIX; `ENOTCAPABLE` (93) and `ECAPMODE` (94) use FreeBSD's numbers.
- **Processes.** A process is one kernel thread plus the Rust state: fd
  table (64 slots), root and cwd, jail, capability mode, and mappings.
  Processes are created by the kernel (`kproc_spawn`). There is no
  fork/exec. A new process gets fds 0, 1 and 2 open on `/dev/console`, and
  inherits its parent's jail, root, cwd and capability mode.
- **User memory.** Every user pointer must be below `USER_TOP`
  (`0x0000800000000000`), non-null, and must not wrap. `copyin`/`copyout`
  also fail with `EFAULT` on unmapped or kernel pages; they are SMAP-aware
  and recover from faults. Each user structure is copied into kernel memory
  once and validated there, so the user cannot change it between the check
  and the use.

## The table and generic validation

`rust/src/syscall.rs` holds one `SysEnt` per call. Each entry has the name,
typed argument descriptors, whether the call is allowed in capability mode,
and the handler. The descriptors are:

| Descriptor | Meaning | Error |
|---|---|---|
| `Int(lo, hi)` | signed range, inclusive | `EINVAL` |
| `Flags(mask)` | unknown bits set | `EINVAL` |
| `Fd` | 0..63 | `EBADF` |
| `FdCwd` | 0..63 or `AT_FDCWD` (-100) | `EBADF` |
| `Ptr(size)` / `OptPtr(size)` | user range; the size is a constant, another argument, or argument × element size | `EFAULT` |

Before any handler runs, the dispatcher checks in this order:

1. The number is unknown → `ENOSYS`.
2. The process is in capability mode and the call is not allowed there →
   `ECAPMODE`.
3. Integer ranges and flag masks.
4. Descriptor ranges.
5. Pointer ranges.

The handler then applies its own policy: Capsicum rights, jail rules and
resource limits.

`sysctl debug.syscall_trace=1` prints every call and its result on the
console.

## Blocking and locking

All Rust kernel state sits behind one IRQ-disabling lock, GIANT. Two other
locks never take another lock:

- **SCHED** holds the EEVDF run queue. It is used from the timer IRQ and
  the C scheduler.
- **ACCT** holds jail memory accounting. C's `uvm_alloc` calls into it,
  possibly while GIANT is held.

The lock order is GIANT → SCHED and GIANT → ACCT.

A call that must wait does this:

1. It registers the caller's thread on a wait key (pipe, child, futex word,
   kevent, ring).
2. It calls `magia_sleep_prepare(timeout)` while still holding GIANT, so a
   waker (which needs GIANT) cannot slip in.
3. It drops GIANT and calls `magia_sleep_commit()`.
4. It runs again. Handlers are restartable: progress such as "changes
   already applied" or "SQEs already consumed" is kept in a per-call
   context. Spurious wakeups are allowed.

There are no signals, so sleeps are only ended by a wakeup or a timeout.

## Calls

Cap = allowed in Capsicum capability mode.

| # | Call | Arguments → result | Errors (beyond the generic ones) | Cap | Idea from |
|---|---|---|---|---|---|
| 1 | `exit` | `(status)` → does not return | | yes | POSIX |
| 2 | `getpid` | `()` → pid | | yes | POSIX |
| 3 | `getppid` | `()` → parent pid | | yes | POSIX |
| 4 | `read` | `(fd, buf, len)` → bytes | `EBADF` (not open for reading), `ENOTCAPABLE` (no `CAP_READ`), `EISDIR` | yes | POSIX |
| 5 | `write` | `(fd, buf, len)` → bytes | `EBADF`, `ENOTCAPABLE` (`CAP_WRITE`), `EPIPE`, `EFBIG`, `EISDIR` | yes | POSIX |
| 6 | `openat` | `(dirfd, path, flags)` → fd | `ENOENT`, `EEXIST`, `EISDIR`, `ENOTDIR`, `EROFS`, `ENAMETOOLONG`, `EMFILE`, `ENOTCAPABLE`, `ECAPMODE` | yes | POSIX `openat`, Capsicum rules |
| 7 | `close` | `(fd)` → 0 | `EBADF` | yes | POSIX |
| 8 | `lseek` | `(fd, off, whence)` → new offset | `ESPIPE` (pipe, kqueue, ring), `EINVAL` (negative result), `ENOTCAPABLE` (`CAP_SEEK`) | yes | POSIX |
| 9 | `mkdirat` | `(dirfd, path)` → 0 | `EEXIST`, `ENOENT`, `EROFS`, `ENOTCAPABLE` (`CAP_MKDIRAT`) | yes | POSIX |
| 10 | `unlinkat` | `(dirfd, path, flags)` → 0 | `ENOENT`, `EISDIR` (needs `AT_REMOVEDIR`), `ENOTDIR`, `ENOTEMPTY`, `EBUSY` (`/`, `/dev`), `EROFS`, `ENOTCAPABLE` (`CAP_UNLINKAT`) | yes | POSIX |
| 11 | `getdents` | `(fd, buf, len)` → bytes of 64-byte `m_dirent` records, 0 at the end | `ENOTDIR`, `EINVAL` (len < 64), `ENOTCAPABLE` (`CAP_READ`) | yes | BSD `getdirentries` |
| 12 | `fstat` | `(fd, m_stat*)` → 0 | `ENOTCAPABLE` (`CAP_FSTAT`) | yes | POSIX |
| 13 | `dup` | `(fd)` → lowest free fd, same open file and rights | `EMFILE` | yes | POSIX |
| 14 | `pipe` | `(int32 fds[2])` → 0; `fds[0]` reads, `fds[1]` writes | `EMFILE` (nothing leaks) | yes | POSIX |
| 20 | `sched_yield` | `()` → 0; gives up the rest of the EEVDF slice | | yes | POSIX |
| 21 | `nanosleep` | `(ns)` → 0, rounded up to ms | | yes | POSIX (takes a number, not a timespec) |
| 22 | `sched_setweight` | `(pid or 0, weight 1..10000)` → 0 | `ESRCH`, `ECAPMODE` (another pid), `EPERM` (raising it inside a jail) | yes | Linux cgroup `cpu.weight` |
| 23 | `setpriority` | `(pid or 0, nice -20..19)` → 0 | as above | yes | POSIX nice |
| 24 | `procinfo` | `(pid or 0 for self, proc_info*)` → 0 | `ESRCH` (unknown or not visible) | yes | FreeBSD `kinfo_proc` |
| 25 | `pslist` | `(int32 *pids or 0, max)` → number of visible pids, writes up to `max` | | **no** | |
| 26 | `waitpid` | `(pid or -1, int32 *status or 0, WNOHANG)` → reaped pid, or 0 with `WNOHANG` | `ECHILD`, `EINVAL` (pid 0) | yes | POSIX; status is the raw exit value |
| 30 | `cap_enter` | `()` → 0; cannot be undone; inherited by children | | yes | FreeBSD Capsicum |
| 31 | `cap_getmode` | `(u32*)` → 0, writes 0 or 1 | | yes | FreeBSD Capsicum |
| 32 | `cap_rights_limit` | `(fd, rights)` → 0 | `ENOTCAPABLE` (would add a right), `EINVAL` (unknown bits) | yes | FreeBSD Capsicum |
| 33 | `cap_rights_get` | `(fd, u64*)` → 0 | | yes | FreeBSD Capsicum |
| 40 | `jail_create` | `(jail_params*)` → jid | `ENOENT`/`ENOTDIR` (path), `ENAMETOOLONG` (hostname ≥ 64), `EINVAL` (weight, limit above the parent's, unknown flags), `EPERM` (flag the parent lacks), `EAGAIN` (`children_max` reached, or depth > 8) | **no** | FreeBSD jails |
| 41 | `jail_attach` | `(jid)` → 0; root and cwd become the jail path | `ESRCH`, `EPERM` (not own jail or a descendant), `EAGAIN` (`pids_max`) | **no** | FreeBSD jails |
| 42 | `jail_get` | `(jid, jail_info*)` → 0 | `ESRCH` (not own jail or a descendant) | **no** | FreeBSD `jail_get` |
| 43 | `gethostname` | `(buf, len)` → 0, NUL-terminated | `ENAMETOOLONG` (buffer too small) | yes | POSIX, per jail (UTS namespace idea) |
| 44 | `sethostname` | `(buf, len)` → 0 | `EPERM` (jail without `JAIL_ALLOW_SET_HOSTNAME`), `ENAMETOOLONG` | **no** | as above |
| 45 | `jail_set` | `(jid, what, value)` → 0; `what` is `JAIL_SET_CPU_WEIGHT`, `_MEM_MAX` or `_PIDS_MAX` | `EPERM` (own jail: only a strict ancestor may change limits), `ESRCH`, `EINVAL` | **no** | FreeBSD `jail_set`, cgroup limits |
| 50 | `kqueue` | `()` → fd | `EMFILE` | yes | FreeBSD kqueue |
| 51 | `kevent` | `(kq, changes*, nchanges ≤ 64, events*, nevents ≤ 64, timeout_ms: -1 forever, 0 poll)` → number of events | `EBADF` (not a kqueue), `ENOTCAPABLE` (`CAP_KQUEUE_CHANGE`/`_EVENT`), any change error when there is no room to report it; per change: `ENOMEM` (a kqueue holds at most 1024 notes), `EINVAL` (TIMER/USER ident above 2^31-1) | yes | FreeBSD kqueue |
| 60 | `uring_setup` | `(entries: power of 2, 1..256; uring_params*)` → fd | `EINVAL`, `EPERM` (kill switch or jail permission), `ENOMEM` (jail limit) | yes | Linux io_uring |
| 61 | `uring_enter` | `(fd, to_submit, min_complete, flags=0)` → SQEs consumed | `EINVAL` (not a ring, `min_complete` > CQ size), `ENOTCAPABLE` (`CAP_URING`), `EPERM` (kill switch) | yes | Linux io_uring |
| 70 | `futex` | `(u32 *uaddr, FUTEX_WAIT or FUTEX_WAKE, val, timeout_ms; 0 = forever)` → WAIT 0, WAKE number woken | `EAGAIN` (`*uaddr != val`), `ETIMEDOUT`, `EINVAL` (misaligned, unknown op), `EFAULT` | yes | Linux futex |
| 80 | `sysctl` | `(name, namelen ≤ 128, oldp, u64 *oldlenp, newp, newlen)` → 0 | `ENOENT`, `EISDIR` (interior node), `ENOMEM` (buffer too small; `*oldlenp` set to the needed size), `EPERM` (read-only, or a write from a jail), `EINVAL` (range or size), `ECAPMODE` | yes (CAPRD nodes only) | FreeBSD sysctl |
| 81 | `uname` | `(utsname*)` → 0; nodename is the jail's hostname | | yes | POSIX |
| 90 | `mmap` | `(0, len 1..16 MiB, prot ⊆ READ\|WRITE)` → user address of zeroed pages | `EINVAL` (address hint, length), `EACCES` (`PROT_EXEC`: user code is mapped only by the kernel, W^X), `ENOMEM` (jail limit) | yes | POSIX, anonymous only |
| 91 | `munmap` | `(addr, len)` → 0 | `EINVAL` (must match one earlier mapping exactly) | yes | POSIX |

### VFS (`vfs.rs`, `fs.rs`)

- **File systems.** ramfs holds directories and regular files (at most
  1 MiB per file, `EFBIG`). devfs is mounted at `/dev`; its namespace is
  read-only (`EROFS`). It has `null`, `zero` and `console`; writing
  `console` prints, reading it returns 0. At boot `/tmp` and `/etc/motd`
  exist.
- **Path limits.** A path is at most 255 bytes and a component at most 53
  (`ENAMETOOLONG`).
- **Process root.** Absolute paths start at the process root, which is the
  jail root. `..` at the root stays there.
- **Pipes.** The buffer is 4 KiB. `read` blocks while the pipe is empty and
  writers remain, and returns 0 at EOF. `write` may be partial, blocks when
  the buffer is full, and returns `EPIPE` when no readers remain.
- **File descriptions.** `O_APPEND`, `O_TRUNC`, `O_EXCL` and
  `O_DIRECTORY` are supported. An open file survives `unlink`. A failed
  write leaves the file unchanged.
- **Shared I/O path.** `read`, `write` and the ring `READ`/`WRITE` ops all
  call the same two functions, `io_read` and `io_write`.

### Capsicum (`cap.rs`)

Each descriptor has a 64-bit rights mask; open and pipe give `CAP_ALL`. The
right each call needs:

| Call | Right |
|---|---|
| `read` | `CAP_READ` |
| `write` | `CAP_WRITE` |
| `lseek` | `CAP_SEEK` |
| `fstat` | `CAP_FSTAT` |
| `getdents` | `CAP_READ` |
| `openat` on the dirfd | `CAP_LOOKUP`, plus `CAP_READ`/`CAP_WRITE`/`CAP_CREATE`/`CAP_FTRUNCATE` depending on the flags |
| `mkdirat` | `CAP_LOOKUP` + `CAP_MKDIRAT` |
| `unlinkat` | `CAP_LOOKUP` + `CAP_UNLINKAT` |
| `kevent` registration on an fd | `CAP_EVENT` |
| `kevent` on the kqueue fd | `CAP_KQUEUE_CHANGE` / `CAP_KQUEUE_EVENT` |
| `uring_enter` | `CAP_URING` |
| positioned ring I/O | `CAP_SEEK` in addition |

A file opened through a dirfd inherits the dirfd's rights.

In capability mode:

- `AT_FDCWD` → `ECAPMODE`.
- Absolute paths, and any `..` that would climb above the dirfd →
  `ENOTCAPABLE`.
- `pslist`, `jail_*` and `sethostname` → `ECAPMODE`.
- Changing another pid's priority → `ECAPMODE`.
- sysctl can read only nodes marked CAPRD and cannot write anything.

### Jails and resource accounting (`jail.rs`, `acct.rs`, `proc.rs`)

The jail tree is the only resource-accounting tree. This follows the cgroup
v2 idea of a single unified hierarchy: memory, process counts and CPU
weight are all charged or scaled along the same parent chain, so no two
controllers can disagree about where a process belongs.

- **Memory.** A charge succeeds only if no ancestor would go over its
  `mem_max`. User windows from `mmap` and `uring_setup` are charged to the
  jail that was current when they were allocated, and uncharged there even
  if the process has moved since.
- **Processes.** `pids_max` is counted per subtree. Spawn and attach return
  `EAGAIN` at the limit.
- **CPU.** A thread's scheduler weight is its nice/weight value multiplied
  by `cpu_weight/100` of every jail above it.
- **Visibility.** A process sees only processes in its own jail subtree
  (the PID namespace idea). This applies to `procinfo`, `pslist`,
  `waitpid`, priorities and `EVFILT_PROC`.
- **Privileges.** `JAIL_ALLOW_SET_HOSTNAME` and `JAIL_ALLOW_RING` are
  granted only if the parent jail has them and the creator asks for them.
  Jail 0 has both.

### kqueue (`kqueue.rs`)

| Filter | Fires when | `data` |
|---|---|---|
| `EVFILT_READ` | pipe has bytes, or a file has bytes after the offset | bytes available |
| `EVFILT_WRITE` | pipe has space | free space |
| `EVFILT_PROC` + `NOTE_EXIT` | the process exits; delivered once | exit status |
| `EVFILT_TIMER` | `data` ms have passed; periodic unless `EV_ONESHOT` | expirations since the last delivery |
| `EVFILT_USER` | a change with `NOTE_TRIGGER` | 0 |

On pipes, `EV_EOF` is set when the other end is closed.

Supported flags are `EV_ADD`, `EV_DELETE`, `EV_ENABLE`, `EV_DISABLE`,
`EV_ONESHOT`, `EV_CLEAR` and `EV_RECEIPT`:

- Descriptor filters are level triggered. With `EV_CLEAR` they report only
  when `data` has changed.
- A change that fails becomes an `EV_ERROR` event with `data = errno` if
  the event list has room; otherwise the call returns that error.
- Closing a descriptor removes its notes.

### io_uring-style rings (`uring.rs`)

`uring_setup` maps one shared region into the caller's user window and
returns its offsets in `uring_params`. The region holds:

| Part | Size |
|---|---|
| `uring_ring_hdr` | 64 bytes |
| `u32` SQ index array | `entries` |
| SQEs | `entries` × 40 bytes, 64-byte aligned |
| CQEs | `2 × entries` × 16 bytes |

The user can write to the whole region at any time, so the kernel treats it
as hostile:

- It keeps private copies of `sq_head` and `cq_tail`.
- It reads `sq_tail` once per call and each index once.
- It copies each SQE before validating it.
- It consumes at most one ring's worth of entries per call.
- An index ≥ `entries` increments `sq_dropped`.
- When the completion queue is full, it increments `cq_overflow` and writes
  nothing.

Ops:

| Op | Result |
|---|---|
| `NOP` | 0 |
| `READ` / `WRITE` | as read/write; `off = URING_OFF_CURRENT` uses the file offset; a pipe that would block gives `-EAGAIN` |
| `TIMEOUT` | completes after `off` ms with `-ETIME`, from the timer tick; at most `entries` timeouts may be pending per ring, further ones complete at once with `-EAGAIN` (bounds kernel memory, which is not charged to a jail) |
| unknown opcode or non-zero SQE flags | `-EINVAL` in the CQE |

With `min_complete > 0`, `uring_enter` sleeps until that many CQEs are
waiting. It returns at once if nothing in flight could ever complete.

Attack-surface controls. In June 2023 Google reported that 60% of the
kernel exploit submissions to its kCTF vulnerability reward program in the
previous year used io_uring, and that it had disabled io_uring on ChromeOS
and its production servers. Sources: Research Desk,
[`docs/research/kernel-pros.md`](research/kernel-pros.md) (row 4, source
S22), and K. Koczka, "Learnings from kCTF VRP's 42 Linux kernel exploits
submissions", Google Online Security Blog, 2023-06-14,
<https://security.googleblog.com/2023/06/learnings-from-kctf-vrps-42-linux.html>
("in the past year, there has been a clear trend: 60% of the submissions
exploited the io_uring component"; checked 2026-11-20). Magia therefore:

1. keeps only four ops;
2. sends `READ`/`WRITE` through the same `io_read`/`io_write` path as the
   syscalls, so Capsicum rights, fd lookup and pointer checks cannot drift
   apart;
3. has a global kill switch, `sysctl kern.ring.enable` (writable from jail
   0; when it is 0, `uring_setup` and `uring_enter` return `EPERM`);
4. has a per-jail permission, `JAIL_ALLOW_RING`: `uring_setup` from a jail
   without it returns `EPERM`.

Closing the fd or exiting frees the region and its memory charge.

### futex (`futex.rs`)

`WAIT` compares the word and queues the caller under GIANT, the same lock
`WAKE` takes, so no wakeup can be lost. Magia has a single address space,
so the user address is the key. `WAKE` wakes the oldest waiters first.
Exit removes the process's waiters.

### sysctl (`sysctl.rs`)

The tree is a FreeBSD-style MIB. Every node has a dotted name and a numeric
OID (`kern.ostype` = `1.1`). u64 values are 8 bytes little-endian; strings
are NUL-terminated.

| Node | Notes |
|---|---|
| `kern.ostype`, `kern.osrelease`, `kern.version` | |
| `kern.hostname` | caller's jail; writing follows the sethostname rules |
| `kern.sched.base_slice_ns` | RW, 100 µs..100 ms |
| `kern.sched.context_switches` | |
| `kern.proc.nprocs` | not CAPRD |
| `kern.syscalls` | |
| `kern.ring.enable` | RW, 0 or 1 |
| `hw.machine`, `hw.ncpu`, `hw.pagesize`, `hw.physmem` | |
| `vm.stats.free_pages`, `vm.kmem_inuse` | |
| `security.jail.jailed`, `security.capability_mode` | about the caller |
| `debug.syscall_trace` | RW, not CAPRD |

C code adds nodes without touching this table by placing a
`SYSCTL_U64_RO`/`SYSCTL_STR_RO`/`SYSCTL_U64_RW` entry in the `set_sysctl`
linker set (FreeBSD's self-registering sysctl idea). Writes are allowed
only from jail 0, except `kern.hostname`.

## Testing

- **Host tests.** `cd rust && cargo test --target x86_64-unknown-linux-gnu`
  runs every policy rule above against a mock kernel environment: fake
  clock, fake user memory, and sleeps that time out.
- **In-kernel tests.** `kernel/ktest/sys_*.c` (keys 3xx–8xx) run the same
  calls from real kernel processes, using real user windows, copyin/copyout
  and sleeping.
