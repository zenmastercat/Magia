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
#   2. monitor: boot with "fadt8042=0" (firmware that clears the FADT 8042
#               flag), type commands at the magia> prompt over the serial
#               line and one over the PS/2 keyboard (QEMU sendkey via
#               tests/hmp.py), check their output, then "reboot": QEMU
#               (-no-reboot) must exit through the FADT reset register.
#   2b. usbkbd: the same with QEMU's xHCI + usb-kbd: 26 commands (312 keys,
#               over 600 HID reports) typed on the USB keyboard (QEMU
#               sendkey) must all run; then "poweroff": ACPI S5 must end
#               QEMU by itself (run without -no-reboot, so a reset hangs).
#   2c. desktop: the target desktop's USB shape (qemu.sh QEMU_USB=desktop):
#               typing through a keyboard behind a hub, hot-plug with
#               device_add/device_del on a hub port and a root port, mouse.
#   3. crash:   negative tests of the hardening - each boot must panic with
#               a specific message (stack protector, thread stack canary,
#               write to kernel text).
#   4. bigmem:  boot "ktest nox2apic" with -m 32G (BIGMEM_MEM; falls back to the
#               largest size QEMU can allocate and says so).
#   5. bios:    boot "ktest" through SeaBIOS (Limine BIOS stages).
#   6. noapic:  boot "ktest noapic" (8259 PIC + LAPIC timer).
#   7. kvm:     boot "ktest bench" with -accel kvm -cpu host,+invtsc when
#               /dev/kvm is usable (x2APIC + TSC-deadline timer), then the
#               benchmark harness; BENCH lines checked and kept in
#               build/bench-kvm.txt; skipped otherwise.
#   8. more ktest boots, in the background: "desktop" (the desktop USB
#               shape, no i8042), "legacy" (noapic pittick: the PIT tick),
#               "nopit" (nopit nohpet: a gated PIT, no HPET; the TSC
#               frequency falls back and the boot does not hang).
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

# ---------------------------------------------------------------- 8. more ktest boots (in the background)
# name|command line|environment for qemu.sh|a line the log must contain
extra_cases="desktop|ktest kt-desktop|QEMU_USB=desktop|kbd: no i8042 (the controller does not respond)
legacy|ktest noapic pittick||timer: PIT at 1000 Hz
nopit|ktest nopit nohpet||PIT channel 2 does not count (gated?)"
echo "$extra_cases" | {
	while IFS='|' read -r name cmd envs want; do
		(
			[ -n "$envs" ] && export $envs
			qemu "$cmd" </dev/null >"$LOGDIR/ktest-$name.log" 2>&1
			echo $? >"$LOGDIR/ktest-$name.status"
		) &
	done
	wait
} &
extra_pid=$!

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
	printf 'reboot\r'
	sleep 0.3
} | QEMU_USB=xhci MONITOR="unix:$sock,server,nowait" qemu "monitor fadt8042=0" >"$log" 2>&1
st=$?
rm -f "$sock"
clean=$(tr -d '\r' <"$log")
mfail=0
for want in "magia> uname" "Magia" "console" "1b36:000d class 0c/03/30" "hwinfo | " "1b36:000d 0c0330" "unknown command 'frobnicate'" "magia> echo kbd-OK" "reboot: ACPI reset register (I/O 0xcf9 = 0x0f)" \
	"kbd: FADT says no i8042; probing anyway" "PS/2 keyboard on i8042 (scancode set 1, US layout) although the FADT flag is clear"; do
	if ! printf '%s\n' "$clean" | grep -qF "$want"; then
		echo "   missing in monitor output: $want"
		mfail=1
	fi
done
# dmesg repeats the boot log: the ECAM line appears twice
[ "$(printf "%s\n" "$clean" | grep -c "^pci: ECAM")" -ge 2 ] || { echo "   dmesg did not print the boot log"; mfail=1; }
# the echo command's own output line, not just the echoed keystrokes
printf '%s\n' "$clean" | grep -qx 'kbd-OK' || { echo "   keyboard command did not run"; mfail=1; }
# -no-reboot: the reset ends QEMU with status 0; the fallback must not have been needed
printf '%s\n' "$clean" | grep -q 'did not reset' && { echo "   the reset register did not reset"; mfail=1; }
[ "$st" -eq 0 ] || { echo "   QEMU exit status $st, expected 0 (reset)"; mfail=1; }
if [ $mfail -ne 0 ]; then
	echo "FAIL: monitor smoke test (see $log)"
	fail=1
