#!/usr/bin/env bash
#
# M8 acceptance: the direct topology.
#
#   bash tests/run_m8.sh <pvm_router> <gvm_app> <dsp_engine>
#
# The application owns the audio segment, the host only describes and relays,
# and the DSP consumes it under its own clock.  Two things must hold:
#   - the chain still transfers every frame
#   - there is exactly one data segment, and no second one is created

set -uo pipefail

PVM="${1:?usage: run_m8.sh <pvm_router> <gvm_app> <dsp_engine>}"
GVM="${2:?usage: run_m8.sh <pvm_router> <gvm_app> <dsp_engine>}"
DSP="${3:?usage: run_m8.sh <pvm_router> <gvm_app> <dsp_engine>}"

WORK="$(mktemp -d)"
SOCK="/tmp/mc_m8_$$.sock"
PERIODS=100
EXPECTED=$((PERIODS * 960))
failures=0

cleanup() {
    rm -rf "$WORK"
    rm -f "$SOCK" "$SOCK.dsp"
}
trap cleanup EXIT

ok() { printf '  [ok]   %s\n' "$1"; }
bad() { printf '  [FAIL] %s\n' "$1"; failures=$((failures + 1)); }

"$PVM" "$SOCK" --topology direct > "$WORK/pvm.log" 2>&1 &
PVM_PID=$!
for _ in $(seq 1 200); do [ -S "$SOCK" ] && break; sleep 0.01; done

"$DSP" "$SOCK" --topology direct > "$WORK/dsp.log" 2>&1 &
DSP_PID=$!
sleep 0.2

"$GVM" "$SOCK" "$PERIODS" > "$WORK/gvm.log" 2>&1
GVM_RC=$?
wait "$PVM_PID"; PVM_RC=$?
wait "$DSP_PID"; DSP_RC=$?

echo "--- gvm ---"; cat "$WORK/gvm.log"
echo "--- pvm ---"; cat "$WORK/pvm.log"
echo "--- dsp ---"; cat "$WORK/dsp.log"

[ "$GVM_RC" -eq 0 ] && ok "gvm exit 0" || bad "gvm exit $GVM_RC"
[ "$PVM_RC" -eq 0 ] && ok "pvm exit 0" || bad "pvm exit $PVM_RC"
[ "$DSP_RC" -eq 0 ] && ok "dsp exit 0" || bad "dsp exit $DSP_RC"

grep -q "attached the same segment" "$WORK/pvm.log" \
    && ok "the host described the application's segment instead of building its own" \
    || bad "the direct attach path was not taken"

grep -q "dsp end created" "$WORK/pvm.log" \
    && bad "a second data segment was created" \
    || ok "no second data segment was created"

dsp_frames=$(sed -n 's/^DSP_STATS frames=\([0-9]*\).*/\1/p' "$WORK/dsp.log")
gvm_xruns=$(sed -n 's/^GVM_STATS .* xruns=\([0-9]*\).*/\1/p' "$WORK/gvm.log")

[ "$dsp_frames" = "$EXPECTED" ] && ok "dsp consumed $dsp_frames frames directly" \
    || bad "dsp consumed ${dsp_frames:-none}, expected $EXPECTED"
[ "${gvm_xruns:-1}" -eq 0 ] && ok "no underrun seen by the application" \
    || bad "application saw ${gvm_xruns:-?} underruns"

# The DSP owns the clock in this topology, so its pacing must show up as real
# wall time rather than an instant drain of the ring.
elapsed=$(sed -n 's/^GVM_STATS .* elapsed_ms=\([0-9.]*\).*/\1/p' "$WORK/gvm.log")
if [ -n "${elapsed:-}" ]; then
    awk -v e="$elapsed" 'BEGIN { exit (e > 1500) ? 0 : 1 }' \
        && ok "2.0 s of audio took ${elapsed} ms (DSP clock paced the writer)" \
        || bad "wall time ${elapsed} ms is too short for 2.0 s of audio"
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "direct topology: ok"
    exit 0
fi
echo "direct topology: FAILED ($failures)"
exit 1
