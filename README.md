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

## What it does (stage 1)

- Boots through **Limine** (UEFI and BIOS) from a GPT disk image; the
  kernel is a static **PIE** that Limine relocates and slides (**KASLR**),
  with a randomised direct map of all RAM (no size cap; tested with 32 GiB).
- Framebuffer console (Spleen 8x16 font from OpenBSD) plus serial; PS/2
  keyboard; the `magia>` debug monitor works on screen and on the serial
  line.
- ACPI tables (RSDP, XSDT/RSDT, MADT, HPET, MCFG, FADT) parsed **in Rust**
  as untrusted input, fuzzed by host tests.
- Local APIC (xAPIC and x2APIC), I/O APIC, HPET; 1000 Hz tick from the
  TSC-deadline timer or the LAPIC timer calibrated against the HPET;
  `noapic` keeps the legacy 8259 PIC + PIT path.
- ChaCha20 kernel RNG (arc4random-style, fast key erasure, reseeded from
  RDSEED/RDRAND, TSC jitter and Limine's entropy); the stack-protector
  guard and thread-stack canaries come from it.
- Hardening: W^X page tables, NX, CR0.WP, SMEP, SMAP, UMIP, an
  **immutable kernel image** after boot (remaps refused, kernel page-table
  pages read-only - OpenBSD's `mimmutable` idea), stack protector,
  zero-initialised stack variables.
- From 0.1: buddy page allocator, slab-style `kmalloc`, preemptive kernel
  threads with an **EEVDF** policy (Rust), `syscall`/`sysret`, and in Rust:
  syscall table with typed validation, **Capsicum**, **jails** with
  hierarchical accounting, VFS (ramfs, devfs, pipes), **kqueue**, an
  io_uring-style ring, **futex**, **sysctl**.
- 91 in-kernel tests (`ktest`) and 153 host `cargo test`s; `make test`
  boots the image nine times (OVMF/UEFI, 32 GiB, SeaBIOS, `noapic`, KVM,
  the monitor, three deliberate crashes).

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
`ls /dev`, `cat /etc/motd`, `sysctl`, `mem`, `uptime`, `poweroff`.

Kernel command line words: `ktest` (run the in-kernel tests and power
off), `noapic` (8259 PIC + PIT), `nox2apic`, `notscdeadline`, `nohpet`,
`nokbd`, `verbose` (print each boot step).

## Booting a real PC from a USB stick

`make image` only ever writes `build/magia.img`; nothing in the build
touches a disk. To try Magia on real hardware, write the image to a USB
stick **yourself**, and only after checking which device is the stick -
`dd` to the wrong device destroys that disk:

```sh
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS   # find the stick: TRAN "usb", the size you expect
# say it is /dev/sdX (NOT a partition like /dev/sdX1, and NOT your system disk);
# unmount anything mounted from it, then:
sudo dd if=build/magia.img of=/dev/sdX bs=4M conv=fsync status=progress
sync
```

Boot the PC from the stick (UEFI with Secure Boot off, or legacy
BIOS/CSM). The Limine menu offers the normal boot, `noapic`, and `ktest`.
What to expect on the target PC (Core Ultra 9 285K, RTX 5080): the
framebuffer console uses the firmware's GOP mode; such boards usually have
**no PS/2 keyboard and maybe no COM port**, so input needs the stage-2 USB
(xHCI + HID) driver - see [docs/ROADMAP.md](docs/ROADMAP.md).

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - boot, memory layout, interrupts, subsystems
- [docs/TESTING.md](docs/TESTING.md) - what `make test` checks
- [docs/ROADMAP.md](docs/ROADMAP.md) - stages 2-4 and known gaps
- [docs/IDEAS.md](docs/IDEAS.md) - every borrowed idea and its source
- [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md) - adapted files and their licences
- [docs/LANGUAGES.md](docs/LANGUAGES.md) - the C/Rust split and why
- [docs/SYSCALLS.md](docs/SYSCALLS.md) - the system call ABI
- [docs/plan-stage1.md](docs/plan-stage1.md), [docs/plan.md](docs/plan.md) - milestone plans

## License

BSD-2-Clause, see [LICENSE](LICENSE). Third-party parts keep their own
permissive licences; see [NOTICE](NOTICE).
