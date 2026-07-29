# gwemu-headless

Headless, scriptable gwemu in a container: boot Game & Watch firmware
with **no window**, drive it with a **timeline script**, and capture
**screenshots / video / audio** — reproducibly. Built for test suites
and CI; there is no assertion machinery (control in, media out —
comparing artifacts is your job) and deliberately no interactive control
surface (wall-clock-driven input is unrepeatable; the timeline runs
inside QEMU on the virtual clock, so every action lands on the same
guest microsecond, every run, on any host).

Full background and caveats: [`docs/headless-capture.md`](../../docs/headless-capture.md).

## Build

From the **repo root**:

```sh
docker build -t gwemu-headless -f contrib/docker-headless/Dockerfile .
```

## Run

```sh
mkdir -p out
docker run --rm \
  -v $PWD/backup/qemu-images:/images:ro \
  -v $PWD/out:/out \
  -v $PWD/demo.tl:/demo.tl:ro \
  gwemu-headless \
    --timeline /demo.tl \
    --record demo.mp4
```

Firmware images are mounted read-only; the entrypoint works on scratch
copies, so **no state leaks between runs** (every run boots pristine
flash). Artifacts land in `/out`.

### Options

| Flag | Meaning |
|---|---|
| `--bank1 F --bank2 F --extflash F` | firmware images; auto-detected when `/images` holds exactly one `<game>-bank1*.bin` triple |
| `--sd F` | optional SD card image (raw or qcow2) |
| `--timeline F` | timeline script (see below) — the only control surface; without it the run goes until the container is stopped |
| `--record NAME.mp4` | whole-session A/V capture, muxed on exit; `.mkv` etc. by extension; `.flac`/`.ogg`/`.m4a`/`.mp3` = audio-only |
| `--fps N` | recording frame rate (default 60) |
| `--deterministic` | `-icount shift=auto,sleep=off` (experimental — see docs; **not** a speed-up button) |
| `--qmp-port N` | expose QMP on `tcp:0.0.0.0:N` for debugging |
| `--keep-raw` | keep the raw `.frames`/`.wav` intermediates |
| `-- ...` | pass everything after `--` to qemu verbatim |

## Timeline scripts

A timeline is a plain text file: one action per line, `#` comments.
Timestamps are **guest time** (`[MM:]SS[.fff]`) or **display frames**
(`@N` = Nth LTDC vblank since boot). Parse errors abort at startup with
`file:line`.

```sh
# demo.tl -- boot into retro-go, browse the game list, grab evidence.

0:02     hold game+left 3.0      # boot combo: hold GAME+LEFT for 3s
                                 # (bank-select into retro-go)

0:14     screenshot menu.png     # coverflow is up by now

0:16     press right             # next game (100ms tap)
0:17.5   press right
@1250    screenshot list.png     # frame-addressed: exactly vblank #1250

0:20     press a                 # launch the selected game
0:35     screenshot ingame.png   # 15s into gameplay

0:36     hold b 1.5              # held button (1.5s)
0:40     press a+b               # chorded press

0:45     quit                    # end the run (flushes the recording)
```

- **Actions**: `press <buttons>` (down + 100ms + up), `hold <buttons>
  <seconds>`, `release <buttons>`, `screenshot <file>` (PNG; relative
  paths land in `/out`), `quit`.
- **Buttons**: `pause game time a b left down right up pwr start
  select` — chord with `+` (e.g. `game+left`).
- Frame addressing (`@N`) is vblank-aligned but not a fixed wall unit:
  firmware may run the panel at 50/60/72/75Hz.

A minimal CI-style smoke test is just:

```sh
# smoke.tl -- does the firmware still reach its menu?
0:02   hold game+left 3.0
0:14   screenshot menu.png
0:15   quit
```

Two runs of the same timeline produce byte-identical screenshots
(measured; the single exception is firmware that displays the RTC —
the wall-clock digits are the only pixels that legitimately differ).

## Without Docker

The same entrypoint runs bare on any Linux host with ffmpeg:

```sh
GWEMU_BIN=./build/qemu-system-arm GWEMU_OUT=./out \
  contrib/docker-headless/entrypoint.sh \
  --bank1 backup/qemu-images/zelda-bank1-patched.bin \
  --bank2 backup/qemu-images/retro-go-bank2.bin \
  --extflash backup/qemu-images/zelda-extflash-patched-plus-retro-go.bin \
  --timeline smoke.tl --record run.mp4
```

Or drive the binary directly — the features are env-gated
(`GNW_TIMELINE`, `GNW_OUT`, `GNW_RECORD`, `GNW_RECORD_FPS`) and
`-display none` is genuinely windowless; see the doc linked above.

Timeline *recording* (`GNW_TIMELINE_RECORD`) is not among them: it
captures live keystrokes, which needs a window to press into and live
video to time the presses against, so it is a GUI-only feature and
`--record-timeline` is rejected here. Record a script with `-display
gwemu`, then replay it with `--timeline`.
