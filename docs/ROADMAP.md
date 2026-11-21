# Roadmap and known gaps

Magia grows in stages towards "stage 4": a kernel that runs on the
author's real PC (Intel Core Ultra 9 285K, 24 cores, 30 GB RAM, RTX 5080,
UEFI). FreeBSD is the primary example, OpenBSD supplies compact drivers
and security features, Linux gives ideas only (GPL: never its code).
File-level choices for stages 2-4 (FreeBSD/OpenBSD files, licences,
sizes) are in the porting map kept with the project notes; this file
lists the plan and, honestly, what is *not* built. Items marked (RD) come
from Research Desk's comparison
([`docs/research/kernel-pros.md`](research/kernel-pros.md), row in
brackets).

## Stage 1 (done): from QEMU toy to a kernel that can boot a UEFI PC

Limine boot (UEFI + BIOS image), static PIE with KASLR, all RAM managed,
framebuffer console + PS/2 keyboard, ACPI tables in Rust, LAPIC/x2APIC +
I/O APIC + HPET + TSC deadline, ChaCha20 RNG, immutable kernel image. Plan
and per-milestone tests: [plan-stage1.md](plan-stage1.md).

## Stage 2 (done): a keyboard on a PC without PS/2

PCIe enumeration over ECAM (MCFG), BARs, MSI-X/MSI, an xHCI host
controller and a USB HID boot keyboard on a root port; `lspci`, `usb`.
Plan and tests: [plan-stage2.md](plan-stage2.md).

## Stage 2b (done): real-hardware bring-up

