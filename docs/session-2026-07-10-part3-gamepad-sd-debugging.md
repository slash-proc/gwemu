# Session state — 2026-07-10, part 3 (gamepad, real-SD debugging, DMA2D)

Continuation of `docs/session-2026-07-10-part2-state.md`. That doc's "Next
task: real OCTOSPI command decoding" was picked up separately; this session
started from real xpad gamepad support and ended up chasing five real
hangs/crashes end-to-end while testing against a **real physical SD card**.

## What landed

See `CHANGELOG.md`'s "2026-07-10 (part 2)" entry for the terse list. This
doc is the narrative + technique writeup, since most of these bugs were
found via live disassembly with **no debug symbols at all** for the
firmware image that exposed them (a patched stock/Zelda firmware image the
project owner supplied, no matching ELF in this repo).

## The debugging technique that carried this session

Most of today's bugs share one shape: **real firmware writes a "start"
register, then polls a status/flag bit that real hardware sets as a side
effect — our device model was a plain register shadow, so the flag never
changed, and firmware spun forever (or, worse, kept reading a FIFO/buffer
that never signaled "empty", walking a pointer off the end of RAM).**
Found five different instances of this exact pattern today (SPI1 RXP,
ssi-sd.c CMD58, JPEG EOCF/IFTF/IFNFF, TIM2-block UIF, RTC ICSR write-flags).
It's worth treating as a checklist item for any *new* device stub, not just
something to whack-a-mole reactively: **whenever a stub's write handler
lets firmware set a "go" bit, ask what status bit real hardware sets in
response, and whether anything in this codebase ever sets it back.**

When there's no debug-symbol ELF for the exact firmware binary under test
(true for the "patched-zelda-bank1.bin" image used at the end of this
session), the technique that worked:

1. Attach gdb to QEMU's gdbstub (`-s` on the QEMU command line, then
   `target remote localhost:1234` — works with *any* ELF as the "symbol
   file" argument, even a wrong/unrelated one, since `x/Ni $pc` and
   `info registers` don't need symbols at all).
2. If the firmware is genuinely stuck (not just slow), sampling
   `info registers pc` a few times a couple seconds apart will show the
   *same* PC repeatedly — confirms a real spin, not just infrequent
   sampling luck. (**Note for next session:** batch this — background the
   QEMU launch, sleep ~5s once, then take ~5 quick PC samples in a loop
   without re-launching or re-checking `ps` between each one. Doing this
   step by step burns a lot of turns for no benefit.)
3. `x/Ni $pc-N` to see the loop body. The classic idiom to recognize:
   `ldr rX, [rY, #off]` (read a peripheral/struct field) immediately
   followed by a bit-test (`lsls`/`lsrs`/`ands`) and a conditional branch
   back to (or near) the load — that's a status-bit poll.
4. `info registers` on the base-address register (usually the one used in
   the `ldr` right before the loop) — compare its value against this
   project's `include/hw/arm/gnw_h7b0_soc.h` `#define ..._BASE_ADDRESS`
   constants. This is usually enough to identify *which* peripheral is
   being polled without any symbols at all (worked cleanly for TIM5 at
   `0x40000C00`, GPIOA at `0x58020010`).
5. Cross-reference the peripheral's real register layout against
   `STM32H7B0.svd` (bit position of the offset being tested) and, if
   available, `sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_*.c` for what
   real firmware's HAL does at that point (even though this specific
   firmware isn't HAL-based/isn't the SDK's own code, the *hardware*
   register semantics are the same either way).
6. Even with a matching-but-not-byte-identical ELF (e.g. a different build
   of the same retro-go source tree), `addr2line` against it can still
   resolve function *names* correctly often enough to be useful — worth
   trying before falling back to pure disassembly. It resolved
   `JPEG_ReadInputData`/`JPEG_Process`/`HAL_DMA2D_Init` correctly against
   builds that didn't byte-match at all, because the compiler placed the
   same HAL library functions at stable-ish addresses across nearby
   builds. Don't fully trust it, though — cross-check with the
   disassembly-only technique when the resolved call graph doesn't make
   logical sense (this happened once: `emulator_start` "called from"
   `lfs_fs_traverse_`, which is nonsensical — a sign the ELF didn't
   actually match and the resolution was a coincidental near-symbol hit).

