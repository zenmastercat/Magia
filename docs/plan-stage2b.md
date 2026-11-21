# Stage 2b plan: real-hardware bring-up

Working plan written before the code, like `plan-stage1.md` and
`plan-stage2.md`. Goal: Magia becomes usable on the author's two machines
(the desktop measured in the project notes: MSI B860 board, Core Ultra 9
285K, two xHCI controllers - CPU 8086:7ec0 at 00:0d.0 and chipset
8086:7f6e at 80:14.0 - with the Keychron K2 Max keyboard, 3434:0a20, full
speed, behind a Genesys USB 2.0 hub, 05e3:0608, on chipset port 6, and a
Logitech receiver, 046d:c548, on chipset port 4; and a ThinkPad X13 Gen 1
Intel, whose details are still being researched). The user decided that
nothing runs on real hardware until the kernel is done, so every device
path is tested in QEMU with the closest emulated hardware. "Done" means
`nix develop -c make test` passes from a fresh clone.

The kernel is also the subject of an IEEE paper comparing it with Linux,
so this stage starts the benchmark harness and keeps recording
development data (outside the repo).

## Ground rules (as before)

- FreeBSD/OpenBSD code only under BSD/ISC terms, original headers kept,
  every adapted file in `docs/THIRD_PARTY.md`. Linux: ideas only.
- Hardware access in C; everything a device or firmware sends that has
  structure (hub descriptors, port status, mouse reports, AML) is parsed
  in Rust with host tests on malformed input.
- Smallest complete change (Ponytail); every deliberate gap gets a
  `shortcut:` comment. Tests first where practical (Superpowers TDD).

## Sources

- Hubs: OpenBSD `sys/dev/usb/uhub.c` rev 1.99 (NetBSD Foundation,
  BSD-2-Clause): attach order (hub descriptor, power every port, wait
  bPwrOn2PwrGood), explore loop (clear C_PORT_* bits, reset a connected
  port, speed from wPortStatus, detach on any connect change), status
  bitmap from the interrupt endpoint. OpenBSD `sys/dev/usb/xhci.c`
  (`xhci_context_setup`): route string, Hub/NPORTS/MTT/TT think time in
  the slot context, TT hub slot and port for full/low-speed devices
  below a high-speed hub (`myhsport` in `usb_subr.c`). USB 2.0 ch. 11,
  USB 3.2 ch. 10, xHCI 1.2 sections 4.3 (slot set-up), 4.6.8 (Reset
  Endpoint), 4.6.10 (Set TR Dequeue Pointer), 4.8.3 (halted endpoints),
  6.2.2 (slot context).
- HID boot mouse: HID 1.11 appendix B.2 (Rust, as the keyboard).
- ACPI: ACPI 6.5 sections 4.8.3.6 (reset register), 5.2.9 (FADT),
  7.4.2 (`\_S5`), 16.1.6/16.1.7 (entering S5: SLP_TYPa/b + SLP_EN into
  PM1a/PM1b control), 20.2 (AML grammar: NameOp, PackageOp, PkgLength,
  data objects). OpenBSD `sys/dev/acpi/acpi.c` (`acpi_reset`,
  `acpi_sleep_pm`) for the order of register writes (ideas; the code is
  written from the specification). No ACPICA.
- PS/2 probe: IBM PC/AT technical reference; OpenBSD `pckbc.c` (as in
  stage 1).
- Benchmarks: Intel's "How to Benchmark Code Execution Times on Intel
  IA-32 and IA-64 Instruction Set Architectures" (lfence/rdtsc,
  rdtscp/lfence pairs, measuring the timing overhead), CPUID
  0x80000007 EDX[8] for the invariant TSC; LMbench (McVoy and Staelin)
  for the choice of primitives.

## Milestones

1. **Kernel log** (`kernel/klog.c`): every console byte also goes into
   a 64 KiB ring in memory; `dmesg` prints it; the framebuffer console
   keeps a scroll-back of the last 1000 lines and the monitor scrolls it
   with Page Up / Page Down (PS/2 and USB keyboards send the VT220
   sequences `ESC [5~` / `ESC [6~`, which a serial terminal sends too);
   `panic()` draws a full-screen report: a header with the message and
   as many of the last log lines (including the backtrace) as fit;
   `hwinfo` puts PCI functions (BDF, vendor:device, class) and USB
   devices (controller, port path, VID:PID, speed, driver) packed into
   columns on one screen, for a photograph.
   Tests: ktests for the ring (wrap, partial first line dropped, long
   writes), the scroll-back (lines leave the screen and come back with
   Page Up, return with Page Down, new output snaps to the bottom), the
   escape-sequence decoding of both keyboards, the panic screen (header
   cells, last log line on the last row, pixel colours) and `hwinfo`
   (lists the xHCI and the keyboard); the monitor run types `dmesg` and
   `hwinfo` over serial.
