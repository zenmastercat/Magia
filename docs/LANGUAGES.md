# C and Rust in Magia: who does what, and why

Magia is written in two languages on purpose. The rule is simple:

> **C for code that talks to hardware. Rust for code that parses or checks
> untrusted input, and for policy.**

## The split

| Area | Language | Files |
|---|---|---|
| Boot entry from Limine (stack switch, stack guard) | GNU assembly | `arch/x86_64/boot.S` |
| Limine requests and hand-off | C | `kernel/boot.c` |
| Trap/IRQ entry, `syscall` entry, context switch, user-copy fixup | GNU assembly | `arch/x86_64/{isr,syscall,switch}.S` |
| GDT/TSS/IDT, CPU features, NX/WP/SMEP/SMAP/UMIP | C | `kernel/cpu.c`, `kernel/trap.c` |
| PIC, PIT, LAPIC, I/O APIC, HPET, TSC | C | `kernel/{pic,timer,lapic,ioapic,hpet}.c` |
| Serial and framebuffer consoles, PS/2 keyboard | C | `kernel/{console,fbcon,kbd,kbd_map}.c` |
| ChaCha20 RNG | C | `kernel/rng.c` |
| ACPI table parsing (firmware input) | Rust | `rust/src/acpi.rs` |
| Buddy page allocator, page tables (W^X), kmalloc zones | C | `kernel/{pmm,vmm,kmem}.c` |
| User windows, `copyin`/`copyout` (SMAP window + fault fixup) | C | `kernel/uvm.c` |
| Threads, sleep/wakeup, preemption (mechanism) | C | `kernel/sched.c` |
| Debug monitor, ktest runner | C | `kernel/monitor.c`, `kernel/ktest/` |
| Syscall table + typed argument validation + dispatch | Rust | `rust/src/syscall.rs` |
| Capsicum capability mode and per-fd rights | Rust | `rust/src/` (see SYSCALLS.md) |
| Jails + hierarchical resource accounting | Rust | `rust/src/` |
| VFS, ramfs, devfs, pipes | Rust | `rust/src/` |
| kqueue, io_uring-style rings, futex | Rust | `rust/src/` |
| sysctl MIB tree | Rust | `rust/src/` |
| EEVDF scheduler *policy* (who runs next) | Rust | `rust/src/sched.rs` |
| Process table | Rust | `rust/src/` |

The Rust crate is a `#![no_std]` static library with **zero external
dependencies**; it uses only `core` and `alloc` (with a global allocator
backed by the C `kmalloc`). It never unwinds: a Rust panic calls
`magia_panic()`, which is a kernel panic. All crossings go through one
header, `include/magia/rust.h`, and one Rust module that declares the C
side; strings cross as `(pointer, length)` pairs and user pointers are
plain integers that only `copyin`/`copyout` may dereference.

## Why this split

**Speed is not the reason, either way.** For the same work, Rust and C
compile to code of about the same speed: both go through LLVM-class
optimizers, neither has a garbage collector or a runtime. The measured
evidence from Linux, collected by Research Desk
(`kernel-pros.md`, row 17):

- The Rust-for-Linux NVMe driver is within about **+/-3 %** of the C driver
  for random reads on one core (S101, maintainers' own plots, v6.15 rebase).
- The Rust null-block driver is mostly between **-5 % and +15 %** of C
  (S102, v6.19-rc5 rebase).
- A peer-reviewed study (USENIX ATC 2024, S103) found Rust drivers "on par"
  with C "within a 20 % gap" when they implement the same features; the
  big outliers (an e1000 driver 11x slower in ping latency) came from
  *missing features* and coarse locks, not from the language. Costs it did
  find: binaries 1.2x-2.4x larger, bounds checks in hot loops, emulated bit
  fields.

So for Magia, which runs on one CPU in QEMU, the speed difference is noise.

**Memory safety on untrusted input is the reason.** The code that reads
data an attacker controls - syscall arguments, paths, sysctl names, kevent
change lists, io_uring submission entries that user space can rewrite
while the kernel reads them - is exactly where C kernels get buffer
overflows, use-after-free and integer-overflow bugs. In safe Rust, out of
bounds indexing panics instead of corrupting memory, lifetimes rule out
use-after-free in the subsystem's own data structures, and `Option`/`Result`
make "forgot to check the error" a compile error. Policy code (rights
masks, jail limits, scheduler arithmetic) is also where logic is densest,
and it can be unit-tested on the host with `cargo test` because it does
not touch hardware.

Linux reached the same conclusion: Rust support started as an experiment
in 6.1 and in December 2025 the maintainers declared the experiment over -
"Rust is here to stay" (S97, `kernel-pros.md` row 16). The reasons given
were safety (one report: 41 % of fixes to the C null_blk driver were
memory-safety fixes, S102), and Linux's rule is that drivers use safe
abstractions rather than raw C bindings. The first CVE in Rust kernel code
(CVE-2025-68260, Binder) was a race inside an `unsafe` block - a reminder
that the benefit comes from keeping `unsafe` small, which Magia does by
confining it to the FFI, lock and user-access modules. FreeBSD, by
contrast, has no Rust in the kernel tree (S106: "We do NOT currently
support Rust in the kernel"); its concern was build cost.

**Why not all Rust?** The hardware-facing code is mostly `unsafe` by
nature (port I/O, MSRs, page-table bits, interrupt frames, context
switches). Writing it in Rust would mean one large `unsafe` block per
function, so the safety argument would not improve, while C keeps it short
and matches every datasheet and manual. Hand-written assembly is used only
where neither language can express the code (entry stubs, `swapgs`,
`sysretq`, `iretq`, the register save in the context switch).

**Why not all C?** Then the parsers and policy - the part that faces
untrusted input - would lose memory safety, and the policy could not be
tested on the host without a large mocking layer.

## Rules that keep the boundary safe

1. User pointers are integers in Rust (`u64`), never references. The only
   way to read or write them is `magia_copyin`/`magia_copyout` (C), which
   range-check against `USER_TOP`, check the page tables for the `U` bit,
   open the SMAP window and recover from faults.
2. Every user structure is copied into kernel memory once, then validated
   (no double fetch). Shared rings are read field by field into locals.
3. Locks: Rust state lives behind IRQ-disabling locks; no Rust lock is held
   across a sleep. C never calls back into Rust while holding a C spinlock
   except the documented accounting hook (`rs_jail_mem_charge`), which takes
   only its own lock.
4. ABI structs are `#[repr(C)]` mirrors of `include/magia/abi.h`; at boot,
   C passes `sizeof` of each and Rust panics on a mismatch.
5. No external crates: everything in the kernel is reviewable in-tree.

Sources: Research Desk, *Magia kernel design: what FreeBSD and Linux each
do best* ([`docs/research/kernel-pros.md`](research/kernel-pros.md),
2026-11-20), rows 16-17; source IDs S97, S101, S102, S103, S106 as listed
there.
