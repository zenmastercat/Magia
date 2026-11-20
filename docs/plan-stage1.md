# Stage 1 plan: from QEMU toy to a kernel that can boot a real UEFI PC

Working plan written before the code (like `plan.md` for 0.1). Each step
names the test that proves it. "Done" means `nix-shell --run 'make test'`
passes from a fresh clone, now booting through OVMF (UEFI) in QEMU with
`-smp 4 -m 8G`.

## Ground rules (changed from 0.1)

- FreeBSD and OpenBSD code may be adapted when its licence is BSD-2,
  BSD-3, ISC, MIT-style or public domain. Every adapted file keeps its
  original copyright header (or carries a "Portions adapted from ..."
  header with the original lines) and is listed in `docs/THIRD_PARTY.md`.
  Never CDDL, GPL, `contrib/`, or files with unclear terms.
- Linux: ideas only, re-implemented, cited in `docs/IDEAS.md`.
- Hardware stays in C; anything that parses firmware input (ACPI) is Rust.
- Test first where cheap (Rust parsers get host tests with malformed
  tables before the kernel uses them).
- Commit after every milestone.

## Milestones

1. **Limine boot, PIE kernel, all RAM** (replaces multiboot1).
   - `include/limine.h` (0BSD, from the Limine protocol repo); requests for
     base revision 6, HHDM, memory map, executable address, framebuffer,
     RSDP, command line, entropy, firmware type.
   - Kernel linked as a static PIE at `0xffffffff80000000`
     (`-fpie`, Rust `relocation-model=pie`); Limine applies the
     `R_X86_64_RELATIVE` relocations and, with `kaslr: yes`, slides the
     image; the HHDM base is randomised too. Magia reuses Limine's HHDM
     offset as its own direct-map base.
   - pmm/vmm from the Limine memory map: no 1 GiB cap, direct map sized to
     RAM (2 MiB pages where aligned, only RAM/ACPI types are mapped WB),
     bootloader-reclaimable memory freed after the switch to Magia's page
     tables.
   - `make image` -> `build/magia.img` (GPT, FAT32 ESP, Limine UEFI +
     BIOS); tests boot it with OVMF; kernel command line per test through
     an SMBIOS OEM string (`limine:config:`), so one image serves all runs.
   - Test: existing 75 ktests + monitor + crash tests under OVMF; new
     ktest allocates and touches pages above 4 GiB; a `-m 32G` boot.
2. **Framebuffer console** with the Spleen 8x16 font (BSD-2, from OpenBSD
   `sys/dev/wsfont/spleen8x16.h`), serial kept as secondary output; PS/2
   keyboard input so `magia>` works on the screen too.
   Test: ktest reads glyph pixels back; monitor test types over serial and
   over the PS/2 keyboard (QEMU `sendkey`).
3. **ACPI in Rust**: RSDP -> XSDT/RSDT -> MADT, HPET, MCFG, FADT with
   checksum, length and alignment checks (semantics from FreeBSD
   `sys/x86/acpica/madt.c`; no ACPICA, no AML).
   Test: host cargo tests with good and malformed tables (truncated,
   bad checksum, bad entry lengths, overlapping, huge counts, random
   mutations); ktest checks the QEMU tables (4 CPUs, 1 IOAPIC, HPET).
4. **LAPIC (xAPIC + x2APIC), IOAPIC, HPET, TSC deadline**; the 8259 is
   masked; LAPIC timer calibrated against HPET (PIT fallback); `noapic`
   keeps the legacy PIC + PIT path. Register names adapted from FreeBSD
   `sys/x86/include/apicreg.h` (BSD-3).
   Test: ktests for timer ticking, calibration sanity, IOAPIC routing of
   COM1; a KVM boot (when /dev/kvm exists) exercises x2APIC + TSC deadline.
5. **Kernel RNG**: ChaCha20 (public-domain `chacha_private.h` as carried by
   OpenBSD) with an arc4random-style buffered generator, fast key erasure
   and reseeding from RDSEED/RDRAND, TSC jitter and Limine entropy.
   Stack-protector guard and thread-stack canaries come from it.
   Test: known-answer test of the ChaCha20 block, non-repeating output,
   reseed counter advances.
