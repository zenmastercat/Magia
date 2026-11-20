# Magia kernel design: what FreeBSD and Linux each do best

Prepared by Research Desk for Mr.Main, 2026-11-20. Scope: x86_64 kernel only, written from scratch (no code merge). Versions read: FreeBSD 15.1 manual pages and the freebsd-src main branch; Linux kernel docs built for 7.4.0-rc3, Linux man-pages 6.19 and the torvalds/linux master branch. Sources were opened between 2026-11-19 and 2026-11-20. Magia's own plan (`docs/plan.md`, LICENSE) was read to compare each pick with what is already planned.

Labels: **Verified** = stated in a primary source I opened; **Reported** = stated by a source but it is the author's or vendor's own measurement, or a report of statements; **Inference** = my reasoning; **Unknown** = not found. Every 'Magia pick' is Inference. The same rows are in `kernel-pros.json`.

## Answer

Where Magia's plan has already decided, it takes the better-supported side: Capsicum, jails, kqueue, a typed sysctl tree and a master syscall table from FreeBSD; EEVDF and ring-based I/O from Linux. For parts not yet planned, the evidence favours blk-mq queues with GEOM-style layering, RCU path lookup, folio-style page units, VNET with one filter VM, DTrace-style static probes and the Linux/KSPP hardening baseline. Three changes to the existing plan are supported: narrow and gate the io_uring-style rings, keep resource accounting in one cgroup-v2-style hierarchy, and add the Linux hardening the plan lacks (strong stack protector, zero-initialised stacks, kCFI/IBT, later KASLR). Confidence: medium-high for the facts (primary sources, dated); the picks themselves are inference.

## Summary

