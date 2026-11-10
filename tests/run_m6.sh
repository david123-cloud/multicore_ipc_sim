#!/usr/bin/env bash
#
# M6 acceptance: the real ALSA backend.
#
#   bash tests/run_m6.sh <gvm_app> [device]
#
# Plays exactly 1.000 s of audio (50 periods of 960 frames at 48 kHz) through
# libasound and checks that:
#   - the state machine refuses illegal calls on this backend too
#   - the wall clock matches the audio duration, which proves the device really
#     paces the writer instead of accepting everything instantly
#   - drain and close complete without error
#
# The default device is "pulse" (WSLg -> Windows audio).  On a machine with a
# sound card, pass hw:0,0 instead and the same test applies.

set -uo pipefail

GVM="${1:?usage: run_m6.sh <gvm_app> [device]}"
DEVICE="${2:-pulse}"

WORK="$(mktemp -d)"
PERIODS=50
AUDIO_MS=1000
failures=0

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

ok() { printf '  [ok]   %s\n' "$1"; }
bad() { printf '  [FAIL] %s\n' "$1"; failures=$((failures + 1)); }

"$GVM" --backend alsa --device "$DEVICE" "$PERIODS" --check-states \
    > "$WORK/gvm.log" 2>&1
GVM_RC=$?

cat "$WORK/gvm.log"

[ "$GVM_RC" -eq 0 ] && ok "gvm exit 0 on the alsa backend" || bad "gvm exit $GVM_RC"

grep -q '\[ok\]   write in setup refused' "$WORK/gvm.log" \
    && ok "write before prepare refused" || bad "write before prepare was accepted"
grep -q '\[ok\]   prepare while running refused' "$WORK/gvm.log" \
    && ok "prepare while running refused" || bad "prepare while running was accepted"

frames=$(sed -n 's/^GVM_STATS frames=\([0-9]*\).*/\1/p' "$WORK/gvm.log")
xruns=$(sed -n 's/^GVM_STATS .* xruns=\([0-9]*\).*/\1/p' "$WORK/gvm.log")
elapsed=$(sed -n 's/^GVM_STATS .* elapsed_ms=\([0-9.]*\).*/\1/p' "$WORK/gvm.log")

[ "$frames" = "$((PERIODS * 960))" ] && ok "wrote $frames frames" \
    || bad "wrote ${frames:-none}, expected $((PERIODS * 960))"

if [ -n "${elapsed:-}" ]; then
    printf '  audio %d ms, wall %.1f ms\n' "$AUDIO_MS" "$elapsed"
    awk -v e="$elapsed" -v a="$AUDIO_MS" \
        'BEGIN { r = e / a; exit (r > 0.85 && r < 1.60) ? 0 : 1 }' \
        && ok "device paced the writer in real time" \
        || bad "wall time ${elapsed} ms does not match 1000 ms of audio"
else
    bad "no timing reported"
fi

[ "${xruns:-0}" -eq 0 ] && ok "no underrun while writing steadily" \
    || bad "unexpected ${xruns:-?} underrun(s)"

echo
if [ "$failures" -eq 0 ]; then
    echo "alsa backend: ok"
    exit 0
fi
echo "alsa backend: FAILED ($failures)"
exit 1
