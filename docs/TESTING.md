# Testing Magia

One command runs everything:

```sh
nix develop -c make test        # or: nix-shell --run 'make test'
```

`make test` = `make cargo-test` + `make host-test` + `make ktest`. It
exits non-zero if any part fails. Logs of the QEMU runs are kept in
`build/*.log`, the benchmark lines of the KVM run in
`build/bench-kvm.txt`. On the development PC the whole run takes about a
minute.

## 1. Host tests of the Rust code (`make cargo-test`)

`cd rust && cargo test --target x86_64-unknown-linux-gnu` (177 tests)

The Rust crate is `no_std` in the kernel but builds with `std` under
`cfg(test)`. Kernel services (clock, console, user memory, sleeping,
waking, page allocation) come through the `kenv` layer, which in tests is a
thread-local mock. Each test builds its own kernel-state struct, so tests
run in parallel without shared globals.

Every policy rule has positive and negative tests: Capsicum rights, jail
limits and visibility, hierarchical accounting, EEVDF fairness and lag,
sysctl errors, kqueue filters, ring validation, futex, pipes, and the
syscall table's generic validation.

**ACPI parser** (`rust/src/acpi.rs`, 14 tests): a fake physical memory
holds a whole machine (RSDP -> XSDT -> MADT, HPET, MCFG, FADT, DSDT,
SSDT). Good tables parse completely; then malformed input: truncated
tables and structures, bad checksums (RSDP, extended RSDP, SDTs), bad and
huge lengths, entry areas that are not a multiple of the entry size,
entries of length 0/1/too short/running past the end, overlapping tables
(root entries pointing into the RSDP or the XSDT), duplicate and zero
entries, huge counts (300 CPUs, 40 I/O APICs, 200 root entries, 20 ECAM
windows are capped and flagged), wrong address spaces and alignments,
ACPI 1.0 FADT and RSDT, and 4000 random mutations of the whole machine
(with and without repaired checksums) plus every truncation of every
table: the parser must never panic. Power (stage 2b): the FADT reset
register, PM1a/PM1b control (X_ GAS first, legacy ports otherwise),
SMI_CMD/ACPI_ENABLE; `\_S5` from the DSDT, from an SSDT when the DSDT has
none, a DSDT with a bad checksum or pointing nowhere; reset registers in
an unknown address space or I/O above 0xffff are refused.

**AML reader** (`rust/src/aml.rs`, 8 tests): QEMU q35's `\_S5`
(ZeroOps) and typical firmware encodings (root prefix, BytePrefix), every
integer data object, one-element packages, 2-byte PkgLength with padding;
decoys (`_S5_` in a string, a NameOp `_S5_` that is not a package);
errors: signature, length, short tables, no `\_S5`, PkgLength past the
body, zero elements, unknown opcodes, values above 7 (also when encoded
wide: Word 0x0100, DWord 1 << 24), OnesOp, an element past the package
end; every truncation of a table; 20000 random bodies (half with the
`NameOp _S5_ PackageOp` bytes planted).

**USB parsers** (`rust/src/usb.rs`, 7 tests; `rust/src/usbhub.rs`, 7
tests): QEMU's usb-kbd device and configuration descriptors; errors
(empty, short, wrong length or type, bad max packet size); evil
configuration walks (zero-length descriptor, descriptors running past the
end, wTotalLength larger or smaller than the data, OUT/bulk/mouse
endpoints only, an endpoint before the keyboard interface); a composite
keyboard (non-boot HID interface first, the boot keyboard at alternate
setting 1 skipped, the one at alternate setting 0 taken); boot reports
(new keys only, keys moving between slots, Shift/Ctrl/Caps Lock,
ErrorRollOver ignored without changing state, Page Up/Down as whole
VT220 sequences, short and long reports, a full output buffer); hubs
(QEMU's hub interface, a multi-TT hub's alternate setting, hubs without
an endpoint or with only an OUT endpoint, an endpoint that belongs to a
later interface, two hub interfaces), USB 2 and SuperSpeed hub
descriptors (port counts, TT think time, power-on delay, every error),
port status, the boot mouse interface and its reports; 5000 random
buffers through every parser.

