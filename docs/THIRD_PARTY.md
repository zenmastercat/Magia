# Third-party code in Magia

Magia is BSD-2-Clause. It contains or uses the following code from other
projects, all under permissive licences (BSD-2-Clause, BSD-3-Clause, ISC,
MIT, 0BSD or public domain). Adapted files keep their original copyright
header. Nothing here is GPL or CDDL; Linux is used for ideas only
(docs/IDEAS.md).

## In the source tree

| File in Magia | Origin | Licence | Changes |
|---|---|---|---|
| `include/limine.h` | Limine boot protocol header, `limine-protocol/include/limine.h` from the Limine 12.10.0 release tarball (Mintsuki and contributors) | 0BSD | none (verbatim) |
| `kernel/font/spleen8x16.h` | Spleen 8x16 font, OpenBSD `sys/dev/wsfont/spleen8x16.h` rev 1.7 (Frederic Cambus, 2018-2020); OpenBSD src commit 8e1a11b4 | BSD-2-Clause | none (verbatim); `kernel/fbcon.c` supplies the few `wsdisplay_font` declarations it expects |
| `include/magia/apicreg.h` | FreeBSD `sys/x86/include/apicreg.h` (Peter Wemm and Steve Passe, 1996), FreeBSD src commit 90bc40e2 | BSD-2-Clause (FreeBSD variant: clause 2 forbids using the developer's name for endorsement) | subset: register index enum and LAPIC, LVT, timer and I/O APIC field definitions verbatim; memory-layout structs dropped; Magia header guard and a note added below the original copyright |
| `kernel/crypto/chacha_private.h` | D. J. Bernstein's chacha-merged.c (version 20080118) as carried by OpenBSD `sys/crypto/chacha_private.h` rev 1.4, OpenBSD src commit 8e1a11b4 | public domain | one line: `#include <sys/systm.h>` replaced by `<magia/kernel.h>`; `kernel/rng.c` supplies `u_int` |

## Fetched at build time (not in git)

| What | Origin | Licence | How |
|---|---|---|---|
| Limine 12.10.0 binaries: `BOOTX64.EFI`, `limine-bios.sys`, the `limine` host tool (`limine.c`, `bios-install`) | `limine-binary.tar.gz` from the Limine GitHub release v12.10.0 | BSD-2-Clause; the parts built into the binaries are listed with their licences in the tarball's `3RDPARTY.md` | `Makefile` downloads it, checks SHA-256 `365ba878...e841e2`, builds the host tool; `make image` copies `LICENSE` and `3RDPARTY.md` next to the binaries on the ESP (`/boot/limine/`) |

Limine is a separate program (the bootloader); no Limine code is linked
into the kernel.
