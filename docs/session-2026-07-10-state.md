# Session state — 2026-07-10

Context ran long and got cut off mid-investigation. This is a dump of
exactly where things stand so the next session can pick up without
re-deriving all of this. Read this before touching LTDC/DMA2D,
boot-speed, or the retro-go crash again.

## What's done and committed

Everything through commit `5482a047e9` on branch `gnw-h7b0`:

- Phase 1 (memory map, RCC/PWR/OSPI/ADC/SPI1/SPI2 clock+peripheral
  stubs, boot-from-flash) is complete. See `STATUS.md` for the full
  gap-by-gap history.
- Real LTDC device (`hw/display/gnw_h7b0_ltdc.c`): Layer1/RGB565-only,
  2x nearest-neighbor upscale, reads the guest framebuffer each frame
  and blits into a real QEMU display window. **Confirmed working** —
  the user directly observed a live SDL window showing real rendered
  content (retro-go's fatal-exception crash screen, in full color,
  readable red/white text on blue background).
- `gnw-chainloader` (`gnw_chainloader.bin` at repo root, gitignored,
  not tracked) boots and runs continuously with no BusFaults, past
  every peripheral gap found so far (RCC clock init, PWR
  supply-config, OSPI JEDEC-ID + SFDP reads, ADC, SPI1 SD-card probe,
  SPI2 LCD-panel init, LTDC init). Confirmed via `g_scan.state`
  reaching `STATE_COMPLETE` (9) and PC legitimately advancing through
  `partition_scan_update`, `spi1_power_on`, etc. — not stuck on a
  single instruction.
- retro-go (`retro-go-temp/elf/gw_retro_go_bank1.elf`, gitignored, not
  tracked) boots far enough to hit its own fatal-exception handler and
  render a real crash screen with diagnostic text — proving the full
  pipeline (boot → clock/power/OSPI/ADC/SPI init → LTDC → text
  rendering) works end to end.

## Fixed: chainloader was permanently stuck in a POR-standby trap before even reaching the app

