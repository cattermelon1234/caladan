#!/bin/sh
# Runs the probe against the simulated kernel and checks exit codes and output.
# Usage: test/run_tests.sh path/to/probe_sim

set -u
PROBE=${1:?usage: run_tests.sh PROBE_SIM}
IFACE=lo0
[ "$(uname -s)" = Linux ] && IFACE=lo
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
pass=0; fail=0

# run NAME EXPECTED_EXIT [ENV=VAL ...] -- PROBE_ARGS...
run() {
	name=$1; want=$2; shift 2
	env_args=""
	while [ "$1" != "--" ]; do env_args="$env_args $1"; shift; done
	shift
	env $env_args "$PROBE" "$@" >"$OUT" 2>&1
	got=$?
	if [ "$got" -ne "$want" ]; then
		echo "FAIL $name: exit $got, expected $want"; sed 's/^/    /' "$OUT"
		fail=$((fail + 1)); return 1
	fi
	return 0
}

# Signal variant: start, wait, send SIGINT, expect clean exit.
run_signal() {
	name=$1; shift
	"$PROBE" "$@" >"$OUT" 2>&1 & pid=$!
	sleep 1
	kill -INT "$pid"
	wait "$pid"; got=$?
	if [ "$got" -ne 0 ]; then
		echo "FAIL $name: exit $got, expected 0"; sed 's/^/    /' "$OUT"
		fail=$((fail + 1)); return 1
	fi
	return 0
}

expect() {	# expect NAME PATTERN  -- output must contain PATTERN
	if grep -q -- "$2" "$OUT"; then return 0; fi
	echo "FAIL $1: output lacks '$2'"; sed 's/^/    /' "$OUT"
	fail=$((fail + 1)); return 1
}
reject() {	# reject NAME PATTERN  -- output must not contain PATTERN
	if ! grep -q -- "$2" "$OUT"; then return 0; fi
	echo "FAIL $1: output contains '$2'"; sed 's/^/    /' "$OUT"
	fail=$((fail + 1)); return 1
}
ok() { echo "ok   $1"; pass=$((pass + 1)); }

BALANCED="free=2048 kernel=2048 held=0 (expected"

# --- option parsing -------------------------------------------------------

t="help exits 0"
run "$t" 0 -- --help && expect "$t" "Usage:" && ok "$t"

t="unknown option is rejected"
run "$t" 1 -- -i $IFACE --bogus && expect "$t" "Unknown option" && ok "$t"

t="missing interface is rejected"
run "$t" 1 -- -q 0 && expect "$t" "Provide an interface" && ok "$t"

t="positional argument is rejected"
run "$t" 1 -- -i $IFACE extra && expect "$t" "no positional" && ok "$t"

t="negative queue is rejected"
run "$t" 1 -- -i $IFACE -q -1 && expect "$t" "Invalid queue ID" && ok "$t"

t="queue above uint32 is rejected"
run "$t" 1 -- -i $IFACE -q 4294967296 && expect "$t" "Invalid queue ID" && ok "$t"

t="bad mode is rejected"
run "$t" 1 -- -i $IFACE -m fast && expect "$t" "Invalid mode" && ok "$t"

t="non-numeric count is rejected"
run "$t" 1 -- -i $IFACE -c abc && expect "$t" "Invalid count" && ok "$t"

t="nonexistent interface fails before setup"
run "$t" 1 -- -i nosuchif0 && expect "$t" "Cannot find interface" \
	&& reject "$t" "initialization succeeded" && ok "$t"

# --- initialization -------------------------------------------------------

t="requested mode is verified against the socket"
run "$t" 1 SIM_MODE=copy -- -i $IFACE -m zero-copy -c 10 -s 0 \
	&& expect "$t" "Verify selected AF_XDP mode" \
	&& reject "$t" "Published" && ok "$t"

t="copy mode binds when the socket reports copy"
run "$t" 0 SIM_MODE=copy -- -i $IFACE -m copy -c 10 -s 0 \
	&& expect "$t" "verified mode: copy" && ok "$t"

# --- receive and recycle --------------------------------------------------

t="100k packets flow through 4096 frames and the ledger balances"
run "$t" 0 -- -i $IFACE -m zero-copy -c 100000 -s 0 \
	&& expect "$t" "Reached packet limit" \
	&& expect "$t" "$BALANCED" \
	&& expect "$t" "errors: ledger=0 bad_desc=0 fill_failures=0" \
	&& ok "$t"

t="packet classification counts IPv4 and TCP/UDP"
if run "$t" 0 -- -i $IFACE -m zero-copy -c 3000 -s 0; then
	packets=$(sed -n 's/^  packets: \([0-9]*\) in.*/\1/p' "$OUT")
	ipv4=$(sed -n 's/^  ethertype: ipv4=\([0-9]*\) .*/\1/p' "$OUT")
	if [ -n "$packets" ] && [ "$packets" = "$ipv4" ] \
		&& expect "$t" "ipv6=0 arp=0 other=0 runt=0" \
		&& expect "$t" "ipv4 protocol: tcp=[1-9][0-9]* udp=[1-9][0-9]* icmp=0"; then
		ok "$t"
	else
		echo "FAIL $t: packets=$packets ipv4=$ipv4"; fail=$((fail + 1))
	fi
fi

t="empty RX ring with need-wakeup set issues a kick"
run "$t" 0 SIM_IDLE=3 SIM_WAKE=2 -- -i $IFACE -m zero-copy -c 20000 -s 0 \
	&& expect "$t" "$BALANCED" \
	&& expect "$t" "wakeups: [1-9][0-9]*" && ok "$t"

t="SIGINT during polling cleans up and exits 0"
run_signal "$t" -i $IFACE -m zero-copy -s 0 \
	&& expect "$t" "Stop requested" && expect "$t" "$BALANCED" && ok "$t"

t="periodic stats line is printed"
"$PROBE" -i $IFACE -m zero-copy -s 1 >"$OUT" 2>&1 & pid=$!
sleep 2.2; kill -INT "$pid"; wait "$pid"
expect "$t" "^rx: [0-9]* packets" && ok "$t"

# --- fault detection ------------------------------------------------------

t="frame delivered twice is caught, not recycled twice, and fails the run"
run "$t" 1 SIM_DUP=5000 -- -i $IFACE -m zero-copy -c 20000 -s 0 \
	&& expect "$t" "ledger: received frame [0-9]* while held" \
	&& expect "$t" "$BALANCED" \
	&& expect "$t" "fill_failures=0" \
	&& expect "$t" "Frame ownership did not reconcile\|ledger errors" \
	&& ok "$t"

t="descriptor outside UMEM is caught and fails the run"
run "$t" 1 SIM_BAD=5000 -- -i $IFACE -m zero-copy -c 20000 -s 0 \
	&& expect "$t" "is outside UMEM" \
	&& expect "$t" "$BALANCED" \
	&& expect "$t" "bad_desc=[1-9]" && ok "$t"

t="kernel that stops delivering leaves the probe polling; SIGINT still reconciles"
SIM_HOLD=3000 "$PROBE" -i $IFACE -m zero-copy -s 0 >"$OUT" 2>&1 & pid=$!
sleep 1; kill -INT "$pid"; wait "$pid"; got=$?
if [ "$got" -eq 0 ] && expect "$t" "$BALANCED" \
	&& expect "$t" "packets: 30[0-9][0-9] in"; then
	ok "$t"
else
	echo "FAIL $t: exit $got"; sed 's/^/    /' "$OUT"; fail=$((fail + 1))
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