else
	echo "   monitor: 10 commands over serial (hwinfo, dmesg), 1 over the PS/2 keyboard (FADT flag cleared); reboot through the ACPI reset register"
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
} | QEMU_USB=kbd QEMU_NOREBOOT=0 MONITOR="unix:$sock,server,nowait" qemu "monitor" >"$log" 2>&1
st=$?
rm -f "$sock"
clean=$(tr -d '\r' <"$log")
ufail=0
printf '%s\n' "$clean" | grep -qx 'usb-OK' || { echo "   the last command typed on the USB keyboard did not run"; ufail=1; }
ran=$(printf '%s\n' "$clean" | grep -cx 'usb-[0-9][0-9]')
[ "$ran" -eq 25 ] || { echo "   $ran of 25 numbered commands typed on the USB keyboard ran"; ufail=1; }
reports=$(printf '%s\n' "$clean" | sed -n 's/^c0 p.* 0627:0001 .* kbd: \([0-9]*\) reports.*/\1/p' | tail -1)
[ "${reports:-0}" -ge 600 ] || { echo "   ${reports:-0} USB keyboard reports counted, expected at least 600"; ufail=1; }
printf '%s\n' "$clean" | grep -q 'MSI-X vector' || { echo "   xHCI not on MSI-X"; ufail=1; }
# ACPI S5: QEMU powers off by itself (status 0); with reboots allowed, a
# reset instead would hang here until the timeout
printf '%s\n' "$clean" | grep -qF 'poweroff: ACPI S5 (SLP_TYPa 0 into PM1a control I/O 0x604)' ||
	{ echo "   poweroff did not take the ACPI S5 path"; ufail=1; }
printf '%s\n' "$clean" | grep -q 'still running after the S5 write' && { echo "   S5 did not power off"; ufail=1; }
[ "$st" -eq 0 ] || { echo "   QEMU exit status $st, expected 0 (ACPI poweroff)"; ufail=1; }
if [ $ufail -ne 0 ]; then
	echo "FAIL: USB keyboard test (see $log)"
	fail=1
else
	echo "   usbkbd: 26 commands (312 keys) typed on the USB keyboard ran; $reports HID reports over MSI-X; ACPI S5 poweroff"
fi

# ---------------------------------------------------------------- 2c. desktop USB
# The target desktop's shape (QEMU_USB=desktop in qemu.sh): no i8042, two
# xHCIs, a keyboard behind a hub, a second keyboard on a root port, a
# mouse. sendkey goes to the most recently created keyboard, so the order
# of typing, device_add and device_del below picks the keyboard: kh
# (behind the hub), then kh2 hot-plugged on another hub port, kh again
# after kh2 is removed, kr (root port) after kh is removed, and kr2
# hot-plugged on a root port after kr is removed.
log="$LOGDIR/monitor-desktop.log"
echo "== desktop: two xHCIs, keyboards behind a hub and on a root port, hot-plug, mouse, no i8042"
: >"$log"
rm -f "$sock"
hmp() { python3 -I "$HERE/hmp.py" "$sock" "$@" >/dev/null; }
{
	i=0
	while ! grep -q 'magia> ' "$log" 2>/dev/null; do
		sleep 0.2
		i=$((i + 1))
		[ $i -gt 600 ] && break
	done
	hmp 'type:echo hub-OK\n'
	sleep 0.5
	hmp 'device_add usb-kbd,bus=xhci2.0,port=3.2,id=kh2'
	sleep 2
	hmp 'type:echo hubhot-OK\n'
	sleep 0.5
	hmp 'device_del kh2'
	sleep 1.5
	hmp 'type:echo hub2-OK\n'
	sleep 0.5
	hmp 'device_del kh'
	sleep 1.5
	hmp 'type:echo root-OK\n'
	sleep 0.5
	hmp 'device_del kr'
	sleep 1.5
	hmp 'device_add usb-kbd,bus=xhci2.0,port=2,id=kr2'
	sleep 2
	hmp 'type:echo roothot-OK\n' 'mouse_move 10 -5' 'mouse_move 10 -5' 'mouse_move 10 -5'
	sleep 1
	printf 'usb\r'
	sleep 0.5
	printf 'poweroff\r'
	sleep 0.3
} | QEMU_USB=desktop QEMU_NOREBOOT=0 MONITOR="unix:$sock,server,nowait" qemu "" >"$log" 2>&1
st=$?
rm -f "$sock"
clean=$(tr -d '\r' <"$log")
dfail=0
for want in hub-OK hubhot-OK hub2-OK root-OK roothot-OK; do
	printf '%s\n' "$clean" | grep -qx "$want" || { echo "   '$want' (typed on a USB keyboard) did not run"; dfail=1; }
