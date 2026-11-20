#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Verify that embedding .ksyms in the second link pass did not move code.
a=$(nm -n "$1" | awk '$2 ~ /^[Tt]$/ { print $1, $3 }' | md5sum)
b=$(nm -n "$2" | awk '$2 ~ /^[Tt]$/ { print $1, $3 }' | md5sum)
if [ "$a" != "$b" ]; then
	echo "error: text symbols moved between link passes" >&2
	exit 1
fi