Found while resuming this session: `gnw_chainloader.bin` (current
build, may be a newer firmware revision than whichever one produced
the "STATE_COMPLETE" observation above — `../gnw-chainloader`'s source
isn't version-pinned here) was **not** progressing at all — PC parked
permanently at `0x08000e6c` in `stub.elf`, confirmed identical across
multiple gdb checks 6+ seconds apart (real hang, not slow).

Root cause: `src/chainloader/stub_main.c:64`'s
`if (RCC->RSR & RCC_RSR_PORRSTF)` branch. On real hardware this only
fires on a genuine cold power-on and deliberately parks the CPU in
`SCB_SCR.SLEEPDEEP` + `WFI` forever, waiting for a physical WKUP1
button press, specifically to prevent auto-boot when the device is
just plugged into USB. `RCC_RSR_RESET_VALUE` (`gnw_h7b0_rcc.h`) had
`PORRSTF` set as part of a realistic all-four-flags-on power-on value
(`0x00E80000`) — correct for real hardware's *first ever* power-on,
but since QEMU resets to this same value on *every* launch, every
single emulated boot looked like a fresh POR and hit this permanent
standby trap. We don't model the WKUP1 pin, so there was no way out.

**Fixed** by clearing just the `PORRSTF` bit
(`RCC_RSR_RESET_VALUE` now `0x00680000`, keeping
`CDRSTF|BORRSTF|PINRSTF`) — `PINRSTF` (pin reset) is a more accurate
stand-in for "the board was just reset/relaunched", which is what
every QEMU boot actually is. Confirmed fix: PC now advances into the
decompressed app in AXI SRAM (`0x2400xxxx` range, was never reached
before), and `g_scan.state` progresses through real states
(`STATE_PROBE_SD` observed ~8s in) instead of never leaving the flash
stub. Live SDL window now also shows a plain dark-grey background
instead of the LTDC's uninitialized-black default, indicating the app
is now driving the display — screen still has no menu text, which is
Open problem 1 below, now able to be investigated for real since the
firmware actually runs.

## Open problem 1: chainloader's menu never draws

`gnw-chainloader` runs with no faults, but the screen stays a solid
background color (RGB565 `0x2125`) — the menu (header/footer/list
text) never appears, confirmed by dumping the entire 320x240
framebuffer and finding exactly one unique pixel value across all
76800 pixels.

Traced as far as: `board_console_type` reads `CONSOLE_NONE` (0,
correctly, since our internal-flash bank2 is empty so
`board_is_valid_app()` correctly returns false), `g_theme_pending` is
`false`, so the theme-pending gate in `ui_manager.c`'s `ui_draw()`
(the `if (g_theme_pending) { ...; return; }` early-out at the top of
`ui_draw()`, `src/chainloader/ui/ui_manager.c:311`) is **not** the
blocker — that was a dead-end theory, ruled out empirically by reading
the live global.

**Not yet found**: why `ui_draw()` (ELF address `0x24004164` in
`gnw-chainloader/build/app/app.elf`) never appears to execute. PC
samples over many minutes never landed above `~0x24006000`, all within
`HAL_Delay`/`HAL_GetTick`/SPI-helper functions. Two live theories,
neither confirmed:

1. It's just slow (see Open problem 2) and given enough real time it
   would get there — plausible given `g_scan.state` DID reach
   `STATE_COMPLETE` after enough real-time waiting, so forward
   progress is real, just slow.
2. There's a genuine remaining hang somewhere between
   `partition_scan_start()` completing and `menu_run()`'s first
   `ui_draw()` call (e.g. inside `gui_init()`'s own settle logic, or a
   periodic SD hot-plug re-poll — `spi1_power_on()` was observed being
   called a *second* time after `STATE_COMPLETE`, consistent with a
   periodic re-check inside the main loop, which would mean
   `menu_run()` **has** started and `ui_draw()` legitimately isn't
   producing new pixels for some other reason).

**Next step**: with a fast build (see Open problem 2), re-run and
either (a) confirm PC eventually reaches `0x24004164`+ and see what
the menu actually looks like once given enough real time, or (b) if it
truly never gets there even with a fast build, set a real breakpoint
at `*0x24004164` (see gdb gotchas below) and get a clean hit/no-hit
answer.

## Open problem 2: emulation was ~20-25x slower than real-time, and the release-mode fix introduced a NEW, worse hang

### The slowdown

Confirmed via a completely undisturbed 8-second wait: `uwTick` (the
guest's HAL millisecond counter, symbol `uwTick` in `app.elf`, live
address `0x2400dde0` in the running decompressed image) only reached
`333` after 8 real seconds. That means guest-visible time was passing
at roughly 1/24th of real-time. A firmware "200ms" or "2000ms" bounded
timeout (e.g. `spi1_power_on()`'s SD-card CMD0 probe, or the CMD41
polling loop) was therefore taking on the order of tens of real
seconds to elapse — explaining why chainloader *looked* hung when it
was actually just working through legitimate bounded retries very
slowly.

Root-caused to the build configuration: `build/` (the original,
still-good debug build) has `buildtype=debug`, `qom_cast_debug=true`,
`optimization=2`. `qom_cast_debug=true` means every single
`GNW_H7B0_XXX(opaque)` QOM cast macro — which fires on *every* MMIO
access to every device we wrote (RCC, PWR, OSPI, ADC, SPI, LTDC) —
does a runtime type-check. Given how MMIO-heavy this workload is
(every register poll goes through this), that's plausibly a huge chunk
of the 20-25x factor.

### The "fix" and the new problem

Created `build-release/` (separate directory, original `build/` left
untouched and still fully working) configured via:

```
cd build-release
../configure --target-list=arm-softmmu
meson configure -Dbuildtype=release -Dqom_cast_debug=false -Ddebug=false -Dwerror=false
ninja qemu-system-arm
```

(`-Db_ndebug=true` was tried and reverted — QEMU's `osdep.h` has a hard
`#error building with NDEBUG is not supported`, so plain C `assert()`
stays active regardless; that's fine, `qom_cast_debug` and
`buildtype=release` are the two settings that actually matter for
speed.)

Also needed, unrelated to this project: `hw/ssi/xilinx_spips.c` had a
pre-existing `-Wstringop-overflow` warning that `-O3` promotes to a
hard error under this build's `-Werror` (which is hardcoded on this
file's compile command regardless of the global `-Dwerror=false` — did
not investigate why). Silenced with a `#pragma GCC diagnostic ignored`
around the offending loop (see the committed diff — this is a genuine
upstream-QEMU false positive as far as I could tell in the time spent,
not something specific to our board).

**The release build compiles clean and launches (window opens
correctly), but `gnw_chainloader.bin` running under it shows 0 CPU
time accumulated across all 3 QEMU threads (checked via
`/proc/PID/task/*/stat` utime/stime, not just `ps`, over 8+ seconds,
both with and without `-s`/gdbserver attached).** This is qualitatively
different from "slow" — it's not accumulating any CPU time at all,
which normally means the process is blocked/parked on something, not
merely executing slowly. **Not diagnosed before context ran out.**

