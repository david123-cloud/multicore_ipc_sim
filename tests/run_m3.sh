#!/usr/bin/env bash
#
# M3 acceptance: ALSA-style PCM state machine, period notifications, and
# underrun reporting through the shared memory transport.
#
#   bash tests/run_m3.sh <pvm_router> <gvm_app>
#
# Scenario A: well-behaved application.  States must be enforced and no xrun
#             may be reported.
# Scenario B: the application stalls for 250 ms.  The device keeps consuming,
#             the ring empties, and both sides must report the underrun.

set -uo pipefail

PVM="${1:?usage: run_m3.sh <pvm_router> <gvm_app>}"
GVM="${2:?usage: run_m3.sh <pvm_router> <gvm_app>}"

WORK="$(mktemp -d)"
PERIODS=100
EXPECTED_FRAMES=$((PERIODS * 960))
failures=0

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

ok() { printf '  [ok]   %s\n' "$1"; }
bad() { printf '  [FAIL] %s\n' "$1"; failures=$((failures + 1)); }

# run_pair <tag> <gvm args...>
run_pair() {
    local tag="$1"
    shift
    local sock="/tmp/mc_ipc_m3_${tag}_$$.sock"
    local pvm_pid

    rm -f "$sock"
    "$PVM" "$sock" > "$WORK/${tag}_pvm.log" 2>&1 &
    pvm_pid=$!

    for _ in $(seq 1 200); do
        [ -S "$sock" ] && break
        sleep 0.01
    done
    if [ ! -S "$sock" ]; then
        bad "$tag: router never listened"
        return 1
    fi

    "$GVM" "$sock" "$PERIODS" "$@" > "$WORK/${tag}_gvm.log" 2>&1
    local gvm_rc=$?
    wait "$pvm_pid"
    local pvm_rc=$?

    rm -f "$sock"
    echo "--- $tag gvm ---"
    cat "$WORK/${tag}_gvm.log"
    echo "--- $tag pvm ---"
    cat "$WORK/${tag}_pvm.log"

    [ "$gvm_rc" -eq 0 ] || bad "$tag: gvm exit $gvm_rc"
    [ "$pvm_rc" -eq 0 ] || bad "$tag: pvm exit $pvm_rc"
    return 0
}

echo "=== scenario A: state machine, no underrun ==="
if run_pair normal --check-states; then
    grep -q '\[ok\]   write in setup refused' "$WORK/normal_gvm.log" \
        && ok "write before prepare refused" || bad "write before prepare was accepted"
    grep -q '\[ok\]   start in setup refused' "$WORK/normal_gvm.log" \
        && ok "start before prepare refused" || bad "start before prepare was accepted"
    grep -q '\[ok\]   prepare while running refused' "$WORK/normal_gvm.log" \
        && ok "prepare while running refused" || bad "prepare while running was accepted"

    gvm_frames=$(sed -n 's/^GVM_STATS frames=\([0-9]*\).*/\1/p' "$WORK/normal_gvm.log")
    gvm_xruns=$(sed -n 's/^GVM_STATS .* xruns=\([0-9]*\).*/\1/p' "$WORK/normal_gvm.log")
    pvm_under=$(sed -n 's/^PVM_gvm_STATS .* underruns=\([0-9]*\).*/\1/p' "$WORK/normal_pvm.log")

    [ "$gvm_frames" = "$EXPECTED_FRAMES" ] && ok "wrote $gvm_frames frames" \
        || bad "wrote ${gvm_frames:-none}, expected $EXPECTED_FRAMES"
    [ "${gvm_xruns:-1}" -eq 0 ] && ok "client saw 0 xruns" \
        || bad "client saw ${gvm_xruns:-?} xruns"
    [ "${pvm_under:-1}" -eq 0 ] && ok "device saw 0 underruns" \
        || bad "device saw ${pvm_under:-?} underruns"
fi

echo
echo "=== scenario B: stalled application must report an underrun ==="
if run_pair stall --stall; then
    gvm_xruns=$(sed -n 's/^GVM_STATS .* xruns=\([0-9]*\).*/\1/p' "$WORK/stall_gvm.log")
    pvm_under=$(sed -n 's/^PVM_gvm_STATS .* underruns=\([0-9]*\).*/\1/p' "$WORK/stall_pvm.log")

    [ "${gvm_xruns:-0}" -ge 1 ] 2>/dev/null && ok "client reported $gvm_xruns xrun(s)" \
        || bad "client reported no xrun despite the stall"
    [ "${pvm_under:-0}" -ge 1 ] 2>/dev/null && ok "device reported $pvm_under underrun(s)" \
        || bad "device reported no underrun despite the stall"
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "pcm state machine: ok"
    exit 0
fi
echo "pcm state machine: FAILED ($failures)"
exit 1
