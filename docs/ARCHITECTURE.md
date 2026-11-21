# Magia architecture

Magia is an experimental x86_64 kernel: uniprocessor, one address space,
kernel only (no userland yet). C and assembly drive the hardware; a
`no_std` Rust crate implements policy and everything that parses untrusted
input, including firmware tables (see LANGUAGES.md). This document follows
a boot and then describes each subsystem.

```
            +-------------------- Rust: rust/src (libmagia_rs.a) -------------------+
 syscalls   | syscall table + validation -> Capsicum -> VFS(ramfs,devfs,pipe)      |
 ---------> | jails + accounting, process table, kqueue, rings, futex, sysctl MIB  |
 (ring 3 or | EEVDF policy (rs_sched_*)  ACPI (acpi.rs, aml.rs)  USB (usb.rs, usbhub.rs) |
  ksyscall) +----------^---------------------------------------^--------------------+
                       | magia_* (rust.h, acpi.h)              | rs_* (rust.h)
 +---------------------+---------------------------------------+-------------------+
 | C: kernel/  boot.c (Limine)  sched.c (threads)  uvm.c (user windows, copyin)     |
 |   pmm.c (buddy)  vmm.c (W^X paging, immutable image)  kmem.c (zones)  rng.c      |
 |   trap.c  cpu.c  timer.c  lapic.c  ioapic.c  hpet.c  pic.c  acpi.c              |
 |   console.c  klog.c  fbcon.c (Spleen)  kbd.c  pci.c  xhci.c  power.c  hwinfo.c    |
 |   bench.c  bench_stats.c  monitor.c  ktest/                                      |
 +----------------------------------------------------------------------------------+
 | asm: arch/x86_64/ boot.S isr.S syscall.S switch.S user_blob.S                     |
 +----------------------------------------------------------------------------------+
   Limine (UEFI or BIOS) loads build/magia.img:/boot/magia.elf, a static PIE
```

## Address space

