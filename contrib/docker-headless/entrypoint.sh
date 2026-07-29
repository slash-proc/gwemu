#!/bin/bash
# gwemu headless entrypoint -- timeline-scripted, capture-capable QEMU runs
# for test suites / CI. Deliberately standalone (NOT derived from
# scripts/boot_qemu.sh). See docs/headless-capture.md.
#
# Layout conventions (override with flags):
#   /images  - firmware images (read-only mount recommended)
#   /out     - artifacts: screenshots, recording, intermediates
set -euo pipefail

QEMU="${GWEMU_BIN:-/usr/local/bin/qemu-system-arm}"
IMAGES="${GWEMU_IMAGES:-/images}"
OUT="${GWEMU_OUT:-/out}"
BANK1= BANK2= EXTFLASH= SD=
TIMELINE= RECORD= FPS=60 DETERMINISTIC=0 KEEP_RAW=0 QMP_PORT=
EXTRA_ARGS=()

usage() {
    cat <<EOF
usage: entrypoint [options]
  --bank1 F --bank2 F --extflash F   firmware images (default: first match of
                                     <game>-bank1.bin etc. under $IMAGES)
  --sd F                             optional SD card image (qcow2 or raw)
  --timeline F                       timeline script (see docs) -- the only
                                     control surface; without it the run goes
                                     until killed
  --record NAME.mp4|.mkv|.flac|...   whole-session capture, muxed on exit
  --fps N                            recording frame rate (default 60)
  --deterministic                    -icount shift=auto,sleep=off (experimental)
  --qmp-port N                       expose QMP on tcp:0.0.0.0:N (debug only)
  --keep-raw                         keep .frames/.wav intermediates
  -- ...                             pass remaining args to qemu verbatim
EOF
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
    --bank1) BANK1=$2; shift 2;;
    --bank2) BANK2=$2; shift 2;;
    --extflash) EXTFLASH=$2; shift 2;;
    --sd) SD=$2; shift 2;;
    --timeline) TIMELINE=$2; shift 2;;
    # Timeline recording (GNW_TIMELINE_RECORD) captures LIVE keystrokes, so it
    # needs both a display backend delivering input events and a human watching
    # the video to know when to press. This entrypoint hardcodes -display none
    # below, and a container has no live video output to watch, so recording
    # here can only ever write an empty file. Reject it loudly rather than let
    # a run look like it captured something. Record timelines with the GUI
    # (-display gwemu), then replay them here with --timeline.
    --record-timeline)
        echo "error: --record-timeline does not work headless -- it records live" \
             "keystrokes, and this container runs -display none with no video to" \
             "watch. Record with the GUI (-display gwemu, GNW_TIMELINE_RECORD=F)," \
             "then replay the script here with --timeline." >&2
        exit 1;;
    --record) RECORD=$2; shift 2;;
    --fps) FPS=$2; shift 2;;
    --deterministic) DETERMINISTIC=1; shift;;
    --qmp-port) QMP_PORT=$2; shift 2;;
    --keep-raw) KEEP_RAW=1; shift;;
    --) shift; EXTRA_ARGS=("$@"); break;;
    *) usage;;
    esac
done