2. **USB for real desktops** (`kernel/xhci.c`, `rust/src/usb.rs`):
   - every xHCI controller (up to 4), each with its own rings, MSI-X
     vector and device table;
   - one `usb` thread does all enumeration (boot and later), so commands
     and control transfers never run concurrently; interrupts only
     consume events and wake it;
   - port-change events and hot-plug: attach on connect, detach on
     disconnect (Disable Slot, pages freed, recursive below hubs);
   - USB 2 and USB 3 hub class driver (OpenBSD `uhub.c` order), hub and
     port-status data parsed in Rust; route strings; the slot context of
     a hub (Hub, NPORTS, TT think time, MTT after SET_INTERFACE to the
     multi-TT alternate setting) and of full/low-speed devices below a
     high-speed hub (TT hub slot id and port);
   - several keyboards at once (per-keyboard Rust state), composite
     devices (the boot-keyboard interface wherever it is in the
     configuration, alternate setting 0, configurations up to 4 KiB);
   - review F6: a halted endpoint (interrupt or EP0) gets Reset Endpoint,
     CLEAR_FEATURE(ENDPOINT_HALT) after a stall, Set TR Dequeue Pointer
     and a re-queued transfer; after 3 errors in a row without a good
     transfer the endpoint is given up (a flood guard like F4);
   - HID boot mouse with counters in `usb` (optional item; small once
     the endpoint code is shared).
   Tests: Rust host tests for every new parser (good, malformed, fuzz);
   a pure-C TT/route computation checked by ktest against desktop-like
   topologies (full-speed keyboard below a high-speed single-TT and
   multi-TT hub, behind a full-speed hub below a high-speed hub, on a
   root port); ktest of the stall recovery against QEMU's own endpoint
   state checks (a control request QEMU stalls halts EP0, and a test
   endpoint QEMU stalls halts an interrupt endpoint; Reset Endpoint and
   Set TR Dequeue must succeed, which QEMU only allows in the halted /
   stopped state); a desktop-like ktest boot (two `qemu-xhci`, no
   i8042, a `usb-hub` on the second controller with a `usb-kbd` behind
   it, a second `usb-kbd` and a `usb-mouse`) checking controllers, slot
   contexts read back from the device context (route string, root port,
   speed, hub fields); a monitor run on the same topology that types
   through the keyboard behind the hub, hot-plugs keyboards on a root
   port and on a hub port with `device_add` / `device_del` over the QEMU
   monitor and types through them, and moves the mouse.
3. **PS/2 without trusting the FADT**: probe the i8042 even when the
   FADT 8042 flag is clear (logged), every wait bounded; `nokbd` stays.
   A test-only `fadt8042=0` command-line word simulates the cleared
   flag. Tests: the monitor run boots with `fadt8042=0` and types on the
   PS/2 keyboard; the desktop-like boot has no i8042 at all
   (`-machine i8042=off`) and must not hang (probe time is checked).
4. **ACPI power**: FADT reset register (I/O, memory or PCI config space
   GAS) for `reboot`, then the 8042 pulse, then a triple fault;
   `poweroff` through S5: `\_S5` from the DSDT by a minimal AML reader in
   Rust (only the `\_S5` package, bounds-checked, fuzzed), SCI_EN
   checked (ACPI_ENABLE through SMI_CMD if needed, bounded wait), then
   SLP_TYPa/b | SLP_EN into PM1a/PM1b control (or the sleep control
   register on hardware-reduced ACPI). `isa-debug-exit` stays only as
   the ktest exit path. Tests: Rust tests of the FADT fields and the AML
   reader; ktest checks q35's values (reset register I/O 0xcf9,
   `\_S5` = 0/0, PM1a control 0x604); the monitor run ends with `reboot`
   (QEMU exits through the reset register, and the kernel's "fallback"
   message must not appear); the USB keyboard run ends with `poweroff`
   (QEMU must exit by itself with status 0, run without `-no-reboot`
   so a reset would hang the test instead of passing it).
5. **Benchmark harness, kernel side**: `bench` monitor command and
   `ktest bench`: TSC timing (invariant TSC reported from CPUID,
   lfence/rdtsc ... rdtscp/lfence, timing overhead measured and
   reported), each primitive N >= 1000 after a warm-up, median, p99,
   min, in machine-readable `BENCH ...` lines: null system call from
   ring 3 (`syscall` getpid, timed in user mode) and from a kernel
   thread (`ksyscall`), thread context switch (yield ping-pong), timer
   interrupt-to-thread latency (from the TSC deadline when the timer
   runs in that mode), page map/unmap, kmalloc/kfree and page
   alloc/free. Tests: a ktest runs every benchmark and checks the
   statistics are ordered and non-zero; the KVM run boots `ktest bench`
   and checks every line, keeping them in `build/bench-kvm.txt`.
   TESTING.md says these are Magia-internal numbers and that a Linux
   comparison needs the user-space suite of stage 3.
