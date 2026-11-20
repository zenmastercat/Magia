# Testing Magia

One command runs everything:

```sh
nix-shell --run 'make test'
```

`make test` = `make cargo-test` + `make ktest`. It exits non-zero if any
part fails. Logs of the QEMU runs are kept in `build/*.log`. On the
development PC the whole run takes about 25 seconds.

## 1. Host tests of the Rust code (`make cargo-test`)

`cd rust && cargo test --target x86_64-unknown-linux-gnu` (153 tests)

The Rust crate is `no_std` in the kernel but builds with `std` under
`cfg(test)`. Kernel services (clock, console, user memory, sleeping,
waking, page allocation) come through the `kenv` layer, which in tests is a
thread-local mock. Each test builds its own kernel-state struct, so tests
run in parallel without shared globals.

Every policy rule has positive and negative tests: Capsicum rights, jail
limits and visibility, hierarchical accounting, EEVDF fairness and lag,
sysctl errors, kqueue filters, ring validation, futex, pipes, and the
syscall table's generic validation.

**ACPI parser** (`rust/src/acpi.rs`, 12 tests): a fake physical memory
holds a whole machine (RSDP -> XSDT -> MADT, HPET, MCFG, FADT). Good
tables parse completely; then malformed input: truncated tables and
structures, bad checksums (RSDP, extended RSDP, SDTs), bad and huge
lengths, entry areas that are not a multiple of the entry size, entries
of length 0/1/too short/running past the end, overlapping tables (root
entries pointing into the RSDP or the XSDT), duplicate and zero entries,
huge counts (300 CPUs, 40 I/O APICs, 200 root entries, 20 ECAM windows
are capped and flagged), wrong address spaces and alignments, ACPI 1.0
FADT and RSDT, and 4000 random mutations of the whole machine (with and
without repaired checksums) plus every truncation of every table: the
parser must never panic.

## 2. QEMU runs (`make ktest`)

`make ktest` builds `build/magia.img` and runs `tests/run-ktest.sh`,
which boots the image through `tests/qemu.sh`: QEMU `q35`, `-cpu max`,
TCG, `-smp 4 -m 8G`, OVMF (UEFI) firmware, the disk opened with
`snapshot=on` (never written), serial on stdio, `isa-debug-exit` at port
0xf4, a 180 s timeout per boot. The kernel command line for each run is
passed to Limine as an SMBIOS type 11 OEM string
(`-smbios type=11,path=...` holding `limine:config:...`), so one image
serves every run. The runs:

| run | command line / machine | checks |
|---|---|---|
| ktest | `ktest` | all in-kernel tests (below) |
| monitor | (none); HMP socket | 7 commands typed over serial, `echo kbd-OK` typed on the PS/2 keyboard with QEMU `sendkey` (`tests/hmp.py`), `poweroff` |
| crash x3 | `ktest crash=stackprot`, `crash=canary`, `crash=wx` | must panic with the expected message and exit status 7 |
| bigmem | `ktest nox2apic`, `-m 32G` | all RAM managed (about 32 GiB), xAPIC MMIO path; falls back to 24G/16G and says so if QEMU cannot allocate 32G |
| bios | `ktest`, SeaBIOS instead of OVMF | the same image boots through Limine's BIOS stages |
| noapic | `ktest noapic` | the legacy 8259 PIC + PIT path |
| kvm | `ktest`, `-accel kvm -cpu host` | x2APIC and the TSC-deadline timer on the host CPU; skipped when `/dev/kvm` is not usable |

The crash runs go in the background while the others run.

### The in-kernel tests
Tests register with `KTEST(key, name)` into a linker set (FreeBSD
linker-set idea) and run in key order, each in a fresh process:

| keys | file | what |
|---|---|---|
| 00x | `t_core.c` | boot, linker sets, Limine hand-off (base revision, KASLR slide, direct map, symbol lookup under the slide), #BP recovery, CPU protection bits, MSRs, timer/TSC, printf |
| 020 | `t_core.c` | ACPI: the QEMU tables (4 CPUs with APIC ids 0-3, one I/O APIC at 0xfec00000, HPET, MCFG, FADT with 8042, PIT override to GSI 2); the parser may only read ACPI memory |
| 021 | `t_core.c` | console output: kernel addresses printed at link-time values, firmware signature bytes outside 0x20..0x7e shown as `?` |
| 03x | `t_console.c` | framebuffer: a probe line's glyph pixels read back and compared with the Spleen bitmaps; scancode translation (every byte value, every state) |
| 04x | `t_rng.c` | ChaCha20 known answers (RFC 8439 A.1 #1 and 2.3.2, a two-block vector; cross-checked with `openssl enc -chacha20`), non-repeating output, bit balance, buffer bounds, reseed counter, random guard and canaries, `arc4random_uniform` range, `kern.random.weak` |
| 05x | `t_intr.c` | interrupt controller mode (APIC or noapic, x2APIC and TSC deadline when the CPU has them), ticking at HZ, TSC vs HPET calibration, COM1 routed through the I/O APIC (redirection entry checked, then a real IRQ from a UART loopback byte) |
| 1xx | `t_mm.c` | buddy alloc/free/merge, kmalloc stress, alignment, W^X, NX, SMEP, SMAP, NULL faults, page-table W+X scan, copyin/copyout negatives, fault fixup; 121: frames above 4 GiB allocated and touched; 122: bootloader memory reclaimed, all RAM managed; 130-132: the immutable kernel image (remaps of the kernel window and of the image's direct-map alias refused with `-EPERM`, direct PTE/PDE/PDPTE writes fault); 133: MMIO mappings refuse RAM |
| 2xx | `t_sched.c` | threads, sleep timeouts, no lost wakeups, timer preemption, EEVDF weights, yield, thread reaping |
| 3xx-8xx | `sys_*.c` | the system call layer: Capsicum, jails, VFS/pipes, kqueue, rings/futex, sysctl |
| 9xx | `t_ring3.c` | ring-3 blobs making real `syscall`s; ring-3 faults kill only the process; UMIP; monitor commands |

Protection tests use expected-fault recovery: the test arms
`curthread()->fault_jmp` with `ksetjmp`, performs the forbidden access,
and checks that it faulted - the kernel survives. The runner prints
`KTEST PASS|FAIL|TIMEOUT <name> checks=<n>` per test and

```
KTEST SUMMARY total=<n> passed=<n> failed=<n> checks=<n>
```

then writes 0x10 (all passed, QEMU exit status 33) or 1 (status 3) to
`isa-debug-exit`. A panic exits with status 7. The script requires the
summary line, `failed=0`, `passed == total` and status 33 for every run.

## Adding a test

- Rust: add a `#[test]` next to the code; build state with the test
  helpers in `rust/src/testutil.rs`.
- Kernel: anywhere under `kernel/ktest/`, `#include <magia/ktest.h>` and
  write `KTEST(123, my_test) { KT_EQ(x, 1); }`. No list to edit.

## Running by hand

```sh
nix-shell --run 'make run'                          # UEFI boot to magia> (serial on stdio, QEMU window shows the framebuffer)
nix-shell --run 'make run CMDLINE="ktest noapic"'   # any command line
nix-shell --run 'sh tests/qemu.sh build/magia.img ktest -display none -serial stdio'
nix-shell --run 'OVMF=bios sh tests/qemu.sh build/magia.img "" -serial stdio'   # SeaBIOS
```