**Next step, in order of cost**:
1. Try the *systick_test2.elf* isolated test (see below) under
   `build-release` specifically for boot to a running state — if
   *that* also shows 0 CPU time, the bug is `build-release`-wide
   (bad reconfigure, missing accelerator, TCG not actually enabled,
   etc.) and unrelated to chainloader specifically. If systick_test2
   runs fine but chainloader doesn't, the bug is chainloader/our
   SoC-specific and something in the release build exposes a
   pre-existing bug that debug-mode's extra checks somehow masked
   (e.g. UB that behaves differently at `-O3` vs `-O2`).
2. Check `meson introspect --buildoptions build-release` for anything
   accidentally still wrong (accelerator selection, icount, etc.).
3. Compare `qemu-system-arm -M gnw-h7b0 -kernel <trivial spin-loop
   test>` (e.g. `/tmp/flashboot_test.elf` from earlier in this
   session, if it still exists) between `build/` and `build-release/`
   — establishes whether it's a general release-build regression or
   specific to chainloader's more complex boot path.
4. If release mode turns out to be a dead end under time pressure,
   fall back to just tolerating debug-build slowness and doing
   longer background waits (`ScheduleWakeup` at 60s+ deltas) instead
   of polling — the *debug* build (`build/`) is known-good and was
   never broken, only slow.

## Open problem 3: retro-go's fatal exception (separate from the above)

retro-go boots, prints 3 log lines to `logbuf`
("Log started." / "Boot from brownout?" / "boot_magic=0x00000000"),
then hits its own fault handler: `PC=0x00000000`, `LR=0x08005915`.

