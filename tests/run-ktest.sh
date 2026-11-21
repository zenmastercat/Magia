#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 zenmastercat and Magia contributors
#
# run-ktest.sh <magia.img>
#
# Headless QEMU runs of the disk image through OVMF (UEFI) and Limine
# (tests/qemu.sh), q35, -smp 4, -m 8G by default, each with a hard timeout:
#   1. ktest:   boot with "ktest", run every in-kernel test, parse the
#               KTEST SUMMARY line, require isa-debug-exit status 33.
#   2. monitor: boot normally, type commands at the magia> prompt over the
#               serial line and one over the PS/2 keyboard (QEMU sendkey via
#               tests/hmp.py), check their output, "poweroff".
#   2b. usbkbd: the same with QEMU's xHCI + usb-kbd: 26 commands (312 keys,
#               over 600 HID reports) typed on the USB keyboard (QEMU
#               sendkey) must all run.
#   3. crash:   negative tests of the hardening - each boot must panic with
#               a specific message (stack protector, thread stack canary,
#               write to kernel text).
#   4. bigmem:  boot "ktest nox2apic" with -m 32G (BIGMEM_MEM; falls back to the
#               largest size QEMU can allocate and says so).
#   5. bios:    boot "ktest" through SeaBIOS (Limine BIOS stages).
#   6. noapic:  boot "ktest noapic" (8259 PIC + PIT path).
#   7. kvm:     boot "ktest" with -accel kvm -cpu host when /dev/kvm is
#               usable (x2APIC + TSC-deadline timer); skipped otherwise.
# Exits non-zero on any failure. Logs go to build/*.log.
set -u

IMG=${1:-build/magia.img}
TIMEOUT=${KTEST_TIMEOUT:-180}
LOGDIR=$(dirname "$IMG")
HERE=$(dirname "$0")
fail=0

qemu() {
	# $1 = kernel command line, rest = extra QEMU args; stdin = serial input
	cmd=$1
	shift
	timeout --foreground -k 5 "$TIMEOUT" sh "$HERE/qemu.sh" "$IMG" "$cmd" \
		-display none -serial stdio -monitor "${MONITOR:-none}" "$@"
}

# check_ktest <log> <qemu status> <label>
check_ktest() {
	log=$1 st=$2 label=$3
	tr -d '\r' <"$log" | grep -E '^KTEST (FAIL|TIMEOUT|PANIC)|check failed|Magia panic' | sed 's/^/   /'
	summary=$(tr -d '\r' <"$log" | grep '^KTEST SUMMARY' | tail -1)
	echo "   QEMU exit status: $st (33 = all passed)"
	if [ -z "$summary" ]; then
		echo "FAIL: $label: no KTEST SUMMARY line (timeout or crash); last lines:"
		tr -d '\r' <"$log" | tail -15 | sed 's/^/   /'
		fail=1
		return
	fi
	echo "   $summary"
	failed=$(echo "$summary" | sed -n 's/.*failed=\([0-9]*\).*/\1/p')
	total=$(echo "$summary" | sed -n 's/.*total=\([0-9]*\).*/\1/p')
	passed=$(echo "$summary" | sed -n 's/.*passed=\([0-9]*\).*/\1/p')
	if [ "$st" -ne 33 ] || [ "$failed" != 0 ] || [ "$passed" != "$total" ]; then
		echo "FAIL: $label reported failures"
		fail=1
	fi
}

# ---------------------------------------------------------------- 3. crash (in the background)
crash_cases="stackprot:stack protector: stack smashing detected
canary:canary smashed
wx:unhandled #PF page fault in kernel mode"
echo "$crash_cases" | while IFS= read -r case; do
	name=${case%%:*}
	qemu "ktest crash=$name" </dev/null >"$LOGDIR/crash-$name.log" 2>&1
	echo $? >"$LOGDIR/crash-$name.status"
done &
crash_pid=$!

# ---------------------------------------------------------------- 1. ktest
log="$LOGDIR/ktest.log"
echo "== ktest: booting $IMG through OVMF + Limine (${QEMU_ACCEL:-tcg}, -smp ${QEMU_SMP:-4} -m ${QEMU_MEM:-8G}, ${TIMEOUT}s timeout)"
qemu "ktest" </dev/null >"$log" 2>&1
check_ktest "$log" $? ktest

