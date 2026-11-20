#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Print non-blank line counts per language.
count() { cat "$@" 2>/dev/null | grep -cv '^[[:space:]]*$'; }
c=$(count $(find kernel include -name '*.[ch]'))
asm=$(count $(find arch -name '*.S'))
rs=$(count $(find rust/src -name '*.rs'))
echo "C (kernel/, include/): $c"
echo "GNU asm (arch/):       $asm"
echo "Rust (rust/src/):      $rs"
