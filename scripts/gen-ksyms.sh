#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 zenmastercat and Magia contributors
#
# gen-ksyms.sh: turn `nm -n` output (file or "-" for stdin) into an assembly
# file with a sorted table of text symbols, used by panic backtraces.
#   ksym_count: number of entries
#   ksym_table: { u64 addr; u64 name_off } [ksym_count], sorted by addr;
#               name_off is relative to ksym_strtab, so the table needs no
#               relocations and can stay read-only in the PIE kernel.
in="$1"
[ "$in" = "-" ] && in=/dev/stdin
awk '
BEGIN { n = 0 }
$2 ~ /^[Tt]$/ && $1 ~ /^ffffffff8/ && $3 !~ /^\.L/ {
	addr[n] = $1; name[n] = $3; n++
}
END {
	print "\t.section .ksyms, \"a\""
	print "\t.align 8"
	print "\t.global ksym_count, ksym_table, ksym_strtab"
	print "ksym_count:\t.quad " n
	print "ksym_table:"
	for (i = 0; i < n; i++)
		printf "\t.quad 0x%s, .Lks%d - ksym_strtab\n", addr[i], i
	print "ksym_strtab:"
	for (i = 0; i < n; i++) {
		s = name[i]; gsub(/["\\]/, "_", s)
		printf ".Lks%d:\t.asciz \"%s\"\n", i, s
	}
}' "$in"