## Open problem: patched-zelda-bank1.bin still hangs (black screen)

Boot command (see CHANGELOG's qcow2-overlay note for the real-SD setup):

```
build/qemu-system-arm -M gnw-h7b0 \
  -kernel patched-zelda-bank1.bin \
  -device loader,file=retro-go-temp/gw_retro_go_intflash_sd_bank2.bin,addr=0x08100000 \
  -device loader,file=example-extflash-backup.bin,addr=0x90000000 \
  -drive if=sd,format=qcow2,file=/tmp/sd-overlay.qcow2 \
  -display sdl -s \
  -d guest_errors,unimp -D /tmp/qemu-guest-errors.log
```

Progress so far, in order (each fix confirmed to move the hang further, via
the RTC/TIM2 register-write patterns changing in the guest-error log
between attempts):

1. Hung spinning on TIM5 (`SR.UIF` never set after `EGR.UG`) — **fixed**.
2. Hung in an RTC alarm-set retry loop (`ICSR.ALRAWF` zeroed) — **fixed**.
3. **Still hangs**, now cycling through ADC config writes (offsets `0x308`,
   `0x10c`, `0x110`, `0x130`, `0x11c`, `0x114`, `0x1c0` in the combined
   ADC1+ADC2 block — likely ADC2's registers at the `+0x100` stride) plus
   a GPIOA/extflash-pointer check, in a state machine whose state byte
   (fixed global at `0x2000ab90` in this specific build, value `6`) never
   advances. The GPIOA/extflash check looked like a red herring on closer
   inspection (reads a constant, not a live status bit); the real block is
   more likely inside one of three unlabeled subroutine calls in the
   state-6 handler (`bl` targets `0x80129e2`, `0x8012ada`, `0x8012b8a` in
   this build) that weren't stepped into yet.

**Not pursued further this session** — deliberately deprioritized in favor
of `retro-go-sd-bank1-real.bin` (the generic retro-go SD build), which
boots much further already (reaches the real UI, hit and cleared the
JPEG/DMA2D bugs above) and is a higher-signal target for continued testing.
Resume here only if the patched-Zelda image specifically is the goal again
— otherwise this is stale once the underlying `gnw_h7b0_adc.c` stub or the
unlabeled subroutines change.

## Known remaining issues (not yet root-caused)

- **Flicker**: present on both `gnw-chainloader` and retro-go boots,
  predates this session's changes (not something DMA2D/JPEG/RTC/TIM2
  introduced). Likely LTDC vblank/frame-pacing related. **Next session's
  starting point per the project owner.**
- **Runs too fast**: game logic speed appears untethered from real-time
  pacing. Suspected related to the same flicker root cause (no real
  vsync/frame-rate throttle tying the main loop to a real LCD refresh
  interval) — not yet investigated, just a hypothesis from this session's
  final exchange.

## Real SD card testing notes

- The card used (`/dev/mmcblk0`) is a normally MBR-partitioned card (one
  FAT32 partition, type `0x0B`, starting at LBA 2048 — standard 1 MiB
  alignment), *not* the "superfloppy" (FAT filesystem at LBA 0, no
  partition table) layout `docs/session-2026-07-10-part2-state.md`'s test
  image used. **Do not assume gnw-chainloader's FAT/SD code needs
  MBR-parsing support** — the project owner confirmed this exact card
  already works on real physical G&W hardware with this firmware
  unmodified, so FatFs's own internal MBR-fallback logic
  (`FF_MULTI_PARTITION == 0` default behavior: read LBA0, and if it's not
  a direct FAT boot sector, walk the partition table for the first
  FAT/FAT32 entry) is sufficient — confirmed by reverting an
  unnecessary/wrong patch made mid-session that assumed otherwise.
- `File Browser` (in `gnw-chainloader`) still failed to show the SD FAT
  partition even after the ssi-sd.c CMD58 fix let `Partition Viewer`
  detect the card successfully. Not root-caused this session — worth
  revisiting; the qcow2-overlay full-8GB-capacity fix (see CHANGELOG)
  happened afterward and wasn't re-tested against this specific symptom.
