#!/usr/bin/env bash
#
# multicore_ipc_sim — one-command demo.
#
#   ./scripts/demo.sh            # everything
#   ./scripts/demo.sh sim        # three processes, clean run
#   ./scripts/demo.sh fault      # clock jitter + frame drops + app stall
#   ./scripts/demo.sh alsa       # real ALSA output (WSLg pulse by default)
#
# Every scenario prints the numbers that matter: how many frames travelled each
# hop, where they were lost, and whether the wall clock matched the audio.

set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1

BUILD="${BUILD_DIR:-build}"
MODE="${1:-all}"
DEVICE="${ALSA_DEVICE:-pulse}"
WORK="$(mktemp -d)"
failures=0

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

bold() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
note() { printf '   %s\n' "$1"; }
ok()   { printf '   [ok]   %s\n' "$1"; }
bad()  { printf '   [FAIL] %s\n' "$1"; failures=$((failures + 1)); }

ensure_build() {
    if [ ! -x "$BUILD/gvm_app" ] || [ ! -x "$BUILD/pvm_router" ] || [ ! -x "$BUILD/dsp_engine" ]; then
        note "building into $BUILD ..."
        cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug >/dev/null || { bad "cmake configure"; exit 1; }
        cmake --build "$BUILD" --parallel >/dev/null || { bad "build"; exit 1; }
    fi
}

wait_for_socket() {
    for _ in $(seq 1 300); do
        [ -S "$1" ] && return 0
        sleep 0.01
    done
    return 1
}

# run_triad <tag> <gvm args...> -- <pvm args...>
run_triad() {
    local tag="$1"; shift
    local gvm_args=()
    while [ "$1" != "--" ]; do gvm_args+=("$1"); shift; done
    shift

    local sock="/tmp/mc_demo_${tag}_$$.sock"
    rm -f "$sock" "$sock.dsp"
    "$BUILD/pvm_router" "$sock" "$@" > "$WORK/${tag}_pvm.log" 2>&1 &
    local pvm=$!
    if ! wait_for_socket "$sock"; then
        bad "$tag: pvm never listened"
        return 1
    fi
    "$BUILD/dsp_engine" "$sock" > "$WORK/${tag}_dsp.log" 2>&1 &
    local dsp=$!
    sleep 0.2

    "$BUILD/gvm_app" "$sock" "${gvm_args[@]}" > "$WORK/${tag}_gvm.log" 2>&1
    local gvm_rc=$?
    wait "$pvm"; local pvm_rc=$?
    wait "$dsp"; local dsp_rc=$?

    rm -f "$sock" "$sock.dsp"

    [ "$gvm_rc" -eq 0 ] || bad "$tag: gvm exit $gvm_rc"
    [ "$pvm_rc" -eq 0 ] || bad "$tag: pvm exit $pvm_rc"
    [ "$dsp_rc" -eq 0 ] || bad "$tag: dsp exit $dsp_rc"

    grep -E '^GVM_STATS|^PVM_gvm_STATS|^PVM_dsp_STATS|^DSP_STATS|^DSP: rms|^DSP: peak' \
        "$WORK/${tag}_gvm.log" "$WORK/${tag}_pvm.log" "$WORK/${tag}_dsp.log" 2>/dev/null |
        sed 's/^[^:]*log://' | sed 's/^/   /'
}

scenario_sim() {
    bold "1/3  three processes, clean run (2.0 s of 440 Hz)"
    note "GVM writes as fast as it can; the device consumes one period every 20 ms."
    run_triad sim -- 
    note ""
    note "Expect: every hop reports 96000 frames, 0 drops, rms out/in ~= 1.0"
}

scenario_fault() {
    bold "2/3  fault injection (jitter + 15% drops + the app stalls 250 ms)"
    note "The device injects +/-600 us clock jitter, drops 15% of the frames it"
    note "forwards, and the application stops feeding for 250 ms."
    run_triad fault --stall -- --stats --stats-every 50 --jitter-us 600 --drop-pct 15
    note ""
    note "Expect: underruns appear in the stall, drops grow, DSP receives less"
    note "than the application wrote — and every process still exits cleanly."
    grep '^PVM_STAT' "$WORK/fault_pvm.log" 2>/dev/null | tail -3 | sed 's/^/   /'
}

scenario_alsa() {
    bold "3/3  real ALSA output (device: $DEVICE)"
    if ! command -v aplay >/dev/null 2>&1; then
        note "alsa-utils not installed; skipping"
        return 0
    fi
    "$BUILD/gvm_app" --backend alsa --device "$DEVICE" 50 > "$WORK/alsa.log" 2>&1
    local rc=$?
    grep -E '^GVM: backend|^GVM_STATS|^GVM: audio' "$WORK/alsa.log" | sed 's/^/   /'
    [ "$rc" -eq 0 ] && ok "alsa playback exit 0" || bad "alsa playback exit $rc"

    local elapsed
    elapsed=$(sed -n 's/^GVM_STATS .* elapsed_ms=\([0-9.]*\).*/\1/p' "$WORK/alsa.log")
    if [ -n "${elapsed:-}" ]; then
        awk -v e="$elapsed" 'BEGIN { r = e / 1000.0; exit (r > 0.85 && r < 1.60) ? 0 : 1 }' \
            && ok "1.000 s of audio took ${elapsed} ms of wall time (device paced it)" \
            || bad "wall time ${elapsed} ms does not match 1.000 s of audio"
    fi
    note ""
    note "If you heard a 440 Hz tone, the whole chain is real: libasound -> PulseAudio."
}

ensure_build

case "$MODE" in
    sim)   scenario_sim ;;
    fault) scenario_fault ;;
    alsa)  scenario_alsa ;;
    all)
        scenario_sim
        scenario_fault
        scenario_alsa
        ;;
    *)
        echo "usage: $0 [all|sim|fault|alsa]" >&2
        exit 2
        ;;
esac

echo
if [ "$failures" -eq 0 ]; then
    printf '\033[1mdemo: ok\033[0m\n'
    exit 0
fi
printf '\033[1mdemo: FAILED (%d)\033[0m\n' "$failures"
exit 1