# ---------------------------------------------------------------- 2. monitor
# Commands typed over the serial line, then one over the PS/2 keyboard
# (QEMU sendkey through the human monitor socket), then poweroff.
log="$LOGDIR/monitor.log"
sock="$LOGDIR/monitor.sock"
echo "== monitor: driving the magia> prompt over serial and the PS/2 keyboard"
: >"$log"
rm -f "$sock"
{
	i=0
	while ! grep -q 'magia> ' "$log" 2>/dev/null; do
		sleep 0.2
		i=$((i + 1))
		[ $i -gt 600 ] && break
	done
	for cmd in "uname" "sysctl kern.ostype" "ls /dev" "cat /etc/motd" "jails" "ps" "lspci" "hwinfo" "dmesg 400" "frobnicate"; do
		printf '%s\r' "$cmd"
		sleep 0.3
	done
	python3 -I "$HERE/hmp.py" "$sock" 'type:echo kbd-OK\n' >/dev/null
	sleep 1
	printf 'poweroff\r'
	sleep 0.3
} | QEMU_USB=xhci MONITOR="unix:$sock,server,nowait" qemu "monitor" >"$log" 2>&1
st=$?
rm -f "$sock"
clean=$(tr -d '\r' <"$log")
mfail=0
for want in "magia> uname" "Magia" "console" "1b36:000d class 0c/03/30" "hwinfo | " "1b36:000d 0c0330" "unknown command 'frobnicate'" "magia> echo kbd-OK" "poweroff: bye"; do
	if ! printf '%s\n' "$clean" | grep -qF "$want"; then
		echo "   missing in monitor output: $want"
		mfail=1
	fi
done
# dmesg repeats the boot log: the ECAM line appears twice
[ "$(printf "%s\n" "$clean" | grep -c "^pci: ECAM")" -ge 2 ] || { echo "   dmesg did not print the boot log"; mfail=1; }
# the echo command's own output line, not just the echoed keystrokes
printf '%s\n' "$clean" | grep -qx 'kbd-OK' || { echo "   keyboard command did not run"; mfail=1; }
[ "$st" -eq 33 ] || { echo "   QEMU exit status $st, expected 33"; mfail=1; }
if [ $mfail -ne 0 ]; then
	echo "FAIL: monitor smoke test (see $log)"
	fail=1
else
	echo "   monitor: 10 commands over serial (hwinfo, dmesg), 1 over the PS/2 keyboard, poweroff exit status 33"
fi

# ---------------------------------------------------------------- 2b. USB keyboard
# The target PC has no PS/2: type at magia> through QEMU's usb-kbd behind
# the xHCI (sendkey goes to the USB keyboard when one is present), then
# check the commands ran and the USB report counter moved. 26 commands
# are 312 keys, a press and a release report each, one TRB per report:
# the keyboard's transfer ring (255 TRBs per lap) wraps twice (review F1).
log="$LOGDIR/monitor-usb.log"
echo "== usbkbd: typing at magia> through the xHCI USB keyboard"
: >"$log"
rm -f "$sock"
{
	i=0
	while ! grep -q 'magia> ' "$log" 2>/dev/null; do
		sleep 0.2
		i=$((i + 1))
		[ $i -gt 600 ] && break
	done
	keys=
	for n in $(seq 10 34); do keys="${keys}echo usb-$n\\n"; done
	python3 -I "$HERE/hmp.py" "$sock" "type:${keys}echo usb-OK\\n" >/dev/null
	sleep 1
	printf 'usb\r'
	sleep 0.5
	printf 'poweroff\r'
	sleep 0.3
} | QEMU_USB=kbd MONITOR="unix:$sock,server,nowait" qemu "monitor" >"$log" 2>&1
st=$?
rm -f "$sock"
clean=$(tr -d '\r' <"$log")
ufail=0
printf '%s\n' "$clean" | grep -qx 'usb-OK' || { echo "   the last command typed on the USB keyboard did not run"; ufail=1; }
ran=$(printf '%s\n' "$clean" | grep -cx 'usb-[0-9][0-9]')
[ "$ran" -eq 25 ] || { echo "   $ran of 25 numbered commands typed on the USB keyboard ran"; ufail=1; }
reports=$(printf '%s\n' "$clean" | sed -n 's/^keyboard: 0627:0001 .*, \([0-9]*\) reports.*/\1/p' | tail -1)
[ "${reports:-0}" -ge 600 ] || { echo "   ${reports:-0} USB keyboard reports counted, expected at least 600"; ufail=1; }
printf '%s\n' "$clean" | grep -q 'MSI-X vector' || { echo "   xHCI not on MSI-X"; ufail=1; }
[ "$st" -eq 33 ] || { echo "   QEMU exit status $st, expected 33"; ufail=1; }
if [ $ufail -ne 0 ]; then
	echo "FAIL: USB keyboard test (see $log)"
	fail=1
