#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 zenmastercat and Magia contributors
#
# fix-pie.sh <magia.elf>: check the kernel is a clean static PIE and mark it
# ET_DYN. GNU ld writes e_type = ET_EXEC for a PIE whose linker script puts
# it at a fixed non-zero base (0xffffffff80000000), and Limine only
# relocates and slides ET_DYN images. The checks make the patch safe:
#   - every dynamic relocation is R_X86_64_RELATIVE (no symbol lookups),
#   - no text relocations, no writable+executable segment,
#   - a PT_DYNAMIC segment exists (Limine finds the relocations there).
set -eu
elf=$1
bad=$(readelf -rW "$elf" | awk '/^[0-9a-f]+ +[0-9a-f]+ +R_X86_64_/ && $3 != "R_X86_64_RELATIVE"' | head -3)
if [ -n "$bad" ]; then
	echo "fix-pie: non-RELATIVE relocations in $elf:" >&2
	echo "$bad" >&2
	exit 1
fi
if readelf -dW "$elf" | grep -q TEXTREL; then
	echo "fix-pie: $elf has text relocations" >&2
	exit 1
fi
if readelf -lW "$elf" | awk '$1 == "LOAD" && $7 ~ /W/ && $7 ~ /E/' | grep -q .; then
	echo "fix-pie: $elf has a W+X segment" >&2
	exit 1
fi
readelf -lW "$elf" | grep -q '^ *DYNAMIC' || { echo "fix-pie: no PT_DYNAMIC in $elf" >&2; exit 1; }
# e_type is the little-endian u16 at offset 16: 3 = ET_DYN
printf '\003' | dd of="$elf" bs=1 seek=16 conv=notrunc status=none
readelf -h "$elf" | grep -q 'DYN' || { echo "fix-pie: patch failed" >&2; exit 1; }
