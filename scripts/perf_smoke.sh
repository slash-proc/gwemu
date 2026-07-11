#!/usr/bin/env bash
#
# perf_smoke.sh - perf-based regression smoke test for qemu-gnw device models.
#
# Automates the manual workflow used to find the LTDC "one CPU core pegged"
# bug: boot a real ROM, `perf record` a few seconds of steady-state
# execution, `perf report` it, and flag if any of THIS REPO'S OWN
# gnw_h7b0_* functions is eating a suspiciously large share of total
# self-time. Upstream QEMU/TCG/glib/pthread internals are expected to
# dominate a healthy profile and are never flagged, regardless of their
# percentage.
#
# See CLAUDE.md and docs/session-2026-07-10-part5-flicker-speed-dma-battery.md
# for background on the bug class this catches (gnw_h7b0_ltdc_update_display
# redoing a full unconditional per-pixel redraw every UI-refresh tick).
#
# Usage (flash-only boot, extflash loader device):
#   scripts/perf_smoke.sh --qemu build/qemu-system-arm -- \
#     -M gnw-h7b0 -kernel retro-go-bank1-flash.bin \
#     -device loader,file=retro-go-bank1-flash-extflash.bin,addr=0x90000000
#
# Usage (SD-card-backed boot):
#   scripts/perf_smoke.sh --qemu build/qemu-system-arm -- \
#     -M gnw-h7b0 -kernel gw_retro_go_intflash.bin \
#     -drive if=sd,format=qcow2,file=/tmp/sd-overlay.qcow2
#
# Everything after `--` is passed through verbatim as the qemu invocation's
# board/kernel/loader/drive args. -display sdl and -audio driver=alsa are
# added automatically (per this project's established "always visible
# display" test convention) unless already present in the passthrough args.
#
# Options (before `--`):
#   --qemu PATH       Path to qemu-system-arm binary (default: build/qemu-system-arm)
#   --duration N      Seconds of perf record sampling (default: 5)
#   --boot-wait N     Seconds to wait for boot before sampling (default: 8)
#   --threshold PCT   Flag threshold for gnw_h7b0_* self-time percent (default: 10)
#   --top N           Number of top symbols to print in the report (default: 20)
#   -h, --help        Show this help and exit
#
# Exit status: 0 = pass (no gnw_h7b0_* symbol over threshold), 1 = fail
# (flagged), 2 = usage/environment error (missing perf, bad args, boot
# failure, etc).

set -u -o pipefail

QEMU_BIN="build/qemu-system-arm"
DURATION=5
BOOT_WAIT=8
THRESHOLD=10
TOP_N=20
QEMU_ARGS=()

usage() {
    sed -n '2,/^set -u/p' "$0" | sed 's/^# \{0,1\}//' | sed '$d'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --qemu)
            QEMU_BIN="$2"; shift 2 ;;
        --duration)
            DURATION="$2"; shift 2 ;;
        --boot-wait)
            BOOT_WAIT="$2"; shift 2 ;;
        --threshold)
            THRESHOLD="$2"; shift 2 ;;
        --top)
            TOP_N="$2"; shift 2 ;;
        -h|--help)
            usage; exit 0 ;;
        --)
            shift
            QEMU_ARGS=("$@")
            break ;;
        *)
            echo "error: unrecognized option '$1' (did you forget '--' before qemu args?)" >&2
            usage
            exit 2 ;;
    esac
done

