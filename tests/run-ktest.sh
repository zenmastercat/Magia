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
#               serial line, check their output, "poweroff".
#   3. crash:   negative tests of the hardening - each boot must panic with
#               a specific message (stack protector, thread stack canary,
#               write to kernel text).
#   4. bigmem:  boot "ktest" with -m 32G (BIGMEM_MEM; falls back to the
#               largest size QEMU can allocate and says so).
#   5. bios:    boot "ktest" through SeaBIOS (Limine BIOS stages).
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
		-display none -serial stdio -monitor none "$@"
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
log="$LOGDIR/monitor.log"
echo "== monitor: driving the magia> prompt over serial"
: >"$log"
{
	i=0
	while ! grep -q 'magia> ' "$log" 2>/dev/null; do
		sleep 0.2
		i=$((i + 1))
		[ $i -gt 600 ] && break
	done
	for cmd in "uname" "sysctl kern.ostype" "ls /dev" "cat /etc/motd" "jails" "ps" "frobnicate" "poweroff"; do
		printf '%s\r' "$cmd"
		sleep 0.3
	done
} | qemu "monitor" >"$log" 2>&1
st=$?
clean=$(tr -d '\r' <"$log")
mfail=0
for want in "magia> uname" "Magia" "console" "unknown command 'frobnicate'" "poweroff: bye"; do
	if ! printf '%s\n' "$clean" | grep -qF "$want"; then
		echo "   missing in monitor output: $want"
		mfail=1
	fi
done
[ "$st" -eq 33 ] || { echo "   QEMU exit status $st, expected 33"; mfail=1; }
if [ $mfail -ne 0 ]; then
	echo "FAIL: monitor smoke test (see $log)"
	fail=1
else
	echo "   monitor: 8 commands OK, poweroff exit status 33"
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
		echo "== bigmem: ktest with -m $mem"
		QEMU_MEM=$mem qemu "ktest" </dev/null >"$log" 2>&1
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

if [ $fail -ne 0 ]; then
	echo "run-ktest: FAILED"
	exit 1
fi
echo "run-ktest: all QEMU checks passed"
