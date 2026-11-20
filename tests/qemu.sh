#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 zenmastercat and Magia contributors
#
# qemu.sh <magia.img> <kernel command line> [extra QEMU args...]
#
# Boot the disk image in QEMU (q35, OVMF UEFI firmware). The image is
# opened with snapshot=on, so a run never writes to it. The Limine config
# for this run - and with it the kernel command line - is passed as an
# SMBIOS type 11 OEM string starting with "limine:config:", which Limine
# reads before looking for limine.conf on disk; one image serves every
# test (the ESP is found by its FAT label, so BIOS boots work too).
# Environment: QEMU, QEMU_ACCEL, QEMU_MEM, QEMU_SMP, QEMU_CPU, OVMF
# (path to OVMF.fd, or "bios" for SeaBIOS).
set -eu
IMG=$1
CMDLINE=$2
shift 2
: "${QEMU:=qemu-system-x86_64}" "${QEMU_ACCEL:=tcg}" "${QEMU_MEM:=8G}" "${QEMU_SMP:=4}"
: "${QEMU_CPU:=max}" "${OVMF:=${OVMF_FD:-}}"
fw="-bios $OVMF"
if [ "$OVMF" = bios ]; then
	fw= # QEMU default: SeaBIOS, Limine BIOS stage
elif [ -z "$OVMF" ] || [ ! -f "$OVMF" ]; then
	echo "qemu.sh: OVMF firmware not found (set OVMF or OVMF_FD; nix-shell does)" >&2
	exit 2
fi
cfg=$(mktemp "${TMPDIR:-/tmp}/magia-limine.XXXXXX")
trap 'rm -f "$cfg"' EXIT INT TERM
{
	printf 'limine:config:timeout: 0\nquiet: yes\n'
	printf '/Magia\n    protocol: limine\n    path: fslabel(MAGIA):/boot/magia.elf\n    kaslr: yes\n'
	printf '    cmdline: %s\n' "$CMDLINE"
} >"$cfg"
"$QEMU" -machine q35 -cpu "$QEMU_CPU" -accel "$QEMU_ACCEL" -m "$QEMU_MEM" -smp "$QEMU_SMP" \
	$fw -no-reboot \
	-drive "file=$IMG,format=raw,snapshot=on" \
	-smbios "type=11,path=$cfg" \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	"$@"
