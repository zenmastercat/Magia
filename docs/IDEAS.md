# Ideas Magia takes from FreeBSD and Linux

Magia merges *ideas*, never code. Every mechanism below was re-derived from
public descriptions (manual pages, papers, documentation); no source file
of FreeBSD or Linux was copied or translated. Numeric tables are recomputed
from their formula (for example the nice-to-weight table, see EEVDF below).

Two research notes back the choices:

- **REA binary comparison** of the Linux 7.3.4 and FreeBSD 15.1 kernels
  ([`docs/research/rea-binary-comparison.md`](research/rea-binary-comparison.md)):
  FreeBSD registers sysctls, init hooks and probes through *linker sets*;
  Linux modules carry a signature and a build fingerprint (`vermagic`).
- **Research Desk, "Magia kernel design: what FreeBSD and Linux each do
  best"** ([`docs/research/kernel-pros.md`](research/kernel-pros.md) and
  [`.json`](research/kernel-pros.json), 2026-11-20; 17 rows, dated primary sources). Row numbers and
  source IDs (S1, S22, ...) below refer to that file.

Status: **built** = in the tree and tested; **partial** = simplified;
**roadmap** = see ROADMAP.md.

## From FreeBSD

### Linker sets and SYSINIT (built)
*Source:* REA comparison (FreeBSD's `set_sysinit_set`, `set_sysctl_set`,
`set_modmetadata_set`); `kernel-pros.md` row 6.
*Idea:* a registration macro drops a pointer into a named ELF section; the
linker script gathers each set between `__start_set_X` and `__stop_set_X`;
the kernel walks the array at boot. Adding a subsystem, a test or a sysctl
never edits a central list.
*In Magia:* `include/magia/linker_set.h`. Three sets: `SYSINIT` boot hooks
(sorted at link time by a 5-digit key, so the section name order *is* the
boot order), `KTEST` in-kernel tests (3-digit key), and C sysctl nodes
(`include/magia/sysctl.h`) that the Rust MIB tree grafts in at boot.

### Capsicum (built)
*Source:* capsicum(4), cap_enter(2) (S1, S2); Watson et al., USENIX
Security 2010 (S3); `kernel-pros.md` row 1.
*Idea:* two primitives - an irreversible *capability mode* in which global
namespaces (absolute paths, `AT_FDCWD`, other processes, jails, hostnames)
are unreachable, and *per-descriptor rights* that can only shrink.
*In Magia:* `cap_enter`, `cap_getmode`, `cap_rights_limit`,
`cap_rights_get`; every fd operation checks a right (`ENOTCAPABLE`), and
capability mode refuses global-namespace calls (`ECAPMODE`). Rights are one
64-bit mask instead of FreeBSD's versioned `cap_rights_t`. Children inherit
capability mode. Process descriptors (`pdfork`) are on the roadmap.

### Jails (built, merged with Linux namespaces/cgroups)
*Source:* jail(8) (S8); `kernel-pros.md` row 2.
*Idea:* one container object - a process belongs to exactly one jail, sees
only its jail's file tree (path root), hostname and processes, and jails
nest.
*In Magia:* `jail_create/attach/get/set`, per-jail root path, hostname,
process visibility limited to the jail subtree, nesting with depth and
children limits. See "namespaces and cgroups" below for the Linux half.

### kqueue (built)
*Source:* kqueue(2) (S15); `kernel-pros.md` row 3.
*Idea:* one kernel event queue with typed filters and a change list applied
in the same call that harvests events; level- or edge-style via `EV_CLEAR`,
one-shot events, per-event user data.
*In Magia:* `kqueue`, `kevent` with `EVFILT_READ/WRITE` (pipes, files),
`EVFILT_PROC` (`NOTE_EXIT`), `EVFILT_TIMER`, `EVFILT_USER`.

### sysctl MIB tree (built)
*Source:* sysctl(3) (S35); `kernel-pros.md` row 6.
*Idea:* a typed, self-describing tree of kernel variables with numeric OIDs
and dotted names, per-node flags (read-only, writable, readable in
capability mode).
*In Magia:* the tree lives in Rust; C contributes nodes through a linker
set. Nodes carry a "readable in capability mode" flag (FreeBSD's
`CTLFLAG_CAPRD` idea). Writes are validated (range, type) and allowed only
from the root jail. Per-node ABI stability levels (Linux idea from the same
row) are on the roadmap.

### Master syscall table (built)
*Source:* FreeBSD `syscalls.master` concept; `kernel-pros.md` row 12.
*Idea:* one table describes every call - number, name, argument types,
capability-mode flag - and dispatch derives from it.
*In Magia:* the Rust syscall table holds typed argument descriptors; a
generic validator checks fds, user pointers, ranges and flag masks before
any handler runs.

### ULE ideas (partial)
*Source:* sched_ule(4), Roberson 2003 (S24, S25); `kernel-pros.md` row 5.
*Taken:* the scheduler is a replaceable policy behind a narrow interface
(FreeBSD 15 lets you choose 4BSD or ULE at boot). Magia's policy is EEVDF
(below); the C mechanism calls only `rs_sched_add/remove/wakeup/tick/pick`.
ULE's interactivity scoring and per-CPU run queues are roadmap (Magia is
uniprocessor).

