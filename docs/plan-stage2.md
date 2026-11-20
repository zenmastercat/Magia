# Stage 2 plan: a real keyboard on a PC without PS/2

Working plan written before the code, like `plan-stage1.md`. Goal: type at
the `magia>` prompt on the target PC (Intel Core Ultra 9 285K, Z890-class
board), which has no i8042: its keyboards are USB devices behind the
Intel xHCI controller (PCI 8086:7ec0 / 8086:7f6e). "Done" means
`nix-shell --run 'make test'` passes from a fresh clone, with QEMU q35 +
OVMF and `-device qemu-xhci -device usb-kbd`.

## Ground rules (as in stage 1)

- FreeBSD/OpenBSD code only under BSD-2/BSD-3/ISC/MIT/0BSD/public-domain
  terms, original headers kept, every adapted file in `docs/THIRD_PARTY.md`.
  Linux: ideas only.
- Hardware access in C; everything a device sends that has structure
  (USB descriptors, HID reports) is parsed in Rust with host tests on
  malformed input. Fixed-size hardware records (TRBs, PCI config space)
  are read in C with explicit bounds checks and ktests.
- Smallest complete change (Ponytail): no USB hub driver, no hot-plug, no
  full HID report-descriptor parser (the boot protocol makes it
  unnecessary), no isochronous or bulk transfers.

## Sources (porting map, stage 2 rows)

- PCI: FreeBSD `sys/dev/pci` is the model, but the config-space walk
  Magia needs (ECAM, bridges, BARs, capability list, MSI/MSI-X) is a few
  hundred lines, so it is written natively from the PCI Local Bus 3.0 and
  PCIe 4.0 specifications; the ECAM base comes from the MCFG table Magia
  already parses (OpenBSD `acpimcfg.c` as reference). OpenBSD `pci.c`,
  `pcireg.h` carry 4-clause advertising notices and are not used.
- xHCI: OpenBSD `sys/dev/usb/xhcireg.h` (BSD-2, Martin Pieuchot and Hans
  Petter Selasky) adapted for the register and TRB definitions; the
  controller bring-up, ring and command sequences follow OpenBSD
  `sys/dev/usb/xhci.c` (ISC, Martin Pieuchot), adapted without OpenBSD's
  usbdi/bus_dma layers (Magia drives one keyboard, synchronously during
  enumeration, from an interrupt afterwards).
- HID boot keyboard: OpenBSD `ukbd.c`/`hidkbd.c` (ISC + NetBSD BSD-2)
  for the report handling idea (new keys = keys not in the previous
  report; ErrorRollOver ignored); the code is Rust, written from the HID
  1.11 specification and usage tables.

## Milestones

1. **PCI/PCIe enumeration over ECAM** (`kernel/pci.c`): map each bus's
   1 MiB config window lazily, scan bus 0 and follow bridges to their
   secondary buses (bounded, each bus once); record vendor, device,
   class, header type for every function; `lspci` monitor command.
   Test: ktest finds the q35 host bridge (8086:29c0), the LPC bridge
   (8086:2918) and the xHCI controller (1b36:000d, class 0c/03/30); the
   monitor test runs `lspci`.
2. **BARs and MSI/MSI-X**: decode and size BARs (32/64-bit memory, I/O;
   decoding disabled while sizing), bounded capability-list walk (no
   loops, pointers >= 0x40 and dword-aligned, at most 48 entries), MSI-X
   table programming (address 0xFEE00000 | APIC id, data = vector, entry
   unmasked, function enabled, INTx disabled), MSI as fallback; vectors
   from a small pool (56-63) dispatched through the LAPIC.
   Test: ktest checks the xHCI BAR0 (64-bit memory, size and alignment),
   the capability walk (MSI-X found, loop/garbage pointers stop), and the
   programmed MSI-X entry; delivery is proven in milestone 3.
3. **xHCI host controller** (`kernel/xhci.c`): BIOS hand-off (USB legacy
   support capability), reset, DCBAA + scratchpad buffers, command ring,
   one event ring, MSI-X interrupt; port reset, Enable Slot, Address
   Device, GET_DESCRIPTOR (8-byte prefix, max-packet fix-up through
   Evaluate Context, then the full descriptor, parsed in Rust).
   Test: ktest sees the controller running, a device on a root port, its
   device descriptor (QEMU usb-kbd: 0627:0001) and interrupts arriving
   by MSI-X.
4. **USB HID boot keyboard**: GET_DESCRIPTOR(config) parsed in Rust
   (`rust/src/usb.rs`: first boot-keyboard interface and its interrupt-IN
   endpoint), SET_CONFIGURATION, SET_PROTOCOL(boot), SET_IDLE(0),
   Configure Endpoint, interrupt transfers re-queued on completion;
   reports turned into ASCII in Rust (new keys only, modifiers, Caps
   Lock, ErrorRollOver ignored) and pushed into the console input ring.
   Test: host cargo tests with good and malformed descriptors and
   reports (zero lengths, overruns, wrong types, random buffers); a
   monitor run types `echo usb-OK` through QEMU `sendkey` into the
   usb-kbd and checks the command ran and the USB report counter moved;
   the PS/2 monitor run (no usb-kbd) keeps working.
5. **Docs**: README (what works on a real PC now), ROADMAP, TESTING,
   ARCHITECTURE, THIRD_PARTY, NOTICE.

## Risks

- Real Intel xHCI needs the BIOS hand-off, scratchpad buffers, 64-byte
  contexts on some parts, and port power/reset timing that QEMU forgives;
  each is implemented from the spec even though QEMU does not need it.
- Keyboards behind a hub (front-panel headers, monitor hubs) need a hub
  driver: out of scope, recorded as a gap; root-port keyboards work.
- `sendkey` goes to the most recently activated QEMU keyboard; runs that
  test PS/2 boot without usb-kbd.

## Result (stage 2 done)

All milestones are in and `make test` passes from a fresh clone: 159
host tests; 102 ktests in each of the OVMF, 32 GiB, SeaBIOS, noapic and
KVM boots; the serial + PS/2 monitor run, the USB-keyboard monitor run,
the crash runs. Deviations:

- Milestones 3 and 4 are one commit: device enumeration and keyboard
  setup are one code path in `xhci.c`.
- The real-PC claim is untested: everything runs on QEMU's `qemu-xhci`
  (USB 2 keyboard on a high-speed root port, MSI-X). The Intel-specific
  parts (BIOS hand-off, port routing, scratchpads, 64-byte contexts) are
  implemented from the spec and OpenBSD but never exercised by QEMU.
- `SET_PROTOCOL` and `SET_IDLE` failures are tolerated (boot keyboards
  start in the boot protocol; some stall SET_IDLE).
- MSI and MSI-X work in `noapic` mode too (the LAPIC stays enabled as a
  virtual wire), so the USB keyboard does not depend on the I/O APIC.
