#!/usr/bin/env bash
#
# M4 acceptance: three processes, two hops.
#
#   GVM --ring A--> PVM bridge --ring B--> DSP (mini_dsp pipeline)
#
# Checks that the tone survives both hops and the DSP pipeline:
#   - every frame the application wrote reached the DSP
#   - the bridge dropped nothing
#   - the high-pass at 100 Hz leaves the 440 Hz tone essentially untouched

set -uo pipefail

PVM="${1:?usage: run_m4.sh <pvm_router> <gvm_app> <dsp_engine>}"
GVM="${2:?usage: run_m4.sh <pvm_router> <gvm_app> <dsp_engine>}"
DSP="${3:?usage: run_m4.sh <pvm_router> <gvm_app> <dsp_engine>}"

WORK="$(mktemp -d)"
SOCK="/tmp/mc_ipc_m4_$$.sock"
PERIODS=100
EXPECTED_FRAMES=$((PERIODS * 960))
failures=0

cleanup() {
    rm -rf "$WORK"
    rm -f "$SOCK" "$SOCK.dsp"
}
trap cleanup EXIT

ok() { printf '  [ok]   %s\n' "$1"; }
bad() { printf '  [FAIL] %s\n' "$1"; failures=$((failures + 1)); }

"$PVM" "$SOCK" > "$WORK/pvm.log" 2>&1 &
PVM_PID=$!

for _ in $(seq 1 200); do
    [ -S "$SOCK" ] && break
    sleep 0.01
done

"$DSP" "$SOCK" > "$WORK/dsp.log" 2>&1 &
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

gvm_frames=$(sed -n 's/^GVM_STATS frames=\([0-9]*\).*/\1/p' "$WORK/gvm.log")
dsp_frames=$(sed -n 's/^DSP_STATS frames=\([0-9]*\).*/\1/p' "$WORK/dsp.log")
dsp_drops=$(sed -n 's/^PVM_dsp_STATS .* drops=\([0-9]*\).*/\1/p' "$WORK/pvm.log")
rms_line=$(sed -n 's/^DSP: rms in \([0-9.]*\) out \([0-9.]*\).*/\1 \2/p' "$WORK/dsp.log")
rms_in=${rms_line%% *}
rms_out=${rms_line##* }

[ "$gvm_frames" = "$EXPECTED_FRAMES" ] && ok "app wrote $gvm_frames frames" \
    || bad "app wrote ${gvm_frames:-none}, expected $EXPECTED_FRAMES"
[ "$dsp_frames" = "$EXPECTED_FRAMES" ] && ok "dsp received $dsp_frames frames" \
    || bad "dsp received ${dsp_frames:-none}, expected $EXPECTED_FRAMES"
[ "${dsp_drops:-1}" -eq 0 ] && ok "bridge dropped 0 frames" \
    || bad "bridge dropped ${dsp_drops:-?} frames"

if [ -n "${rms_in:-}" ] && [ -n "${rms_out:-}" ]; then
    ratio=$(awk -v a="$rms_in" -v b="$rms_out" 'BEGIN { if (a > 0) printf "%.4f", b / a; else print "0" }')
    printf '  ratio out/in = %s (rms in %s, out %s)\n' "$ratio" "$rms_in" "$rms_out"
    awk -v r="$ratio" 'BEGIN { exit (r > 0.9 && r < 1.1) ? 0 : 1 }' \
        && ok "tone passes the 100 Hz high-pass at unity gain" \
        || bad "unexpected gain through the DSP pipeline"
else
    bad "dsp did not report RMS"
fi

echo
# Spectral check: the DSP analyses channel 0 with a 1024-point FFT, so a
# 440 Hz tone must land on a bin centre near 440 Hz (461.9/421.9 at 46.9 Hz
# resolution are both acceptable, anything further off is a real problem).
peak_hz=$(sed -n 's/^DSP: peak bin [0-9]* -> \([0-9.]*\) Hz.*/\1/p' "$WORK/dsp.log")
if [ -n "${peak_hz:-}" ]; then
    printf '  dsp peak frequency = %s Hz\n' "$peak_hz"
    awk -v f="$peak_hz" 'BEGIN { d = f - 440.0; if (d < 0) d = -d; exit (d <= 30.0) ? 0 : 1 }' \
        && ok "spectrum peaks near 440 Hz" \
        || bad "spectrum peak at ${peak_hz} Hz is too far from 440 Hz"
else
    bad "dsp did not report a peak frequency"
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "three-process pipeline: ok"
    exit 0
fi
echo "three-process pipeline: FAILED ($failures)"
exit 1