if [[ ${#QEMU_ARGS[@]} -eq 0 ]]; then
    echo "error: no qemu args given after '--' (need at least -M gnw-h7b0 -kernel <bin>)" >&2
    usage
    exit 2
fi

if ! command -v perf >/dev/null 2>&1; then
    echo "error: 'perf' is not installed or not on PATH. Install linux-tools / perf and retry." >&2
    exit 2
fi

if [[ ! -x "$QEMU_BIN" ]]; then
    echo "error: qemu binary not found or not executable: $QEMU_BIN (pass --qemu PATH)" >&2
    exit 2
fi

# Add -display sdl / -audio driver=alsa unless the caller already specified
# a -display or -audio option (respect explicit overrides).
have_display=0
have_audio=0
for a in "${QEMU_ARGS[@]}"; do
    [[ "$a" == "-display" ]] && have_display=1
    [[ "$a" == "-audio" ]] && have_audio=1
done
FULL_ARGS=("${QEMU_ARGS[@]}")
if [[ $have_display -eq 0 ]]; then
    FULL_ARGS+=(-display sdl)
fi
if [[ $have_audio -eq 0 ]]; then
    FULL_ARGS+=(-audio driver=alsa)
fi

TMPDIR_WORK=$(mktemp -d /tmp/perf_smoke.XXXXXX)
PERF_DATA="$TMPDIR_WORK/perf.data"
QEMU_LOG="$TMPDIR_WORK/qemu.log"
QEMU_PID=""

cleanup() {
    local ec=$?
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null
        wait "$QEMU_PID" 2>/dev/null
    fi
    rm -rf "$TMPDIR_WORK"
    exit $ec
}
trap cleanup EXIT INT TERM

echo "== perf_smoke: launching qemu-gnw =="
echo "  $QEMU_BIN ${FULL_ARGS[*]}"
"$QEMU_BIN" "${FULL_ARGS[@]}" >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

sleep 1
if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "error: qemu exited immediately. Log:" >&2
    cat "$QEMU_LOG" >&2
    exit 2
fi

echo "== waiting ${BOOT_WAIT}s for boot to settle (pid $QEMU_PID) =="
sleep "$BOOT_WAIT"

if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "error: qemu died during boot wait. Log:" >&2
    cat "$QEMU_LOG" >&2
    exit 2
fi

echo "== recording ${DURATION}s of perf samples =="
if ! perf record -p "$QEMU_PID" -g --call-graph dwarf -o "$PERF_DATA" -- sleep "$DURATION" 2>"$TMPDIR_WORK/perf-record.log"; then
    echo "error: 'perf record' failed. Output:" >&2
    cat "$TMPDIR_WORK/perf-record.log" >&2
    echo "(Common cause: /proc/sys/kernel/perf_event_paranoid too restrictive, or missing CAP_PERFMON.)" >&2
    exit 2
fi

if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "error: qemu died during perf record. Log:" >&2
    cat "$QEMU_LOG" >&2
    exit 2
fi

echo "== analyzing profile =="
REPORT=$(perf report -i "$PERF_DATA" --stdio --sort=overhead,symbol --no-children 2>/dev/null)
if [[ -z "$REPORT" ]]; then
    echo "error: 'perf report' produced no output; perf.data may be empty (permissions? no samples?)" >&2
    exit 2
fi

# perf report --stdio lines of interest look like:
#     35.65%  gnw_h7b0_ltdc_update_display
# or with a comm/dso column depending on perf version:
#     35.65%  qemu-system-a  qemu-system-arm  [.] gnw_h7b0_ltdc_update_display
# Extract (percent, symbol) pairs robustly: first field is "NN.NN%", last
# whitespace-separated field is the symbol name.
declare -a PCTS
declare -a SYMS
while IFS= read -r line; do
    [[ "$line" =~ ^[[:space:]]*# ]] && continue
    [[ "$line" =~ ^[[:space:]]*$ ]] && continue
    pct_field=$(awk '{print $1}' <<<"$line")
    [[ "$pct_field" =~ ^[0-9]+\.[0-9]+%$ ]] || continue
    pct=${pct_field%\%}
    # Column layout: "<pct>%  [.]  <symbol>  <IPC>  [<coverage>]"
    # (or without the "[.]" dso-annotation column, depending on perf version).
    sym=$(awk '{ if ($2 == "[.]") print $3; else print $2 }' <<<"$line")
    PCTS+=("$pct")
    SYMS+=("$sym")
done <<<"$REPORT"

if [[ ${#SYMS[@]} -eq 0 ]]; then
    echo "error: could not parse any symbol/percentage rows out of perf report output" >&2
    exit 2
fi

echo ""
echo "== top $TOP_N symbols by self-time =="
printf '%8s  %s\n' "PCT" "SYMBOL"
for ((i=0; i<${#SYMS[@]} && i<TOP_N; i++)); do
    printf '%7s%%  %s\n' "${PCTS[$i]}" "${SYMS[$i]}"
done
echo ""

FLAGGED=0
FLAG_MSG=""
for ((i=0; i<${#SYMS[@]}; i++)); do
    sym="${SYMS[$i]}"
    pct="${PCTS[$i]}"
    if [[ "$sym" == gnw_h7b0_* ]]; then
        # Compare pct > THRESHOLD using awk (floats).
        over=$(awk -v p="$pct" -v t="$THRESHOLD" 'BEGIN{print (p > t) ? 1 : 0}')
        if [[ "$over" -eq 1 ]]; then
            FLAGGED=1
            FLAG_MSG+=$'\n'"  ${pct}%  ${sym}  (threshold: ${THRESHOLD}%)"
        fi
    fi
done

echo "== perf_smoke summary =="
if [[ $FLAGGED -eq 1 ]]; then
    echo "FAIL: one or more gnw_h7b0_* device-model functions exceeded ${THRESHOLD}% self-time:" >&2
    echo "$FLAG_MSG" >&2
    echo "" >&2
    echo "This is the same signature as the LTDC full-redraw-every-tick bug" >&2
    echo "(gnw_h7b0_ltdc_update_display at 35.65%, fixed by adding dirty tracking)." >&2
    echo "Investigate the flagged function(s) for missing dirty-tracking / busy-polling / unthrottled work." >&2
    trap - EXIT INT TERM
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null
    fi
    rm -rf "$TMPDIR_WORK"
    exit 1
else
    echo "PASS: no gnw_h7b0_* device-model function exceeded ${THRESHOLD}% self-time."
    echo "(Profile shape looks like the expected healthy dominance of TCG/QEMU-core"
    echo "and glib/pthread bookkeeping, not our own device models.)"
    trap - EXIT INT TERM
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null
    fi
    rm -rf "$TMPDIR_WORK"
    exit 0
fi
