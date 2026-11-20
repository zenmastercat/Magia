# Magia

Magia is a small **experimental** x86_64 hobby/research kernel - kernel
only, no userland - that combines ideas from FreeBSD and Linux, written
from scratch in C and Rust.

> **Not derived code.** Magia contains no code from Linux, FreeBSD or any
> other kernel. It takes *ideas* (Capsicum, jails, kqueue, sysctl, linker
> sets from FreeBSD; EEVDF, io_uring-style rings, futex, cgroup-style
> accounting from Linux) and re-implements them from public descriptions.
> Every idea and its source is listed in [docs/IDEAS.md](docs/IDEAS.md).
> It is a learning and research project: not secure, not stable, not for
> real hardware or real data.

## What it does

- Boots under QEMU (multiboot1) into 64-bit long mode, higher-half kernel,
  serial + VGA console with a banner.
- W^X kernel page tables, NX, CR0.WP, SMEP, SMAP (explicit user-access
  window), UMIP; `-fstack-protector-strong` with a random canary,
  zero-initialised stack variables, kernel thread stack canaries.
- Buddy page allocator, slab-style `kmalloc` zones.
- Kernel threads with timer preemption (PIT 1000 Hz); scheduling policy is
  **EEVDF**, written in Rust.
- `syscall`/`sysret` entry, verified by tiny ring-3 machine-code blobs
  inside the test suite; ring-3 faults kill the process, not the kernel.
- Rust (`no_std`, zero external crates): syscall table with typed argument
  validation, **Capsicum** capability mode and fd rights, **jails** with
  hierarchical memory/pid/CPU accounting, VFS with ramfs, devfs and pipes,
  **kqueue**, a narrowed **io_uring-style ring** (with a kill switch),
  **futex**, a **sysctl** MIB tree, the process table.
- `magia>` debug monitor on the serial port.
- In-kernel tests (`ktest`) registered through linker sets, plus host
  `cargo test` for all Rust policy code.

Why two languages: [docs/LANGUAGES.md](docs/LANGUAGES.md) (short version:
C for hardware, Rust where untrusted input is parsed - for memory safety,
not speed; both are about equally fast).

## Setup (one line)

On this machine (NixOS / any system with Nix), from the repository root:

```sh
nix-shell --run 'make test'
```

`shell.nix` provides gcc, binutils, make, QEMU and rustup; the
`rust-toolchain.toml` file makes rustup fetch the stable toolchain and the
`x86_64-unknown-none` target automatically on first use.

Without Nix (Debian/Ubuntu-like systems; this variant was not tested on
the development machine, which is NixOS), install the tools and the Rust
target, then run the same `make test`:

```sh
sudo apt install build-essential qemu-system-x86 curl && curl -sSf https://sh.rustup.rs | sh -s -- -y --target x86_64-unknown-none && . "$HOME/.cargo/env" && make test
```

## Use

```sh
nix-shell --run 'make run'     # boot to the magia> monitor; Ctrl-A x quits QEMU
nix-shell --run 'make test'    # cargo tests + QEMU ktest + monitor + crash tests
nix-shell --run 'make lines'   # line counts per language
```

At the monitor: `help`, `uname`, `ps`, `jails`, `ls /dev`, `cat /etc/motd`,
`sysctl` (list all), `sysctl kern.hostname`, `mem`, `threads`, `uptime`,
`poweroff`.

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - boot, memory layout, traps, threads, syscalls
- [docs/IDEAS.md](docs/IDEAS.md) - every borrowed idea and its source
- [docs/LANGUAGES.md](docs/LANGUAGES.md) - the C/Rust split and why
- [docs/SYSCALLS.md](docs/SYSCALLS.md) - the system call ABI
- [docs/TESTING.md](docs/TESTING.md) - what `make test` checks
- [docs/ROADMAP.md](docs/ROADMAP.md) - known gaps and next steps
- [docs/plan.md](docs/plan.md) - the original milestone plan

## License

BSD-2-Clause, see [LICENSE](LICENSE).
