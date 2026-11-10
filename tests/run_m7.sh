#!/usr/bin/env bash
#
# M7 acceptance: the channel count is a parameter of the whole path.
#
#   bash tests/run_m7.sh <pvm_router> <gvm_app> <dsp_engine>
#
# Runs the three-process chain at 4 and 8 channels and checks that the frames,
# the per-channel energy and the DSP pipeline all agree.  A channel-count
# mismatch between the application and the DSP must be refused by the bridge.

set -uo pipefail

PVM="${1:?usage: run_m7.sh <pvm_router> <gvm_app> <dsp_engine>}"
GVM="${2:?usage: run_m7.sh <pvm_router> <gvm_app> <dsp_engine>}"
DSP="${3:?usage: run_m7.sh <pvm_router> <gvm_app> <dsp_engine>}"

WORK="$(mktemp -d)"
PERIODS=40
failures=0

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

ok() { printf '  [ok]   %s\n' "$1"; }
bad() { printf '  [FAIL] %s\n' "$1"; failures=$((failures + 1)); }

# run_case <channels> [dsp_channels] -> writes logs, echoes nothing
run_case() {
    local channels="$1"
    local dsp_channels="${2:-$1}"
    local tag="ch${channels}_dsp${dsp_channels}"
    local sock="/tmp/mc_m7_${tag}_$$.sock"

    rm -f "$sock" "$sock.dsp"
    "$PVM" "$sock" > "$WORK/${tag}_pvm.log" 2>&1 &
    local pvm=$!
    for _ in $(seq 1 200); do [ -S "$sock" ] && break; sleep 0.01; done
    "$DSP" "$sock" --channels "$dsp_channels" > "$WORK/${tag}_dsp.log" 2>&1 &
    local dsp=$!
    sleep 0.2
    "$GVM" "$sock" "$PERIODS" --channels "$channels" > "$WORK/${tag}_gvm.log" 2>&1
    local gvm_rc=$?
    wait "$pvm"; local pvm_rc=$?
    wait "$dsp"; local dsp_rc=$?
    rm -f "$sock" "$sock.dsp"

    echo "$gvm_rc $pvm_rc $dsp_rc"
}

for channels in 4 8; do
    echo "=== $channels channels ==="
    read -r gvm_rc pvm_rc dsp_rc <<<"$(run_case "$channels")"
    tag="ch${channels}_dsp${channels}"

    [ "$gvm_rc" -eq 0 ] && ok "gvm exit 0" || bad "gvm exit $gvm_rc"
    [ "$pvm_rc" -eq 0 ] && ok "pvm exit 0" || bad "pvm exit $pvm_rc"
    [ "$dsp_rc" -eq 0 ] && ok "dsp exit 0" || bad "dsp exit $dsp_rc"

    expected=$((PERIODS * 960))
    dsp_frames=$(sed -n 's/^DSP_STATS frames=\([0-9]*\).*/\1/p' "$WORK/${tag}_dsp.log")
    dsp_channels=$(sed -n 's/^DSP_STATS .* channels=\([0-9]*\).*/\1/p' "$WORK/${tag}_dsp.log")
    ratio=$(sed -n 's/^DSP: rms in \([0-9.]*\) out \([0-9.]*\).*/\1 \2/p' "$WORK/${tag}_dsp.log")
    rms_in=${ratio%% *}
    rms_out=${ratio##* }

    [ "$dsp_frames" = "$expected" ] && ok "dsp received $dsp_frames frames" \
        || bad "dsp received ${dsp_frames:-none}, expected $expected"
    [ "$dsp_channels" = "$channels" ] && ok "dsp saw $dsp_channels channels" \
        || bad "dsp saw ${dsp_channels:-?} channels, expected $channels"

    if [ -n "${rms_in:-}" ] && [ -n "${rms_out:-}" ]; then
        g=$(awk -v a="$rms_in" -v b="$rms_out" 'BEGIN { if (a > 0) printf "%.4f", b / a; else print "0" }')
        printf '  rms in %s out %s (ratio %s)\n' "$rms_in" "$rms_out" "$g"
        awk -v r="$g" 'BEGIN { exit (r > 0.9 && r < 1.1) ? 0 : 1 }' \
            && ok "per-channel energy preserved through the chain" \
            || bad "unexpected gain ${g}"
    else
        bad "dsp did not report RMS"
    fi
done

echo
echo "=== geometry mismatch must be refused ==="
read -r gvm_rc _pvm_rc _dsp_rc <<<"$(run_case 2 4)"
tag="ch2_dsp4"
if grep -q "geometry mismatch" "$WORK/${tag}_pvm.log" 2>/dev/null; then
    ok "bridge refused a 2ch app with a 4ch dsp"
else
    bad "the mismatch was not detected"
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "channel parameterisation: ok"
    exit 0
fi
echo "channel parameterisation: FAILED ($failures)"
exit 1
