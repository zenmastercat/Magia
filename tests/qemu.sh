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
# Environment: QEMU (may carry a prefix such as "taskset -c 0 qemu-system-x86_64"), QEMU_ACCEL, QEMU_MEM, QEMU_SMP, QEMU_CPU, OVMF
# (path to OVMF.fd, or "bios" for SeaBIOS), QEMU_USB (kbd: an xHCI
# controller with a USB keyboard, the default; xhci: the controller only;
# desktop: the target desktop's shape - no i8042, a second display
# controller (secondary-vga; the desktop has an iGPU and a GPU), two xHCI controllers, a
# usb-mouse on the first, a usb-hub on USB 2 port 3 of the second with a
# usb-kbd behind it (3.1) and a second usb-kbd on USB 2 port 4; QEMU puts
# its USB 3 ports first, so the guest sees these as xHCI root ports 7,
# 7.1 and 8, and the mouse on 5; none), QEMU_NOREBOOT (0: let a reset
# reboot the VM instead of ending QEMU).
set -eu
IMG=$1
CMDLINE=$2
shift 2
: "${QEMU:=qemu-system-x86_64}" "${QEMU_ACCEL:=tcg}" "${QEMU_MEM:=8G}" "${QEMU_SMP:=4}"
: "${QEMU_CPU:=max}" "${OVMF:=${OVMF_FD:-}}" "${QEMU_USB:=kbd}" "${QEMU_NOREBOOT:=1}"
machine=q35
case "$QEMU_USB" in
# an xHCI controller with a USB keyboard, as on the target PC (no PS/2 there)
kbd) usb="-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0" ;;
xhci) usb="-device qemu-xhci,id=xhci" ;; # QEMU sendkey then reaches the PS/2 keyboard
desktop)
	machine=q35,i8042=off
	usb="-device secondary-vga -device qemu-xhci,id=xhci -device usb-mouse,bus=xhci.0,port=1,id=ms
	     -device qemu-xhci,id=xhci2 -device usb-hub,bus=xhci2.0,port=3,id=hub
	     -device usb-kbd,bus=xhci2.0,port=4,id=kr -device usb-kbd,bus=xhci2.0,port=3.1,id=kh" ;;
*) usb= ;;
esac
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
noreboot=-no-reboot
[ "$QEMU_NOREBOOT" = 0 ] && noreboot=
$QEMU -machine "$machine" -cpu "$QEMU_CPU" -accel "$QEMU_ACCEL" -m "$QEMU_MEM" -smp "$QEMU_SMP" \
	$fw $usb $noreboot \
	-drive "file=$IMG,format=raw,snapshot=on" \
	-smbios "type=11,path=$cfg" \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	"$@"
