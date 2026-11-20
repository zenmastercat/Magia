# Testing Magia

One command runs everything:

```sh
nix-shell --run 'make test'
```

`make test` = `make cargo-test` + `make ktest`. It exits non-zero if any
part fails. Logs of the QEMU runs are kept in `build/*.log`.

## 1. Host tests of the Rust policy code (`make cargo-test`)

`cd rust && cargo test --target x86_64-unknown-linux-gnu`

The Rust crate is `no_std` in the kernel but builds with `std` under
`cfg(test)`. Kernel services (clock, console, user memory, sleeping,
waking, page allocation) come through the `kenv` layer, which in tests is a
thread-local mock: fake user memory with bounds-checked `copyin`/`copyout`
(`-EFAULT` outside it), a fake clock, and a sleep that returns "timed out"
at once. Each test builds its own kernel-state struct, so tests run in
parallel without shared globals. Blocking system calls return a `Block`
outcome that tests inspect instead of sleeping.

Every policy rule has positive and negative tests: Capsicum rights
(shrink-only, `ENOTCAPABLE`, `ECAPMODE`, `..` escapes), jail limits and
visibility, hierarchical memory/pid accounting, EEVDF fairness and lag,
sysctl errors, kqueue filters, ring validation (corrupted indexes, bad
opcodes, kill switch), futex, pipes, and the syscall table's generic
validation (`ENOSYS`, `EFAULT`, `EBADF`, `EINVAL`).

## 2. In-kernel tests (`make ktest`)

`tests/run-ktest.sh build/magia32.elf` boots the real kernel in QEMU
(`-machine pc -cpu max -accel tcg`, headless, serial on stdio,
`isa-debug-exit` at port 0xf4), with a 60 s timeout per boot, three times
over:

### a. The ktest run
Kernel command line `ktest`. Tests register with `KTEST(key, name)` into a
linker set (FreeBSD linker-set idea) and run in key order:

| keys | file | what |
|---|---|---|
| 0xx | `kernel/ktest/t_core.c` | boot, linker sets, #BP recovery, CPU protection bits, MSRs, timer/TSC, symbol lookup, printf |
| 1xx | `kernel/ktest/t_mm.c` | buddy alloc/free/merge, kmalloc stress (no leak), alignment, W^X, NX, SMEP, SMAP, NULL faults, page-table W+X scan, copyin/copyout negatives, fault fixup |
| 2xx | `kernel/ktest/t_sched.c` | threads, sleep timeouts, no lost wakeups, timer preemption of CPU-bound threads, EEVDF 2:1 weight share, yield, thread reaping |
| 3xx-8xx | `kernel/ktest/sys_*.c` | the system call layer through `ksyscall()`: Capsicum, jails, VFS/pipes, kqueue, rings/futex, sysctl |
| 9xx | `kernel/ktest/t_ring3.c` | ring-3 blobs making real `syscall`s; ring-3 faults kill only the process; UMIP; monitor commands |

Each test runs in a fresh process (own fd table, jail 0, not in capability
mode). Protection tests use expected-fault recovery: the test arms
`curthread()->fault_jmp` with `ksetjmp`, performs the forbidden access, and
checks that it faulted with a page fault - the kernel survives. The runner
prints `KTEST PASS|FAIL|TIMEOUT <name> checks=<n>` per test and

```
KTEST SUMMARY total=<n> passed=<n> failed=<n> checks=<n>
```

then writes 0x10 (all passed, QEMU exit status 33) or 1 (status 3) to
`isa-debug-exit`. A panic exits with status 7. The script requires the
summary line, `failed=0`, `passed == total` and status 33.

### b. Monitor smoke test
Boots without `ktest`, waits for the `magia>` prompt in the serial output,
then types `uname`, `sysctl kern.ostype`, `ls /dev`, `cat /etc/motd`,
`jails`, `ps`, an unknown command, and `poweroff`, and checks the output.

### c. Deliberate crashes (negative tests of the hardening)
Each boot must panic with a specific message and exit status 7:

| command line | expected panic |
|---|---|
| `ktest crash=stackprot` | `stack protector: stack smashing detected` (compiler canary, `-fstack-protector-strong`) |
| `ktest crash=canary` | `canary smashed` (kernel thread stack canary, checked on context switch) |
| `ktest crash=wx` | `unhandled #PF page fault in kernel mode` (write to kernel text) |

## Adding a test

- Rust: add a `#[test]` next to the code; build state with the test
  helpers in `rust/src/testutil.rs`.
- Kernel: anywhere under `kernel/ktest/`, `#include <magia/ktest.h>` and
  write `KTEST(123, my_test) { KT_EQ(x, 1); }`. No list to edit. Use
  `ksysN()` for system calls and `kt_umem/kt_uput/kt_uget/kt_ustr` for user
  memory (system calls only accept user addresses).

## Running by hand

```sh
nix-shell --run 'make run'                      # boot to the magia> monitor (Ctrl-A x quits)
nix-shell --run 'make all' && qemu-system-x86_64 -cpu max -m 256M -nographic \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 -kernel build/magia32.elf -append ktest
```
