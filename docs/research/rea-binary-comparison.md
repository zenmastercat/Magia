# REA binary comparison: Linux 7.3.4 vs FreeBSD 15.1 (amd64)

Tool: REA (morluto/rea, CLI `rea-agents` 6.9.x) run locally with Node.js 24.22.3, 2026-11-19. Raw JSON reports are next to this file.

Artifacts:
- Linux: this PC's kernel `linux-optimized-7.3.4` (NixOS). `vmlinux` extracted from bzImage (zstd payload at offset 21188, 63.9 MB ELF64). Module: `kvm.ko` (from `kvm.ko.xz`).
- FreeBSD: official `15.1-RELEASE/amd64/kernel.txz`, SHA-256 `49fea8ee…324313`, matches the release MANIFEST. Files `boot/kernel/kernel` and `boot/kernel/zfs.ko`.

## inspect-binary-hardening (kernel images)
| Property | Linux vmlinux | FreeBSD kernel |
|---|---|---|
| NX (PT_GNU_STACK) | unknown (header absent) | enabled |
| RELRO | disabled | partial |
| PIE | disabled | disabled |
| CET (GNU property note) | disabled | disabled |
| Symbol table | stripped | present (not stripped) |
| Stack protector / FORTIFY | unknown (no dynamic imports) | unknown |

Reading: these rows are written for user programs. Both kernels are fixed-address ELF executables, so PIE/RELRO say little; Linux's real mitigations (KASLR via relocations, IBT `endbr64`, SMEP/SMAP, CFI) are runtime/config features that this header-level check cannot see. Treat this table as "what the ELF headers declare", not as which kernel is safer.

## inspect-kernel-module
| | Linux kvm.ko | FreeBSD zfs.ko |
|---|---|---|
| Metadata | `.modinfo` key/values: license=GPL, intree=Y, depends=irqbypass, vermagic=`7.3.4-optimized SMP preempt mod_unload`, retpoline=Y | no `.modinfo`; metadata in `set_modmetadata_set` linker set |
| `struct module` section | present | absent (FreeBSD uses `set_modmetadata`) |
| Appended signature | **present** | absent |
| Registration | `__ksymtab*` exports | linker sets: `set_sysctl_set`, `set_sdt_*` (DTrace probes), `set_sysinit_set`, `set_modmetadata_set` |

## Ideas taken into Magia
1. **Linker sets (FreeBSD):** sysctl nodes, probes and init hooks register themselves at link time in a named ELF section; the kernel walks the section at boot. Magia uses this for ktest cases and init hooks.
2. **Signed modules with version magic (Linux):** loadable code carries a signature and a build fingerprint so a mismatched or tampered module is refused. On the Magia roadmap (Magia has no module loader yet).

Limits: REA reads headers and metadata only; it did not disassemble or run either kernel.
