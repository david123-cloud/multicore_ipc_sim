#!/usr/bin/env bash
#
# M2 acceptance: PVM router + GVM application over the control channel and the
# shared memory ring.  Runs the two processes and checks the counters they
# report against each other.
#
#   bash tests/run_m2.sh <pvm_router> <gvm_app>

set -uo pipefail

PVM="${1:?usage: run_m2.sh <pvm_router> <gvm_app>}"
GVM="${2:?usage: run_m2.sh <pvm_router> <gvm_app>}"

SOCK="/tmp/mc_ipc_m2_$$.sock"
WORK="$(mktemp -d)"
PERIODS=100
EXPECTED_FRAMES=$((PERIODS * 960))

cleanup() {
    rm -rf "$WORK"
    rm -f "$SOCK"
}
trap cleanup EXIT

failures=0
ok() { printf '  [ok]   %s\n' "$1"; }
bad() { printf '  [FAIL] %s\n' "$1"; failures=$((failures + 1)); }

"$PVM" "$SOCK" > "$WORK/pvm.log" 2>&1 &
PVM_PID=$!

# Wait for the listening socket instead of sleeping a fixed amount.
for _ in $(seq 1 200); do
    [ -S "$SOCK" ] && break
    sleep 0.01
done
if [ ! -S "$SOCK" ]; then
    echo "  [FAIL] router never created $SOCK"
    cat "$WORK/pvm.log"
    exit 1
fi

"$GVM" "$SOCK" "$PERIODS" > "$WORK/gvm.log" 2>&1
GVM_RC=$?

wait "$PVM_PID"
PVM_RC=$?

echo "--- gvm ---"
cat "$WORK/gvm.log"
echo "--- pvm ---"
cat "$WORK/pvm.log"

[ "$GVM_RC" -eq 0 ] && ok "gvm exit 0" || bad "gvm exit $GVM_RC"
[ "$PVM_RC" -eq 0 ] && ok "pvm exit 0" || bad "pvm exit $PVM_RC"

gvm_frames=$(sed -n 's/^GVM_STATS frames=\([0-9]*\).*/\1/p' "$WORK/gvm.log")
pvm_frames=$(sed -n 's/^PVM_gvm_STATS frames=\([0-9]*\).*/\1/p' "$WORK/pvm.log")
pvm_periods=$(sed -n 's/^PVM_gvm_STATS .* periods=\([0-9]*\).*/\1/p' "$WORK/pvm.log")
gvm_full=$(sed -n 's/^GVM_STATS .* full_events=\([0-9]*\).*/\1/p' "$WORK/gvm.log")

[ "$gvm_frames" = "$EXPECTED_FRAMES" ] && ok "gvm wrote $gvm_frames frames" \
    || bad "gvm wrote ${gvm_frames:-none}, expected $EXPECTED_FRAMES"
[ "$pvm_frames" = "$EXPECTED_FRAMES" ] && ok "pvm consumed $pvm_frames frames" \
    || bad "pvm consumed ${pvm_frames:-none}, expected $EXPECTED_FRAMES"
[ "${pvm_periods:-0}" -ge 1 ] 2>/dev/null && ok "pvm saw $pvm_periods periods" \
    || bad "pvm period counter missing"
[ "${gvm_full:-0}" -ge 1 ] 2>/dev/null && ok "backpressure observed ($gvm_full full events)" \
    || bad "expected the ring to fill at least once (gvm was not paced)"
[ -f "$SOCK" ] && bad "socket file left behind" || ok "socket file cleaned up"
# A crashed run must not leak the segment: the name is stable and the next
# create reclaims it, and a clean run unlinks it on exit.
[ -e /dev/shm/mcshm_pvm_audio0 ] && bad "shm segment left in /dev/shm" \
    || ok "shm segment released"

if [ "$failures" -eq 0 ]; then
    echo "single-hop ipc: ok"
    exit 0
fi
echo "single-hop ipc: FAILED ($failures)"
exit 1