else
	echo "   usbkbd: 26 commands (312 keys) typed on the USB keyboard ran; $reports HID reports over MSI-X"
fi

# ---------------------------------------------------------------- 3. crash (results)
wait $crash_pid
echo "$crash_cases" | while IFS= read -r case; do
	name=${case%%:*}
	want=${case#*:}
	st=$(cat "$LOGDIR/crash-$name.status" 2>/dev/null || echo none)
	if [ "$st" = 7 ] && tr -d '\r' <"$LOGDIR/crash-$name.log" | grep -qF "$want"; then
		echo "== crash=$name: panicked as required (\"$want\")"
	else
		echo "FAIL: crash=$name: status $st, expected 7 and \"$want\" (see $LOGDIR/crash-$name.log)"
		echo fail >"$LOGDIR/crash.failed"
	fi
done
if [ -f "$LOGDIR/crash.failed" ]; then
	rm -f "$LOGDIR/crash.failed"
	fail=1
fi
rm -f "$LOGDIR"/crash-*.status

# ---------------------------------------------------------------- 4. bigmem
if [ "${BIGMEM:-1}" = 1 ]; then
	log="$LOGDIR/ktest-bigmem.log"
	for mem in ${BIGMEM_MEM:-32G 24G 16G}; do
		echo "== bigmem: ktest with -m $mem (and nox2apic: the xAPIC MMIO path)"
		QEMU_MEM=$mem qemu "ktest nox2apic" </dev/null >"$log" 2>&1
		st=$?
		if grep -q 'cannot set up guest memory' "$log"; then
			echo "   QEMU could not allocate $mem on this host, trying less"
			continue
		fi
		tr -d '\r' <"$log" | grep -E '^pmm: .*managed' | sed 's/^/   /'
		check_ktest "$log" $st "bigmem ($mem)"
		break
	done
fi

# ---------------------------------------------------------------- 5. BIOS
# The same image through SeaBIOS and Limine's BIOS stages (legacy boot).
log="$LOGDIR/ktest-bios.log"
echo "== bios: ktest booted through SeaBIOS + Limine BIOS"
OVMF=bios qemu "ktest" </dev/null >"$log" 2>&1
check_ktest "$log" $? "bios boot"

# ---------------------------------------------------------------- 6. noapic
log="$LOGDIR/ktest-noapic.log"
echo "== noapic: ktest on the legacy 8259 PIC + PIT path"
qemu "ktest noapic" </dev/null >"$log" 2>&1
check_ktest "$log" $? "noapic"

# ---------------------------------------------------------------- 7. KVM
# Hardware virtualisation with the host CPU model: x2APIC and the
# TSC-deadline timer (TCG offers x2APIC but no TSC deadline).
if [ -r /dev/kvm ] && [ -w /dev/kvm ] && [ "${KVM:-1}" = 1 ]; then
	log="$LOGDIR/ktest-kvm.log"
	echo "== kvm: ktest with -accel kvm -cpu host"
	QEMU_ACCEL=kvm QEMU_CPU=host qemu "ktest" </dev/null >"$log" 2>&1
	st=$?
	tr -d '\r' <"$log" | grep -E '^intr:' | sed 's/^/   /'
	check_ktest "$log" $st "kvm"
else
	echo "== kvm: skipped (no usable /dev/kvm)"
fi

if [ $fail -ne 0 ]; then
	echo "run-ktest: FAILED"
	exit 1
fi
echo "run-ktest: all QEMU checks passed"
