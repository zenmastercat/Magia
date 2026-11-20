#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 zenmastercat and Magia contributors
#
# run-ktest.sh <magia32.elf>
#
# Headless QEMU runs of the kernel, each with a hard timeout:
#   1. ktest:   boot with "ktest", run every in-kernel test, parse the
#               KTEST SUMMARY line, require isa-debug-exit status 33.
#   2. monitor: boot normally, type commands at the magia> prompt over the
#               serial line, check their output, "poweroff".
#   3. crash:   negative tests of the hardening - each boot must panic with
#               a specific message (stack protector, thread stack canary,
#               write to kernel text).
# Exits non-zero on any failure. Logs go to build/*.log.
set -u

ELF=${1:-build/magia32.elf}
QEMU=${QEMU:-qemu-system-x86_64}
ACCEL=${QEMU_ACCEL:-tcg}
MEM=${QEMU_MEM:-256M}
TIMEOUT=${KTEST_TIMEOUT:-60}
LOGDIR=$(dirname "$ELF")
fail=0

qemu() {
	# $1 = kernel command line; stdin = serial input; stdout = serial output
	timeout --foreground -k 5 "$TIMEOUT" "$QEMU" -machine pc -cpu max -accel "$ACCEL" -m "$MEM" \
		-no-reboot -display none -serial stdio -monitor none \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-kernel "$ELF" -append "$1"
}

# ---------------------------------------------------------------- 1. ktest
log="$LOGDIR/ktest.log"
echo "== ktest: booting $ELF in QEMU ($ACCEL, ${TIMEOUT}s timeout)"
qemu "ktest" </dev/null >"$log" 2>&1
st=$?
tr -d '\r' <"$log" | grep -E '^KTEST (RUN|FAIL|TIMEOUT|PANIC)|check failed|panic' | grep -v '^KTEST RUN' | sed 's/^/   /'
summary=$(tr -d '\r' <"$log" | grep '^KTEST SUMMARY' | tail -1)
echo "   QEMU exit status: $st (33 = all passed)"
if [ -z "$summary" ]; then
	echo "FAIL: no KTEST SUMMARY line (timeout or crash); last lines:"
	tr -d '\r' <"$log" | tail -15 | sed 's/^/   /'
	fail=1
else
	echo "   $summary"
	failed=$(echo "$summary" | sed -n 's/.*failed=\([0-9]*\).*/\1/p')
	total=$(echo "$summary" | sed -n 's/.*total=\([0-9]*\).*/\1/p')
	passed=$(echo "$summary" | sed -n 's/.*passed=\([0-9]*\).*/\1/p')
	if [ "$st" -ne 33 ] || [ "$failed" != 0 ] || [ "$passed" != "$total" ]; then
		echo "FAIL: ktest reported failures"
		fail=1
	fi
fi

# ---------------------------------------------------------------- 2. monitor
log="$LOGDIR/monitor.log"
echo "== monitor: driving the magia> prompt over serial"
: >"$log"
{
	i=0
	while ! grep -q 'magia> ' "$log" 2>/dev/null; do
		sleep 0.2
		i=$((i + 1))
		[ $i -gt 200 ] && break
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

# ---------------------------------------------------------------- 3. crash
for case in "stackprot:stack protector: stack smashing detected" \
	"canary:canary smashed" \
	"wx:unhandled #PF page fault in kernel mode"; do
	name=${case%%:*}
	want=${case#*:}
	log="$LOGDIR/crash-$name.log"
	qemu "ktest crash=$name" </dev/null >"$log" 2>&1
	st=$?
	if [ "$st" -eq 7 ] && tr -d '\r' <"$log" | grep -qF "$want"; then
		echo "== crash=$name: panicked as required (\"$want\")"
	else
		echo "FAIL: crash=$name: status $st, expected 7 and \"$want\" (see $log)"
		fail=1
	fi
done

if [ $fail -ne 0 ]; then
	echo "run-ktest: FAILED"
	exit 1
fi
echo "run-ktest: all QEMU checks passed"