# Default image discovery: exactly one *-bank1.bin triple in /images.
if [ -z "$BANK1" ]; then
    mapfile -t b1 < <(ls "$IMAGES"/*-bank1*.bin 2>/dev/null | head -2)
    if [ ${#b1[@]} -ne 1 ]; then
        echo "error: pass --bank1/--bank2/--extflash (auto-detect needs" \
             "exactly one *-bank1*.bin in $IMAGES)" >&2
        exit 1
    fi
    BANK1=${b1[0]}
    base=$(basename "$BANK1"); game=${base%%-bank1*}
    BANK2=$(ls "$IMAGES/$game"-bank2*.bin 2>/dev/null | head -1)
    EXTFLASH=$(ls "$IMAGES/$game"-extflash*.bin 2>/dev/null | head -1)
fi
for f in "$BANK1" "$BANK2" "$EXTFLASH"; do
    [ -f "$f" ] || { echo "error: missing image: $f" >&2; exit 1; }
done

mkdir -p "$OUT"

# The machine's persistent-flash backing opens images read-write (guest
# flash writes land in the files). CI mounts are read-only -- and even
# writable mounts would leak state between runs, which is exactly what a
# reproducibility appliance must not do. Work on pristine scratch copies.
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
cp "$BANK1" "$SCRATCH/bank1.bin"; BANK1="$SCRATCH/bank1.bin"
cp "$BANK2" "$SCRATCH/bank2.bin"; BANK2="$SCRATCH/bank2.bin"
cp "$EXTFLASH" "$SCRATCH/extflash.bin"; EXTFLASH="$SCRATCH/extflash.bin"
if [ -n "$SD" ]; then
    cp "$SD" "$SCRATCH/sd.img"; SD="$SCRATCH/sd.img"
fi

ARGS=(-M gnw-h7b0 -display none
      -global gnw-h7b0-soc.bank1-image="$BANK1"
      -global gnw-h7b0-soc.bank2-image="$BANK2"
      -global gnw-h7b0-soc.extflash-image="$EXTFLASH")
[ -n "$SD" ] && ARGS+=(-drive if=sd,file="$SD")
[ -n "$QMP_PORT" ] && ARGS+=(-qmp tcp:0.0.0.0:"$QMP_PORT",server,nowait)
[ "$DETERMINISTIC" = 1 ] && ARGS+=(-icount shift=auto,sleep=off)

export GNW_OUT="$OUT"
[ -n "$TIMELINE" ] && export GNW_TIMELINE="$TIMELINE"

BASENAME=
if [ -n "$RECORD" ]; then
    BASENAME="$OUT/.rec-$$"
    export GNW_RECORD="$BASENAME" GNW_RECORD_FPS="$FPS"
    ARGS+=(-audiodev wav,id=snd0,path="$BASENAME.wav")
else
    ARGS+=(-audiodev none,id=snd0)
fi
ARGS+=(-global gnw-h7b0-sai1.audiodev=snd0)

set +e
"$QEMU" "${ARGS[@]}" "${EXTRA_ARGS[@]}"
status=$?
set -e

if [ -n "$RECORD" ]; then
    out="$OUT/$RECORD"
    ext="${RECORD##*.}"
    # Meta written by the recorder once geometry is known.
    if [ ! -f "$BASENAME.meta" ]; then
        echo "warning: no frames captured, skipping $out" >&2
    else
        # shellcheck disable=SC1090
        source "$BASENAME.meta"   # width= height= fps= pix_fmt=
        case "$ext" in
        flac|ogg|m4a|mp3|wav)
            # Audio-only export.
            ffmpeg -y -loglevel error -i "$BASENAME.wav" "$out"
            ;;
        *)
            # Video is the duration master; pad/truncate audio to it.
            # NOTE: the wav only spans the interval the guest's audio
            # pipeline was running -- a guest that starts audio late
            # yields audio anchored at its start time offset unknown to
            # us, so v1 anchors it at 0 (see docs for the caveat).
            AUDIO_ARGS=()
            if [ -s "$BASENAME.wav" ]; then
                if [ -n "${audio_delay:-}" ]; then
                    AUDIO_ARGS=(-itsoffset "$audio_delay" -i "$BASENAME.wav" -c:a aac -af apad)
                else
                    AUDIO_ARGS=(-i "$BASENAME.wav" -c:a aac -af apad)
                fi
            fi
            ffmpeg -y -loglevel error \
                -f rawvideo -pix_fmt "$pix_fmt" -s "${width}x${height}" \
                -r "$fps" -i "$BASENAME.frames" \
                "${AUDIO_ARGS[@]}" \
                -c:v libx264 -pix_fmt yuv420p -movflags +faststart \
                -fps_mode cfr -t "$(awk "BEGIN{print $(stat -c%s "$BASENAME.frames") / ($width*$height*4) / $fps}")" \
                "$out"
            ;;
        esac
        echo "gwemu-headless: wrote $out"
        if [ "$KEEP_RAW" != 1 ]; then
            rm -f "$BASENAME.frames" "$BASENAME.wav" "$BASENAME.meta"
        fi
    fi
fi

exit $status