6. **Docs**: README, ROADMAP, TESTING, ARCHITECTURE, THIRD_PARTY; a new
   `docs/REAL-HARDWARE.md` for later (writing `build/magia.img` to a USB
   stick, firmware settings, what to photograph when it fails).

## Risks

- QEMU's `usb-hub` is a full-speed USB 1.1 hub and QEMU has no USB 3
  hub, so the Transaction Translator fields and the SuperSpeed hub path
  (SET_HUB_DEPTH, the 0x2a descriptor, BH reset) cannot be exercised in
  QEMU. The TT/route computation is a pure function with ktests on the
  desktop's topology; the SuperSpeed hub code follows the specification
  and OpenBSD and is marked untested.
- Real Intel xHCI controllers report port changes during BIOS hand-off
  and while the link trains; detach-then-attach on every connect change
  (OpenBSD's rule) keeps that safe.
- Real keyboards stall SET_IDLE (now recovered instead of halting EP0).
- AML can define `\_S5` in ways a byte scan does not see (a method, or a
  name built at run time); the reader then reports "not found" and
  `poweroff` falls back to halting with a message. A full AML
  interpreter stays out of scope.
- Firmware may leave the machine in legacy (non-ACPI) mode; the SMI_CMD
  hand-over is implemented but only QEMU (already in ACPI mode) tests it.
- Benchmarks under TCG are meaningless; only KVM numbers are reported,
  and they are Magia-internal until the user-space suite exists.

## Result (stage 2b done)

All milestones are in and `make test` passes from a fresh clone: 177
host `cargo test`s and the host statistics test; 124 ktests in each of
eight ktest boots (OVMF, 32 GiB + nox2apic, SeaBIOS, noapic, KVM with
the benchmarks, the desktop USB shape, the PIT tick, a gated PIT without
HPET); the monitor runs over serial + PS/2 (FADT flag cleared, ending in
`reboot`), over a USB keyboard (ending in `poweroff`) and over hub
keyboards with hot-plug and a mouse; a SeaBIOS poweroff from legacy mode;
the three crash runs. Deviations from the plan:

- Added on request during the stage (ThinkPad X13 research): the TSC
  frequency from CPUID 0x15 first, every PIT wait bounded by the TSC
  (a gated 8254 is logged, not waited for), the LAPIC timer as the tick in
  `noapic` mode too; test hooks `nopit`, `nohpet`, `nocpuidtsc`,
  `pittick`. QEMU leaves CPUID 0x15/0x16 at zero (TCG, and KVM even with
  `+invtsc`), so the 0x15 arithmetic is tested on made-up leaves (ktest
  055) and QEMU runs exercise the fallbacks.
- QEMU keeps only Context Entries of the slot context sent with
  Configure Endpoint, so the hub fields are checked in what Magia sent
  (ktest 078), not read back.
- QEMU's USB 3 ports come first: the desktop shape's devices are on
  xHCI root ports 5-8.
- SeaBIOS leaves the chipset in legacy mode, which made the SMI_CMD
  hand-over testable (the "bios poweroff" run).
- The context-switch benchmark is a sleep/wakeup hand-off, not a yield
  ping-pong: with EEVDF a yield does not always switch (the first KVM run
  rejected it). The KVM run pins QEMU to one P-core: unpinned, the 285K's
  P- and E-cores made one primitive differ by 2x between runs.
- SuperSpeedPlus root ports (Protocol Speed ID 5, Intel 800-series) were
  handled as full speed in the first M2 code; found in review, fixed
  (ktest 079).
- Not done: the optional PS/2 mouse (TrackPoint/touchpad) on the aux
  port.
- Untested on hardware, by the author's decision: everything. QEMU has no
  high-speed or SuperSpeed hub, so the Transaction Translator fields and
  the SuperSpeed hub path are tested on made-up trees and by reading only.

## Security review (stage 2b)

The review found 0 Critical, 0 High, 1 Medium, 5 Low (its summary said
4 Low; the report lists L1-L5), 3 Info. Fixed, each with tests: M1 (bus
master kept only by bridges and the framebuffer's display function; docs
say the bit is a hint and VT-d is the fix), L1 (128-bit CPUID TSC math,
plausible range), L2 (object-level `\_S5` walk with a flagged fallback;
it also found a PkgLength decoding bug in the stage's AML reader), L3
(power registers range-checked, every page of an MMIO access mapped;
SMI_CMD 0xb2 allowed below 0x100), L4 (pages of failed Disable Slots
counted, a controller stops attaching after 3), L5 (connect rate limit
per port, tested with a flapping QEMU port), I2 (error label). I1 and I3
needed no change. Not done: a controller reset to reclaim pages kept
after failed Disable Slots; `\_S5` inside an `If` (the walk skips If
bodies; the fallback still finds it if the walk gets stuck elsewhere).
