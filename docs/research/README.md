# Research notes behind Magia's design

Copies of the research notes cited in `docs/IDEAS.md`, `docs/LANGUAGES.md`,
`docs/ROADMAP.md` and `docs/SYSCALLS.md`, kept here so the links work.
Local machine paths were removed from them; the content is otherwise
unchanged.

| File | What it is |
|---|---|
| `kernel-pros.md`, `kernel-pros.json` | Research Desk, *Magia kernel design: what FreeBSD and Linux each do best* (2026-11-20): 17 subsystem rows, each claim labelled Verified/Reported/Inference with dated primary sources (S1-S110). |
| `rea-binary-comparison.md` | REA (binary analysis tool) header/metadata comparison of the Linux 7.3.4 and FreeBSD 15.1 kernel images and one module each; source of the linker-set and signed-module ideas. |
| `magia-kernel-hardening.yaml` | REA `inspect_binary_hardening` output for an earlier `build/magia.elf`. It predates the split of the boot segment into separate R+X and R+W program headers (security review F6); current builds have no RWX segment (`readelf -lW build/magia.elf`). |

These notes were written by other agents for this project; they describe
FreeBSD and Linux from public documentation and contain no code from either.
