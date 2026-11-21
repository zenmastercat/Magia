# Magia

Magia is a small **experimental** x86_64 hobby/research kernel - kernel
only, no userland yet - written in C (hardware) and `no_std` Rust
(everything that parses untrusted input). It boots on UEFI and BIOS PCs
through Limine and, stage by stage, is meant to grow into a kernel that
runs on a real desktop PC.

> **Where the code comes from.** Most of Magia is written for Magia. Since
> stage 1 it also contains a few files adapted from **FreeBSD** and
> **OpenBSD** under their permissive licences (BSD-2-Clause, public
> domain), each keeping its original copyright header and listed in
> [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md) and [NOTICE](NOTICE).
> **Linux** is used for ideas only, never code. Every borrowed idea and its
> source is in [docs/IDEAS.md](docs/IDEAS.md). It is a learning and
> research project: not secure, not stable, not for real data.

## What it does (stages 1, 2 and 2b)

- Boots through **Limine** (UEFI and BIOS) from a GPT disk image; the
  kernel is a static **PIE** that Limine relocates and slides (**KASLR**),
  with a randomised direct map of all RAM (no size cap; tested with 32 GiB).
- Framebuffer console (Spleen 8x16 font from OpenBSD) plus serial; the
  `magia>` debug monitor takes input from the serial line, a PS/2
  keyboard, or a **USB keyboard** (stage 2).
- **PCIe** enumeration over ECAM (MCFG), BAR sizing, **MSI-X/MSI**;
  `lspci` in the monitor.
- **xHCI USB 3 host controllers** (every one on the PCI bus, each on its
  own MSI-X vector) with **USB 2/3 hubs** (OpenBSD `uhub.c` order, route
  strings, Transaction Translator fields), **hot-plug**, **several HID
  boot keyboards** at once (composite devices included), a **boot
  mouse**, and recovery of halted endpoints; descriptors, hub data and
  reports are parsed in Rust; `usb` in the monitor (stage 2b).
- For machines without a serial port (stage 2b): every console byte is
  kept in a **kernel log** (`dmesg`), the screen scrolls back with Page
  Up/Down, a **panic screen** shows the newest log lines, and `hwinfo`
  puts the PCI and USB IDs on one screen to photograph.
- **ACPI power** (stage 2b): `reboot` through the FADT reset register,
  `poweroff` through S5 with the `\_S5` sleep type read from the DSDT by a
  minimal AML reader in Rust.
- A kernel-side **benchmark harness** (`bench`, `ktest bench`) timing
  system calls, context switches, interrupt latency, page mapping and the
  allocators with the TSC (stage 2b; numbers are Magia-internal).
- ACPI tables (RSDP, XSDT/RSDT, MADT, HPET, MCFG, FADT) parsed **in Rust**
  as untrusted input, fuzzed by host tests.
- Local APIC (xAPIC and x2APIC), I/O APIC, HPET; 1000 Hz tick from the
  TSC-deadline timer or the LAPIC timer; the TSC frequency from CPUID
  0x15, the HPET or a bounded PIT measurement (laptops may gate the PIT);
  `noapic` keeps the legacy 8259 PIC. The i8042 (PS/2) is probed even
  when the ACPI FADT says there is none.