### GEOM-style storage layering (roadmap)
*Source:* geom(4) (S40); `kernel-pros.md` row 7. Magia has no block devices
yet; the plan is providers/consumers for every transform on top of a
blk-mq-style queue (Linux idea, same row).

## From Linux

### EEVDF scheduler (built)
*Source:* the EEVDF paper (Stoica & Abdel-Wahab, 1995) as adopted by Linux
6.6; `kernel-pros.md` row 5.
*Idea:* each runnable entity has a virtual runtime advancing at
1/weight; an entity is *eligible* when it has not received more than its
share (lag >= 0, i.e. vruntime <= the weighted average V); among eligible
entities run the one with the earliest *virtual deadline*
(vruntime + slice/weight). Sleepers keep their lag, so they neither lose
nor gain unfairly.
*In Magia:* `rust/src/sched.rs`. Nice-to-weight is computed from the
formula weight = 1024 / 1.25^nice (Linux ships a hand-tuned table; we do not
copy it). Base slice is a sysctl. Host tests check fairness ratios, lag
preservation and wakeup preemption; ktest 203/204 check timer preemption
and a 2:1 weight split on the real kernel.

### Namespaces and cgroups, folded into jails (built)
*Source:* user_namespaces(7) (S12), Control Group v2 (S13);
`kernel-pros.md` row 2.
*Idea taken:* cgroup v2's *single hierarchy* - one tree, controllers
(memory, pids, cpu weight) enabled per subtree, limits checked against every
ancestor.
*In Magia:* the jail tree **is** the accounting tree (Research Desk change
2). Memory (`mem_max`, charged where user pages are allocated, in
`uvm_alloc`), process count (`pids_max`) and CPU weight (`cpu_weight`,
scaling the EEVDF weight of the jail's processes) are hierarchical: a
charge fails if any ancestor would exceed its limit. A jail is also the
namespace boundary (path root, hostname, pid visibility), so Magia needs no
separate namespace objects.

### io_uring-style rings, narrowed (built)
*Source:* io_uring (Axboe 2019, S18); Google kCTF VRP report, 2023 (S22);
`kernel.io_uring_disabled` (S23); `kernel-pros.md` row 4.
*Idea:* submission and completion rings in memory shared by user space and
kernel, so batches of I/O need one syscall.
*Narrowing (Research Desk change 1):* Google reported that 60 % of kCTF VRP
exploit submissions in 2022-23 used io_uring (S22). Magia's ring therefore
has a tiny fixed opcode set (NOP, READ, WRITE, TIMEOUT), every READ/WRITE
goes through the same fd lookup, Capsicum rights checks and
`copyin`/`copyout` as the `read`/`write` syscalls, the kernel copies each
SQE out of shared memory once before validating it (no double fetch), and
there is a kill switch: sysctl `kern.ring.enable` plus a per-jail
permission flag.

### futex (built)
*Source:* futex(2) concept (Franke, Russell, Kirkwood 2002).
*Idea:* the fast path stays in user space; the kernel only provides
"sleep if the word still equals v" and "wake n sleepers on this address".
*In Magia:* `FUTEX_WAIT` (with timeout) and `FUTEX_WAKE`, keyed by user
address (Magia has one address space).

### Kernel self-protection baseline (partial)
*Source:* Kernel Self-Protection (S87), KSPP recommended settings (S88);
`kernel-pros.md` row 15 (Research Desk change 3).
*Built:* kernel text and rodata never writable (W^X page tables, also no
writable alias of the image in the direct map; ktest scans every live page
table entry); NX; CR0.WP; SMEP; SMAP with an explicit user-access window;
UMIP; `-fstack-protector-strong` with a random canary (RDRAND + TSC at
boot); zero-initialised stack variables (`-ftrivial-auto-var-init=zero`);
kernel thread stack canaries checked on every context switch; ring-3 faults
kill the process, not the kernel. The stack protector, stack canary and W^X
are proven by deliberate-crash boots in `make test`.
*Roadmap:* kCFI (GCC 15 has no `-fsanitize=kcfi`; needs Clang) and IBT
(needs `endbr64` in Rust too, which stable Rust cannot emit yet), KASLR
(Magia is linked at a fixed address), guard pages for kernel stacks.

### Signed modules with a build fingerprint (roadmap)
*Source:* REA comparison - Linux `kvm.ko` carries an appended signature and
`vermagic`. Magia has no module loader; when it gets one, modules will be
refused unless signed and built for the same kernel.

### eBPF (roadmap)
*Source:* `kernel-pros.md` rows 10-11. One small verified bytecode VM shared
by tracing probes (DTrace SDT-style static probes via linker sets) and a
future packet filter.

## Ideas common to both

- **Buddy page allocator with a per-frame descriptor array** (Linux
  `struct page`, FreeBSD `vm_page` + `vm_phys`), slab-style object zones
  for `kmalloc` (FreeBSD UMA, Linux SLUB; originally Bonwick's slab
  allocator). Simplified to power-of-two zones; per-CPU caches are roadmap.
- **Higher-half kernel with a direct map of RAM**, W^X sections.
- **Negative errno returns** (Linux style) with FreeBSD's `ENOTCAPABLE` and
  `ECAPMODE` numbers for Capsicum errors.
- **`syscall`/`sysret` with `swapgs`** and a per-CPU block reached through
  `%gs` - the standard x86_64 kernel entry, written from the AMD/Intel
  manuals.
