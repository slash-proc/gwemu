# 2026-07-12 — stock firmware boot investigation (Mario + Zelda)

## Goal

Get stock (official Nintendo) Game & Watch firmware — as opposed to
homebrew retro-go/gnw-chainloader — booting to visible display output.
Stock firmware has **no bootloader**: it's linear execution starting at
internal flash bank 1 (`0x08000000`), which eventually reads assets from
external flash at `0x90000000`.

## Real fixes landed

1. **`RCC_RSR.SFTRSTF` reset value** (`include/hw/misc/gnw_h7b0_rcc.h`,
   landed earlier this session, already in `dc882746af`). Firmware's own
   init code checks this reset-cause bit to choose a top-level boot
   branch; without it, LTDC/graphics init is skipped entirely. Root-caused
   by direct comparison of actual vs. expected branch behavior in the
   disassembly.

2. **PA0/WKUP1 no longer forced low at GPIO reset**
   (`hw/misc/gnw_h7b0_gpio.c`, commit `da662f6055`). Was previously forced
   low as a workaround for an unrelated early-boot write-storm hang, from
   before `RCC_RSR.SFTRSTF` was fixed. Real disassembly — cross-checked
   against `gnwmanager`'s `gnwmanager/cli/gnw_patch/{mario,zelda}.py`
   patch tooling, which is real, SHA1-hash-verified stock-firmware ground
   truth — shows this pin gates a "state-6 standby" handler (Mario
   `0x08005EF4`, Zelda `0x0800EA8C`, both literally labeled "state-6
   standby" in `gnwmanager`'s own patch comments as part of their
   "warm-boot power-off fix"). Firmware expects this pin high (button not
   held) to run its real display-init work. With `RCC_RSR.SFTRSTF` fixed
   and current LTDC understanding in place, the old storm no longer
   happens — leaving PA0 at its real default now produces genuine forward
   progress on **both** Mario and Zelda with zero live register pokes:
   confirmed via real `SPI2->TXDR` (`0x40003820`) traffic matching the
   known LCD-panel bring-up command byte sequence.

## Open gaps, still being traced

- **LTDC IRQ88 (NVIC `ISER2` bit 24)**: firmware arms an LTDC
  register-reload wait (`RRIF`/`IER.RRIE`), but NVIC is never observed
  enabled for IRQ88 when checked. `hw/display/gnw_h7b0_ltdc.c` does raise
  a real `qemu_irq` (wired via `sysbus_connect_irq` in
  `hw/arm/gnw_h7b0_soc.c`), so the wiring itself is correct. All NVIC
  `ISER` banks read `0x00000000` throughout observed boot so far, meaning
  firmware hasn't reached whatever code is supposed to enable *any*
  interrupt yet — this may resolve on its own once the loop-exit gap
  below is fixed, rather than being an independent bug. Needs re-checking
  after that.
- **Main superloop timeout-counter never arms**: both games' main loop
  (Mario: dispatcher at `0x080072a0`+, byte flag `[r4+7]`; Zelda:
  `FUN_08010600`'s `while(true)`, byte flag `[r4+9]`) only exits once a
  counter exceeds a real threshold (Zelda: `FUN_0800edfc`, threshold
  `0x27`/39). That counter logic is legitimate (not a bug) but never
  starts incrementing because its own enable parameter — read from
  `[r4+0x60]` (live address `0x2000ab90+0x60` = `0x2000abf0` in the tested
  Zelda run) — is `0` and confirmed via hardware watchpoint to never be
  written during ~12s of real execution. Need to find what real
  condition/interrupt/init-order step is supposed to write this field
  nonzero. Next step: check sibling cases in the same jump-table
  dispatcher (`FUN_0800ea00`'s table, indexed by `param_1`/state) for
  whichever one is responsible for arming this — likely a state
  transition that hasn't been reached yet, not a missing peripheral
  register.

## Tooling / workflow notes

- **`gnw-mario-decomp` and `gnw-zelda-decomp`** (sibling repos to this
  one, at `~/Nerd/git/`) hold per-game Ghidra analysis. Same script
  toolkit in both (`scripts/DecompAt.java`, `FindCallers2.java`,
  `FindXrefs3.java`, `FindMovtScalar.java` for MOVW/MOVT-split
  immediates that raw byte search misses, `CreateFunctions2.java` for
  forcing a function boundary Ghidra's auto-analysis missed).
- **`gnwmanager/gnwmanager/cli/gnw_patch/mario.py` and `zelda.py`** (a
  separate local repo) are real, SHA1-hash-verified stock-firmware patch
  offsets with inline comments describing what each patched address does
  on real hardware — not a theory, a documented cross-reference. Check
  these periodically when investigating a stuck point; they cover more of
  the boot path than examined here so far (button-press macros, OTFDEC
  disable, sleep-timer, audio mute, NVRAM/save-data layout, and more).
  Their *data*-modification steps (erasing save regions, disabling save
  encryption) are for producing their own custom-firmware builds and
  should **not** be replicated when tracing stock boot behavior — the
  user's supplied `backup/flash_backup_{mario,zelda}.bin` should always
  be used as-is.
- **Zelda's `backup/flash_backup_zelda.bin` extflash dump is already
  plaintext**, unlike Mario's genuinely OTFDEC-encrypted dump. Confirmed
  via Shannon entropy over the first 64KB: Zelda ~1.53 (structured/
  plaintext), Mario ~7.997 (indistinguishable from random/encrypted).
  Running the OTFDEC decrypt script against an already-plaintext dump
  corrupts it — check entropy before assuming a game's extflash backup
  needs decryption.
- **PC-sampling via repeated QEMU monitor `info registers` calls has a
  real, reproducible bias** toward landing on MMIO-touching instructions
  under MTTCG — several apparent "hangs" this session turned out to be
  real, fast progress once verified with a `stepi 2000`-style burst via
  gdb. Always verify a suspected hang with a step burst before concluding
  it's stuck.
- **gdbstub connection corruption**: killing a `gdb` client with `-9`
  while it's paused at a breakpoint leaves the target vCPU halted with no
  listener attached — the next connection attempt fails with
  `Ignoring packet error` / `vMustReplyEmpty timeout` until the stale
  client is identified (`ss -tnp | grep 1234`) and force-killed, then a
  fresh `gdb` session explicitly issues `delete` + `detach` to resume
  it. Worth a wrapper script that always does clean-attach → action →
  explicit `delete`+`detach`, never a bare `timeout -s KILL` against a
  script that might still be mid-breakpoint.
- **SD card sizing**: QEMU's SD model (`hw/sd/sd.c`) only requires strict
  power-of-2 sizing for cards ≤2GiB (SDSC). Above that (SDHC/SDXC), it
  only requires 512K alignment. The user's `sdcard.img` (~7.4GiB, a real
  `dd` copy) is already 512K-aligned — no qcow2 overlay needed, just
  `-drive if=sd,format=raw,file=sdcard.img`. A qcow2 overlay is only
  useful for non-destructive/discardable writes, not as a required
  workaround.
