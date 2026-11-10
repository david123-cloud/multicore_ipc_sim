#!/usr/bin/env bash
#
# M5b acceptance: observation panel and fault injection on the simulated PCM
# device.
#
#   bash tests/run_m5.sh <pvm_router> <gvm_app> <dsp_engine>
#
# The device is driven with clock jitter, a periodic frame-drop injection, and
# the application stalls once.  We expect:
#   - a CSV statistics row per N periods
#   - underruns reported (the application stalled)
#   - drops reported (injected loss on the second hop)
#   - the DSP to receive strictly fewer frames than the application wrote
#   - every process to exit cleanly anyway

set -uo pipefail

PVM="${1:?usage: run_m5.sh <pvm_router> <gvm_app> <dsp_engine>}"
GVM="${2:?usage: run_m5.sh <pvm_router> <gvm_app> <dsp_engine>}"
DSP="${3:?usage: run_m5.sh <pvm_router> <gvm_app> <dsp_engine>}"

WORK="$(mktemp -d)"
SOCK="/tmp/mc_ipc_m5_$$.sock"
PERIODS=60
WRITTEN=$((PERIODS * 960))
failures=0

cleanup() {
    rm -rf "$WORK"
    rm -f "$SOCK" "$SOCK.dsp"
}
trap cleanup EXIT

ok() { printf '  [ok]   %s\n' "$1"; }
bad() { printf '  [FAIL] %s\n' "$1"; failures=$((failures + 1)); }

"$PVM" "$SOCK" --stats --stats-every 10 --jitter-us 600 --drop-pct 15 \
    > "$WORK/pvm.log" 2>&1 &
PVM_PID=$!

for _ in $(seq 1 200); do
    [ -S "$SOCK" ] && break
    sleep 0.01
done

"$DSP" "$SOCK" > "$WORK/dsp.log" 2>&1 &
DSP_PID=$!
sleep 0.2

"$GVM" "$SOCK" "$PERIODS" --stall > "$WORK/gvm.log" 2>&1
GVM_RC=$?
wait "$PVM_PID"; PVM_RC=$?
wait "$DSP_PID"; DSP_RC=$?

echo "--- gvm ---"; cat "$WORK/gvm.log"
echo "--- pvm (stats rows) ---"; grep '^PVM_STAT ' "$WORK/pvm.log" || true
echo "--- pvm (summary) ---"; grep '^PVM_' "$WORK/pvm.log" || true
echo "--- dsp ---"; cat "$WORK/dsp.log"

[ "$GVM_RC" -eq 0 ] && ok "gvm exit 0" || bad "gvm exit $GVM_RC"
[ "$PVM_RC" -eq 0 ] && ok "pvm exit 0" || bad "pvm exit $PVM_RC"
[ "$DSP_RC" -eq 0 ] && ok "dsp exit 0" || bad "dsp exit $DSP_RC"

rows=$(grep -c '^PVM_STAT ' "$WORK/pvm.log" || true)
[ "${rows:-0}" -ge 3 ] && ok "statistics panel emitted $rows rows" \
    || bad "statistics panel emitted ${rows:-0} rows, expected at least 3"

xruns=$(sed -n 's/^PVM_gvm_STATS .* underruns=\([0-9]*\).*/\1/p' "$WORK/pvm.log")
drops=$(sed -n 's/^PVM_dsp_STATS .* drops=\([0-9]*\).*/\1/p' "$WORK/pvm.log")
dsp_frames=$(sed -n 's/^DSP_STATS frames=\([0-9]*\).*/\1/p' "$WORK/dsp.log")

[ "${xruns:-0}" -ge 1 ] 2>/dev/null && ok "device reported $xruns underrun(s) from the app stall" \
    || bad "no underrun reported despite the stall"
[ "${drops:-0}" -ge 960 ] 2>/dev/null && ok "bridge dropped $drops frames from the injection" \
    || bad "drop injection produced ${drops:-0} drops"
[ "${dsp_frames:-0}" -lt "$WRITTEN" ] 2>/dev/null && ok "dsp received $dsp_frames < $WRITTEN frames" \
    || bad "dsp received ${dsp_frames:-0}, expected fewer than $WRITTEN"

echo
if [ "$failures" -eq 0 ]; then
    echo "fault injection: ok"
    exit 0
fi
echo "fault injection: FAILED ($failures)"
exit 1