For the author's two machines (an MSI B860 desktop with a Core Ultra 9
285K, and a ThinkPad X13 Gen 1 Intel), tested only in QEMU until the
kernel is done (the author's decision). Plan and tests:
[plan-stage2b.md](plan-stage2b.md); how to try it on the machines later:
[REAL-HARDWARE.md](REAL-HARDWARE.md).

- Kernel log ring (`dmesg`), scroll-back with Page Up/Down, a full-screen
  panic report, `hwinfo` (PCI and USB IDs on one screen).
- Every xHCI controller; hot-plug (port-change events, hub status
  changes); USB 2/3 hubs with route strings and Transaction Translator
  fields; several keyboards at once, composite devices; a boot mouse;
  review F6 (halted endpoints are reset and re-queued, given up after 3
  errors in a row).
- i8042 probed even when the FADT says there is none; the TSC frequency
  from CPUID 0x15, the HPET or a bounded PIT measurement; the LAPIC timer
  ticks in every mode that has one (laptops may gate the PIT).
- ACPI reboot (reset register) and poweroff (`\_S5` from AML, PM1
  control, SMI_CMD hand-over from legacy mode).
- A kernel-side benchmark harness for the paper (`bench`).

Still to do on the device side, carried into stages 3-4:

- **Untested on the real machines**: everything above runs on QEMU's
  devices only. QEMU has no high-speed or SuperSpeed hub, so the TT
  fields (the desktop's keyboard sits behind a high-speed Genesys hub)
  and the SuperSpeed hub path (SET_HUB_DEPTH, the 0x2a descriptor) are
  checked by ktests on made-up trees and by reading, not on hardware.
- Laptop input beyond the keyboard: the TrackPoint and the Elan touchpad
  (PS/2 aux port; Elantech v4 later), battery, lid and keys through the
  embedded controller (needs an AML interpreter).
- A usbdi-like layer when the second USB class driver (mass storage)
  comes; HID report descriptors (non-boot keyboards and mice, key repeat,
  layouts); USB Debug Capability (DbC) as a log cable.
- Interrupt remapping (VT-d) so MSI and I/O APIC messages can target
  APIC ids above 255; legacy INTx would need ACPI `_PRT` (AML).
- **DMA remapping (VT-d)**, stage 4: today there is none, so W^X and the
  immutable image do not hold against bus-master DMA (see Known gaps).
- The Linux comparison needs the user-space benchmark suite (stage 3).

## Stage 3: programs (Unix-like userland)

- Per-process address spaces (the kernel half is already identical in
  every PML4: entries 256-511 are pre-created; the kernel window is
  immutable), an ELF loader with ASLR, `fork`/`exec`/`wait`, signals.
- FreeBSD amd64 system-call ABI (numbers and struct layouts) for the
  subset Magia implements, so FreeBSD libc and tools build with small
  changes; Capsicum keeps its FreeBSD numbers. FreeBSD `libc`, `/bin/sh`,
  base commands; OpenBSD `ksh`, `malloc`, `mandoc`; boot to a login
  prompt from a RAM-disk root loaded by Limine as a module.
- OpenBSD security features, re-implemented on Magia's syscall layer:
  - **pledge(2)**: per-process promise sets checked in the Rust syscall
    table (each table entry already carries a capability-mode flag; a
    promise mask is the next column).
  - **unveil(2)**: per-process path allow-list in the VFS lookup.
  - **securelevel**: a one-way kernel level (FreeBSD/OpenBSD semantics)
    that locks sysctls, raw device writes and module loading.
  - **pinsyscalls** / syscall-origin checks: the `syscall` instruction may
    only be executed from the pinned libc stub, building on the immutable
    mappings (`mimmutable`) that stage 1 introduced for the kernel image;
    stage 3 extends immutability to user text and stacks.

## Stage 4: the real PC

- **SMP** on the hybrid Core Ultra (P- and E-cores): x2APIC INIT-SIPI
  bring-up (FreeBSD `mp_x86.c`), per-CPU run queues and EEVDF per CPU
  with load balancing (RD row 5), finer locks (RD row 17), TLB shootdown.
- **Storage**: NVMe (OpenBSD `nvme.c`), AHCI, GPT; a block layer shaped
  for btrfs (below).
- **btrfs as the main disk file system**, on-disk compatible, replacing
  the earlier UFS plan. No BSD/ISC btrfs implementation exists and Linux's
  is GPL, so it is written from the public on-disk format documentation
  (btrfs.readthedocs.io), not from Linux, GRUB, U-Boot or WinBtrfs code.
  Parsing on-disk metadata is untrusted input, so it is Rust. Order:
  read-only mount (superblock, chunk tree, root tree, FS tree, extents,
  crc32c, then xxhash64/sha256/blake2b; zlib and zstd via FreeBSD's
  BSD-licensed copies) -> copy-on-write writes -> subvolumes and
  snapshots. btrfs-progs is used only as an external host tool to create
  and check test images. ramfs stays for `/tmp`. See ARCHITECTURE.md for
  the block-layer/VFS shape this implies.
- **Network**: virtio-net first (QEMU), then the board's Realtek Killer
  E5000 (OpenBSD `if_rge.c`, ISC); FreeBSD's TCP/IP stack.
- **DMA remapping (VT-d)**: per-device IOMMU page tables, so a
  bus-master device reaches only its own buffers and W^X and the
  immutable image hold against DMA too (none today, see Known gaps).
- Graphics stays the firmware framebuffer (no RTX 5080 driver planned).

## Known gaps (after stage 2b)

- **No userland, no ELF loader, no `fork`/`exec`.** Processes are kernel
  threads registered in the Rust process table; ring 3 is exercised only by
  tiny hand-written blobs inside ktest (`arch/x86_64/user_blob.S`).
- **Single address space.** User windows of all processes share one page
  table; a process can address another's window.
- **Uniprocessor.** The other CPUs (4 in the tests, 24 on the target) stay
  parked in firmware; "interrupts off" is the scheduler lock; Rust state
  sits behind a Giant lock.
- **Input on the target machines**: USB boot keyboards and mice on root
  ports and behind hubs (up to 5 tiers, hub ports 1-15), hot-plugged or
  not, and the i8042 keyboard; HID boot protocol only; untested on the
  real machines.
- **xHCI**: interrupt-IN endpoints only; devices without a class driver
  are addressed but unused; up to 4 controllers, 64 slots each; DMA
  needs a 64-bit capable controller (every current Intel one is); PCI
  segment 0 only; firmware's bus numbers and BAR assignments are trusted.
  Every HID boot keyboard is trusted as a console keyboard.
- **ACPI power without AML execution**: no `_PTS`/`_GTS` before S5 (some
  firmware wants them); a `\_S5` built by a method is not found
  (`poweroff` then says so and halts); reset registers in PCI
  configuration space are not written.
- **Benchmarks are Magia-internal** (KVM guest numbers); the paper's
  comparison with Linux needs the stage 3 user-space suite.
- **No DMA remapping (no VT-d / IOMMU).** Any bus-master device (the
  xHCI, or a hostile PCIe / Thunderbolt device on a physical port) can
  write any RAM: kernel text, page tables, the immutable image. W^X, NX
  and the immutable image protect against the CPU only. Bus mastering is
  turned off on every endpoint function Magia does not drive (bridges and
  display controllers excepted), which does not stop a hostile device
  that ignores the bit. VT-d DMA remapping comes in stage 4.
- **I/O APIC and MSI destinations are 8-bit** (xAPIC format): fine for the
  BSP on QEMU and the target (BSP id 0), not for ids above 254 without
  VT-d interrupt remapping.
- **Immutable image limits**: the PML4 and the direct map's own page
  tables stay writable; a kernel write primitive could re-point PML4[511]
  or edit the direct-map PTEs of the image's read-only alias (remapping
  that alias through the VM calls is refused, a raw PTE write is not).
  Fix with per-process PML4s built from a locked template and read-only
  direct-map tables for the image (stage 3).
- **Console logs** show physical addresses (LAPIC, HPET, memory map) but
  no slid kernel addresses; the register dump on a kernel trap needs
  `verbose`.
- **RNG**: no entropy estimation or health tests (SP 800-90B) for the
  TSC-jitter source (when it is the only source the boot log warns and
  `sysctl kern.random.weak` is 1); no `/dev/random` device yet.
- **No kernel stack guard pages.** Overflow is detected by the per-boot
  random canary at the bottom of each stack, not prevented.
- **User code windows are written through the direct map** (a writable
  kernel alias of executable user pages; no single mapping is W+X).
- **No storage, no network.** Jails cannot be removed; no signals,
  process groups or credentials.
- **No tickless idle**; HZ is fixed at 1000.

## Later ideas

1. Process descriptors (`pdfork`, Capsicum) (RD row 1).
2. kCFI + IBT: needs Clang for C (`-fsanitize=kcfi`) and Rust support for
   `-Z cf-protection` on stable (RD row 15, KSPP S88).
3. Ring completions posted to kqueue (`EVFILT_AIO`-like) (RD row 4).
4. A scheduler-class interface with a boot-time choice (FreeBSD 4BSD/ULE),
   later loadable with a safety fallback (Linux sched_ext idea) (RD row 5).
5. sysctl ABI stability levels and boot-time tunables (RD row 6).
6. Signed kernel modules with a build fingerprint; metadata in a linker
   set (FreeBSD `set_modmetadata_set`).
7. Storage on a blk-mq-style queue core with GEOM-style providers/consumers
   (RD row 7); VNET-style per-jail network stacks; one packet filter on a
   small verified bytecode VM (RD rows 10-11); DTrace-style static probes
   in linker sets.
8. Dentry cache with RCU-style lookup once SMP exists (RD row 8); per-CPU
   caches in kmalloc zones (RD row 9).