Traced: `LR=0x08005915` is the return address pushed by a `bl
Error_Handler` at `0x08005910` inside a function `addr2line` resolves
as `MX_RTC_Init` (`main.c:876` per the embedded debug path, source not
available locally — only `gnw-chainloader` has source in this repo
tree, not retro-go's `main.c`/`Error_Handler`). So: something calls
`Error_Handler()`, and *while inside it* (or something it calls), the
CPU jumps through a null function pointer.

Tried: fixing `RCC_RSR`'s reset value to the real hardware default
(see above commit) on the theory that "Boot from brownout?" indicated
a reset-cause misdetection feeding into unusual/rare firmware logic.
**This did not change the crash at all** — identical `PC`/`LR`, same
log output, byte-for-byte. So `RCC_RSR` was not the (or not the whole)
cause. The "Boot from brownout?" line is most likely just
unconditional diagnostic text (note the question mark — reads like a
heuristic/informational message, not something gating a branch), and
the real crash is an unrelated null-pointer bug we don't have the
source to trace further (retro-go's `main.c`/`Error_Handler`
implementation isn't in this repo's tree — only `gw_retro_go_bank1.elf`
and friends under `retro-go-temp/`, gitignored, user-provided).

**UPDATE — still crashes, extflash did NOT fix this.** Initial
same-session gdb spot-checks (at ~10s and ~110s of real wall-clock
time, under the debug build's ~20-25x slowdown) caught the CPU still
legitimately executing GPIO/button-polling code
(`buttons_get`/`HAL_GPIO_ReadPin`) with `uwTick` climbing, which looked
like the crash was avoided. It wasn't — a longer unattended run (no
gdbserver attached, full speed, ~62 real seconds) reached the LTDC
framebuffer and displayed the exact same crash screen text,
`PC=0x00000000 LR=0x08005915`, confirmed visually by the user. So
extflash content only delayed reaching `Error_Handler` (through more
real boot work happening first), it didn't prevent it. See the
extflash section below for what's confirmed vs. not, and go back to
"**Next step**" above (get retro-go source) for this specific crash.

## extflash — wiring confirmed working; did NOT fix the Open-problem-3 crash

Tested the loader-device approach:

```
-device loader,file=/home/doug/Nerd/git/qemu-gnw/example-extflash-backup.bin,addr=0x90000000
```

**The loader mechanism itself works, no code changes needed** — this
is a good, reusable way to populate `EXTFLASH_BASE_ADDRESS`
(`0x90000000`, otherwise a zero-initialized plain-RAM region per
`gnw_h7b0_soc.c`'s `INIT_RAM_REGION(extflash, ...)`) for any future
retro-go/chainloader testing that needs real flash content.

**But it does not fix Open problem 3.** With real extflash content
present, retro-go still eventually hits the identical
`Error_Handler` crash (`PC=0x00000000`, `LR=0x08005915`) — confirmed
via the live SDL window after ~62 real seconds of unattended running.
Early gdb spot-checks (at ~10s/~110s real time) had caught it mid-flight
still legitimately executing GPIO/button-polling code with `uwTick`
climbing, which was a false-positive "looks fixed" signal — it just
hadn't reached the crash point yet at those check-in times. `logbuf`
content (the three boot lines) was identical in both the crashed and
not-yet-crashed states either way, consistent with those being
unconditional diagnostics unrelated to the crash, as suspected before.

**Net effect**: extflash loading is now a proven, ready-to-use tool,
but Open problem 3 (retro-go's `Error_Handler` crash) is unresolved
and still needs retro-go source to trace further — see "**Next step**"
in Open problem 3 above.

Origin note: user added `example-extflash-backup.bin` (64MB, exact
match for `EXTFLASH_SIZE`) at the repo root — gitignored, not tracked.

## gdb gotchas learned this session (save yourself the time)

- `interrupt` + `continue` + `detach` in one `-batch` invocation is
  **unreliable** — `continue` sometimes fails with "Cannot execute
  this command while the target is running" even immediately after a
  successful `interrupt`, seemingly a race in how gdb's remote
  protocol batches commands. Prefer separate invocations:
  `interrupt`+read+`detach` in one, and if you need to explicitly
  resume, do it in a clean follow-up connection.
- Setting a breakpoint requires the target to already be stopped
  (`interrupt` first) — `break` while running silently accepts but
  `continue` afterward errors as above.
- `timeout N gdb ...` occasionally hangs for the tool's full 2-minute
  ceiling instead of respecting `N` — cause not identified, possibly
  an interaction between `timeout`'s SIGTERM and gdb's own signal
  handling while attached to a remote target. If a `timeout`-wrapped
  gdb call doesn't return quickly, don't wait it out — kill and retry
  with a fresh QEMU process rather than trying to recover the gdb
  session.
- Best pattern found for "is it actually stuck": read a live progress
  variable from the ELF's symbol table (e.g. `g_scan.state` at
  `nm ... | grep g_scan`) rather than trying to infer progress from PC
  alone — PC cycling through the same handful of addresses is
  ambiguous (could be a real bounded retry loop or a genuine hang);
  a monotonically-advancing state variable is not.
- `ps`'s `%CPU`/`TIME` columns can round to zero and look identical to
  "genuinely not running" for a process using only a tiny slice of
  CPU — cross-check with raw `utime`/`stime` jiffie counts from
  `/proc/PID/task/*/stat` (field 14/15) before concluding a process is
  truly blocked vs. just using very little CPU.

## Test assets referenced above (all gitignored, not tracked)

- `/home/doug/Nerd/git/qemu-gnw/gnw_chainloader.bin` — chainloader
  combined stub+app image, source at `/home/doug/Nerd/git/gnw-chainloader`
  (full source available, this is how most of Phase 1's gaps got
  diagnosed).
- `/home/doug/Nerd/git/qemu-gnw/retro-go-temp/elf/gw_retro_go_bank1.elf`
  — retro-go build, **no source tree available locally** for this
  specific build (only the compiled ELF + a same-named but likely
  different-version tree at `/home/doug/Nerd/git/game-and-watch-retro-go-sd`
  — check whether that tree's `main.c` actually matches this ELF
  before trusting it for line numbers).
- `/home/doug/Nerd/git/qemu-gnw/example-extflash-backup.bin` — 64MB,
  not yet wired up (see above).
- `/tmp/systick_test2.c` / `.elf` — minimal standalone SysTick-only
  test kernel (no chainloader/retro-go complexity), useful for
  isolating "is our ARMv7M/SysTick infra broken" from "is this specific
  firmware's boot path broken." Confirmed SysTick itself works
  correctly under the debug build; not yet re-tested under
  `build-release`.
- `/tmp/flashboot_test.c` / `.elf` (may or may not still exist) —
  earlier minimal flash-boot-path test kernel from Phase 1 work.

## Build directories

- `build/` — original debug build, **known-good**, use this if
  `build-release/` turns out to be a dead end. Slow (~20-25x) for
  guest-clock-bound waits but otherwise fully correct.
- `build-release/` — release-mode reconfigure, compiles clean, but
  chainloader boot shows 0 accumulated CPU time under it (see Open
  problem 2). Do not assume this is faster/better until that's
  resolved — right now it looks *worse* (possibly not running at all).