- ChaCha20 kernel RNG (arc4random-style, fast key erasure, reseeded from
  RDSEED/RDRAND, TSC jitter and Limine's entropy); the stack-protector
  guard and thread-stack canaries come from it.
- Hardening: W^X page tables, NX, CR0.WP, SMEP, SMAP, UMIP, an
  **immutable kernel image** after boot (OpenBSD's `mimmutable` idea:
  remapping the kernel window or the image's direct-map alias is refused,
  and the page tables under the kernel window are read-only; the direct
  map's own tables are not, see the roadmap), stack protector,
  zero-initialised stack variables; boot stops without NX.
- From 0.1: buddy page allocator, slab-style `kmalloc`, preemptive kernel
  threads with an **EEVDF** policy (Rust), `syscall`/`sysret`, and in Rust:
  syscall table with typed validation, **Capsicum**, **jails** with
  hierarchical accounting, VFS (ramfs, devfs, pipes), **kqueue**, an
  io_uring-style ring, **futex**, **sysctl**.
- 123 in-kernel tests (`ktest`), 177 host `cargo test`s and a host C
  test; `make test` boots the image fifteen times (OVMF/UEFI, 32 GiB,
  SeaBIOS, `noapic`, the PIT tick, a gated PIT without HPET, KVM with
  the benchmarks, a desktop-shaped USB machine without PS/2, the monitor
  over serial + PS/2, over a USB keyboard and over hub keyboards with
  hot-plug, a SeaBIOS poweroff, three deliberate crashes). The QEMU
  machine is q35 with `qemu-xhci`, `usb-hub`, `usb-kbd` and `usb-mouse`.

Why two languages: [docs/LANGUAGES.md](docs/LANGUAGES.md).

## Build and test

On a system with Nix, from the repository root:

```sh
nix-shell --run 'make test'
```

`shell.nix` provides gcc, binutils, make, QEMU, OVMF (UEFI firmware),
mtools, sgdisk, curl, python3 and rustup; `rust-toolchain.toml` makes
rustup fetch the stable toolchain and the `x86_64-unknown-none` target on
first use. The first `make image` downloads the pinned Limine 12.10.0
release binaries from GitHub and checks their SHA-256 (set
`LIMINE_TARBALL=/path/to/limine-binary.tar.gz` to build offline).

Without Nix (untested; Debian/Ubuntu-like systems):

```sh
sudo apt install build-essential qemu-system-x86 ovmf mtools gdisk curl python3
curl -sSf https://sh.rustup.rs | sh -s -- -y --target x86_64-unknown-none && . "$HOME/.cargo/env"
make test OVMF=/usr/share/ovmf/OVMF.fd
```

## Use

```sh
nix-shell --run 'make image'                # build/magia.img (64 MiB GPT disk image)
nix-shell --run 'make run'                  # boot it in QEMU (UEFI), serial on stdio
nix-shell --run 'make run CMDLINE=noapic'   # with a kernel command line
nix-shell --run 'make test'                 # everything (see docs/TESTING.md)
```

At the `magia>` prompt: `help`, `uname`, `cpu`, `ps`, `threads`, `jails`,
`ls /dev`, `cat /etc/motd`, `sysctl`, `mem`, `uptime`, `lspci`, `usb`,
`hwinfo`, `dmesg`, `bench`, `reboot`, `poweroff`. Page Up/Down scroll the
screen.

Kernel command line words: `ktest` (run the in-kernel tests and exit
QEMU; with `bench` also the benchmarks), `noapic` (8259 PIC), `pittick`
(with `noapic`: PIT tick), `nox2apic`, `notscdeadline`, `nohpet`,
`nopit` (act as if the PIT were gated), `nocpuidtsc`, `nokbd` (no PS/2),
`fadt8042=0` (act as if the FADT said no i8042), `noxhci` (no USB),
`verbose` (print each boot step).

## Real hardware

Nothing in the build touches a disk; `make image` only writes
`build/magia.img`. The author decided not to boot Magia on the real
machines (an MSI B860 desktop with a Core Ultra 9 285K, and a ThinkPad
X13 Gen 1 Intel) until the kernel is done; until then every device path
is tested in QEMU with the closest emulated hardware. How to write the
image to a USB stick, the firmware settings for both machines, and what
to photograph when something fails are in
[docs/REAL-HARDWARE.md](docs/REAL-HARDWARE.md).

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - boot, memory layout, interrupts, subsystems
- [docs/TESTING.md](docs/TESTING.md) - what `make test` checks
- [docs/ROADMAP.md](docs/ROADMAP.md) - stages 2-4 and known gaps
- [docs/IDEAS.md](docs/IDEAS.md) - every borrowed idea and its source
- [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md) - adapted files and their licences
- [docs/LANGUAGES.md](docs/LANGUAGES.md) - the C/Rust split and why
- [docs/SYSCALLS.md](docs/SYSCALLS.md) - the system call ABI
- [docs/REAL-HARDWARE.md](docs/REAL-HARDWARE.md) - USB stick, firmware settings, what to photograph
- [docs/plan-stage2b.md](docs/plan-stage2b.md), [docs/plan-stage2.md](docs/plan-stage2.md), [docs/plan-stage1.md](docs/plan-stage1.md), [docs/plan.md](docs/plan.md) - milestone plans

## License

BSD-2-Clause, see [LICENSE](LICENSE). Third-party parts keep their own
permissive licences; see [NOTICE](NOTICE).