done
for want in "usb: c1 p7 FS 0409:55aa class 09: hub" "usb: c1 p7.2 FS 0627:0001 class 00: keyboard" \
	"usb: c1 p7.2 0627:0001 detached" "usb: c1 p7.1 0627:0001 detached" "usb: c1 p8 0627:0001 detached" \
	"usb: c1 p6 HS 0627:0001 class 00: keyboard" "kbd: no i8042" "x 30 y -15"; do
	printf '%s\n' "$clean" | grep -qF "$want" || { echo "   missing in the desktop run: $want"; dfail=1; }
done
printf '%s\n' "$clean" | grep -qF 'poweroff: ACPI S5' || { echo "   poweroff did not take the ACPI S5 path"; dfail=1; }
[ "$st" -eq 0 ] || { echo "   QEMU exit status $st, expected 0 (ACPI poweroff)"; dfail=1; }
if [ $dfail -ne 0 ]; then
	echo "FAIL: desktop USB test (see $log)"
	fail=1
else
	echo "   desktop: typed through a hub keyboard, a hot-plugged hub-port and root-port keyboard, the root keyboard; 2 detaches seen; mouse moved 30,-15"
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

# ---------------------------------------------------------------- 5b. BIOS poweroff
# SeaBIOS leaves the chipset in legacy (non-ACPI) mode: poweroff must
# switch ACPI on through SMI_CMD before S5 can work.
log="$LOGDIR/poweroff-bios.log"
echo "== bios poweroff: ACPI mode through SMI_CMD, then S5"
: >"$log"
{
	i=0
	while ! grep -q 'magia> ' "$log" 2>/dev/null; do
		sleep 0.2
		i=$((i + 1))
		[ $i -gt 600 ] && break
	done
	printf 'poweroff\r'
	sleep 0.3
} | OVMF=bios QEMU_USB=none QEMU_NOREBOOT=0 qemu "" >"$log" 2>&1
st=$?
clean=$(tr -d '\r' <"$log")
if printf '%s\n' "$clean" | grep -qF 'enabling ACPI through SMI_CMD' &&
	printf '%s\n' "$clean" | grep -qF 'poweroff: ACPI S5' &&
	! printf '%s\n' "$clean" | grep -qE 'did not come on|still running' && [ "$st" -eq 0 ]; then
	echo "   legacy mode left through SMI_CMD, S5 powered QEMU off (status 0)"
else
	echo "FAIL: BIOS poweroff (status $st, see $log)"
	fail=1
fi

# ---------------------------------------------------------------- 6. noapic
log="$LOGDIR/ktest-noapic.log"
echo "== noapic: ktest on the legacy 8259 PIC (LAPIC timer tick)"
qemu "ktest noapic" </dev/null >"$log" 2>&1
check_ktest "$log" $? "noapic"

