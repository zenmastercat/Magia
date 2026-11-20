# Magia implementation plan

Short working plan, written before the code. It records the order of work and
the test that proves each step. The final docs (ARCHITECTURE, IDEAS, ...)
describe the result; this file describes how we got there.

## Ground rules

- Everything written from scratch. No code copied from Linux or FreeBSD.
  We take *ideas* and re-derive them (numeric tables such as the nice-to-weight
  table are recomputed from their formula).
- Test first where it is cheap: Rust policy modules get host `cargo test`
  cases before or together with the implementation; every kernel subsystem
  gets an in-kernel `ktest` case registered through a linker set.
- "Done" means `nix-shell --run 'make test'` passes from a fresh `git clone`.

## Milestones

1. **Skeleton**: repo, license, `shell.nix`, `flake.nix`, Makefile, linker
   script. Test: `make` produces `magia.elf` and `magia32.elf`.
2. **Boot**: multiboot1 header, 32-bit stub, PAE + long mode, higher-half
   jump, COM1 serial, VGA, `kprintf`, panic. Test: QEMU prints banner.
3. **CPU**: GDT/TSS (IST for #DF), IDT + exception stubs, PIC remap, PIT at
   1000 Hz, TSC calibration, NX/WP/SMEP/SMAP. Test: ktest triggers and
   recovers a breakpoint exception, tick counter advances.
4. **Memory**: multiboot memory map, buddy page allocator with a `struct page`
   array, new 4-level page tables with W^X (text RX, rodata R+NX,
   data RW+NX, direct map RW+NX), slab-style zones for `kmalloc`.
   Test: ktest stress loop, free page count identical before/after.
5. **Threads**: kernel threads, context switch, sleep/wakeup with timeouts,
   preemption from the timer IRQ, stack canaries at stack bottoms.
6. **Rust crate** (`#![no_std]` staticlib, zero deps, `alloc` only):
   ffi + kenv (mockable kernel environment), KLock, EEVDF scheduler, process
   table, Capsicum rights, jails + resource accounting, VFS + ramfs +
   devfs + pipes, kqueue, io_uring-style rings, futex, sysctl MIB tree,
   syscall table with typed argument validation.
   Test: `cargo test` on the host for every policy rule.
7. **Syscall ABI**: `syscall`/`sysret` MSRs and entry stub, verified by a
   tiny ring-3 machine-code blob inside ktest (not a userland program).
8. **ktest**: kernel threads acting as processes call the dispatcher, with
   per-process user memory windows mapped `U` so pointer validation and
   SMAP-aware copyin/copyout are real. Positive and negative tests.
9. **Monitor**: `magia>` kernel debug monitor on serial.
10. **Harness**: `make test` = `cargo test` + headless QEMU with
    `isa-debug-exit`, 60 s timeout, parse the ktest summary.
11. **Docs + graph**: README, ARCHITECTURE, IDEAS, LANGUAGES, SYSCALLS,
    TESTING; graphify code graph (output git-ignored).

## Key design decisions (details in ARCHITECTURE.md)

- Higher-half kernel at `0xffffffff80000000`, direct map of RAM at
  `0xffff800000000000`, user windows below `0x0000800000000000`.
- Single address space (no per-process page tables yet; no ELF loader).
- Uniprocessor; one "Giant" IRQ-disabling lock around Rust subsystem state
  plus a separate scheduler lock.
- Linux-style negative errno returns; FreeBSD errno numbers for
  `ENOTCAPABLE`/`ECAPMODE`.
- Linker sets (FreeBSD `SET_DECLARE`/`SYSINIT` idea) for boot-time init hooks
  and ktest case registration.