| # | Subsystem | Magia pick (Inference) | Comes from | Already in Magia plan | Confidence | Sources |
|---|---|---|---|---|---|---|
| 1 | [Process sandboxing: Capsicum vs seccomp-bpf / Landlock](#1-sandbox) | Capsicum as the core sandbox: capability mode plus per-descriptor rights plus process descriptors. | FreeBSD (+ Landlock ABI versioning) | yes | high | S1, S2, S3, S4, S5, S6, S7, S82 |
| 2 | [Containers: jails vs namespaces + cgroups v2](#2-containers) | A jail-like container object with VNET-style per-jail network stacks, plus one cgroup-v2-style hierarchy for resource accounting (controllers enabled per subtree). | FreeBSD jails + Linux cgroup v2 | partly | medium | S8, S9, S10, S11, S12, S13, S14 |
| 3 | [Event notification: kqueue vs epoll (and io_uring)](#3-events) | kqueue as Magia's single event queue, including a jail filter (FreeBSD 15.1's kqueue has JAIL and JAILDESC filters) and AIO/ring completions. | FreeBSD | yes | high | S15, S16, S17 |
| 4 | [Asynchronous I/O: io_uring vs FreeBSD aio](#4-aio) | Keep the ring design for speed, but narrow it: a small fixed opcode set that reuses the normal syscall handlers and Capsicum rights checks, a sysctl kill switch like io_uring_disabled, and completions that can also be posted to kqueue. | Linux rings, narrowed | partly | medium | S18, S22, S23, S20, S21, S15 |
| 5 | [CPU scheduler: ULE vs CFS -> EEVDF, plus sched_ext](#5-scheduler) | EEVDF as the default policy, per-CPU run queues when Magia goes SMP, and a scheduler-class interface so a second policy can be chosen at boot (FreeBSD 15 style) and, later, loaded with a safety fallback (sched_ext idea). | Linux EEVDF (+ FreeBSD boot-time choice) | yes | medium | S24, S25, S26, S27, S29, S30, S28, S31, S33, S34 |
| 6 | [Kernel tunables: sysctl MIB tree vs /proc/sys + sysfs](#6-tunables) | FreeBSD-style typed MIB with descriptions and per-node flags (read-only, boot tunable, capability-mode allowed), registered through linker sets; add a Linux-style stability level per node (stable/testing/obsolete). | FreeBSD (+ Linux ABI levels) | yes | high | S35, S36, S1, S37, S38, S39 |
| 7 | [Storage layering: GEOM vs device-mapper / block layer](#7-storage) | A blk-mq-style per-CPU multi-queue core for submission, with GEOM-style providers and consumers for every transform (partitions, encryption, mirroring). | Linux blk-mq + FreeBSD GEOM | no | medium | S40, S41, S43, S42 |
| 8 | [File systems and VFS: ZFS/UFS vs ext4/btrfs](#8-filesystem) | Linux-style dentry cache with RCU-walk path lookup once Magia is SMP; when a disk file system is added, design it around ZFS-style checksums on every block, verified on read and repaired from redundancy. | Linux VFS lookup + ZFS ideas | partly | medium-low | S44, S45, S47, S51, S81, S48, S49, S50, S46 |
| 9 | [Memory management: FreeBSD VM/UMA vs Linux SLUB/folios/MGLRU](#9-memory) | Keep the plan's buddy allocator, struct page array and slab zones; give zones per-CPU caches (UMA/SLUB style) when SMP arrives; use a folio-like multi-page type in the page cache from day one; FreeBSD-style reservation superpages for anonymous memory; generation-based (MGLRU-like) reclaim later. | Both | partly | medium | S52, S53, S54, S55, S56, S32, S58, S57, S59, S109 |
| 10 | [Networking: netgraph, VNET, pf vs nftables, XDP/eBPF](#10-networking) | VNET-style stack instances from the start (one per jail); one packet filter built on a small rule VM with sets and maps (nftables idea) instead of several firewalls; an XDP-like early hook later that shares the verifier with tracing. | FreeBSD VNET + Linux nftables/XDP ideas | no | medium-low | S60, S61, S8, S62, S63, S64, S65, S66, S67, S68, S69, S23 |
| 11 | [Tracing: DTrace vs eBPF / ftrace](#11-tracing) | Static probe points (DTrace SDT style) registered through linker sets now; NOP-patched function-entry hooks (ftrace style) later; one small verified bytecode VM for probe actions, shared with the packet filter. | FreeBSD DTrace probes + Linux ftrace/eBPF ideas | no | medium | S70, S71, S72, S73, S74, S75 |
| 12 | [Syscall ABI stability and documentation](#12-abi) | One master syscall table that generates dispatch, typed argument validation, capability flags and a doc stub; man pages in the Magia tree; a per-ABI personality table so a compatibility ABI can be added later; adopt Linux's 'never break userspace' promise once Magia has users; keep internal APIs unstable. | FreeBSD master table + Linux no-regressions rule | yes | high | S76, S77, S39, S84, S82, S83, S85 |
| 13 | [License: BSD-2-Clause vs GPLv2 and the effect on reuse](#13-license) | Keep Magia's BSD-2-Clause licence, add an SPDX line to every file (both projects use SPDX), and keep the documented clean-room rule (ideas only, no copied code). | FreeBSD (BSD-2) | yes | high (facts); legal reading is Inference | S78, S79, S80, S81, S95, L1 |
| 14 | [Driver breadth](#14-drivers) | Do not chase breadth: target QEMU's virtual devices first (virtio), then a few common real devices; keep drivers in-tree with an unstable internal API; shape the driver interfaces so a LinuxKPI-like shim could host permissively licensed Linux drivers later, if the no-copy rule is ever relaxed. | Linux in-tree model + FreeBSD LinuxKPI lesson | no | medium | S94, S92, S93, S110, S77 |
| 15 | [Security mitigations: KASLR, SMEP/SMAP, CFI, stack protector, W^X](#15-mitigations) | Linux/KSPP as the baseline. | Linux / KSPP | partly | high (facts); pick is Inference | S86, S89, S90, S91, S87, S88, L1 |
| 16 | [Rust in each kernel: where it is used and why](#16-rust) | Keep the plan's split: C for boot, CPU, MMU and context switch; a no_std Rust crate with zero external dependencies for policy subsystems. | Linux model | yes | medium | S97, S96, S98, S99, S100, S102, S105, S106, S107, S108, L1 |
| 17 | [C vs Rust performance in kernel code](#17-rust_perf) | Speed is not a reason against Rust in Magia. | Evidence only | n/a | medium | S103, S101, S102, S104, L1 |

<a id="1-sandbox"></a>
## 1. Process sandboxing: Capsicum vs seccomp-bpf / Landlock

**FreeBSD:** Capsicum (since 9.0): cap_enter(2) puts a process in capability mode, which cuts access to global namespaces (file-system paths, PIDs); each file descriptor carries rights limited with cap_rights_limit(2); process descriptors replace PIDs. Capability mode cannot be left and is inherited by children.

**Linux:** seccomp-bpf (filter mode since 3.5; seccomp() syscall since 3.17) runs a BPF filter on each syscall number and its register arguments. Landlock (since 5.13) lets an unprivileged process stack rulesets that restrict file-system access, TCP/UDP ports and some IPC scopes, with a versioned ABI.

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | One deny-by-default model: rights travel with descriptors; very little code to adopt (about 100 lines for Chromium in 2010); measured cost small: capability-aware syscalls a few percent slower (2010) | Programs must open what they need before entering capability mode (Inference); the Linux port proposed in 2014 is not in mainline, so there is no cross-platform reuse (Inference) |
| Linux | seccomp filters apply to unmodified programs; Landlock is unprivileged and stackable; its ABI grows by numbered versions | seccomp filters cannot dereference pointers, so they cannot check paths; the kernel docs say filtering 'isn't a sandbox'; three overlapping mechanisms (seccomp, Landlock, LSMs) (Inference) |

**Magia pick (Inference):** Capsicum as the core sandbox: capability mode plus per-descriptor rights plus process descriptors. Mark each syscall as allowed or not in capability mode in the master syscall table, as FreeBSD's syscalls.master does with CAPENABLED. Borrow Landlock's numbered-ABI idea for adding new rights later. Leave seccomp-style filters out until Magia has a userland.

**Why:** Magia has no legacy programs to keep working, which removes Capsicum's main cost (retrofitting). The model is simpler to reason about than a syscall filter and was cheap in the only measurement found.

**Fit with the Magia plan:** Matches the plan (Rust 'Capsicum rights', FreeBSD errno ENOTCAPABLE/ECAPMODE). **Confidence:** high.

| Claim | Label | Sources |
|---|---|---|
| Capability mode removes access to global namespaces; rights are limited per descriptor; process descriptors exist; first appeared in FreeBSD 9.0. | Verified | S1 |
| In capability mode only about 60 of over 15,000 sysctl parameters stay reachable. | Verified | S1 |
| Capability mode cannot be cleared and is inherited by fork/pdfork children. | Verified | S2 |
| 2010 measurements: Chromium sandbox about 100 lines with Capsicum vs 11,301 with Linux seccomp (strict mode) plus userspace wrapper, 22,350 with Windows ACLs; cap_new 50.7% slower than dup; capability-aware syscalls 'just a few percent slower'. | Verified | S3 |
| seccomp BPF filters may not dereference pointers; the docs say 'System call filtering isn't a sandbox' but a way to minimise exposed kernel surface. | Verified | S4 |
| seccomp filter mode on x86-64 since Linux 3.5; seccomp() syscall since 3.17. | Verified | S5 |
| Landlock: unprivileged, first introduced in Linux 5.13; restricts file system, TCP/UDP ports and scopes; at most 16 stacked rulesets; ABI versions 1-12 listed in the 7.4-rc3 docs. | Verified | S6 |
| Capsicum-for-Linux patches (Google, 2014) were described as 'a hard sell' and are not in mainline. | Inference | S7, S6 |
| FreeBSD main flags 300 of 560 numbered syscall-table lines CAPENABLED (my count, 2026-11-19). | Verified | S82 |

<a id="2-containers"></a>
## 2. Containers: jails vs namespaces + cgroups v2

**FreeBSD:** Jails (since 4.0): one kernel object per container with named parameters; hierarchical jails since 8.0; VNET jails get their own network stack. Resource limits via rctl(8) (since 9.0). The Handbook now also covers OCI containers with Podman.

**Linux:** Eight namespace types (cgroup, ipc, network, mount, pid, time, user, uts) combined per process; unprivileged user namespaces since 3.8. Resource control via cgroup v2, a single unified hierarchy.

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | One first-class object is easy to audit and name (Inference); VNET gives each jail a full network stack | rctl write-bandwidth/IOPS limits are counted at the file-system cache, not the device |
| Linux | Namespaces compose freely; cgroup v2's single tree replaced v1's multiple hierarchies, which 'wasn't useful in practice' | Unprivileged user namespaces became a step in several privilege-escalation chains; Ubuntu 23.10 restricted them; a 'container' is a userland convention over many kernel pieces (Inference) |

**Magia pick (Inference):** A jail-like container object with VNET-style per-jail network stacks, plus one cgroup-v2-style hierarchy for resource accounting (controllers enabled per subtree). Do not add unprivileged user namespaces.

**Why:** The jail object keeps isolation in one place; cgroup v2 is the better-documented accounting model; the user-namespace record argues against unprivileged nesting.

**Fit with the Magia plan:** Matches the plan ('jails + resource accounting'); suggests making the accounting hierarchical. **Confidence:** medium.

| Claim | Label | Sources |
|---|---|---|
| jail(8) appeared in FreeBSD 4.0; hierarchical jails in 8.0; jail.conf in 9.1; parameters include vnet and children.max. | Verified | S8 |
| The Handbook covers VNET jails and has a chapter on OCI containers including Podman. | Verified | S9 |
| rctl(8) appeared in FreeBSD 9.0; writebps/writeiops are accounted to the file-system cache, not devices. | Verified | S10 |
| Linux has 8 namespace types; time namespaces since 5.6, cgroup namespaces since 4.6. | Verified | S11 |
| Unprivileged user namespaces since Linux 3.8. | Verified | S11, S12 |
| cgroup v2 uses one unified hierarchy; v1's multiple hierarchies 'wasn't useful in practice'; no-internal-process rule; threaded mode. | Verified | S13 |
| Ubuntu (2023-10-09): unprivileged user namespaces are 'broadly used as a step in several privilege escalation exploit chains'; 23.10 restricts them. | Verified | S14 |

<a id="3-events"></a>
## 3. Event notification: kqueue vs epoll (and io_uring)

**FreeBSD:** kqueue/kevent (since 4.1): one call submits changes and collects events; filters cover READ, WRITE, AIO, VNODE, PROC, PROCDESC, SIGNAL, TIMER, USER, EMPTY, JAIL and JAILDESC.

**Linux:** epoll: an interest list of file descriptors, edge- or level-triggered, scales to large numbers of descriptors. Non-fd events need fd wrappers (Inference: not checked here).

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | Generic: one queue for fds, files, processes, signals, timers, user events and AIO completion; kernel keeps the state, so cost does not grow with the number of watched objects per call (2001 design) | No source found on kqueue weaknesses at scale (Unknown) |
| Linux | Well-known scaling for large fd sets | Interest is tied to the open file description: a closed fd that was dup'd keeps reporting events until all duplicates are closed |

**Magia pick (Inference):** kqueue as Magia's single event queue, including a jail filter (FreeBSD 15.1's kqueue has JAIL and JAILDESC filters) and AIO/ring completions.

**Why:** One generic queue with typed filters is easier to secure and document than per-type descriptor wrappers.

**Fit with the Magia plan:** Matches the plan (kqueue in the Rust crate). **Confidence:** high.

| Claim | Label | Sources |
|---|---|---|
| kqueue filters include READ, WRITE, AIO, VNODE, PROC, PROCDESC, SIGNAL, TIMER, USER, EMPTY, JAIL, JAILDESC; kqueue/kevent since FreeBSD 4.1; kqueuex() since 14.0. | Verified | S15 |
| poll/select are stateless and O(N) per call; kqueue keeps state in the kernel and is generic beyond sockets and pipes. | Verified | S16 |
| epoll scales to large numbers of fds; a closed fd that was duplicated keeps reporting events until all duplicates are closed. | Verified | S17 |

<a id="4-aio"></a>
## 4. Asynchronous I/O: io_uring vs FreeBSD aio

**FreeBSD:** POSIX aio. Sockets, raw disks and local regular files are always truly asynchronous ('safe'); other file types are 'unsafe' and off by default (vfs.aio.enable_unsafe). Completions can be delivered through kqueue (EVFILT_AIO). aio_readv since 13.0, aio_read2 since 14.1.

**Linux:** io_uring: submission and completion rings shared between user space and kernel, so many requests need no syscall each. A sysctl (kernel.io_uring_disabled, 0/1/2) can restrict or disable it.

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | Small, POSIX-defined surface; integrates with kqueue | Only some file types are truly async by default |
| Linux | Author's 2019 benchmark: 1.7M 4 KiB IOPS polled vs 608K with Linux native aio; about 1.2M non-polled | Google (2023): 60% of kCTF VRP exploit submissions in the prior year used io_uring; disabled on ChromeOS and production Google servers, blocked for Android apps |

**Magia pick (Inference):** Keep the ring design for speed, but narrow it: a small fixed opcode set that reuses the normal syscall handlers and Capsicum rights checks, a sysctl kill switch like io_uring_disabled, and completions that can also be posted to kqueue.

**Why:** Rings win on throughput, but io_uring's breadth made it the top exploit target in Google's programme; a narrow, rights-checked ring keeps most of the gain with less new surface (Inference).

**Fit with the Magia plan:** Plan has 'io_uring-style rings'; add the opcode allowlist and kill switch. **Confidence:** medium.

| Claim | Label | Sources |
|---|---|---|
| io_uring: 1.7M 4K IOPS polled vs 608K for Linux aio; non-polled about 1.2M (author's own benchmark, 2019). | Reported | S18 |
| 60% of kCTF VRP submissions exploited io_uring; it is disabled on ChromeOS, unreachable to Android apps via seccomp-bpf and disabled on production Google servers (2023-06-14). | Verified | S22 |
| kernel.io_uring_disabled (0/1/2) with io_uring_group; 'Enabling this shrinks the kernel's attack surface.' | Verified | S23 |
| FreeBSD aio: sockets, raw disks, local regular files always async; other types unsafe and off by default; aio in all kernels since 11.0. | Verified | S20 |
| aio_readv since FreeBSD 13.0, aio_read2 since 14.1; AIO completion via EVFILT_AIO. | Verified | S21, S15 |

<a id="5-scheduler"></a>
## 5. CPU scheduler: ULE vs CFS -> EEVDF, plus sched_ext

**FreeBSD:** ULE (since 5.1): per-CPU run queues, CPU affinity, topology and hyper-threading awareness, interactivity heuristics. FreeBSD 15 builds ULE and 4BSD together and picks one at boot (ULE default).

**Linux:** EEVDF replaced CFS in 6.6 (2023-10-29): each task has lag and a virtual deadline; tasks may request a time slice via sched_setattr(). sched_ext (from 6.12, 2024-11-17) lets BPF programs implement a scheduler class that can be switched at runtime, with automatic fallback.

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | Per-CPU queues and locks from the start (2003 design); better long-run load balance than CFS in the 2018 study | ULE 'may cause starvation', even with identical threads (2018 study) |
| Linux | Deadlines give a defined latency knob per task; sched_ext allows experiments without risking the system: default restored on error, stall or SysRq-S | 2018 study: no overall winner over ULE; CFS-era data only (no ULE vs EEVDF study found) |

**Magia pick (Inference):** EEVDF as the default policy, per-CPU run queues when Magia goes SMP, and a scheduler-class interface so a second policy can be chosen at boot (FreeBSD 15 style) and, later, loaded with a safety fallback (sched_ext idea).

**Why:** Measured evidence shows no overall winner, so pick the design with the clearest fairness and latency definition and keep it swappable.

**Fit with the Magia plan:** Matches the plan (EEVDF in Rust; nice-to-weight table recomputed from its formula). **Confidence:** medium.

| Claim | Label | Sources |
|---|---|---|
| ULE: per-CPU run queues, affinity, topology aware, interactivity heuristics; first appeared in FreeBSD 5.1. | Verified | S24, S25 |
| 2018 study with ULE ported into Linux: 'There is no overall winner'; ULE 'may cause starvation'; CFS balances load faster, ULE achieves better long-run balance. | Verified | S26 |
| Linux began moving to EEVDF in 6.6; lag and virtual deadline; slices requestable via sched_setattr(). | Verified | S27, S29 |
| Linux 6.6 released 2023-10-29 replacing CFS with EEVDF. | Verified | S30 |
| sched_ext: BPF-defined scheduler class, switchable at runtime; system integrity kept whatever the BPF scheduler does; first pieces in 6.12 (2024-11-17). | Verified | S28, S31 |
| FreeBSD 15: ULE and 4BSD compiled together, selected at boot (kern.sched.name), ULE default; GENERIC amd64 gets both (commit 2026-02-11). | Verified | S33, S34 |
| No published comparison of ULE with EEVDF was found. | Unknown | - |

<a id="6-tunables"></a>
## 6. Kernel tunables: sysctl MIB tree vs /proc/sys + sysfs

**FreeBSD:** One sysctl MIB tree: numeric OIDs also addressable by name, typed values (integers, strings, tables), kernel-side registration with a description string per node, and per-node flags (e.g. which nodes are reachable in capability mode).

**Linux:** /proc/sys text files for tunables and sysfs for device/kernel objects ('one value per file', ASCII). The binary sysctl(2) syscall was removed in 5.5. Stability is documented per file in Documentation/ABI (stable, testing, obsolete, removed).

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | Typed, self-describing, one namespace; per-node policy flags (capability mode allows about 60 of over 15,000 nodes) | No per-node stability level documented like Linux's ABI files (Inference) |
| Linux | Plain files work with any tool; ABI stability levels are written down | Untyped text; two separate trees; binary interface removed |

**Magia pick (Inference):** FreeBSD-style typed MIB with descriptions and per-node flags (read-only, boot tunable, capability-mode allowed), registered through linker sets; add a Linux-style stability level per node (stable/testing/obsolete).

**Why:** Typed nodes with flags serve both the sandbox and documentation; Linux's stability levels make the promise explicit.

**Fit with the Magia plan:** Matches the plan (sysctl MIB tree in Rust, linker sets). **Confidence:** high.

| Claim | Label | Sources |
|---|---|---|
| sysctl MIB is an array of integers, also accessible by name (sysctlbyname); typed values. | Verified | S35 |
| Kernel nodes are added with SYSCTL_ADD_* macros that take a description string. | Verified | S36 |
| About 60 of over 15,000 sysctl parameters are allowed in capability mode. | Verified | S1 |
| sysfs attributes should be ASCII text files, preferably one value per file. | Verified | S37 |
| The binary sysctl(2) syscall was removed in Linux 5.5; use /proc/sys instead. | Verified | S38 |
| Documentation/ABI levels: stable (at least 2 years; syscalls expected never to change), testing, obsolete, removed. | Verified | S39 |

<a id="7-storage"></a>
## 7. Storage layering: GEOM vs device-mapper / block layer

**FreeBSD:** GEOM: a modular framework of classes, geoms, providers and consumers that transform disk I/O requests; classes can stack in any order (only cycles are forbidden). Classes include ELI (encryption), MIRROR, RAID, PART_*, UZIP and GATE.

**Linux:** device-mapper targets (crypt, integrity, verity, thin, cache, raid, zoned, vdo, snapshot, linear, stripe, ...) on top of blk-mq, the multi-queue block layer with per-CPU software queues mapped to hardware queues.

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | One uniform graph for every transformation; topology-agnostic and extensible | No current scaling measurement of GEOM found (Unknown) |
| Linux | blk-mq removed the single request-queue lock bottleneck and scaled to about 10 million IOPS (2013); broad set of production targets | No weakness documented in the sources read (Unknown) |

**Magia pick (Inference):** A blk-mq-style per-CPU multi-queue core for submission, with GEOM-style providers and consumers for every transform (partitions, encryption, mirroring).

**Why:** blk-mq has the measured scaling; GEOM has the cleaner composition model; they sit at different layers, so both fit.

**Fit with the Magia plan:** Not yet in the plan (no block layer yet). **Confidence:** medium.

| Claim | Label | Sources |
|---|---|---|
| GEOM is a 'modular disk I/O request transformation framework'; classes stack in any order, only cycles are disallowed; many classes (ELI, MIRROR, RAID, PART_*, UZIP, GATE). | Verified | S40 |
| device-mapper targets include crypt, integrity, verity, thin, cache, raid, zoned, vdo, snapshot, linear, stripe. | Verified | S41 |
| 2013 measurements: the single-queue block layer lock was the bottleneck; blk-mq scaled to 10 million IOPS. | Verified | S43, S42 |

<a id="8-filesystem"></a>
## 8. File systems and VFS: ZFS/UFS vs ext4/btrfs

**FreeBSD:** ZFS (OpenZFS): checksums on all data with self-repair from redundancy, pooled storage, ARC/L2ARC/ZIL. UFS with soft-updates journaling. VFS built on vnodes with use, hold and write counts; vnodes are recycled across file systems.

**Linux:** ext4 (mature default) and btrfs (RAID5/6 still 'unstable'). ZFS is not in mainline. VFS with a dentry cache and RCU-walk lockless path lookup, falling back to REF-walk.

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | ZFS end-to-end checksums and self-healing in base | ZFS is CDDL: FreeBSD allows it only in modules, not the static kernel |
| Linux | RCU-walk path lookup avoids locks on the common path | btrfs RAID56 unstable (status page, 2026-10-16); ZFS stays out of mainline over licensing (Torvalds, 2020) |

**Magia pick (Inference):** Linux-style dentry cache with RCU-walk path lookup once Magia is SMP; when a disk file system is added, design it around ZFS-style checksums on every block, verified on read and repaired from redundancy. Keep ramfs/devfs for now.

**Why:** Path lookup is the hottest VFS path and RCU-walk is documented in depth; end-to-end checksums are the main correctness gain ZFS shows.

**Fit with the Magia plan:** Plan has VFS + ramfs + devfs + pipes; no disk file system yet. **Confidence:** medium-low.

| Claim | Label | Sources |
|---|---|---|
| ZFS checksums all data and self-repairs from redundancy; pooled storage; ARC, L2ARC, ZIL; development moved to OpenZFS. | Verified | S44 |
| UFS soft updates journaling (tunefs -j) reduces fsck time. | Verified | S45 |
| btrfs RAID56 is marked 'unstable'. | Verified | S47 |
| Torvalds (2020-01-06): he cannot merge ZFS without Oracle's legal sign-off and advises 'Don't use ZFS.' (his opinion, not a ruling). | Reported | S51 |
| CDDL is approved in FreeBSD for DTrace and ZFS; GPL/CDDL code must not be compiled into the static GENERIC kernel. | Verified | S81 |
| Linux VFS uses a dentry cache; RCU-walk lockless lookup with REF-walk fallback. | Verified | S48, S49 |
| FreeBSD vnodes carry usecount, holdcnt and writecount and are recycled across file systems. | Verified | S50 |
| ext4 documentation and options (mature in-tree file system). | Verified | S46 |
| FreeBSD's own lockless path-lookup work was not checked. | Unknown | - |

<a id="9-memory"></a>
## 9. Memory management: FreeBSD VM/UMA vs Linux SLUB/folios/MGLRU

**FreeBSD:** VM objects with shadow objects for copy-on-write, unified with the buffer cache. UMA zone allocator with per-CPU caches and NUMA policies. Reservation-based transparent superpages (since 7.2 on amd64/i386), enabled by default in the current amd64 pmap.

**Linux:** SLUB is the slab allocator (SLAB was removed in 6.8). Folios: a type for a page that is never a tail page, used to handle multi-page units cleanly. MGLRU: generation-based page reclaim. Transparent huge pages with always/madvise modes, khugepaged and multi-size THP.

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | Superpages without a background collapse daemon: reservations promote pages in place (Inference from the design); reservation superpages gave gains 'often exceeding 30%' (2002) | VM complexity is driven by performance (Dillon) |
| Linux | MGLRU at Google: 40% less kswapd CPU, 85% fewer low-memory kills (p75), 18% better app launch; folios: clearer types; 7% faster kernel builds reported from related page-cache work | Folio conversion is a multi-year retrofit (Inference) |

**Magia pick (Inference):** Keep the plan's buddy allocator, struct page array and slab zones; give zones per-CPU caches (UMA/SLUB style) when SMP arrives; use a folio-like multi-page type in the page cache from day one; FreeBSD-style reservation superpages for anonymous memory; generation-based (MGLRU-like) reclaim later.

**Why:** Folio-style typing is cheap in a new kernel and expensive to retrofit; reservations get superpages without a scanning daemon; MGLRU has the best measured reclaim data.

**Fit with the Magia plan:** Plan has buddy + struct page + slab zones and a single address space for now. **Confidence:** medium.

| Claim | Label | Sources |
|---|---|---|
| FreeBSD VM: vm_object layering with shadow objects for copy-on-write, unified with the buffer cache. | Verified | S52 |
| UMA zones: per-CPU caches 'with linear scalability on SMP', NUMA policies, slabs via kegs. | Verified | S53 |
| Reservation-based superpages: benefits 'often exceeding 30%' (OSDI 2002, FreeBSD/Alpha). | Verified | S54 |
| FreeBSD 7.2 (2009) added transparent superpages for amd64/i386 application memory. | Verified | S55 |
| Current amd64 pmap sets pg_ps_enabled = 1. | Verified | S56 |
| SLAB allocator removed in Linux 6.8 (2024-03-10). | Verified | S32 |
| MGLRU: 40% less kswapd CPU, 85% fewer low-memory kills at p75, 18% app-launch improvement; rolled out to tens of millions of ChromeOS users (Google's numbers). | Reported | S58, S57 |
| A folio is a page guaranteed not to be a tail page; a commenter (M. Wilcox) reported 7% faster kernel compiles from earlier THP page-cache work. | Reported | S59 |
| Linux THP supports always/madvise/never, khugepaged and multi-size THP (mTHP). | Verified | S109 |

<a id="10-networking"></a>
## 10. Networking: netgraph, VNET, pf vs nftables, XDP/eBPF

**FreeBSD:** netgraph: graphs of nodes and hooks that supplement the stack. VNET: several isolated network stacks in one kernel, attached to jails. pf (from OpenBSD 4.5, reworked for multi-CPU) alongside IPFW and IPFILTER. netmap fast packet I/O in base.

**Linux:** nftables: an in-kernel virtual machine for rules, with sets, maps and incremental updates, replacing protocol-specific iptables code. XDP: eBPF programs at the driver hook, checked by the eBPF verifier.

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | VNET makes per-container stacks a kernel feature (2003 design); netmap: 14.88 Mpps on one 900 MHz core (2012) | Three firewalls in base, each with its own syntax |
| Linux | XDP up to 24 Mpps per core while keeping the kernel's security boundary (2018); nftables: new protocols often need no new kernel code | XDP peak below DPDK (2018); unprivileged BPF can be switched off, and a build option makes that the default |

**Magia pick (Inference):** VNET-style stack instances from the start (one per jail); one packet filter built on a small rule VM with sets and maps (nftables idea) instead of several firewalls; an XDP-like early hook later that shares the verifier with tracing.

**Why:** Per-jail stacks fit the container model; one VM plus one verifier is less code than several filter engines (Inference).

**Fit with the Magia plan:** Not yet in the plan (no network stack yet). **Confidence:** medium-low.

| Claim | Label | Sources |
|---|---|---|
| netgraph: nodes and hooks in arbitrary graphs that 'supplement rather than replace' the stack; origin Whistle Communications. | Verified | S60 |
| Clonable network stack: multiple independent stacks in one kernel (2003); jails take a vnet parameter. | Verified | S61, S8 |
| FreeBSD pf derives from OpenBSD 4.5, 'heavily modified' for multi-CPU; base ships PF, IPFW and IPFILTER. | Verified | S62, S63 |
| nftables: in-kernel VM, a new protocol 'will often not require a new kernel'; incremental rule updates; sets, maps, concatenations. | Verified | S64, S65 |
| XDP: up to 24 Mpps per core, below DPDK peak, keeps the kernel security boundary (2018). | Verified | S66 |
| netmap: 14.88 Mpps on a 900 MHz core, 20x faster than conventional APIs (2012); in FreeBSD base and also on Linux. | Verified | S67, S68 |
| eBPF verifier: rejects unbounded loops via DAG check, simulates paths, prevents kernel-address leaks to unprivileged users. | Verified | S69 |
| unprivileged_bpf_disabled; BPF_UNPRIV_DEFAULT_OFF makes the default 2 (disabled). | Verified | S23 |

<a id="11-tracing"></a>
## 11. Tracing: DTrace vs eBPF / ftrace

**FreeBSD:** DTrace: providers expose probes that cost nothing when disabled; D scripts run with safety guarantees. FreeBSD has full kernel DTrace (userland is experimental) and, since 14.0, kinst (probe any instruction in a kernel function).

**Linux:** ftrace: function tracer and latency tracers; dynamic ftrace turns mcount/fentry call sites into NOPs at boot and patches them back when enabled. eBPF programs, checked by the verifier, attach to probes; bpftrace offers a DTrace-like language. Oracle maintains a Linux DTrace built on BPF.

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | 'Zero probe effect' when disabled and a safety-first design (2004); a mature language (D) and probe naming | Userland DTrace on FreeBSD is experimental |
| Linux | One verified bytecode serves tracing, networking and scheduling (sched_ext); ftrace is near-zero cost when off | The verifier is a large, complex component (Inference) |

**Magia pick (Inference):** Static probe points (DTrace SDT style) registered through linker sets now; NOP-patched function-entry hooks (ftrace style) later; one small verified bytecode VM for probe actions, shared with the packet filter.

**Why:** Static probes are cheap to add while the code is young; a single verified VM serves several subsystems, as eBPF shows.

**Fit with the Magia plan:** Plan already uses linker sets; tracing not yet planned. **Confidence:** medium.

| Claim | Label | Sources |
|---|---|---|
| DTrace: 'zero probe effect' when disabled, designed to be 'absolutely safe', about 30,000 kernel probes on small systems, D language (2004). | Verified | S70 |
| FreeBSD: full kernel DTrace, experimental userland DTrace. | Verified | S71 |
| kinst provider (any instruction in a kernel function) since FreeBSD 14.0. | Verified | S72 |
| Dynamic ftrace converts mcount/fentry call sites to NOPs at boot and patches them when tracing is enabled. | Verified | S73 |
| Oracle's dtrace-utils is the 'official Linux port of the DTrace tracing tool' and needs a BPF toolchain. | Verified | S74 |
| bpftrace is 'inspired by awk, C, and predecessor tracers such as DTrace and SystemTap'. | Verified | S75 |

<a id="12-abi"></a>
## 12. Syscall ABI stability and documentation

**FreeBSD:** One master file (syscalls.master) generates the syscall tables; COMPAT4..COMPAT15 entries compiled under COMPAT_FREEBSDx options keep old binaries running; a CAPENABLED flag per syscall. Stable branches must keep ABI and API compatibility; the kernel's own binary interface (KBI) is defined; one kernel can serve several userland ABIs (FreeBSD amd64 and i386, Linux x86_64 and i386). Syscall man pages live in the source tree.

**Linux:** 'We don't cause regressions' is the first rule: userland interfaces (syscalls, procfs, sysfs) must keep working; in-kernel APIs are deliberately unstable. Documentation/ABI marks interfaces stable/testing/obsolete/removed. Syscall man pages are a separate project.

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | Single source of truth for the syscall table; explicit compatibility shims per old release; docs maintained next to the code (219 section-2 pages in lib/libsys) | Compatibility promise is per stable branch; old ABIs need compile-time COMPAT options |
| Linux | Strongest userland promise: applications keep working across releases; freedom to rework internal APIs (USB reworked at least three times) | No stable module API: out-of-tree drivers break; syscall docs live outside the kernel tree |

**Magia pick (Inference):** One master syscall table that generates dispatch, typed argument validation, capability flags and a doc stub; man pages in the Magia tree; a per-ABI personality table so a compatibility ABI can be added later; adopt Linux's 'never break userspace' promise once Magia has users; keep internal APIs unstable.

**Why:** FreeBSD's generator removes drift between table, checks and docs; Linux's rule is the clearest user promise but costs more once made, so make it when there are users.

**Fit with the Magia plan:** Plan has a 'syscall table with typed argument validation' and Linux-style negative errno returns. **Confidence:** high.

| Claim | Label | Sources |
|---|---|---|
| 'We don't cause regressions' is the first rule of Linux kernel development; it covers procfs, sysfs and other userland interfaces but not kernel-internal interfaces such as the module API. | Verified | S76 |
| Linux has no stable in-kernel API or binary interface by design; the syscall interface 'is very stable over time, and will not break'; in-kernel USB interfaces were reworked at least three times. | Verified | S77 |
| Documentation/ABI stable interfaces are guaranteed at least 2 years; syscalls are expected never to change. | Verified | S39 |
| The Linux man-pages project documents the kernel and C library interfaces as a separate project. | Verified | S84 |
| syscalls.master generates the sysent tables and defines COMPAT4..COMPAT15 entry types and the CAPENABLED flag. | Verified | S82 |
| FreeBSD-STABLE 'must maintain ABI and API compatibility with earlier versions of that branch'; the amd64 kernel supports FreeBSD amd64/i386 and Linux x86_64/i386 userland ABIs. | Verified | S83 |
| 219 section-2 syscall man pages are in freebsd-src lib/libsys (my count, 2026-11-20). | Verified | S85 |

<a id="13-license"></a>
## 13. License: BSD-2-Clause vs GPLv2 and the effect on reuse

**FreeBSD:** BSD-2-Clause for the compilation and for new code; the static GENERIC kernel must be BSD-licensed or similar; GPL, APSL and CDDL code is allowed only in loadable modules (CDDL approved for DTrace and ZFS).

**Linux:** GPL-2.0-only with the syscall note: programs that only use syscalls are not derived works. Individual files may be GPL-compatible or dual-licensed (e.g. GPL plus BSD or MIT). Every file carries an SPDX identifier.

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | Permissive: code can be reused in almost any project with notices kept | No obligation for downstream users to share changes (Inference) |
| Linux | Copyleft keeps derived kernels open (Inference); syscall note keeps user programs free of GPL obligations | GPL-2.0-only code cannot be copied into a BSD-licensed kernel |

**Magia pick (Inference):** Keep Magia's BSD-2-Clause licence, add an SPDX line to every file (both projects use SPDX), and keep the documented clean-room rule (ideas only, no copied code). If code is ever imported, only BSD/MIT-licensed code or the permissive side of dual-licensed Linux files is compatible.

**Why:** BSD-2 matches FreeBSD's policy and keeps every FreeBSD idea and permissively licensed driver reusable; GPL-only Linux code would force a licence change. Legal reading, not legal advice.

**Fit with the Magia plan:** Magia's LICENSE is BSD 2-Clause; the plan forbids copying code from Linux or FreeBSD. **Confidence:** high (facts); legal reading is Inference.

| Claim | Label | Sources |
|---|---|---|
| Linux is GPL-2.0-only with an explicit syscall exception; files may be dual-licensed with BSD or MIT; SPDX identifiers are required. | Verified | S78 |
| Syscall note: user programs using kernel services by normal system calls are not 'derived work'; only GPL v2 is valid. | Verified | S79 |
| FreeBSD's compilation is under BSD-2 terms (Copyright 1992-2026 The FreeBSD Project). | Verified | S80 |
| New FreeBSD code should use BSD-2-Clause; 'GPL, APSL, CDDL, etc, licensed software must not be compiled into the static GENERIC kernel'; allowed in pre-compiled modules; CDDL approved for DTrace and ZFS. | Verified | S81 |
| Linux iwlwifi and rtw89 are 'GPL-2.0 OR BSD-3-Clause'; i915 and amdgpu carry MIT permission text; nvme host core is 'GPL-2.0'. | Verified | S95 |
| Magia is BSD 2-Clause and its plan forbids copying code from Linux or FreeBSD. | Verified | L1 |
| A BSD-2 kernel may import BSD/MIT code and the permissive side of dual-licensed files, but not GPL-2.0-only code without relicensing (not legal advice). | Inference | S78, S81 |

<a id="14-drivers"></a>
## 14. Driver breadth

**FreeBSD:** Native drivers in sys/dev plus Linux drivers run through the LinuxKPI compatibility layer: Wi-Fi (iwlwifi, rtw88, rtw89; mt76 in progress) in sys/contrib/dev, and AMD/Intel GPU drivers ported one Linux version at a time in drm-kmod (ports). The FreeBSD Foundation funds a laptop-support project.

**Linux:** Drivers are kept in-tree and are fixed together with internal API changes; drivers/ is about 70% of the source tree by bytes.

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | LinuxKPI reuses permissively licensed Linux drivers 'mostly unmodified' | Lag and gaps: DRM drivers at Linux 6.12 LTS for FreeBSD 15.1 (Q2 2026); ath11k/ath12k TODO and brcmfmac missing shims (Q1 2026) |
| Linux | Largest driver set; in-tree drivers stay working when APIs change | Out-of-tree drivers get no stable API |

**Magia pick (Inference):** Do not chase breadth: target QEMU's virtual devices first (virtio), then a few common real devices; keep drivers in-tree with an unstable internal API; shape the driver interfaces so a LinuxKPI-like shim could host permissively licensed Linux drivers later, if the no-copy rule is ever relaxed.

**Why:** Even FreeBSD relies on Linux for GPU and Wi-Fi drivers; a hobby kernel cannot close that gap by writing drivers alone.

**Fit with the Magia plan:** Plan covers serial, VGA, PIC and PIT only. **Confidence:** medium.

| Claim | Label | Sources |
|---|---|---|
| Linux master: drivers/ = 38,199 files, 1,165 MB, 69.5% of 1,676 MB of source bytes. FreeBSD main sys/: 548 MB, of which sys/dev 190 MB (34.6%) and sys/contrib/dev 98 MB (17.9%, incl. iwlwifi, rtw88, rtw89, mediatek, athk). Bytes, not device counts (my count, 2026-11-20). | Verified | S94 |
| FreeBSD uses 'permissively licensed Linux wireless drivers, mostly unmodified' via LinuxKPI, updated to Linux v6.19; iwlwifi, rtw88, rtw89 ship; mt76 in progress; ath11k/ath12k TODO; brcmfmac lacks cfg80211 shims (Q1 2026). | Verified | S92 |
| drm-kmod: 'The DRM drivers ported from Linux to FreeBSD using linuxkpi'. | Verified | S93 |
| Q2 2026: AMD and Intel DRM drivers ported one Linux version at a time; Linux 6.12 LTS port finished for FreeBSD 15.1; Foundation funds the Laptop Support and Usability project. | Verified | S110 |
| In-tree Linux drivers are fixed when internal interfaces change. | Verified | S77 |

<a id="15-mitigations"></a>
## 15. Security mitigations: KASLR, SMEP/SMAP, CFI, stack protector, W^X

**FreeBSD:** Userland ASLR, PIE, W^X policy and PROT_MAX; RELRO; stack protector (kernel built with -fstack-protector and one global canary on amd64); SMAP and SMEP used automatically on amd64; per-vulnerability hardware mitigation sysctls. No KASLR; kernel CFI not committed.

**Linux:** Kernel text and read-only data never writable (STRICT_KERNEL_RWX since 4.11), __ro_after_init, SMEP/SMAP, stack protector (strong), KASLR for kernel and modules, structure-layout randomisation, zero-initialised stacks, hardened usercopy, IBT and kCFI/FineIBT, page-table isolation, kernel-stack erasing (KSPP list).

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | Simple, documented per-process controls (proccontrol, sysctl) | No KASLR (bug report, 2026-05-23); kCFI review accepted 2026-06-03 but not in main as of 2026-11-20 |
| Linux | Broadest kernel self-protection set, with a maintained recommended configuration | Many options are build-time choices; defaults vary by distribution (Inference) |

**Magia pick (Inference):** Linux/KSPP as the baseline. Magia's plan already has NX/WP/SMEP/SMAP, W^X kernel mappings and stack canaries at thread-stack bottoms. Add next: compiler stack protector (strong) and zero-initialised stack variables for the C code, guard pages for kernel stacks, hardened copyin/copyout, Clang kCFI plus IBT for C; KASLR once the kernel can relocate (it is linked at a fixed higher-half address).

**Why:** Hardening is cheapest before code grows; Linux's list is documented and measured in production; FreeBSD lacks KASLR and kernel CFI.

**Fit with the Magia plan:** Partly in the plan (NX/WP/SMEP/SMAP, W^X, stack canaries, SMAP-aware copyin/copyout). **Confidence:** high (facts); pick is Inference.

| Claim | Label | Sources |
|---|---|---|
| FreeBSD mitigations(7) lists ASLR/PIE, W^X (allow_wx), PROT_MAX, RELRO, SSP (kernel -fstack-protector, single global canary except aarch64 option), FORTIFY_SOURCE, SMAP/SMEP automatic on amd64, Capsicum; no KASLR or kernel CFI entry. | Verified | S86 |
| Bug 295522 (2026-05-23) states 'FreeBSD does not implement KASLR'. | Reported | S89 |
| amd64 FreeBSD kernels can load anywhere in the lower 4 GB physically since 2025-06 (relocation, not randomisation). | Verified | S90 |
| FreeBSD kCFI review D46193 was accepted on 2026-06-03, but main has no KCFI build option and commit search finds no 'kcfi' commit (2026-11-20). | Inference | S91 |
| Linux self-protection: executable code and read-only data must not be writable; __ro_after_init; SMEP/SMAP; stack canary; KASLR of kernel and module base; structure layout randomisation; memory initialisation. | Verified | S87 |
| KSPP recommends STRICT_KERNEL_RWX (since 4.11), STACKPROTECTOR_STRONG, HARDENED_USERCOPY, INIT_STACK_ALL_ZERO, RANDOMIZE_BASE, X86_KERNEL_IBT, CFI_CLANG, PAGE_TABLE_ISOLATION, RANDSTRUCT_FULL, ZERO_CALL_USED_REGS, KSTACK_ERASE (since 6.17). | Verified | S88 |
| Magia plan: NX/WP/SMEP/SMAP, W^X layout, stack canaries at stack bottoms, SMAP-aware copyin/copyout; kernel linked at 0xffffffff80000000. | Verified | L1 |

<a id="16-rust"></a>
## 16. Rust in each kernel: where it is used and why

**FreeBSD:** No Rust in the kernel tree. A 2025 proof of concept builds optional Rust userland programs ('We do NOT currently support Rust in the kernel'). Out of tree, a Rust kernel programming interface (KPI) with a virtio-sound driver and Apple M2 drivers is in development (x86-64 and aarch64 only; interfaces not yet stable). The 2024 debate stalled on build cost and missing build integration.

**Linux:** Rust support merged in 6.1 as an experiment, declared concluded at the 2025 Maintainers Summit ('Rust is here to stay'). Mainline Rust users include the Android Binder driver (6.18), parts of the Nova NVIDIA driver, the Tyr GPU driver, rnull, two PHY drivers and the DRM panic QR generator; Android 16 ships Rust ashmem. Drivers must use safe subsystem abstractions, not raw C bindings. The first CVE in Rust kernel code (CVE-2025-68260, Binder) was a race in an unsafe block.

| | Strengths | Weaknesses |
|---|---|---|
| FreeBSD | Rust KPI avoids external crates and allocating std parts, aiming for C-like stability | No in-tree kernel Rust; 2024 estimate that Rust in base would 'double our build times' |
| Linux | Production use on millions of Android devices; maintainers report Rust drivers 'far safer'; 41% of C null_blk fixes were memory-safety fixes | Mixed-language toolchain cost; not all architectures/configurations work; unsafe blocks still produce CVEs |

**Magia pick (Inference):** Keep the plan's split: C for boot, CPU, MMU and context switch; a no_std Rust crate with zero external dependencies for policy subsystems. Confine unsafe code and FFI to one module that exposes safe abstractions (Linux's rule), require a written safety argument for every unsafe block, and pin the toolchain.

**Why:** The reason both projects give for Rust is memory safety, not speed; Linux's experience shows the gain comes from safe abstractions and that remaining bugs cluster in unsafe code.

**Fit with the Magia plan:** Matches the plan (rust/ crate, stable toolchain, x86_64-unknown-none, 'ffi + kenv'). **Confidence:** medium.

| Claim | Label | Sources |
|---|---|---|
| Rust support was merged in v6.1 as an experiment; at the 2025 Maintainers Summit the experiment was deemed concluded: 'Rust is here to stay'; not everything works for every configuration, architecture or toolchain. | Verified | S97 |
| Nova pieces merged; Binder merged for 6.18; Android 16 on 6.12 ships Rust ashmem; Rust code grew about fivefold in a year; DRM maintainer: 'about a year away' from requiring Rust for new drivers; Kroah-Hartman: Rust drivers 'proving to be far safer'. | Reported | S96 |
| Rust users in mainline: AMCC QT2025 PHY, Android Binder, ASIX PHY, DRM panic QR generator, Nova, rnull, Tyr; outside mainline: ashmem, Apple AGX, NVMe, PuzzleFS. Each subsystem decides whether to take Rust. | Verified | S98 |
| Drivers should not use C bindings directly; subsystems provide safe abstractions. | Verified | S99 |
| CVE-2025-68260 (published 2025-12-16): race condition in an unsafe list removal in Rust Binder. | Verified | S100 |
| 41% of fixes merged for the C null_blk driver were memory-safety fixes. | Reported | S102 |
| Android: memory-safety share of vulnerabilities fell from 76% to 24% over six years as new code moved to memory-safe languages (mostly userland code). | Reported | S105 |
| FreeBSD optional Rust (May 2025) covers userland only: 'We do NOT currently support Rust in the kernel.' | Verified | S106 |
| 2024: Rust in FreeBSD base would 'double our build times'; Losh: status quo cannot change until someone does build integration. | Verified | S107 |
| Out-of-tree FreeBSD Rust KPI: virtio sound driver, Apple M2 drivers; x86-64 and aarch64 only; KPIs 'not very stable'; no dependency on the wider Rust ecosystem; non-allocating std subset (2026-01-10). | Verified | S108 |
| Magia plan: C core plus a no_std Rust staticlib with zero dependencies; toolchain stable, target x86_64-unknown-none. | Verified | L1 |

<a id="17-rust_perf"></a>
## 17. C vs Rust performance in kernel code

**FreeBSD:** No FreeBSD kernel measurement found (Unknown).

**Linux:** Three sources measure Rust kernel drivers against C: a peer-reviewed study (2024), the Rust-for-Linux maintainers' own benchmarks (2025 rebases), and a master's thesis (2025).

| | Strengths | Weaknesses |
|---|---|---|
| Linux | Mature, feature-matched Rust drivers are within about +/-3% (NVMe) and -5% to +15% (null block) of C in maintainers' benchmarks | 2024 study: up to 61% slower or 67% faster by configuration; e1000 11x slower ping latency (missing features); binaries 1.2x-2.4x larger; thesis: GPIO module 8-14% slower and 28-46% more energy |

**Magia pick (Inference):** Speed is not a reason against Rust in Magia. Watch the known costs: binary size, bounds checks in hot loops, coarse locks and emulated bit fields. Magia's single Giant lock is fine for uniprocessor but should become finer-grained locks before SMP.

**Why:** Where the Rust and C drivers implement the same features, the measured gap is small; the large gaps traced to missing features or coarse locking.

**Fit with the Magia plan:** Plan: uniprocessor, one Giant lock around Rust subsystem state. **Confidence:** medium.

| Claim | Label | Sources |
|---|---|---|
| ATC 2024: Rust drivers 'on par' with C 'within a 20% gap'; e1000 11x slower ping latency (lacks features such as prefetch); Binder about 10% latency gap; NVMe/null block from 61% degradation to 67% improvement by setting; binaries 1.2x (binder) to 2.4x (gpio) larger; causes: coarse locks, bounds checks, emulated bit fields, pointer-heavy ownership. | Verified | S103 |
| Rust NVMe driver (v6.15 rebase): (R-C)/C between about -3% and +3% for random reads on one core (Ryzen 5 7600, Samsung 990 Pro); the driver is 'not currently suitable for general use'. Values read from the plot. | Reported | S101 |
| rnull (v6.19-rc5): IOPS difference vs C mostly between -5% and +15% across block sizes and queue depths; authors warn earlier results may mislead because distributions were not normal. Values read from the plot. | Reported | S102 |
| Thesis (Raspberry Pi 4 GPIO module): Rust +8%, +11%, +14.3% execution time and +46.15%, +27.68%, +33.86% energy across three variants. | Reported | S104 |
| Magia plan: uniprocessor with one Giant IRQ-disabling lock plus a scheduler lock. | Verified | L1 |

## Conflicts between sources

| Topic | Source A (date) | Source B (date) | Which I weight more, and why |
|---|---|---|---|
| First CVE in Rust kernel code | LWN, 2025-12-13 [S96]: Kroah-Hartman said no CVE had yet been issued for Rust code. | NVD, 2025-12-16 [S100]: CVE-2025-68260 published for Rust Binder. | Both were true on their dates; NVD is newer and primary, so the current fact is: one CVE exists, in an unsafe block. |
| How fast Rust kernel drivers are versus C | ATC 2024 [S103]: usually within 20%, but -61% to +67% by setting and e1000 11x slower. | Rust-for-Linux maintainers, 2025 rebases [S101, S102]: NVMe within about +/-3%, rnull -5% to +15%. Thesis 2025 [S104]: GPIO 8-14% slower. | I weight the maintainers' newer, feature-matched benchmarks most for 'can Rust match C' (they have confidence intervals), while noting they are the authors' own numbers. ATC 2024 is peer-reviewed but measured older drivers with missing features; the thesis is one small driver. |
| Capsicum's 2010 comparison with Linux | Capsicum paper, 2010-08 [S3]: Linux Chromium sandbox 11,301 lines vs about 100 with Capsicum. | seccomp filter mode arrived in Linux 3.5 [S5], after the paper; Landlock in 5.13 [S6]. | Treat the line counts as historical; they compare with seccomp strict mode, not today's seccomp-bpf or Landlock. |
| io_uring: speed vs safety | Axboe, 2019 [S18]: rings give about 2.8x the IOPS of Linux aio (author's benchmark). | Google, 2023-06-14 [S22] and Linux sysctl docs [S23]: io_uring was the top exploit target and can be disabled. | Not a contradiction but a trade-off; both are primary for their own claims. The Magia pick keeps rings but narrows them. |
| ULE vs Linux scheduler | Bouron et al., 2018-07 [S26]: no overall winner between ULE and CFS. | Linux replaced CFS with EEVDF in 6.6, 2023-10-29 [S30]. | The only head-to-head study predates EEVDF; no ULE-vs-EEVDF comparison was found, so the current answer is Unknown. |
| KASLR in FreeBSD | Commit 2025-06-22 [S90]: amd64 kernels are now physically relocatable. | Bug 295522, 2026-05-23 [S89] and mitigations(7) [S86]: no KASLR. | Relocation is a prerequisite, not randomisation; I weight the 2026 bug report and the man page: FreeBSD has no KASLR. |

## What would change the answer

- Magia needs to run Linux binaries: FreeBSD's multi-ABI design (one kernel, several userland ABIs) moves from 'later' to 'core' [S83].
- The no-copy rule is relaxed or the licence becomes GPL-2.0: driver and file-system picks change, because Linux code could then be imported [S78, S81].
- Magia goes SMP: per-CPU run queues, RCU-walk path lookup, per-CPU allocator caches and finer locks become urgent rather than later [S49, S53, S103].
- A published ULE-vs-EEVDF benchmark appears: the scheduler pick could change [S26].
- FreeBSD commits kernel CFI or KASLR, or Linux DRM starts requiring Rust for new drivers (stated as about a year away in Dec 2025): the mitigation and Rust comparisons would need updating [S91, S96].
- The project prefers fewer languages: a C-only core changes the Rust row; the evidence then supports C with KSPP-style hardening [S88].

## Not verified or limited

- The 2018 ULE vs CFS paper: only the abstract page was read; the PDF refused automated download (HTTP 403).
- Rust-for-Linux benchmark values were read by eye from the SVG plots (rendered locally), not from raw data.
- FreeBSD kernel CFI 'not committed' is inferred from absence: no KCFI option in main's sys/conf files and no commit-search hit for 'kcfi' on 2026-11-20.
- No published ULE-vs-EEVDF comparison and no kqueue-vs-epoll benchmark were found or run.
- FreeBSD's own lockless path-lookup work was not checked.
- Driver breadth is measured in source bytes, not supported devices; generated register headers inflate Linux's count.
- Google's Android memory-safety data is mostly userland code; applying it to kernel code is inference.
- The LWN Rust session report relays statements made in a meeting (labelled Reported).
- ZFS licensing: Torvalds' view is an opinion, not a court ruling. All licence readings here are not legal advice.
- kernel.org docs, the nftables wiki and some FreeBSD docs show no page date; the HTTP Last-Modified date is given instead.
- Whether Clang kCFI can cover Magia's Rust code was not checked.

## Sources

Format: ID. Title. Publisher or venue. Published. Accessed. Link.

- **S1.** capsicum(4). FreeBSD Project, FreeBSD 15.1 manual. Published: 2026-01-23 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=capsicum&sektion=4&format=ascii>
- **S2.** cap_enter(2). FreeBSD Project, FreeBSD 15.1 manual. Published: 2023-03-09 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=cap_enter&sektion=2&format=ascii>
- **S3.** Watson, Anderson, Laurie, Kennaway: Capsicum: practical capabilities for UNIX. USENIX Security 2010. Published: 2010-08. Accessed: 2026-11-19. <https://www.usenix.org/legacy/event/sec10/tech/full_papers/Watson.pdf>
- **S4.** Seccomp BPF (SECure COMPuting with filters). kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/userspace-api/seccomp_filter.html>
- **S5.** seccomp(2). Linux man-pages 6.19 (man7.org). Published: 2026-06-05 (page date). Accessed: 2026-11-19. <https://man7.org/linux/man-pages/man2/seccomp.2.html>
- **S6.** Landlock: unprivileged access control. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/userspace-api/landlock.html>
- **S7.** Corbet: Capsicum for Linux. LWN.net. Published: 2014-07-02. Accessed: 2026-11-19. <https://lwn.net/Articles/604015/>
- **S8.** jail(8). FreeBSD Project, FreeBSD 15.1 manual. Published: 2025-11-13 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=jail&sektion=8&format=ascii>
- **S9.** FreeBSD Handbook, ch. 17 Jails and Containers. FreeBSD Documentation Portal. Published: no date shown (HTTP Last-Modified 2026-11-18). Accessed: 2026-11-19. <https://docs.freebsd.org/en/books/handbook/jails/>
- **S10.** rctl(8). FreeBSD Project, FreeBSD 15.1 manual. Published: 2018-02-26 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=rctl&sektion=8&format=ascii>
- **S11.** namespaces(7). Linux man-pages 6.19 (man7.org). Published: 2026-02-08 (page date). Accessed: 2026-11-19. <https://man7.org/linux/man-pages/man7/namespaces.7.html>
- **S12.** user_namespaces(7). Linux man-pages 6.19 (man7.org). Published: 2026-02-08 (page date). Accessed: 2026-11-19. <https://man7.org/linux/man-pages/man7/user_namespaces.7.html>
- **S13.** Control Group v2. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/admin-guide/cgroup-v2.html>
- **S14.** Restricted unprivileged user namespaces are coming to Ubuntu 23.10. Canonical, Ubuntu blog. Published: 2023-10-09. Accessed: 2026-11-19. <https://ubuntu.com/blog/ubuntu-23-10-restricted-unprivileged-user-namespaces>
- **S15.** kqueue(2). FreeBSD Project, FreeBSD 15.1 manual. Published: 2026-01-24 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=kqueue&sektion=2&format=ascii>
- **S16.** Lemon: Kqueue: A generic and scalable event notification facility. USENIX ATC 2001, FREENIX track. Published: 2001 (PDF Last-Modified 2001-05-01). Accessed: 2026-11-19. <https://people.freebsd.org/~jlemon/papers/kqueue.pdf>
- **S17.** epoll(7). Linux man-pages 6.19 (man7.org). Published: 2026-02-08 (page date). Accessed: 2026-11-19. <https://man7.org/linux/man-pages/man7/epoll.7.html>
- **S18.** Axboe: Efficient IO with io_uring. kernel.dk (author's paper). Published: 2019 (PDF dated 2019-10-15). Accessed: 2026-11-19. <https://kernel.dk/io_uring.pdf>
- **S20.** aio(4). FreeBSD Project, FreeBSD 15.1 manual. Published: 2021-01-02 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=aio&sektion=4&format=ascii>
- **S21.** aio_read(2). FreeBSD Project, FreeBSD 15.1 manual. Published: 2025-03-27 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=aio_read&sektion=2&format=ascii>
- **S22.** Koczka: Learnings from kCTF VRP's 42 Linux kernel exploits submissions. Google Online Security Blog. Published: 2023-06-14. Accessed: 2026-11-19. <https://security.googleblog.com/2023/06/learnings-from-kctf-vrps-42-linux.html>
- **S23.** Documentation for /proc/sys/kernel/. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/admin-guide/sysctl/kernel.html>
- **S24.** sched_ule(4). FreeBSD Project, FreeBSD 15.1 manual. Published: 2012-08-10 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=sched_ule&sektion=4&format=ascii>
- **S25.** Roberson: ULE: A Modern Scheduler for FreeBSD. USENIX BSDCon 2003. Published: 2003-09. Accessed: 2026-11-19. <https://www.usenix.org/legacy/event/bsdcon03/tech/full_papers/roberson/roberson.pdf>
- **S26.** Bouron et al.: The Battle of the Schedulers: FreeBSD ULE vs. Linux CFS (abstract page; PDF refused automated download). USENIX ATC 2018. Published: 2018-07. Accessed: 2026-11-19. <https://www.usenix.org/conference/atc18/presentation/bouron>
- **S27.** EEVDF Scheduler. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/scheduler/sched-eevdf.html>
- **S28.** Extensible Scheduler Class (sched_ext). kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/scheduler/sched-ext.html>
- **S29.** Corbet: An EEVDF CPU scheduler for Linux. LWN.net. Published: 2023-03-09. Accessed: 2026-11-19. <https://lwn.net/Articles/925371/>
- **S30.** Linux 6.6 (release summary). KernelNewbies. Published: release 2023-10-29. Accessed: 2026-11-19. <https://kernelnewbies.org/Linux_6.6>
- **S31.** Linux 6.12 (release summary). KernelNewbies. Published: release 2024-11-17. Accessed: 2026-11-19. <https://kernelnewbies.org/Linux_6.12>
- **S32.** Linux 6.8 (release summary). KernelNewbies. Published: release 2024-03-10. Accessed: 2026-11-19. <https://kernelnewbies.org/Linux_6.8>
- **S33.** tuning(7). FreeBSD Project, FreeBSD 15.1 manual. Published: 2026-04-09 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=tuning&sektion=7&format=ascii>
- **S34.** commit a3b5daf4242f: sys: enable both SCHED_ULE and SCHED_4BSD for some configs (stable/15). dev-commits-src-all mailing list. Published: 2026-02-11. Accessed: 2026-11-19. <https://lists.freebsd.org/archives/dev-commits-src-all/2026-February/068522.html>
- **S35.** sysctl(3). FreeBSD Project, FreeBSD 15.1 manual. Published: 2025-07-31 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=sysctl&sektion=3&format=ascii>
- **S36.** sysctl(9). FreeBSD Project, FreeBSD 15.1 manual. Published: 2025-09-28 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=sysctl&sektion=9&format=ascii>
- **S37.** sysfs - The filesystem for exporting kernel objects. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/filesystems/sysfs.html>
- **S38.** sysctl(2). Linux man-pages 6.19 (man7.org). Published: man-pages 6.19 (page date not recorded). Accessed: 2026-11-19. <https://man7.org/linux/man-pages/man2/sysctl.2.html>
- **S39.** Linux ABI description. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/admin-guide/abi.html>
- **S40.** geom(4). FreeBSD Project, FreeBSD 15.1 manual. Published: 2024-07-08 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=geom&sektion=4&format=ascii>
- **S41.** Device Mapper (index of targets). kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/admin-guide/device-mapper/index.html>
- **S42.** Multi-Queue Block IO Queueing Mechanism (blk-mq). kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/block/blk-mq.html>
- **S43.** Bjørling, Axboe, Nellans, Bonnet: Linux Block IO: Introducing Multi-queue SSD Access on Multi-core Systems. SYSTOR 2013. Published: 2013 (PDF dated 2013-05-21). Accessed: 2026-11-19. <https://kernel.dk/blk-mq.pdf>
- **S44.** FreeBSD Handbook, ch. 23 The Z File System (ZFS). FreeBSD Documentation Portal. Published: no date shown (HTTP Last-Modified 2026-11-18). Accessed: 2026-11-19. <https://docs.freebsd.org/en/books/handbook/zfs/>
- **S45.** tunefs(8). FreeBSD Project, FreeBSD 15.1 manual. Published: FreeBSD 15.1 (page date not recorded). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=tunefs&sektion=8&format=ascii>
- **S46.** ext4 General Information. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/admin-guide/ext4.html>
- **S47.** Status (feature stability table). BTRFS documentation. Published: no date shown (HTTP Last-Modified 2026-10-16). Accessed: 2026-11-19. <https://btrfs.readthedocs.io/en/latest/Status.html>
- **S48.** Overview of the Linux Virtual File System. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/filesystems/vfs.html>
- **S49.** Pathname lookup. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/filesystems/path-lookup.html>
- **S50.** vnode(9). FreeBSD Project, FreeBSD 15.1 manual. Published: 2025-07-15 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=vnode&sektion=9&format=ascii>
- **S51.** Torvalds: forum post on ZFS in the thread 'Nuances related to Spinlock implementation'. Real World Technologies forum. Published: 2020-01-06. Accessed: 2026-11-19. <https://www.realworldtech.com/forum/?threadid=189711&curpostid=189841>
- **S52.** Dillon: Design elements of the FreeBSD VM system. FreeBSD Documentation Portal (article). Published: c. 2000 (no exact date shown). Accessed: 2026-11-19. <https://docs.freebsd.org/en/articles/vm-design/>
- **S53.** uma(9). FreeBSD Project, FreeBSD 15.1 manual. Published: 2023-01-16 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=uma&sektion=9&format=ascii>
- **S54.** Navarro, Iyer, Druschel, Cox: Practical, transparent operating system support for superpages. OSDI 2002. Published: 2002-12. Accessed: 2026-11-19. <https://www.usenix.org/legacy/event/osdi02/tech/full_papers/navarro/navarro.pdf>
- **S55.** FreeBSD 7.2-RELEASE Release Notes. FreeBSD Project. Published: 2009-05. Accessed: 2026-11-19. <https://www.freebsd.org/releases/7.2R/relnotes/>
- **S56.** sys/amd64/amd64/pmap.c (main branch): pg_ps_enabled = 1. FreeBSD source (cgit). Published: main branch as fetched. Accessed: 2026-11-19. <https://cgit.freebsd.org/src/plain/sys/amd64/amd64/pmap.c>
- **S57.** Multi-Gen LRU (admin guide). kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/admin-guide/mm/multigen_lru.html>
- **S58.** Yu Zhao: [PATCH mm-unstable v15 00/14] Multi-Gen LRU Framework (cover letter). linux-mm via lore.kernel.org. Published: 2022-09-18. Accessed: 2026-11-19. <https://lore.kernel.org/all/20220918080010.2920238-1-yuzhao@google.com/>
- **S59.** Corbet: Clarifying memory management with page folios (incl. comment by M. Wilcox, 2021-03-19). LWN.net. Published: 2021-03-18. Accessed: 2026-11-19. <https://lwn.net/Articles/849538/>
- **S60.** netgraph(4). FreeBSD Project, FreeBSD 15.1 manual. Published: 2021-09-29 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=netgraph&sektion=4&manpath=FreeBSD+15.1-RELEASE&format=ascii>
- **S61.** Zec: Implementing a Clonable Network Stack in the FreeBSD Kernel. USENIX ATC 2003, FREENIX track. Published: 2003-06. Accessed: 2026-11-19. <https://www.usenix.org/legacy/event/usenix03/tech/freenix03/full_papers/zec/zec.pdf>
- **S62.** pf(4). FreeBSD Project, FreeBSD 15.1 manual. Published: 2025-07-02 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=pf&sektion=4&format=ascii>
- **S63.** FreeBSD Handbook, ch. 34 Firewalls. FreeBSD Documentation Portal. Published: no date shown (HTTP Last-Modified 2026-11-18). Accessed: 2026-11-19. <https://docs.freebsd.org/en/books/handbook/firewalls/>
- **S64.** What is nftables?. nftables wiki. Published: no date shown (HTTP Last-Modified 2026-08-20). Accessed: 2026-11-19. <https://wiki.nftables.org/wiki-nftables/index.php/What_is_nftables%3F>
- **S65.** Main differences with iptables. nftables wiki. Published: no date shown (HTTP Last-Modified 2026-08-20). Accessed: 2026-11-19. <https://wiki.nftables.org/wiki-nftables/index.php/Main_differences_with_iptables>
- **S66.** Høiland-Jørgensen et al.: The eXpress Data Path: Fast Programmable Packet Processing in the Operating System Kernel. CoNEXT 2018 (author PDF, xdp-project). Published: 2018-12. Accessed: 2026-11-19. <https://raw.githubusercontent.com/xdp-project/xdp-paper/master/xdp-the-express-data-path.pdf>
- **S67.** Rizzo: netmap: A Novel Framework for Fast Packet I/O (abstract page). USENIX ATC 2012. Published: 2012-06. Accessed: 2026-11-19. <https://www.usenix.org/conference/atc12/technical-sessions/presentation/rizzo>
- **S68.** netmap(4). FreeBSD Project, FreeBSD 15.1 manual. Published: 2024-10-10 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=netmap&sektion=4&format=ascii>
- **S69.** eBPF verifier. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/bpf/verifier.html>
- **S70.** Cantrill, Shapiro, Leventhal: Dynamic Instrumentation of Production Systems. USENIX ATC 2004. Published: 2004-06. Accessed: 2026-11-19. <https://www.usenix.org/legacy/event/usenix04/tech/general/full_papers/cantrill/cantrill.pdf>
- **S71.** FreeBSD Handbook, ch. 28 DTrace. FreeBSD Documentation Portal. Published: no date shown (HTTP Last-Modified 2026-11-18). Accessed: 2026-11-19. <https://docs.freebsd.org/en/books/handbook/dtrace/>
- **S72.** dtrace_kinst(4). FreeBSD Project, FreeBSD 15.1 manual. Published: 2025-07-16 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=dtrace_kinst&sektion=4&format=ascii>
- **S73.** ftrace - Function Tracer. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/trace/ftrace.html>
- **S74.** oracle/dtrace-utils README. GitHub (Oracle), devel branch. Published: no date shown. Accessed: 2026-11-19. <https://raw.githubusercontent.com/oracle/dtrace-utils/devel/README.md>
- **S75.** bpftrace README. GitHub (bpftrace), master branch. Published: no date shown. Accessed: 2026-11-19. <https://raw.githubusercontent.com/bpftrace/bpftrace/master/README.md>
- **S76.** Reporting regressions. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/admin-guide/reporting-regressions.html>
- **S77.** Kroah-Hartman: The Linux Kernel Driver Interface (stable-api-nonsense). kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (text from the 2.6 era; HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/process/stable-api-nonsense.html>
- **S78.** Linux kernel licensing rules. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/process/license-rules.html>
- **S79.** LICENSES/exceptions/Linux-syscall-note. torvalds/linux master (GitHub). Published: file in master as fetched. Accessed: 2026-11-19. <https://raw.githubusercontent.com/torvalds/linux/master/LICENSES/exceptions/Linux-syscall-note>
- **S80.** COPYRIGHT (Copyright (c) 1992-2026 The FreeBSD Project). freebsd-src main (GitHub mirror). Published: file in main as fetched. Accessed: 2026-11-19. <https://raw.githubusercontent.com/freebsd/freebsd-src/main/COPYRIGHT>
- **S81.** FreeBSD Licensing Policy. FreeBSD Documentation Portal. Published: no date shown (HTTP Last-Modified 2026-11-18). Accessed: 2026-11-19. <https://docs.freebsd.org/en/articles/license-guide/>
- **S82.** sys/kern/syscalls.master. freebsd-src main (GitHub mirror). Published: file in main as fetched. Accessed: 2026-11-19. <https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/kern/syscalls.master>
- **S83.** Committer's Guide. FreeBSD Documentation Portal. Published: no date shown (HTTP Last-Modified 2026-11-18). Accessed: 2026-11-20. <https://docs.freebsd.org/en/articles/committers-guide/>
- **S84.** The Linux man-pages project. kernel.org. Published: no date shown (HTTP Last-Modified 2026-07-10). Accessed: 2026-11-20. <https://www.kernel.org/doc/man-pages/>
- **S85.** lib/libsys directory listing (219 section-2 man pages). freebsd-src main via GitHub API. Published: main as fetched. Accessed: 2026-11-20. <https://github.com/freebsd/freebsd-src/tree/main/lib/libsys>
- **S86.** mitigations(7). FreeBSD Project, FreeBSD 15.1 manual. Published: 2025-01-29 (page date). Accessed: 2026-11-19. <https://man.freebsd.org/cgi/man.cgi?query=mitigations&sektion=7&format=ascii>
- **S87.** Kernel Self-Protection. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/security/self-protection.html>
- **S88.** Recommended Settings. Kernel Self Protection Project (KSPP). Published: no date shown (HTTP Last-Modified 2026-05-14). Accessed: 2026-11-19. <https://kspp.github.io/Recommended_Settings>
- **S89.** FreeBSD Bugzilla 295522: kcmp comparison results leak kernel object address ordering (informational - no KASLR). freebsd-bugs mailing list. Published: 2026-05-23. Accessed: 2026-11-20. <https://lists.freebsd.org/archives/freebsd-bugs/2026-May/034684.html>
- **S90.** commit 308659acbff3: libkvm/kvm_amd64: account for relocatable kernels. dev-commits-src-all mailing list. Published: 2025-06-22 (commit date). Accessed: 2026-11-20. <https://lists.freebsd.org/archives/dev-commits-src-all/2025-June/056536.html>
- **S91.** D46193: sys: implement Kernel CFI from clang (status Accepted, 2026-06-03); plus check of main sys/conf/kern.mk, sys/conf/options and GitHub commit search for 'kcfi'. FreeBSD Phabricator; freebsd-src main. Published: review accepted 2026-06-03. Accessed: 2026-11-20. <https://reviews.freebsd.org/D46193>
- **S92.** Zeeb: LinuxKPI 802.11 and Native Wireless Update. FreeBSD Status Report, Q1 2026. Published: 2026 Q1 report (Jan-Mar 2026). Accessed: 2026-11-20. <https://www.freebsd.org/status/report-2026-01-2026-03/lkpi-wireless/>
- **S93.** freebsd/drm-kmod README. GitHub (FreeBSD). Published: no date shown. Accessed: 2026-11-20. <https://github.com/freebsd/drm-kmod>
- **S94.** Own count of source bytes per directory via the GitHub git/trees API (Linux master tree 8dac6df92a2e; FreeBSD main tree a8fd8a5a365a). Research Desk measurement. Published: 2026-11-20. Accessed: 2026-11-20. <https://api.github.com/repos/torvalds/linux/git/trees/master>
- **S95.** Licence headers of Linux driver files (iwlwifi/iwl-drv.c, rtw89/core.c, i915/i915_driver.c, amdgpu/amdgpu_device.c, nvme/host/core.c). torvalds/linux master (GitHub). Published: files in master as fetched. Accessed: 2026-11-20. <https://github.com/torvalds/linux/tree/master/drivers>
- **S96.** Corbet: The state of the kernel Rust experiment. LWN.net. Published: 2025-12-13. Accessed: 2026-11-20. <https://lwn.net/Articles/1050174/>
- **S97.** Ojeda: [PATCH] rust: conclude the Rust experiment. LWN.net mailing-list mirror. Published: 2025-12-13. Accessed: 2026-11-20. <https://lwn.net/Articles/1050308/>
- **S98.** Rust kernel policy (and site navigation: Users in mainline / outside mainline). Rust for Linux project site. Published: no date shown. Accessed: 2026-11-19. <https://rust-for-linux.com/rust-kernel-policy>
- **S99.** General Information (Rust). kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-20. <https://docs.kernel.org/rust/general-information.html>
- **S100.** CVE-2025-68260: rust_binder: fix race condition on death_list. NVD (NIST), source kernel.org CNA. Published: 2025-12-16 (published). Accessed: 2026-11-20. <https://nvd.nist.gov/vuln/detail/CVE-2025-68260>
- **S101.** NVMe Driver (Rust; performance plots per rebase, read for v6.15). Rust for Linux project site. Published: no date shown (latest plot: v6.15 rebase). Accessed: 2026-11-20. <https://rust-for-linux.com/nvme-driver>
- **S102.** Null Block Driver (Rust rnull; performance plots, read for v6.19-rc5). Rust for Linux project site. Published: no date shown (latest plot: v6.19-rc5 rebase). Accessed: 2026-11-20. <https://rust-for-linux.com/null-block-driver>
- **S103.** Li, Guo, Yang, Wang, Xu: An Empirical Study of Rust-for-Linux: The Success, Dissatisfaction, and Compromise. USENIX ATC 2024. Published: 2024-07. Accessed: 2026-11-20. <https://www.usenix.org/system/files/atc24-li-hongyu.pdf>
- **S104.** Garber: Rust in the Linux Kernel: Analyzing Rust Implementations of Device Drivers (master's thesis). TU Wien (repositUm). Published: 2025. Accessed: 2026-11-20. <https://repositum.tuwien.at/handle/20.500.12708/219628>
- **S105.** Vander Stoep, Rebert: Eliminating Memory Safety Vulnerabilities at the Source. Google Online Security Blog. Published: 2024-09-25. Accessed: 2026-11-20. <https://security.googleblog.com/2024/09/eliminating-memory-safety-vulnerabilities-Android.html>
- **S106.** Status report on optional Rust in FreeBSD support (quotes Shawn Webb's report). LWN.net brief. Published: 2025-05-21. Accessed: 2026-11-20. <https://lwn.net/Articles/1022040/>
- **S107.** Losh: Re: The Case for Rust (in the base system) (quotes A. Somers, 2024-01-20). freebsd-hackers mailing list. Published: 2024-07-31. Accessed: 2026-11-20. <https://lists.freebsd.org/archives/freebsd-hackers/2024-July/003437.html>
- **S108.** Muñoz: Kernel Rust Support. FreeBSD Status Report, Q4 2025. Published: 2026-01-10 (last modified). Accessed: 2026-11-20. <https://www.freebsd.org/status/report-2025-10-2025-12/kernel_rust/>
- **S109.** Transparent Hugepage Support. kernel.org docs, Linux 7.4.0-rc3. Published: no date shown (HTTP Last-Modified 2026-11-15). Accessed: 2026-11-19. <https://docs.kernel.org/admin-guide/mm/transhuge.html>
- **S110.** FreeBSD Status Report, Second Quarter 2026 (Foundation projects; DRM drivers entry). FreeBSD Project. Published: 2026 Q2 report (Apr-Jun 2026). Accessed: 2026-11-20. <https://www.freebsd.org/status/report-2026-04-2026-06/>
- **L1.** Magia implementation plan (docs/plan.md) and LICENSE (BSD 2-Clause). this repository (Magia). Published: 2026-11-19 (file dates). Accessed: 2026-11-20. (docs/plan.md in this repository)
