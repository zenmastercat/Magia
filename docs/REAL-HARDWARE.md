# Magia on real hardware

Status: **not run on real hardware yet.** The author decided (stage 2b)
not to boot Magia on the real machines until the kernel is done; until
then every device path is tested in QEMU with the closest emulated
hardware (docs/TESTING.md). This page is for that later day: how to put
the image on a USB stick, which firmware settings the two target machines
need, and what to photograph when something fails - neither machine has
a serial port, so the screen is the only log.

## The machines

| Machine | What Magia should find | Input |
|---|---|---|
| Desktop: MSI MAG B860 TOMAHAWK WIFI, BIOS 1.B10, Core Ultra 9 285K | two xHCI controllers: `c0` = CPU 8086:7ec0 at 00:0d.0, `c1` = chipset 8086:7f6e at 80:14.0; no i8042 | Keychron K2 Max (3434:0a20, full speed) behind a Genesys USB 2.0 hub (05e3:0608) on chipset port 6: `usb` should list the hub and the keyboard below it (path `6.N`); a Logitech receiver (046d:c548) on chipset port 4 (boot keyboard and mouse) |
| ThinkPad X13 Gen 1 Intel (20T2/20T3), BIOS N2YET | chipset xHCI 8086:02ed (12 USB 2 + 6 USB 3 ports), the Thunderbolt chip's xHCI 8086:15c1; the internal keyboard on the embedded controller's i8042 | the internal keyboard (PS/2); TrackPoint and touchpad are PS/2 too but have no driver yet |

Port numbers in `usb` and `hwinfo` are the xHCI's root port numbers. On
Intel controllers the USB 2 ports usually come first and the USB 3 ports
after them, so a port Linux calls `3-6` (USB 2 bus, port 6) is likely
`c1 p6`, and a USB 3 device on the same connector shows up on a higher
port number.

## Build the image

```sh
nix develop -c make image      # build/magia.img: 64 MiB GPT disk, FAT32 ESP, Limine for UEFI and BIOS
```

The kernel command line is in `boot/limine.conf`; the Limine menu offers
the normal boot, `verbose` (prints each boot step), `noapic`, and `ktest`
(the in-kernel tests expect QEMU's devices, so some fail on real hardware;
they still exercise the drivers).

## Write it to a USB stick

**This overwrites the whole device you name. Writing to the wrong device
destroys that disk (your system disk included). Magia's scripts never do
this; you do it by hand, once you are sure.**

1. Without the stick plugged in, list the disks:
   `lsblk -o NAME,SIZE,MODEL,TRAN,RM,MOUNTPOINTS`
2. Plug the stick in and list them again. The stick is the new entry,
   with `TRAN` `usb`, `RM` 1 and the size you expect. Note its name,
   for example `sdX` - the whole device, **not** a partition such as
   `sdX1`, and never an `nvme...` device.
3. Unmount anything the desktop mounted from it
   (`udisksctl unmount -b /dev/sdX1` for each partition shown).
4. Write and flush:

   ```sh
   sudo dd if=build/magia.img of=/dev/sdX bs=4M conv=fsync status=progress
   sync
   ```

5. Run `lsblk` again: the stick now shows two partitions (the BIOS boot
   partition and a 62 MiB EFI system partition labelled `MAGIA`).

A graphical tool that restores a disk image (GNOME Disks: "Restore Disk
Image") does the same; the same warning applies.

## Firmware settings

| | Desktop (MSI B860) | ThinkPad X13 Gen 1 |
|---|---|---|
| Setup key | Del | F1 at the logo (F9 loads defaults, F10 saves) |
| One-time boot menu | F11 | F12 at the logo |
| Boot mode | UEFI (Limine's BIOS stages also work with CSM) | "UEFI Only" (the default) |
| Secure Boot | already off (measured) | **on by default: turn it off** (Security -> Secure Boot -> Disabled). The keys can be restored later ("Restore Factory Keys"); Magia's own keys could be enrolled one day |
| Other | - | "8254 Timer Clock Gating" may be on: Magia does not depend on the PIT (TSC frequency from CPUID 0x15 or the HPET, every PIT wait bounded). "Kernel DMA Protection" may stay on; Magia does not use VT-d yet. Thunderbolt pre-boot support is off by default, so devices on the Thunderbolt port may not appear |

The laptop facts come from Lenovo's documentation and its BIOS simulator
(which Lenovo says may differ from the real BIOS); check them on the
machine.

## What to photograph

Take sharp, straight photos of the whole screen; several are fine.

1. **If the boot stops or the screen turns red**: the panic screen as it
   is. The red header has the message; below it are the newest kernel
   log lines, including the backtrace.
2. **If it stops without a red screen**: the screen as it is (the last
   lines tell which step was running; `verbose` on the kernel command
   line prints each boot step), and how long you waited.
3. **At the `magia>` prompt**, if typing works (any keyboard):
   - `hwinfo` - one screen: CPU, firmware, memory, ACPI facts (8042 flag,
     reset register, `\_S5`), every PCI function and every USB device;
   - `dmesg`, then Page Up / Page Down for earlier pages - the boot log
     (look for `xhci:`, `usb:`, `kbd:`, `timer:`, `acpi:` lines);
   - `usb` - each controller's counters and each device with its route,
     TT fields, report and error counts;
   - `cpu`, `mem`, `uptime`;
   - `bench` - only when asked for numbers.
4. **If no keyboard works**: the screen after the boot (it shows the
   `xhci:`, `usb:` and `kbd:` lines). Try another rear USB port; a
   keyboard behind a hub should work since stage 2b.
5. Note which port each keyboard was in, and whether it was plugged in
   before power-on or later (hot-plug).

Power: `poweroff` should switch the machine off through ACPI S5; if it
prints "still running after the S5 write" or "no \_S5", photograph that
and hold the power button. `reboot` uses the ACPI reset register, then
the keyboard controller and a triple fault.

## Safety

Magia has no storage drivers (NVMe, SATA): it cannot read or write the
machine's disks. It drives only the USB controllers, the PS/2 controller,
the interrupt controllers, timers and the framebuffer. Device DMA is not
restricted (no IOMMU yet, see ROADMAP.md): do not plug untrusted devices
in while Magia runs.

## Later: a log cable instead of photos

Both desktop xHCI controllers expose the USB Debug Capability (DbC), and
the laptop's chipset controller probably does. With a USB 3 A-to-A debug
cable to a second computer, a DbC driver could stream the kernel log
without a serial port. It is not built yet.
