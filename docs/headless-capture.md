# Headless capture (Docker / CI)

Run gwemu with **no window**, drive it with a **timeline script**, and
capture **screenshots / video / audio** — reproducibly. Built for test
suites and CI ("does this firmware still behave the same"), but with no
assertion machinery of its own: control in, media out; comparing the
artifacts is the caller's job. Quick-reference usage with worked timeline
examples: `contrib/docker-headless/README.md`.

There is deliberately no interactive control surface (no QMP commands, no
CLI client, no REST): wall-clock-driven input is inherently unrepeatable.
The timeline runs *inside QEMU on the virtual clock*, so every action
lands on the same guest microsecond, every run, on any host.

## Quick start (Docker)

```sh
docker build -t gwemu-headless -f contrib/docker-headless/Dockerfile .

docker run --rm \
  -v $PWD/backup/qemu-images:/images:ro \
  -v $PWD/out:/out \
  -v $PWD/demo.tl:/demo.tl:ro \
  gwemu-headless --timeline /demo.tl --record demo.mp4
```

Flags: `--bank1/--bank2/--extflash` (auto-detected when `/images` holds
exactly one `*-bank1*.bin` triple), `--sd`, `--fps`, `--deterministic`,
`--qmp-port` (debug), `--keep-raw`, and `-- <raw qemu args>`.

## Quick start (bare metal, no Docker)

The entrypoint runs anywhere:

```sh
GWEMU_BIN=./build/qemu-system-arm GWEMU_OUT=./out \
  contrib/docker-headless/entrypoint.sh \
  --bank1 ... --bank2 ... --extflash ... \
  --timeline demo.tl --record demo.mp4
```

Or drive QEMU directly — the features are env-gated in the binary itself:

```sh
GNW_TIMELINE=demo.tl GNW_OUT=./out GNW_RECORD=./out/cap GNW_RECORD_FPS=60 \
  ./build/qemu-system-arm -M gnw-h7b0 -display none \
    -global gnw-h7b0-soc.bank1-image=... ... \
    -audiodev wav,id=snd0,path=./out/cap.wav \
    -global gnw-h7b0-sai1.audiodev=snd0
```

`-display none` is genuinely windowless (plain upstream-QEMU main path —
no SDL initialized at all).

## Timeline format

Line-based; `#` comments; blank lines ignored.

```
0:02     hold game+left 3.0     # chord held 3.0 guest-seconds
0:14     press a                # down + 100ms + up
@840     press a+b              # at LTDC vblank #840 (frame addressing)
5:12.5   screenshot menu.png    # PNG into $GNW_OUT (or absolute path)
6:00     quit
```

- **Addressing**: `[MM:]SS[.fff]` guest time, or `@N` = Nth display
  vblank since machine start. Frame addressing is inherently
  vblank-aligned; note frames are not a fixed wall unit (firmware can run
  the panel at 50/60/72/75Hz).
- **Actions**: `press` (100ms tap), `hold <buttons> <seconds>`,
  `release`, `screenshot <file>`, `quit`. Buttons: `pause game time a b
  left down right up pwr start select`, chorded with `+`.
- Parse errors abort at startup with file:line.
- `GNW_AUTO_INPUT` (the older `t:btn[:hold]` one-liner env) still works
  for quick interactive use.

### Authoring a timeline by recording one (GUI only, not headless)

`GNW_TIMELINE_RECORD=<file>` writes a timeline script from what you
actually press, which is far easier than hand-timing a script. Run it
under the GUI:

```sh
GNW_TIMELINE_RECORD=demo.tl ./build/qemu-system-arm -M gnw-h7b0 ... -display gwemu
```

then replay it here with `GNW_TIMELINE`/`--timeline`.

**This is deliberately unavailable headless**, and the docker entrypoint
rejects `--record-timeline` with an error rather than accepting it.
Recording hangs off the GPIO device's QEMU input handler, so it only sees
events a display backend delivers — under `-display none` nothing ever
arrives and the output file would be empty. More to the point, there is
no way to watch a container's video live, so there is nothing to time
your presses against. Record with a window, replay without one.

## Recording

`GNW_RECORD=<basename>` (the entrypoint's `--record` sets this) samples
the display at `GNW_RECORD_FPS` (default 60) **on the virtual clock**
into raw frames + captures audio via QEMU's stock `wav` audiodev — both
virtual-clocked, so A/V stay in sync regardless of host speed, and a
run that emulated slower (or faster) than realtime still plays back at
true speed. The entrypoint muxes on exit with ffmpeg:

- Video extensions (`.mp4` default profile: libx264, yuv420p,
  +faststart; `.mkv` etc. by extension). Video duration is the master;
  audio is padded to fit.
- Audio-only extensions (`.flac` recommended — lossless keeps captures
  bit-comparable; `.ogg`/`.m4a`/`.mp3` work).
- Screenshots are PNG (lossless) always.

Whole-session only: recording arms at boot and ends at exit (v1 scope).

## Reproducibility

Verified property (this is the headline): **two identical headless runs
produce byte-identical screenshots** at the same timeline timestamps —
on the same machine, at plain realtime, no icount needed. The guest is
deterministic given identical images/SD content, and the timeline's
virtual-clock injection removes the last wall-clock dependency.

Caveats:

- Anything that feeds host state into the guest breaks this — in
  practice that's the RTC alone. Measured: runs from two *different
  builds* (host gcc vs container gcc, half an hour apart) were
  pixel-identical except the menu clock's digit region. Byte-identical
  hashes therefore hold between runs sharing the same RTC readout;
  otherwise compare with the clock region masked, or use fixed content,
  or test in-game states (which depend only on input).
- `--deterministic` (`-icount shift=auto,sleep=off`) is experimental and
  NOT a speed-up button: icount ties apparent guest-CPU speed to the
  shift value, so a fixed shift changes what the firmware experiences
  (measured: `shift=0` made the guest CPU-starved and altered results),
  and `shift=auto` paces to realtime. Fast-forward only genuinely helps
  idle-heavy guests. Prefer plain realtime unless you need cross-machine
  timing stability and have validated your workload under icount.
- The audio wav spans only the interval the guest's audio pipeline was
  running; v1 anchors it at t=0 in the mux. Firmware that starts audio
  late gets its audio track shifted early — fine for
  continuous-audio workloads (games), imperfect for boot sequences.
