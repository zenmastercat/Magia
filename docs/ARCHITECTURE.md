# Magia architecture

Magia is an experimental x86_64 kernel: uniprocessor, one address space,
kernel only (no userland). C and assembly drive the hardware; a `no_std`
Rust crate implements policy and everything that parses untrusted input
(see LANGUAGES.md). This document follows a boot and then describes each
subsystem.

```
            +-------------------- Rust: rust/src (libmagia_rs.a) -------------------+
 syscalls   | syscall table + validation -> Capsicum -> VFS(ramfs,devfs,pipe)      |
 ---------> | jails + accounting, process table, kqueue, rings, futex, sysctl MIB  |
 (ring 3 or | EEVDF policy (rs_sched_*)                                            |
  ksyscall) +----------^---------------------------------------^--------------------+
                       | magia_* (rust.h)                      | rs_* (rust.h)
 +---------------------+---------------------------------------+-------------------+
 | C: kernel/  sched.c (threads, sleep/wake, preempt)  uvm.c (user windows, copyin) |
 |   pmm.c (buddy)  vmm.c (W^X paging)  kmem.c (zones)  trap.c  cpu.c  timer.c      |
 |   console.c  monitor.c (magia>)  ktest/ (runner + tests)  rustglue.c             |
 +----------------------------------------------------------------------------------+
 | asm: arch/x86_64/ boot.S isr.S syscall.S switch.S user_blob.S                     |
 +----------------------------------------------------------------------------------+
```

## Address space

| Range | Use |
|---|---|
| `0x0000_0000_0000_0000` - `0x0000_0000_ffff_ffff` | unmapped (NULL and low addresses fault) |
| `0x0000_0001_0000_0000` (`USER_WIN_BASE`) - `0x0000_7fff_ffff_ffff` | user windows (U bit), one guard page after each |
| `0xffff_8000_0000_0000` (`PHYS_MAP_BASE`) | direct map of RAM, RW+NX, 2 MiB pages; the kernel image's frames are mapped read-only here |
| `0xffff_ffff_8000_0000` (`KERNEL_VMA`) | kernel image: `.text` RX, `.rodata` R+NX, `.data/.bss` RW+NX, 4 KiB pages, global |

All processes share one page table (per-process tables are on the roadmap).

## Boot

1. QEMU's multiboot loader (`-kernel build/magia32.elf`, an `objcopy` of
   the ELF64 to ELF32 because QEMU only loads 32-bit multiboot ELFs) enters
   `_start` in `arch/x86_64/boot.S` in 32-bit protected mode.
2. `boot.S` checks for long mode, builds throw-away page tables mapping the
   first GiB three times (identity, direct map, kernel window), enables PAE,
   `EFER.LME` (+`NXE`), paging and `CR0.WP`, far-jumps to 64-bit code, then
   to the higher half. It seeds `__stack_chk_guard` from RDRAND and the TSC
   and calls `kmain(magic, mbi)`.
3. `kmain` (`kernel/main.c`) starts the console, copies the command line,
   then runs every **SYSINIT** hook in link order (FreeBSD linker-set idea):