| Range | Use |
|---|---|
| `0x0000_0000_0000_0000` - `0x0000_0000_ffff_ffff` | unmapped (NULL and low addresses fault) |
| `0x0000_0001_0000_0000` (`USER_WIN_BASE`) - `0x0000_7fff_ffff_ffff` | user windows (U bit), one guard page after each |
| `dmap_base` (Limine's HHDM offset: `0xffff_8000_0000_0000` + a random multiple of 1 GiB, up to 16 TiB) | direct map of physical memory: every RAM, ACPI and firmware-data range of the memory map, RW+NX write-back (2 MiB pages where aligned); the kernel image's frames read-only; the framebuffer write-combining; MMIO uncached on demand (`vmm_map_mmio`) |
| `kernel_virt_base` = `KERNEL_VMA` (`0xffff_ffff_8000_0000`) + KASLR slide (random multiple of 4 KiB) | kernel image: `.text` RX, `.rodata` R+NX, `.data/.bss` RW+NX, 4 KiB pages, global; **immutable** after boot (PML4 slot 511) |

All processes share one page table (per-process tables are stage 3).
`phys_to_virt(pa)` is `pa + dmap_base`; `virt_to_phys()` subtracts
`dmap_base`, or for image addresses `kernel_virt_base - kernel_phys_base`.

## Boot

1. **Limine** (UEFI `EFI/BOOT/BOOTX64.EFI` or BIOS stages) reads its
   config - from `/boot/limine/limine.conf` on the ESP, or, in the tests,
   from an SMBIOS OEM string `limine:config:...` that carries the kernel
   command line - and loads `/boot/magia.elf`, a static PIE linked at
   `KERNEL_VMA`. With `kaslr: yes` it applies the `R_X86_64_RELATIVE`
   relocations at a random slide, randomises the HHDM base, and enters
   `_start` (`arch/x86_64/boot.S`) in long mode on its own page tables,
   interrupts off. Base revision 6 is requested; the kernel stops with a
   message on COM1 if the loader does not support it.
2. `_start` switches to the kernel's boot stack, seeds the stack guard
   from RDRAND^TSC, then calls `boot_early()` (`kernel/boot.c`), which
   copies every Limine response the kernel needs (memory map, HHDM offset,
   image physical/virtual base, command line, framebuffer, RSDP, firmware
   type, entropy) into `bootinfo`, and `rng_early_init()`, which seeds the
   ChaCha20 RNG and replaces the stack guard and the thread-stack canary
   with RNG output (no stack-protected frame is live yet).
3. `kmain` (`kernel/main.c`) starts the consoles (COM1 and the
   framebuffer), then runs every **SYSINIT** hook in link order (FreeBSD
   linker-set idea):

| key | hook | file |
|---|---|---|
| 10000 | `cpu`: GDT+TSS (IST stacks for #DF and NMI), IDT, CPU features, WP/NX/SMEP/SMAP/UMIP, PGE | `cpu.c`, `trap.c` |
| 10001 | `rng_report`: print the RNG's entropy sources | `rng.c` |
| 20000 | `pmm`: buddy allocator over the usable RAM of the Limine memory map | `pmm.c` |
| 20001 | `vmm`: W^X kernel page tables, direct map of all RAM, switch CR3 | `vmm.c` |
| 20002 | `pmm_reclaim`: free bootloader-reclaimable memory | `pmm.c` |
| 30000 | `kmem`: kmalloc zones | `kmem.c` |
| 30001 | `acpi`: Rust parses RSDP -> XSDT -> MADT, HPET, MCFG, FADT | `acpi.c`, `rust/src/acpi.rs` |
| 40000 | `intr`: 8259 remap+mask, LAPIC, I/O APIC, HPET, TSC and LAPIC timer calibration, tick (or 8259 + PIT with `noapic`) | `timer.c`, `lapic.c`, `ioapic.c`, `hpet.c`, `pic.c` |
| 40001 | `kbd`: PS/2 keyboard (probed even if the FADT says no 8042; bounded) | `kbd.c` |
| 40002 | `console_input`: COM1 receive interrupt | `console.c` |
| 40003 | `pci`: ECAM enumeration (bus 0 and bridges), BAR sizing | `pci.c` |
| 50000 | `sched`: boot thread = tid 1, idle thread = tid 0 | `sched.c` |
| 50001 | `interrupts_on` | `main.c` |
| 50002 | `xhci`: every USB controller (MSI-X), the `usb` thread's first scan (hubs, keyboards, mice) | `xhci.c`, `rust/src/usb.rs`, `rust/src/usbhub.rs` |
| 60000 | `rust`: `rs_init()` - jail 0, process 0, VFS, sysctl tree | `rustglue.c` |
| 70000 | `syscall`: `EFER.SCE`, `STAR`, `LSTAR`, `SFMASK` | `syscall.c` |
| 90000 | `vmm_lock`: lock the kernel image (immutable from here on) | `vmm.c` |

4. With `ktest` on the command line it runs the in-kernel tests and exits
   QEMU through `isa-debug-exit`; otherwise the boot thread becomes the
   `magia>` monitor (serial and screen/keyboard).

## Disk image

`make image` builds `build/magia.img` (64 MiB) from plain files, without
root or loop devices: `sgdisk` writes a GPT with a 62 MiB EFI system
partition at 1 MiB and a small BIOS boot partition before it; `mformat`/
`mcopy` (mtools) build the FAT32 ESP (`/EFI/BOOT/BOOTX64.EFI`,
`/boot/limine/{limine-bios.sys,limine.conf,LICENSE,3RDPARTY.md}`,
`/boot/magia.elf`); `limine bios-install` adds the BIOS boot code. Limine
12.10.0's release binaries are downloaded once and checked against a
pinned SHA-256.

## CPU, traps, interrupts

- GDT: kernel code/data, a 32-bit user code placeholder (required by the
  `sysret` layout), user data, user code (64-bit), TSS. The TSS has no I/O
  bitmap, so ring-3 port I/O faults. Limine's GDT is replaced before its
  memory is reclaimed.
- `isr.S` stubs push a uniform `struct trapframe` and call
  `trap_dispatch()`; `swapgs` happens only when the trap came from ring 3,
  so `%gs` always points at `struct percpu` in the kernel.
- Kernel exceptions panic with a symbolized backtrace (the second link
  pass embeds a sorted symbol table in `.ksyms`; it holds link-time
  addresses and lookups subtract the KASLR slide). Kernel addresses are
  printed at their link-time value and the register dump only appears
  with `verbose`, so a console log does not reveal the slide. Exceptions: `#BP`
  returns (counted), a `#PF` inside `uaccess_copy` returns `-EFAULT`, and
  a fault while the thread armed `fault_jmp` resumes at `ksetjmp` (used by
  ktest to prove W^X/NX/SMEP/SMAP and the immutable page tables).
- Ring-3 exceptions kill the process with exit status 128 + vector.
- Vectors: ISA IRQ n is vector 32 + n in both modes; 48 is the LAPIC
  timer, 254 the LAPIC error interrupt, 255 spurious.
- **APIC mode** (default): the 8259s are remapped and fully masked. The
  LAPIC runs in x2APIC mode (MSRs) when the CPU has it, else xAPIC (MMIO,
  mapped uncached); LINT0/1 follow the MADT NMI entries. I/O APICs from the
  MADT start with every pin masked; `irq_enable(irq)` routes ISA IRQs
  through the MADT overrides (polarity, trigger) to the BSP. The 1000 Hz
  tick comes from the **TSC-deadline** timer when the CPU has it (KVM on
  the target CPU), else the LAPIC timer in periodic mode, calibrated
  against the TSC. The TSC frequency comes from **CPUID 0x15** (Intel,
  invariant TSC; the SDM's crystal table when the CPU reports 0;
  cross-checked with leaf 0x16), else the **HPET** main counter, else PIT
  channel 2 - every PIT wait bounded by the TSC, because laptop firmware
  may gate the 8254 - else CPUID 0x16, else a logged guess;
  `kern.tsc_source` says which. Register names:
  `include/magia/apicreg.h`, from FreeBSD.
- **Legacy mode** (`noapic`, or no usable MADT): the 8259 for ISA
  interrupts, LAPIC LINT0 reopened as a virtual wire (Limine masks every
  LVT entry), and still the LAPIC timer as the tick; PIT channel 0 ticks
  only without a LAPIC or with `pittick`.

## Firmware tables (ACPI)

`kernel/acpi.c` passes the RSDP's physical address to
`rs_acpi_parse()` (`rust/src/acpi.rs`). The parser reads memory only
through `magia_acpi_map()`, which refuses any range that is not ACPI or
firmware data in the memory map. It validates checksums, lengths
(36 bytes .. 1 MiB per table), entry sizes and counts, rejects tables
that overlap the RSDP or root table, skips corrupt optional tables, and
fills `struct acpi_info` (fixed arrays: 256 CPUs, 16 I/O APICs, 32
overrides, 8 ECAM windows, 64 tables). MADT semantics follow FreeBSD
`madt.c`. No ACPICA and no AML interpreter: from the FADT it also takes
the reset register, the PM1a/PM1b control blocks (X_ GAS first), SMI_CMD
and ACPI_ENABLE and the sleep control register, and `rust/src/aml.rs`
finds `Name(\_S5, Package(){...})` in the DSDT (then the SSDTs) and
reads the first two integers - the sleep types for poweroff - with every
offset bounds-checked. It walks the definition block object by object
(Name with its data, Scope, Device, Processor, PowerResource and
ThermalZone bodies; Method, If/Else/While, Buffer, Package, Field and
friends skipped whole by their PkgLength; OperationRegion with integer
arguments; External, Alias, Mutex, Event), so `_S5_` bytes inside a
buffer, string or method body do not match. A `\_S5` inside an `If` is
not seen by the walk. If the walk meets AML it does not know, it falls
back to a byte scan of the whole table, and `hwinfo` / the boot log say
"(byte scan: AML walk stopped at DSDT+offset: bytes)": that result can
in principle come from another object's data (firmware is trusted), and
the bytes tell which AML to teach the walk next.

`kernel/power.c` uses them: `reboot` writes the reset register (I/O or
memory), then falls back to the 8042 reset pulse and a triple fault;
`poweroff` switches legacy-mode firmware to ACPI mode through SMI_CMD
(bounded wait for SCI_EN), then writes SLP_TYPa/b and SLP_EN into PM1a
(and PM1b) control, or the sleep control register on hardware-reduced
ACPI, each step logged. Every register is range-checked before it is
touched (`power_reg_ok`): I/O ports 0x100..0xffff (SMI_CMD may also be
the standard APM port 0xb2), or device memory that is not RAM, the
kernel or the framebuffer, mapped across every page the access touches;
PCI configuration space is not written. A refused reset register falls
back to the 8042.

## PCI and USB (stage 2)

- **pci.c**: configuration space through ECAM (base and bus range from
  MCFG, segment 0); each bus's 1 MiB window is mapped uncached when first
  scanned, after checking that it lies inside the MCFG range (aligned, no
  wrap, below 2^52) and, through `vmm_map_mmio()`, that it is not RAM or
  the framebuffer (the same check covers BARs and MSI-X tables); bus 0 is scanned and bridges are followed to their secondary
  buses (each bus once, depth bounded). Config accesses are bounded to the
  function's 4 KiB. BARs are sized with decoding (and interrupts) off.
  Capability lists are walked with the spec's limits (dword-aligned
  pointers in 0x40..0xff, no revisits, at most 48). `pci_enable_msi()`
  programs MSI-X (table checked to lie inside its BAR; all entries masked,
  entry 0 to the BSP) or MSI, disables INTx; vectors 56-63 are handed out
  and dispatched through the LAPIC like the timer. Legacy INTx is not
  supported: its routing lives in ACPI `_PRT` methods (AML).
- **No DMA remapping (no VT-d / IOMMU).** A bus-master device can read
  and write any physical memory, so W^X, NX and the immutable kernel
  image hold against the CPU only, not against device DMA (a faulty
  xHCI, a hostile PCIe or Thunderbolt device). As cheap hardening the
  scan turns bus mastering off on every endpoint function
  (`pci_bus_master_policy`); only `xhci_start()` and `pci_enable_msi()`
  turn it back on for the device Magia drives. Bridges keep it (it only
  forwards their children's DMA, and a driven child may sit behind one),
  and so does the one display function whose BAR holds the boot
  framebuffer (an integrated GPU may scan it out of RAM); other display
  controllers lose it. The bus-master bit is a host hint, not a security
  boundary: a hostile device can ignore it and do DMA anyway, and devices
  behind bridges are only as fenced as their own bit. The real fix is
  VT-d / AMD-Vi DMA remapping, planned for stage 4.
- **xhci.c**: every xHCI controller (PCI class 0c/03/30, up to 4), mostly
  after OpenBSD's driver: BIOS hand-off, Intel port routing, reset,
  device-context array and scratchpad buffers, a command ring and one
  event ring per controller (one 4 KiB page each, single segment, link
  TRB), each on its own MSI-X (or MSI) vector; the interrupt handler finds
  its controller by the vector. One kernel thread, `usb`, does all
  enumeration - the first scan at boot (which `xhci_sysinit` waits for),
  root-port changes from port-change events, hub port changes from the
  hubs' status-change endpoints, recovery of halted endpoints, ktest
  requests - so commands and control transfers never overlap. The
  interrupt handler only consumes events (at most 32 per interrupt),
  completes the transfer the thread waits for, hands keyboard and mouse
  reports to Rust, re-queues interrupt transfers and wakes the thread;
  with interrupts off as the lock, the thread consumes events the same
  way while it waits, so it also works polled. A device: Enable Slot,
  Address Device (slot context with route string, speed, root port, TT
  hub slot and port for a full/low-speed device below a high-speed hub,
  MTT), the device descriptor (first 8 bytes, EP0 max-packet fix-up
  through Evaluate Context, then all 18), the whole configuration (up to
  4 KiB, parsed in Rust): a hub, or a boot keyboard and/or boot mouse
  wherever their interfaces are (alternate setting 0); SET_CONFIGURATION,
  SET_PROTOCOL(boot), one Configure Endpoint for its interrupt-IN
  endpoints, SET_IDLE(0). Hubs follow OpenBSD's uhub: SET_HUB_DEPTH
  (SuperSpeed), the hub descriptor (parsed in Rust), SET_INTERFACE to a
  multi-TT alternate setting, the slot context's Hub/NPORTS/TT think
  time/MTT with the status endpoint, power every port, wait, then each
  port: GET_STATUS (parsed in Rust), clear the change bits, detach on a
  connect change, reset a connected port and attach at the speed it
  reports. Connect attempts are rate-limited per port (5 per 10 s, a
  failed one counting twice; then the port waits for the next window and
  is looked at again), so a flapping port or hub cannot keep the thread
  busy. Detach is Disable Slot and the device's pages freed, recursively
  below hubs; if Disable Slot fails the pages are kept (the controller may
  still use them) and counted, and after 3 such failures the controller
  attaches nothing more until reboot (no controller reset to reclaim
  them). A transfer error marks the
  endpoint; the thread resets it (Reset Endpoint if halted,
  CLEAR_FEATURE(ENDPOINT_HALT) after a stall, Set TR Dequeue Pointer to
  the enqueue position) and queues again; after 3 errors in a row it is
  given up. EP0 errors (a stalled request) are recovered the same way at
  once. Event TRBs are checked (type, slot, endpoint, completion code,
  residual length) and only our own ring addresses are compared, never
  dereferenced; events for gone slots count as stale. Every keyboard has
  its own Rust report state (8 at most).
- **Trusted keyboards**: every device that claims a HID boot keyboard
  interface (class 3, subclass 1, protocol 1) becomes a console keyboard;
  its keystrokes go straight into the console input ring. Nothing
  authenticates it, so with physical access a BadUSB-style device can
  type at `magia>` (as with any USB keyboard). Each one is logged at
  attach (`usb: cN pPATH SPEED VID:PID ...: keyboard`) and listed by `usb`
  and `hwinfo`.
- **usb.rs, usbhub.rs**: device/configuration descriptors, boot keyboard
  and mouse interfaces and reports, hub interfaces, hub descriptors (USB
  2 and SuperSpeed) and port status from devices, parsed in Rust (see
  TESTING.md for the malformed-input tests).

## Consoles and input

- `console.c`: COM1 (16550) is the primary console; every write also
  goes into the kernel log (`klog.c`: a 64 KiB ring of bytes, kept from
  the first boot message; `dmesg [n]` prints it without logging it again)
  and to `fbcon.c`, a text console on the Limine framebuffer (32 bpp,
  write-combining) with the Spleen 8x16 font from OpenBSD. The console
  keeps a ring of 1024 lines (the screen plus the scroll-back), each with
  an attribute (normal, or the panic header's white on red), and a copy
  of what is on screen, and redraws only changed cells, never reading the
  framebuffer. Page Up/Down move the view through the scroll-back; new
  output returns to the live screen.
- `panic()` prints the message and the backtrace, then draws the panic
  screen: a fresh page with a red header (the message, and "photograph
  this screen") and the newest kernel log lines below it, one per row -
  the only report a machine without a serial port can give.
- `hwinfo.c`: CPU, firmware, memory, ACPI facts, every PCI function and
  every USB controller and device, packed into as many 28-character
  columns as the screen is wide, on a fresh page.
- Input: COM1's receive interrupt, the PS/2 keyboard (`kbd.c`, i8042 in
  translated scancode set 1, `kbd_map.c` US layout; probed even when the
  FADT's 8042 flag is clear, every controller wait bounded to 20 ms by
  the TSC) and the USB boot keyboards (`xhci.c` + `usb.rs`) push bytes
  into one ring that `console_getc_wait()` sleeps on. Page Up/Down come
  in from every source as the VT220 sequences ESC [5~ / ESC [6~, which the
  monitor decodes.

## Kernel RNG

`rng.c`: ChaCha20 (D. J. Bernstein's public-domain code as carried by
OpenBSD) driven like OpenBSD's arc4random: a 1 KiB keystream buffer,
re-key from the first 40 bytes of every refill (fast key erasure),
handed-out bytes wiped. Reseeds at boot, after 1.6 MB of output and on
`rng_reseed()`, each source mixed in by its own re-key: Limine's boot
entropy (once), RDSEED, RDRAND, TSC jitter. API: `arc4random_buf`,
`arc4random`, `arc4random64`, `arc4random_uniform`.

## Memory

- **pmm.c** - binary buddy allocator, orders 0-10 (4 KiB - 4 MiB), one
  `struct page` (40 bytes) per frame up to the highest RAM address, placed
  in the first usable range that fits (320 MiB for 32 GiB). Usable RAM
  above 1 MiB is free at boot; bootloader-reclaimable memory joins once
  Magia's page tables and GDT are live. Free blocks are doubly linked;
  freeing merges with the buddy (`pfn ^ (1 << order)`).
- **vmm.c** - builds the final page tables: per-section kernel
  permissions; the direct map from the memory map (RAM, ACPI, firmware
  data write-back; framebuffer write-combining through PAT entry 5, which
  Limine programs; reserved ranges and holes unmapped); refuses to create
  a W+X mapping. ktest 115 walks every live entry and asserts no leaf is
  both writable and executable; boot stops if the CPU has no NX.
  `vmm_map_mmio()` maps device registers uncached at `dmap_base + pa` and
  refuses any range that overlaps RAM in the memory map (usable, kernel,
  bootloader-reclaimable) or the framebuffer, so a firmware table or BAR
  that names RAM as an I/O APIC, HPET, ECAM window or device register
  block cannot turn register writes into kernel-memory writes; that
  device is skipped.
- **Immutable kernel image** (OpenBSD `mimmutable` idea): the last boot
  step locks the kernel. Exactly what is locked:
  - the kernel window (PML4 slot 511: text, rodata, data, bss mappings):
    `vmm_map`/`vmm_protect`/`vmm_unmap` return `-EPERM` there, and every
    page-table page below PML4[511] (PDPT, PDs, PTs) becomes read-only in
    the direct map (2 MiB direct-map pages are split as needed), so a
    direct write to one of those PTEs faults;
  - the image's alias in the direct map (`dmap_base` + the image's
    physical frames, mapped read-only since boot): the same three calls
    return `-EPERM` for it.
  Not locked: the PML4 itself and the direct map's own page tables,
  including the PTEs of that read-only alias, stay writable, so a kernel
  write primitive could still edit them directly (see ROADMAP.md).
- **kmem.c** - power-of-two zones of 16..2048 bytes, one page per slab,
  free list and in-use count kept in `struct page`; bigger requests take
  whole buddy blocks. Double frees are detected and panic.
- **uvm.c** - user windows: `uvm_alloc(jid, npages, flags)` charges the jail
  first (`rs_jail_mem_charge`, hierarchical limits), then maps zeroed pages
  `U|W|NX` (data) or `U|R|X` (code, `UVM_EXEC`). `copyin`/`copyout` check the
  range against `USER_TOP`, check every page for `U` (and `W`), open the
  SMAP window (`stac`), copy with `uaccess_copy` (fault fixup), close it
  (`clac`).

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

## Rust subsystems (`rust/src/`)

| Module | Role |
|---|---|
| `ffi.rs` | the C -> Rust boundary: every `rs_*` export, the global instances behind their locks |
| `kenv.rs` | the only path from Rust to C services (`magia_*`); a thread-local mock under `cargo test` |
| `klock.rs` | `KLock`: IRQ-disabling spinlock (Giant for kernel state; separate SCHED and ACCT locks) |
| `abi.rs` | `#[repr(C)]` mirror of `include/magia/abi.h`, size-checked against C at boot |
| `usb.rs` | USB device/configuration descriptors and HID boot-keyboard reports from devices, parsed as untrusted input; reports to ASCII and VT220 sequences |
| `acpi.rs` | static ACPI tables (RSDP, XSDT/RSDT, MADT, HPET, MCFG, FADT with its power fields) parsed as untrusted input; no heap, fixed-size results, fuzzed by host tests |
| `aml.rs` | the `\_S5` package from a DSDT or SSDT: an object-level walk with a flagged byte-scan fallback (not an AML interpreter), fuzzed |
| `usbhub.rs` | hub interface, hub descriptors, port status, boot mouse interface and reports from USB devices |
| `uaccess.rs` | typed `copyin`/`copyout` of user structures and strings (copied once, then validated) |
| `syscall.rs` | the syscall table: typed argument descriptors, capability-mode flag, generic validation, dispatch, blocking loop |
| `kernel.rs` | the kernel state struct: processes, jails, open files, VFS, pipes, kqueues, rings |
| `proc.rs` | process table: spawn, exit, waitpid, nice/weights, listing, visibility |
| `cap.rs` | Capsicum rights and capability-mode rules |
| `jail.rs`, `acct.rs` | jails (root, hostname, visibility, nesting) and the single hierarchical accounting tree |
| `vfs.rs`, `fs.rs`, `pipe.rs` | in-memory namespace with ramfs + devfs, file system calls, pipes |
| `kqueue.rs` | kqueue/kevent filters READ, WRITE, PROC, TIMER, USER |
| `uring.rs` | narrowed io_uring-style rings (NOP, READ, WRITE, TIMEOUT; kill switch; per-jail permission) |
| `futex.rs` | futex wait/wake keyed by user address |
| `sysctl.rs` | the MIB tree, including C nodes from the linker set |
| `sched.rs` | EEVDF policy |
| `dbg.rs` | text output for the monitor (`ps`, `jails`, `ls`, `cat`, `sysctl`, `uname`) |

State lives in plain structs owned by one global behind an IRQ-disabling
Giant lock; the scheduler and the jail memory accounting have their own
locks because C calls them from the timer IRQ and from inside
`uvm_alloc`. `unsafe` is confined to `ffi.rs` (including the ACPI physical-memory reader), `acpi.rs` (one `mem::zeroed` of an all-integer struct), `kenv.rs`, `klock.rs`,
`abi.rs`, `lib.rs` (allocator, panic handler), `sysctl.rs` (reading C
sysctl pointers) and `uring.rs` (volatile access to the shared ring).
Blocking calls register the caller on a wait list, call
`magia_sleep_prepare` while still holding Giant, drop Giant, then
`magia_sleep_commit`, and retry.

## Debug monitor

`kernel/monitor.c`: a line editor on the console input ring (COM1, PS/2
and USB keyboards), echoing to serial and the screen; Page Up/Down scroll
the screen. Commands: `help uname cpu ps threads jails mem uptime ls cat
sysctl echo lspci usb hwinfo dmesg bench bt panic reboot poweroff`.

## Benchmarks

`kernel/bench.c` (`bench [n]`, `ktest bench`) runs in a process of its
own and times kernel primitives with the TSC (lfence; rdtsc; lfence /
rdtscp; lfence): the empty measurement, `syscall` getpid from ring 3
(timed in user mode by a blob in `user_blob.S`) and through `ksyscall`,
a sleep/wakeup hand-off between two threads, timer interrupt to woken
thread (from the TSC deadline that fired), `vmm_map`/`vmm_unmap`,
`kmalloc`/`kfree`, `page_alloc`/`page_free`. `bench_stats.c` sorts the
samples (heapsort) and reports min, median, p99, max and mean, in
cycles and tenths of ns, as `BENCH` lines (docs/TESTING.md).

## Planned: block layer and VFS shaped for btrfs (stage 4)

btrfs is the stage-4 disk file system (on-disk compatible, written in Rust
from the public format documentation; see ROADMAP.md). The interfaces
stage 2-4 add are shaped for it now, though no btrfs code exists yet:

- **Block layer**: a request carries a device id plus a byte offset, not a
  single disk, because btrfs maps logical addresses through its chunk tree
  onto several devices (RAID1/10, DUP); 4 KiB sectors, 16 KiB metadata
  nodes, I/O sizes that are multiples of the sector size; completion
  callbacks rather than synchronous reads (copy-on-write writes are
  batched per transaction).
- **Checksums are the file system's job**: every block read is verified
  (crc32c first, xxhash64/sha256/blake2b later) before anything parses it,
  and a bad copy can be retried from another mirror, so the block layer
  returns the device a block came from.
- **VFS**: inode numbers are 64-bit and unique only within a subvolume
  (the VFS key is (subvolume, inode)); snapshots appear as directories;
  ramfs stays for `/tmp`.

## Build

`make` compiles C with `-ffreestanding -fpie -mno-red-zone
-mgeneral-regs-only -fstack-protector-strong -ftrivial-auto-var-init=zero`,
builds the Rust staticlib for `x86_64-unknown-none` (PIE relocation model,
small code model, frame pointers), and links a static PIE twice with `-z
text` (no text relocations): the second pass embeds the symbol table
(`scripts/check-text-stable.sh` verifies no code moved). The symbol table
stores name offsets, not pointers, so it needs no relocations.
`scripts/fix-pie.sh` checks that every dynamic relocation is
`R_X86_64_RELATIVE`, that there are no text relocations and no W+X
segment, then marks the ELF `ET_DYN` (GNU ld writes `ET_EXEC` for a PIE
with a fixed non-zero base, and Limine relocates only `ET_DYN`).