## 2. Host test of the benchmark statistics (`make host-test`)

`kernel/bench_stats.c` has no kernel dependencies; built with
`-DBENCH_STATS_TEST` it is its own test program (sorting edge cases and
1000 random values, median/p99/mean on known data, cycle to ns
conversion with rounding and saturation, the decimal formatter with short
buffers).

## 3. QEMU runs (`make ktest`)

`make ktest` builds `build/magia.img` and runs `tests/run-ktest.sh`,
which boots the image through `tests/qemu.sh`: QEMU `q35`, `-cpu max`,
TCG, `-smp 4 -m 8G`, OVMF (UEFI) firmware, `-device qemu-xhci -device
usb-kbd` (`QEMU_USB=kbd`; `xhci` for the controller only; `desktop` for
the target desktop's shape, below; `none`), the disk opened with
`snapshot=on` (never written), serial on stdio, `isa-debug-exit` at port
0xf4 (only the ktest exit path uses it), `-no-reboot` unless
`QEMU_NOREBOOT=0`, a 180 s timeout per boot. The kernel command line for
each run is passed to Limine as an SMBIOS type 11 OEM string
(`-smbios type=11,path=...` holding `limine:config:...`), so one image
serves every run.

`QEMU_USB=desktop` is the closest QEMU gets to the author's desktop
(two xHCI controllers, the keyboard behind a USB 2.0 hub, a receiver on
a root port, no PS/2): `-machine q35,i8042=off`, a `qemu-xhci` with a
`usb-mouse`, and a second `qemu-xhci` with a `usb-hub` on USB 2 port 3, a
`usb-kbd` behind it on hub port 1, and a `usb-kbd` on USB 2 port 4.
QEMU puts its USB 3 ports first, so the guest sees root ports 7, 7.1, 8
and 5. QEMU's `usb-hub` is a full-speed hub and QEMU has no high-speed or
SuperSpeed hub, so the Transaction Translator fields a full-speed
keyboard behind the desktop's high-speed Genesys hub needs are tested on
made-up trees (ktest 077), not on emulated hardware.

The runs:

| run | command line / machine | checks |
|---|---|---|
| ktest | `ktest` | all in-kernel tests (below) |
| monitor | `fadt8042=0`; HMP socket; xHCI without usb-kbd | the FADT 8042 flag treated as clear: the i8042 is probed anyway and found; 10 commands typed over serial (`lspci`, which must list the xHCI; `hwinfo`; `dmesg 400`, which must repeat the boot log); `echo kbd-OK` typed on the PS/2 keyboard with QEMU `sendkey` (`tests/hmp.py`); `reboot`: QEMU (`-no-reboot`) must exit with status 0 through the FADT reset register (I/O 0xcf9), without the fallback message |
| usbkbd | (none); HMP socket; xHCI + usb-kbd; reboots allowed | 26 commands (312 keys) typed with `sendkey`, which QEMU routes to the most recently created keyboard: all must run, `usb` must show at least 600 HID reports (the keyboard's 255-TRB ring wraps twice) and MSI-X; `poweroff`: ACPI S5 must end QEMU with status 0 (a reset instead would reboot and hang the test) |
| desktop | `QEMU_USB=desktop`; HMP socket; reboots allowed | typing through the keyboard behind the hub; `device_add` of a keyboard on hub port 3.2 (hub status change, attach) and typing on it; `device_del` of it and of the hub keyboard (detach) and typing on the remaining root-port keyboard; `device_del` of that and `device_add` of one on root port 2 (port change event, attach) and typing on it; `mouse_move` three times: x 30, y -15; the attach/detach log lines; ACPI S5 poweroff |
| crash x3 | `ktest crash=stackprot`, `crash=canary`, `crash=wx` | must panic with the expected message (the panic screen is drawn) and exit status 7 |
| bigmem | `ktest nox2apic`, `-m 32G` | all RAM managed (about 32 GiB), xAPIC MMIO path; falls back to 24G/16G and says so if QEMU cannot allocate 32G |
| bios | `ktest`, SeaBIOS instead of OVMF | the same image boots through Limine's BIOS stages |
| bios poweroff | (none), SeaBIOS, no USB | SeaBIOS leaves the chipset in legacy (non-ACPI) mode: `poweroff` must switch ACPI on through SMI_CMD, then S5 must end QEMU (status 0) |
| noapic | `ktest noapic` | the legacy 8259 PIC with the LAPIC timer as the tick |
| kvm | `ktest bench`, `-accel kvm -cpu host,+invtsc`, pinned to one host CPU | x2APIC, the TSC-deadline timer and the invariant TSC on the host CPU; after the ktests the benchmark harness: every primitive must report n >= 1000, ordered statistics and a sane median; BENCH lines in `build/bench-kvm.txt`; skipped when `/dev/kvm` is not usable |
| desktop (ktest) | `ktest kt-desktop`, `QEMU_USB=desktop` | the in-kernel tests on the desktop shape (ktest 078 checks the topology); no i8042, found quickly |
| legacy | `ktest noapic pittick` | the PIT channel 0 tick (only used without a LAPIC timer, or on request) |
| nopit | `ktest nopit nohpet` | a gated PIT (it never counts) and no HPET: the bounded PIT wait gives up and logs it, the TSC frequency falls back (CPUID 0x16, then a logged guess, as QEMU sets neither 0x15 nor 0x16), the LAPIC timer ticks, the boot does not hang |

The crash runs and the last three ktest boots go in the background while
the others run.

### The in-kernel tests
Tests register with `KTEST(key, name)` into a linker set (FreeBSD
linker-set idea) and run in key order, each in a fresh process:

| keys | file | what |
|---|---|---|
| 00x | `t_core.c` | boot, linker sets, Limine hand-off (base revision, KASLR slide, direct map, symbol lookup under the slide), #BP recovery, CPU protection bits, MSRs, timer/TSC, printf |
| 020 | `t_core.c` | ACPI: the QEMU tables (4 CPUs with APIC ids 0-3, one I/O APIC at 0xfec00000, HPET, MCFG, FADT with 8042 unless i8042=off, PIT override to GSI 2); the parser may only read ACPI memory |
| 021 | `t_core.c` | console output: kernel addresses printed at link-time values, firmware signature bytes outside 0x20..0x7e shown as `?` |
| 022 | `t_core.c` | ACPI power on q35: reset register I/O 0xcf9 = 0x0f, PM1a control I/O 0x604, `\_S5` = 0/0 from the DSDT; SCI_EN on under OVMF, SMI_CMD hand-over possible under SeaBIOS |
| 030-032 | `t_console.c` | framebuffer: a probe line's glyph pixels read back and compared with the Spleen bitmaps; scancode translation (every byte value, every state); the i8042 probe ignores a clear FADT flag and gives up on a missing controller in under 0.1 s |
| 034-039 | `t_klog.c` | `hwinfo` lists the q35 PCI functions and the USB keyboard; `dmesg` is not logged again; the kernel log ring (wrap, the partial first line dropped, tails, a write longer than the ring); console output is logged; scroll-back (lines leave the screen and come back two pages up, clamped to the history, new output snaps back); Page Up/Down from PS/2 scancodes and through the VT220 decoder (other sequences swallowed, long digit runs bounded); the panic screen (header text, red background pixel, newest log line) |
| 04x | `t_rng.c` | ChaCha20 known answers (RFC 8439 A.1 #1 and 2.3.2, a two-block vector; cross-checked with `openssl enc -chacha20`), non-repeating output, bit balance, buffer bounds, reseed counter, random guard and canaries, `arc4random_uniform` range, `kern.random.weak` |
| 050-055 | `t_intr.c` | interrupt controller mode (APIC or noapic; the PIT ticks only with `pittick`), ticking at HZ, TSC vs HPET, COM1 routed through the I/O APIC (redirection entry checked, then a real IRQ from a UART loopback byte); the TSC source order (`kern.tsc_source`) under `nopit`/`nohpet`; the CPUID 0x15 arithmetic on leaves QEMU does not model (the X13's Comet Lake with the SDM crystal table, a 285K-like CPU, a base-frequency mismatch, unusable leaves) |
| 06x | `t_pci.c` | ECAM enumeration of q35 (host bridge, LPC, one or two xHCIs), bounded config reads; BAR decode/size of the xHCI (64-bit, 16 KiB, restored); capability walk on a fake config space in RAM (loops, pointers into the header, unaligned, 48-entry chain, MSI-X table outside its BAR refused without leaking a vector); the programmed MSI-X entry; 064: bus mastering off on every function Magia does not drive; 065: a 64-bit BAR claimed in the last slot is refused; 066: ECAM windows outside the MCFG range (unaligned, wrapping, past 2^52) and framebuffer MMIO mappings are refused |
| 070-079 | `t_usb.c` | every xHCI running on its own MSI-X vector, a No Op completed through the interrupt; the keyboard attached (0627:0001, EP 0x81) and its slot context read back from the device context (route, speed, root port, configured); the Rust parsers refuse garbage through the C interface; 073: 300 No Ops one at a time, so the command ring wraps; 074: at most 32 events per interrupt; 075: review F6 on EP0: a request QEMU stalls, Reset Endpoint + Set TR Dequeue (QEMU refuses both in the wrong endpoint state), then GET_DESCRIPTOR works; 076: review F6 on an interrupt endpoint: a test endpoint QEMU stalls is reset 3 times and given up, the keyboard's endpoint keeps running; 077: route strings and TT hub slot/port/MTT on made-up trees (the desktop's full-speed keyboard below a high-speed hub, multi-TT, a full-speed hub below a high-speed one, ports above 15, depth 5); 078: the desktop topology (hub on c1 port 7 with 8 ports, keyboard at 7.1 with route 1, the slot contexts sent and kept, keyboard at 8, mouse at c0 port 5); 079: speed names for every Protocol Speed ID (SuperSpeedPlus included) |
| 1xx | `t_mm.c` | buddy alloc/free/merge, kmalloc stress, alignment, W^X, NX, SMEP, SMAP, NULL faults, page-table W+X scan, copyin/copyout negatives, fault fixup; 121: frames above 4 GiB allocated and touched; 122: bootloader memory reclaimed, all RAM managed; 130-132: the immutable kernel image (remaps of the kernel window and of the image's direct-map alias refused with `-EPERM`, direct PTE/PDE/PDPTE writes fault); 133: MMIO mappings refuse RAM |
| 2xx | `t_sched.c` | threads, sleep timeouts, no lost wakeups, timer preemption, EEVDF weights, yield, thread reaping |
| 3xx-8xx | `sys_*.c` | the system call layer: Capsicum, jails, VFS/pipes, kqueue, rings/futex, sysctl |
| 90x-910 | `t_ring3.c` | ring-3 blobs making real `syscall`s; ring-3 faults kill only the process; UMIP; monitor commands |
| 920-921 | `t_bench.c` | the benchmark statistics on known data; every benchmark runs 1000 samples with ordered statistics, the ping-pong really switches |

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

## 4. Benchmarks (`bench`, `ktest bench`)

`kernel/bench.c` times kernel primitives with the TSC: lfence; rdtsc;
lfence before, rdtscp; lfence after (Intel's white paper "How to
Benchmark Code Execution Times on Intel IA-32 and IA-64 Instruction Set
Architectures"), one sample per operation, 2000 samples by default
(`bench [n]`, 1000 <= n <= 16384) after a warm-up of n/10 (at least 100),
then min, median, p99, max and mean, in cycles and in ns:

| name | what one sample is |
|---|---|
| `tsc_overhead` | the empty measurement (subtract it from the others if you need to) |
| `null_syscall_ring3` | `syscall` getpid from ring 3 and back, timed in user mode (a user blob in `arch/x86_64/user_blob.S`) |
| `null_syscall_kernel` | `ksyscall(getpid)`: the Rust dispatcher without the ring change |
| `ctx_switch` | half of a round trip between two kernel threads that hand the CPU to each other with sleep/wakeup (one switch with its wakeup and sleep; the run is refused unless every round trip was exactly two switches). A yield ping-pong is not used: with EEVDF a yield does not always switch |
| `irq_to_thread` | from the timer interrupt (the TSC deadline that fired, in TSC-deadline mode; the handler's entry otherwise) to the thread it woke running |
| `page_map`, `page_unmap` | one 4 KiB kernel mapping made / removed (`vmm_map`, `vmm_unmap` with its TLB flush) |
| `kmalloc_free_64` | `kmalloc(64)` + `kfree` |
| `page_alloc_free` | `page_alloc(0)` (which zeroes the page) + `page_free` |

Output (one line each, machine readable):

```
BENCH-INFO kernel=Magia-0.2.0 cpu="..." tsc_hz=... tsc_source=hpet invariant_tsc=1 rdtscp=1 timer=tsc_deadline hz=1000 n=2000 warmup=200
BENCH name=null_syscall_ring3 n=2000 median_cycles=... p99_cycles=... min_cycles=... max_cycles=... mean_cycles=... median_ns=... p99_ns=... min_ns=...
BENCH-END
```

`tests/run-ktest.sh` runs them under KVM (`-cpu host,+invtsc`, because
`-cpu host` hides the invariant-TSC bit), with QEMU pinned to one host
CPU (`taskset`: the first P-core of a hybrid CPU from
`/sys/devices/cpu_core/cpus`, or `BENCH_CPU`): unpinned, the vCPU moved
between the 285K's P- and E-cores and the same primitive differed by 2x
between runs. The lines are kept in `build/bench-kvm.txt`. Under TCG the numbers mean nothing; the ktest only
checks that every benchmark ran.

**These numbers are Magia-internal.** They describe Magia's own paths in
a KVM guest (or, later, on the real machines) and are not comparable to
Linux as they stand: a Linux comparison needs the same operations done
from user space through each system's system call interface, built
unchanged for both, on the same machine and core, with the same
frequency and mitigation settings. That user-space suite comes with the
userland in stage 3 (the paper plan's fairness rules apply).

## Adding a test

- Rust: add a `#[test]` next to the code; build state with the test
  helpers in `rust/src/testutil.rs`.
- Kernel: anywhere under `kernel/ktest/`, `#include <magia/ktest.h>` and
  write `KTEST(123, my_test) { KT_EQ(x, 1); }`. No list to edit.
- A test that only makes sense on one QEMU shape checks the command line
  (`cmdline_has("kt-desktop")`).

## Running by hand

```sh
nix develop -c make run                          # UEFI boot to magia> (serial on stdio, QEMU window shows the framebuffer)
nix develop -c make run CMDLINE="ktest noapic"   # any command line
nix develop -c sh tests/qemu.sh build/magia.img ktest -display none -serial stdio
nix develop -c sh -c 'OVMF=bios sh tests/qemu.sh build/magia.img "" -serial stdio'          # SeaBIOS
nix develop -c sh -c 'QEMU_USB=desktop sh tests/qemu.sh build/magia.img "" -serial stdio'   # the desktop's USB shape
nix develop -c sh -c 'QEMU_ACCEL=kvm QEMU_CPU=host,+invtsc sh tests/qemu.sh build/magia.img "ktest bench" -display none -serial stdio'
```