| key | hook | file |
|---|---|---|
| 10000 | `cpu`: GDT+TSS (IST stacks for #DF and NMI), IDT, CPU features, WP/NX/SMEP/SMAP/UMIP | `cpu.c`, `trap.c` |
| 20000 | `pmm`: buddy allocator from the multiboot memory map | `pmm.c` |
| 20001 | `vmm`: W^X kernel page tables, switch CR3 | `vmm.c` |
| 30000 | `kmem`: kmalloc zones | `kmem.c` |
| 40000 | `intr`: PIC remap, TSC calibration, PIT at 1000 Hz | `timer.c`, `pic.c` |
| 50000 | `sched`: boot thread = tid 1, idle thread = tid 0 | `sched.c` |
| 50001 | `interrupts_on` | `main.c` |
| 60000 | `rust`: `rs_init()` - jail 0, process 0, VFS, sysctl tree | `rustglue.c` |
| 70000 | `syscall`: `EFER.SCE`, `STAR`, `LSTAR`, `SFMASK` | `syscall.c` |

4. With `ktest` on the command line it runs the in-kernel tests and exits
   QEMU through `isa-debug-exit`; otherwise the boot thread becomes the
   `magia>` serial monitor.

## CPU, traps, interrupts

- GDT: kernel code/data, a 32-bit user code placeholder (required by the
  `sysret` layout), user data, user code (64-bit), TSS. The TSS has no I/O
  bitmap, so ring-3 port I/O faults.
- `isr.S` stubs push a uniform `struct trapframe` and call
  `trap_dispatch()`; `swapgs` happens only when the trap came from ring 3,
  so `%gs` always points at `struct percpu` in the kernel.
- Kernel exceptions panic with registers and a symbolized backtrace (the
  second link pass embeds a sorted symbol table in `.ksyms`), except:
  `#BP` returns (counted), a `#PF` inside `uaccess_copy` returns `-EFAULT`,
  and a fault while the thread armed `fault_jmp` resumes at `ksetjmp`
  (used by ktest to prove W^X/NX/SMEP/SMAP).
- Ring-3 exceptions kill the process with exit status 128 + vector, through
  the normal Rust exit path.
- 8259 PIC at vectors 32-47, PIT channel 0 at 1000 Hz; the TSC is
  calibrated against PIT channel 2 and gives `now_ns()`.

## Memory

- **pmm.c** - binary buddy allocator, orders 0-10 (4 KiB - 4 MiB), one
  `struct page` per frame placed right after the kernel image. Free blocks
  are doubly linked; freeing merges with the buddy (`pfn ^ (1 << order)`).
- **vmm.c** - builds the final page tables: per-section kernel permissions,
  direct map with 2 MiB pages, refuses to create a W+X mapping
  (`vmm_map` panics). ktest 115 walks every live entry and asserts no leaf
  is both writable and executable.
- **kmem.c** - power-of-two zones of 16..2048 bytes, one page per slab,
  free list and in-use count kept in `struct page`; bigger requests take
  whole buddy blocks. One empty slab is cached per zone; `kmem_reclaim()`
  returns the rest. Double frees are detected (poison word + free-list
  scan) and panic.
- **uvm.c** - user windows: `uvm_alloc(jid, npages, flags)` charges the jail
  first (`rs_jail_mem_charge`, hierarchical limits), then maps zeroed pages
  `U|W|NX` (data) or `U|R|X` (code, `UVM_EXEC`). `UVM_CONTIG` returns a
  kernel alias for shared rings. `copyin`/`copyout` check the range against
  `USER_TOP`, check every page for `U` (and `W`), open the SMAP window
  (`stac`), copy with `uaccess_copy` (fault fixup), close it (`clac`).

## Threads and scheduling

- `struct thread` (C) owns the kernel stack (16 KiB, canary at the bottom,
  checked on every switch), saved `rsp`, sleep state. tid = slot index;
  tid 0 is idle, tid 1 is the boot thread.
- `switch_context` saves callee-saved registers and swaps stacks; a new
  thread starts in `thread_start_asm` -> `thread_entry_c`.
- **Mechanism in C, policy in Rust**: `schedule()` asks
  `rs_sched_pick(prev, prev_runnable, now)` for the next tid. The timer IRQ
  calls `rs_sched_tick(now)`; when it reports an exhausted slice,
  `need_resched` is set and the IRQ exit path calls `schedule()`, so
  kernel code is preempted whenever interrupts are enabled.
- Sleep/wakeup without lost wakeups: `sleep_prepare(timeout)`; publish
  yourself on a wait list; drop locks; `sleep_commit()`. A `thread_wake()`
  between prepare and commit cancels the sleep. Timeouts are checked by
  the timer tick.
- Processes: `kproc_spawn()` creates a thread and a Rust process-table
  entry (`rs_proc_spawn`). A process exits through `SYS_exit` (Rust: close
  fds, free mappings, notify the parent and kqueues), then `thread_exit()`.

## System calls

- Ring 3: `syscall` -> `syscall_entry` (`swapgs`, kernel stack from
  `%gs:8`, build a trapframe, enable interrupts) -> `syscall_c()` ->
  `rs_syscall(pid, nr, args)` -> `sysretq`. `SFMASK` clears IF, TF, DF and
  AC on entry, so SMAP is enforced inside the kernel.
- Kernel threads acting as processes call `ksyscall()`, which enters the
  same `rs_syscall()`.
- The Rust dispatcher validates arguments from the syscall table, checks
  capability mode, runs the handler, and loops over sleeps for blocking
  calls. See SYSCALLS.md for every call.

## Rust subsystems

See `rust/src/` and SYSCALLS.md. State lives in plain structs owned by one
global behind an IRQ-disabling Giant lock (the scheduler and the jail
memory accounting have their own locks because C calls them from the timer
IRQ and from inside `uvm_alloc`). Kernel services are reached through a
small `kenv` layer that is mocked in host tests, so every policy module runs
under `cargo test` on Linux.

## Debug monitor

`kernel/monitor.c`: a line editor on COM1 (IRQ 4 fills a ring buffer, the
monitor thread sleeps until input). Commands: `help uname cpu ps threads
jails mem uptime ls cat sysctl echo bt panic reboot poweroff`.

## Build

`make` compiles C with `-ffreestanding -mcmodel=kernel -mno-red-zone
-mgeneral-regs-only -fstack-protector-strong -ftrivial-auto-var-init=zero`,
builds the Rust staticlib for `x86_64-unknown-none` (static relocation
model, kernel code model, frame pointers), links twice (the second pass
embeds the symbol table; `scripts/check-text-stable.sh` verifies no code
moved), and converts the result to ELF32 for QEMU.