# ---------------------------------------------------------------- 7. KVM
# Hardware virtualisation with the host CPU model: x2APIC and the
# TSC-deadline timer (TCG offers x2APIC but no TSC deadline), and the
# invariant TSC bit (+invtsc), which -cpu host leaves out. After the
# ktests the kernel runs the benchmark harness ("bench"): every primitive
# must report sane numbers; the BENCH lines go to build/bench-kvm.txt.
if [ -r /dev/kvm ] && [ -w /dev/kvm ] && [ "${KVM:-1}" = 1 ]; then
	log="$LOGDIR/ktest-kvm.log"
	echo "== kvm: ktest bench with -accel kvm -cpu host,+invtsc"
	# one host core for the whole VM (the paper's fairness rule; on a
	# hybrid CPU the first P-core, else what BENCH_CPU says)
	pin=${BENCH_CPU:-}
	[ -z "$pin" ] && [ -r /sys/devices/cpu_core/cpus ] && pin=$(sed 's/[-,].*//' /sys/devices/cpu_core/cpus)
	if [ -n "$pin" ] && command -v taskset >/dev/null; then
		echo "   pinned to host CPU $pin"
		QEMU="taskset -c $pin ${QEMU:-qemu-system-x86_64}" QEMU_ACCEL=kvm QEMU_CPU=host,+invtsc qemu "ktest bench" </dev/null >"$log" 2>&1
	else
		echo "   not pinned to a host CPU (set BENCH_CPU): numbers may mix core types"
		QEMU_ACCEL=kvm QEMU_CPU=host,+invtsc qemu "ktest bench" </dev/null >"$log" 2>&1
	fi
	st=$?
	tr -d '\r' <"$log" | grep -E '^intr:' | sed 's/^/   /'
	check_ktest "$log" $st "kvm"
	# the last run only (ktest 921 runs the harness too, with 1000 samples)
	tr -d '\r' <"$log" | awk '/^BENCH-INFO/ { buf = "" } /^BENCH/ { buf = buf $0 "\n" } END { printf "%s", buf }' \
		>"$LOGDIR/bench-kvm.txt"
	bfail=0
	grep -q '^BENCH-INFO .*invariant_tsc=1 rdtscp=1 timer=tsc_deadline' "$LOGDIR/bench-kvm.txt" ||
		{ echo "   BENCH-INFO missing or not invariant TSC / rdtscp / TSC deadline"; bfail=1; }
	grep -qx 'BENCH-END' "$LOGDIR/bench-kvm.txt" || { echo "   no BENCH-END"; bfail=1; }
	# name|largest sane median in ns (generous: a KVM guest, any host)
	while IFS='|' read -r name max; do
		line=$(grep "^BENCH name=$name " "$LOGDIR/bench-kvm.txt")
		med=$(echo "$line" | sed -n 's/.* median_ns=\([0-9]*\)\.[0-9].*/\1/p')
		p99=$(echo "$line" | sed -n 's/.* p99_cycles=\([0-9]*\) .*/\1/p')
		mc=$(echo "$line" | sed -n 's/.* median_cycles=\([0-9]*\) .*/\1/p')
		n=$(echo "$line" | sed -n 's/.* n=\([0-9]*\) .*/\1/p')
		if [ -z "$med" ] || [ "${n:-0}" -lt 1000 ] || [ "$mc" -le 0 ] || [ "$p99" -lt "$mc" ] || [ "$med" -gt "$max" ]; then
			echo "   $name: not sane: ${line:-missing}"
			bfail=1
		else
			echo "   $name: median $med ns (n=$n)"
		fi
	done <<EOB
tsc_overhead|1000
null_syscall_ring3|20000
null_syscall_kernel|20000
ctx_switch|50000
irq_to_thread|500000
page_map|50000
page_unmap|50000
kmalloc_free_64|20000
page_alloc_free|100000
EOB
	[ $bfail -eq 0 ] || { echo "FAIL: kvm bench (see $LOGDIR/bench-kvm.txt)"; fail=1; }
else
	echo "== kvm: skipped (no usable /dev/kvm)"
fi

# ---------------------------------------------------------------- 8. more ktest boots (results)
wait $extra_pid
while IFS='|' read -r name cmd envs want; do
	log="$LOGDIR/ktest-$name.log"
	echo "== $name: $cmd${envs:+ ($envs)}"
	tr -d '\r' <"$log" | grep -E '^(intr|timer|kbd):' | sed 's/^/   /'
	check_ktest "$log" "$(cat "$LOGDIR/ktest-$name.status" 2>/dev/null || echo 255)" "$name"
	tr -d '\r' <"$log" | grep -qF "$want" || { echo "FAIL: $name: missing \"$want\""; fail=1; }
	rm -f "$LOGDIR/ktest-$name.status"
done <<EOF
$extra_cases
EOF

if [ $fail -ne 0 ]; then
	echo "run-ktest: FAILED"
	exit 1
fi
echo "run-ktest: all QEMU checks passed"
