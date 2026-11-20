# Roadmap and known gaps

Magia 0.1 is deliberately small. This file lists, honestly, what is *not*
built. Items marked (RD) come from Research Desk's comparison
([`docs/research/kernel-pros.md`](research/kernel-pros.md), row in
brackets).

## Known gaps in 0.1

- **No userland, no ELF loader, no `fork`/`exec`.** Processes are kernel
  threads registered in the Rust process table; ring 3 is exercised only by
  tiny hand-written blobs inside ktest (`arch/x86_64/user_blob.S`).
- **Single address space.** User windows of all processes share one page
  table; a process can address another's window (the kernel's own
  `copyin`/`copyout` checks are per page, not per process). Per-process
  PML4s (copying the kernel half, entries 256-511, which `vmm_init`
  already pre-creates) are the next memory milestone.
- **Uniprocessor.** One CPU; "interrupts off" is the scheduler lock; Rust
  state sits behind a Giant lock. SMP needs per-CPU run queues, finer
  locks, IPIs and TLB shootdown. Research Desk notes Rust's measured
  overheads come largely from coarse locks (row 17), so this matters before
  SMP.
- **Legacy timers and interrupt controller.** PIT + 8259 PIC; no
  LAPIC/IOAPIC/HPET, no tickless idle.
- **At most 1 GiB of RAM managed** (the boot direct map covers 1 GiB).
- **No kernel stack guard pages.** Kernel stacks come from the direct map;
  overflow is detected by a canary at the bottom (checked on every context
  switch), not prevented. Guard pages need a separate kernel-stack VA area.
- **User code windows are written through the direct map.** The physical
  page of a user code window has a writable kernel alias while it is
  executable in ring 3 (no single mapping is W+X). A loader should map code
  read-only after copying.
- **No storage, no network, no real drivers** beyond serial, VGA text, PIT
  and PIC.
- **Jails cannot be removed**, and jail charges for an existing mapping stay
  with the jail that was charged when it was created.
- **No signals / kill**, no process groups, no credentials (uid/gid).

## Next

1. Per-process page tables; then an ELF loader and a tiny init in ring 3.
2. Process descriptors (`pdfork`, Capsicum) (RD row 1).
3. LAPIC timer, IOAPIC, then SMP with per-CPU run queues; EEVDF per CPU
   with load balancing (RD row 5).
4. Kernel stack guard pages; KASLR once the kernel is relocatable (RD row
   15); hardened usercopy object-size checks.
5. kCFI + IBT: needs Clang for C (`-fsanitize=kcfi`, GCC 15 lacks it) and
   Rust support for `-Z cf-protection` on stable (RD row 15, KSPP S88).
6. Ring completions posted to kqueue (`EVFILT_AIO`-like) (RD row 4).
7. A scheduler-class interface so a second policy can be chosen at boot
   (FreeBSD 15 4BSD/ULE choice), later loadable with a safety fallback
   (Linux sched_ext idea) (RD row 5).
8. sysctl ABI stability levels per node (stable/testing/obsolete, Linux
   ABI docs idea) and boot-time tunables (RD row 6).
9. Kernel modules: signed, with a build fingerprint (Linux `vermagic` +
   signature, from the REA binary comparison); metadata in a linker set
   (FreeBSD `set_modmetadata_set`).
10. virtio drivers first (RD row 14); storage on a blk-mq-style queue core
    with GEOM-style providers/consumers for partitions, encryption,
    mirroring (RD row 7); checksummed blocks in any disk filesystem (RD
    row 8).
11. VNET-style per-jail network stacks and one packet filter on a small
    verified bytecode VM shared with tracing (eBPF/nftables ideas) (RD rows
    10-11); DTrace-style static probes registered through linker sets.
12. Dentry cache with RCU-style lookup once SMP exists (RD row 8); per-CPU
    caches in kmalloc zones (RD row 9).