6. **Immutable kernel image** (OpenBSD `mimmutable(2)` idea): after boot,
   text/rodata stay read-only and their page-table pages become read-only
   too; `vmm_map`/`vmm_protect`/`vmm_unmap` refuse the range (`-EPERM`).
   Test: ktest proves a later remap fails and a direct PTE write faults.
7. **Docs**: README (build, `make image`, USB instructions with `lsblk`
   check), ARCHITECTURE, ROADMAP (stages 2-4, pledge/unveil/securelevel/
   pinsyscalls for stage 3), TESTING, IDEAS, THIRD_PARTY, NOTICE.

## Plan change: btrfs is the stage-4 disk file system (docs only in stage 1)

The user decided "this kernel is built with btrfs in mind": the main disk
file system in stage 4 is btrfs, on-disk compatible, replacing the UFS
plan. No BSD/ISC btrfs implementation exists and Linux's is GPL, so it is
written from the public on-disk format documentation
(btrfs.readthedocs.io), not from Linux/GRUB/U-Boot/WinBtrfs code; parsing
on-disk metadata is untrusted input, so it is Rust. Order: read-only mount
(superblock, chunk tree, root tree, FS tree, extents, crc32c, then
xxhash64/sha256/blake2b, zlib and zstd via FreeBSD's BSD-licensed copies)
-> copy-on-write writes -> subvolumes and snapshots. btrfs-progs
(`mkfs.btrfs`, `btrfs check`) are used only as external host tools on
image files. ramfs stays. Stage 1 records this in ROADMAP/ARCHITECTURE/
IDEAS and shapes the planned block-layer/VFS interfaces for it
(multi-device, per-block checksums, CoW, 4 KiB sectors with 16 KiB nodes);
no btrfs code is written in stage 1.

## Risks

- PIE + kernel: assembly must use RIP-relative addressing; the symbol
  table for backtraces stores link-time addresses (subtract the slide).
- TCG may lack x2APIC/TSC-deadline: keep xAPIC and periodic LAPIC paths
  and test both (KVM run when available).
- Real hardware (Core Ultra 9 285K): no PS/2 keyboard and maybe no COM
  port; keyboard needs xHCI + HID (stage 2). Recorded in ROADMAP.

## Result (stage 1 done)

All seven milestones are in, each in its own commit, and `make test`
passes from a fresh clone (153 host tests; 91 ktests in each of the
OVMF, 32 GiB, SeaBIOS, noapic and KVM boots; monitor and crash runs).
Deviations from the text above, and why:

- Milestone 3 (ACPI) was committed before milestone 2 (console); the
  content of each matches the plan.
- QEMU runs use the `q35` machine (it has MCFG/ECAM like a real PC); the
  32 GiB run boots with `nox2apic` so the xAPIC MMIO path is tested too.
- The RSDP 16-byte alignment check applies only to the BIOS scan: under
  UEFI the RSDP comes from the EFI configuration table and OVMF places it
  at an address ending in ...14 (ACPI has no alignment rule there).
- FreeBSD's `apicreg.h` is BSD-2-Clause (SPDX header), not BSD-3 as noted
  above.
- TCG offers x2APIC but no TSC-deadline timer: TCG runs test the periodic
  LAPIC timer, the KVM run (`-cpu host` on the Core Ultra 9 285K) tests
  x2APIC + TSC deadline.
- The immutable range is the whole kernel window (PML4 slot 511: text,
  rodata, data, bss), not only text/rodata: refusing the whole slot keeps
  every table under it read-only without exceptions.
- GNU ld marks a PIE with a fixed non-zero base `ET_EXEC`; after checking
  the image (RELATIVE relocations only, no TEXTREL, no W+X segment)
  `scripts/fix-pie.sh` sets `ET_DYN`, which Limine requires to relocate.
- The image also has a 1 MiB BIOS boot partition (GPT), which Limine's
  `bios-install` needs for its stage 2.
