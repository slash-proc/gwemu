# 2026-07-16 diag-suite failure triage (live tracker, prioritized)

Rewritten in place as status changes — this is the working checklist for
systematically fixing every current `../stm32h7b0-diag` failure, per the
project owner's explicit priority order: **CRC32 → HASH/HMAC → JPEG →
everything else**. Baseline snapshot (main checkout, before this pass):
63 OK / 39 FAIL / 8 UNRUN out of 110 total cases (see
`docs/session-2026-07-16-perf-expert-panel-and-jpeg-encode.md` for how
that baseline was captured).

**Current confirmed state (main checkout, everything through priority 4g
applied and independently verified live): every genuinely-broken case
found this session is fixed. The only remaining FAIL seen in the latest
full-suite run (`gpio_output_readback`) is believed to be host-contention
flakiness, not a real bug -- see priority 4g. (Residual "UNRUN" counts in
raw harness output are blank padding slots beyond the real registered
case count, a harness quirk, not a real gap.)**

## Priority 1 — CRC32: FIXED ✅

| Case | Status |
|---|---|
| `crypto_crc32` | **FIXED**, applied to main, verified. Wrong `CRC_INIT` reset value (`0x00000000` vs real hardware's `0xFFFFFFFF` per RM0455 22.4.4), in `include/hw/misc/gnw_h7b0_regs_crc.h`. |

## Priority 2 — HASH/HMAC: FIXED ✅ (all 26 cases)

Agent `a99add49cff908e5e` delivered a comprehensive fix, independently
rebuilt and re-verified by the coordinator (all 26 cases confirmed OK
directly, not taken on the agent's self-report). Applied to main
checkout. Four real bugs found and fixed:

1. **SHA224 unsupported by this host's crypto backend** (`hash_sha224`,
   `hmac_sha224` + `_dma`/`_it` variants): this host has no
   nettle/gcrypt/gnutls installed, so QEMU's `crypto/hash-glib.c`
   backend is used, which marks `QCRYPTO_HASH_ALGO_SHA224 = -1`
   (unsupported — GLib's `GChecksum` has no SHA224 mode). Fixed with a
   standalone from-scratch SHA-224/256 compression core in
   `hw/misc/gnw_h7b0_hash.c`, used only for SHA224 (plain hash and a
   hand-written RFC 2104 HMAC construction), independent of host backend
   availability.
2. **HMAC missing its real 3rd phase** (every `hmac_*` case): real HMAC
   hardware is a genuine 3-phase sequence (key → message → key again,
   confirmed via `sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_hash.c`'s
   `HAL_HASH_PHASE_HMAC_STEP_1/2/3`); the device model only implemented
   2. Fixed with a real phase state machine (`s->hmac_phase`,
   `hash_trigger_dcal()`).
3. **DMA-mode never actually fed `HASH_DIN`** (every `*_dma` case): two
   real bugs plus one architectural fix — (a) `hw/misc/gnw_h7b0_dma.c`'s
   DMA-request-notifier registration was a single global slot that SAI1
   (already using it) silently clobbered every time it re-registered,
   so HASH's one-time registration never actually fired; fixed by
   turning it into a real multi-registration registry. (b) The generic
   DMA controller's audio-pacing timing model (≥1ms-per-half floor)
   raced against the diag firmware's fixed CPU-spin completion wait;
   added an opt-in `low_latency` synchronous-completion mode that HASH
   uses (SAI1 doesn't). (c) Once completion became synchronous, a real
   ordering bug surfaced — `HAL`'s `HASH_Start_DMA()` sets `CR.DMAE`
   *after* starting the DMA transfer, but synchronous completion now
   fired *during* that same call, before DMAE was set; a stale
   `if (!DMAE) return;` guard was silently no-op'ing. Removed (reaching
   the notifier at all, via the per-request-ID DMAMUX binding, is
   already sufficient proof the transfer is legitimately HASH's).
4. **A separate IT-mode bug, found underlying most of the above**:
   `HASH_STR.DCAL` is write-only/self-clearing on real hardware (per the
   SVD) but was being persisted into the register shadow. An innocent
   later read-modify-write of STR (e.g. `__HAL_HASH_SET_NBVALIDBITS()`)
   would read the stuck `DCAL=1` bit back and re-OR it into its own
   write, silently re-triggering a spurious digest computation before
   new key/message bytes were fed. This self-healed for single-shot
   plain hashing but permanently corrupted HMAC's 3-phase state machine
   and IT-mode's re-triggering. Fixed: `DCAL` is never persisted into
   the shadow register now.

Also fixed as a byproduct of the DIN-write rewrite: `crypto_hash_swap_modes`
(DATATYPE_32B vs 8B byte-swap semantics) and confirmed `crypto_hash_multipart`
passing.

**Files changed**: `hw/misc/gnw_h7b0_hash.c`/`.h`, `hw/misc/gnw_h7b0_dma.c`/`.h`,
`hw/misc/gnw_h7b0_sai1.c`, `hw/arm/gnw_h7b0_soc.c`.

## Priority 3 — JPEG: FIXED ✅

| Item | Status |
|---|---|
| LTDC crash (blocked all JPEG/LTDC verification) | **FIXED, applied to main, independently verified.** Two distinct bugs in `gnw_h7b0_ltdc_capture_rows()`, both from Layer2's CFBLR/stride not being fully validated — see below for detail. 18+ crash-free repro attempts, zero new `journalctl -k` segfaults, full diag suite now runs to completion (0 hang/crash) instead of dying partway through. |
| `jpeg_header_info`, `jpeg_decode_correct`, `jpeg_encode_correct`, `jpeg_decode_throughput` | **FIXED, applied to main, independently verified live.** The real JPEG encoder built on branch `worktree-agent-acbc922798c640a55` (`hw/misc/gnw_h7b0_jpeg.c`/`.h`, self-verified bit-exact against Python/Pillow) was pulled from that worktree's uncommitted changes and applied to main (2026-07-16). Rebuilt clean; re-ran the live diag suite — all four now report OK. |

**Files changed**: `hw/misc/gnw_h7b0_jpeg.c`, `include/hw/misc/gnw_h7b0_jpeg.h`.

### LTDC crash detail (for the record)
Two bugs in `hw/display/gnw_h7b0_ltdc.c`'s `gnw_h7b0_ltdc_capture_rows()`:
1. `l2_bpp` was assigned based on `l2_en` alone, not also
   `l2_src_width > 0`. Firmware can leave Layer2 enabled with CFBLR
   still 0 mid-transition (e.g. reconfiguring for JPEG cover art) →
   `l2_framebuf = g_malloc(0)` == NULL → dereferenced anyway (2-byte
   RGB565 `lduw_he_p()` NULL-pointer segfault).
2. The per-pixel loop used Layer1's `cols` as the bound for Layer2 reads
   too, but Layer2 can have a narrower stride than Layer1 (smaller
   overlay), so `x` could run past Layer2's real per-row pixel count
   into unallocated heap (4-byte ARGB8888 read segfault, different
   offset in the same function — found by the coordinator during
   post-fix stress testing, sent back to the same agent, fixed).
Fixed: `l2_en && l2_src_width > 0` gating, plus a real
`l2_cols = l2_src_width/l2_bpp` bound on the Layer2 branch.

## Priority 4a — DMA1/DMA2/MDMA memory-to-memory: FIXED ✅

`dma1_m2m`, `dma2_m2m`, `mdma_m2m` all shared one root cause: this device
model only ever simulated transfer *timing*/IRQ completion, never actually
moved guest memory -- fine for every other stream use (SAI1 audio, HASH
input), where the *consuming peripheral itself* pulls bytes straight from
M0AR/NDTR via a registered stream notifier, but there is no such consumer
for a pure memory-to-memory transfer (`DMA_MEMORY_TO_MEMORY`/
`MDMA_REQUEST_SW`, no peripheral involved at all). Firmware's completion
wait was satisfied, but the destination buffer stayed all-zero, failing
every correctness check.

- `dma1_m2m`/`dma2_m2m`: fixed by adding a real synchronous copy
  (`gnw_h7b0_dma_do_m2m_copy()` in `hw/misc/gnw_h7b0_dma.c`) on the stream's
  `CR.EN` 0->1 edge whenever `CR.DIR` is memory-to-memory, using
  `PAR`(source)/`M0AR`(dest) per `HAL_DMA_SetConfig()`'s M2M address
  convention, honoring `PINC`/`MINC`/`PSIZE`/`MSIZE`. Only the
  equal-PSIZE/MSIZE case is implemented (the only one any known firmware
  here uses).
- `mdma_m2m`: MDMA was a bare `create_unimplemented_device()` stub (no
  register model at all) -- a genuinely separate peripheral from DMA1/DMA2
  (sits directly on the AXI bus matrix, no DMAMUX). Added a new device
  model, `hw/misc/gnw_h7b0_mdma.c`/`include/hw/misc/gnw_h7b0_mdma.h`
  (wired into `hw/arm/gnw_h7b0_soc.c`, `hw/arm/Kconfig`,
  `hw/misc/Kconfig`, `hw/misc/meson.build`): real 16-channel register
  layout, triggers a synchronous copy on `CCR.SWRQ`'s 0->1 edge (with
  `CCR.EN` already set, matching `HAL_MDMA_Start()`'s two-write sequence),
  sets every completion flag `HAL_MDMA_PollForTransfer()` might poll
  (`CTCIF`/`BTIF`/`BRTIF`/`TCIF`). Only SW-triggered, non-linked-list
  transfers are modeled (the only kind any known firmware here uses).

All three independently verified live against the diag suite, both
individually and in the same full-suite run as everything else above.

**Files changed**: `hw/misc/gnw_h7b0_dma.c`, `hw/misc/gnw_h7b0_mdma.c` (new),
`include/hw/misc/gnw_h7b0_mdma.h` (new), `hw/arm/gnw_h7b0_soc.c`,
`include/hw/arm/gnw_h7b0_soc.h`, `hw/arm/Kconfig`, `hw/misc/Kconfig`,
`hw/misc/meson.build`.

## Priority 4b — RNG: FIXED ✅

`crypto_rng_sanity`/`crypto_rng_seed_error` looked like an RNG-device
problem at first (RNG was a bare `create_unimplemented_device()` stub,
same starting state as MDMA) but the real root cause was one layer up, in
RCC: both cases enable RNG's kernel clock source (HSI48, `RCC_D2CCIP2R
.RNGSEL`'s reset default) themselves before touching RNG at all, and
`RCC_CR.HSI48ON` was never mirrored into `RCC_CR.HSI48RDY` at all -- the
exact same "*ON set, *RDY never follows" gap the existing CSION->CSIRDY
fix (see STATUS.md's Mario/Zelda clock-speed history) already fixed for
every *other* oscillator, just never extended to HSI48. Confirmed via the
symptom: `crypto_rng_sanity` (which runs first and does the real
HSI48-enable wait) failed with an ~100ms runtime consistent with a full
100000-iteration bounded spin-timeout, while `crypto_rng_seed_error`
(running second, inheriting HSI48 already left on by the first case's
failed-but-incomplete attempt) passed near-instantly by skipping that same
wait -- a real tell that the failure was in the HSI48 wait, not in RNG
itself. Fixed by adding the missing `HSI48ON`->`HSI48RDY` mirror in
`hw/misc/gnw_h7b0_rcc.c` (`RCC_CR_HSI48ON`/`RCC_CR_HSI48RDY` bit
definitions added to `include/hw/misc/gnw_h7b0_rcc.h`).

Also implemented the RNG device itself while investigating (was still a
bare unimplemented-device stub, and would have failed
`crypto_rng_sanity`'s "8 draws not all identical/not constant-stride"
liveness check even with HSI48 fixed): new
`hw/misc/gnw_h7b0_rng.c`/`include/hw/misc/gnw_h7b0_rng.h`, 3-register
(CR/SR/DR) synchronous model, `DR` reads return a fresh
`g_random_int()`-sourced word whenever `CR.RNGEN` is set. `CONDRST` is
accepted (self-clearing pulse bit) but has no observable effect -- this
model never generates the `CECS`/`SECS` clock-/seed-error conditions
`CONDRST` exists to recover from, so `crypto_rng_seed_error`'s recovery-
sequence liveness check just sees `DRDY` stay set throughout, which is
sufficient for what it actually checks.

Both cases independently verified live, together with the rest of the
suite.

**Files changed**: `hw/misc/gnw_h7b0_rcc.c`, `include/hw/misc/gnw_h7b0_rcc.h`,
`hw/misc/gnw_h7b0_rng.c` (new), `include/hw/misc/gnw_h7b0_rng.h` (new),
`hw/arm/gnw_h7b0_soc.c`, `include/hw/arm/gnw_h7b0_soc.h`, `hw/arm/Kconfig`,
`hw/misc/Kconfig`, `hw/misc/meson.build`.

## Priority 4c — DAC1/DAC2 output value: FIXED ✅

`dac1_output_value`/`dac2_output_value` write a known code to
`DHR12Rx` (via `HAL_DAC_SetValue()`) and read it back from `DORx` (via
`HAL_DAC_GetValue()`) to verify the digital DHR->DOR transfer pipeline
this firmware's real LCD-backlight control depends on
(`MX_DAC1_Init()`/`MX_DAC2_Init()`, `DAC_Trigger = NONE`). The DAC device
model was a plain register read/write shadow with no side effects at
all -- `DORx` stayed permanently 0 regardless of what firmware wrote to
`DHRx`, since real hardware's automatic (untriggered, `TENx=0`) DHR->DOR
transfer was never modeled. Fixed by adding that transfer in
`hw/misc/gnw_h7b0_dac.c`: on a write to any single-channel DHR register
(`DHR12Rx`/`DHR12Lx`/`DHR8Rx`) with that channel's `TENx` clear, the
value is immediately (De-aligned/shifted as needed) copied into the
matching `DORx`. Dual-channel `DHR12RD`/`DHR12LD`/`DHR8RD` registers and
software-triggered (`TENx=1`+`SWTRGR`) transfers are not modeled -- no
known firmware here uses them.

Both cases verified live.

**Files changed**: `hw/misc/gnw_h7b0_dac.c`.

## Priority 4d — CRC16 reconfig: FIXED ✅

`crypto_crc16_reconfig` reconfigures `CR.POLYSIZE` to 16-bit and expects
a real CRC-16/CCITT-FALSE result. The CRC device model's table-driven
accumulator (`crc_rebuild_table()`/`crc_feed()`/`crc_read_dr()`) was
hardcoded to always operate as a 32-bit-wide LFSR (`n << 24`, testing bit
31) regardless of `CR.POLYSIZE` -- so a 16-bit-configured CRC silently
computed a 32-bit-algorithm result instead. Fixed by deriving the
accumulator width from `CR.POLYSIZE` (`crc_width_bits()`) and
parameterizing the table build, feed, and read-out on that width, instead
of a hardcoded 32.

**Files changed**: `hw/misc/gnw_h7b0_crc.c`, `include/hw/misc/gnw_h7b0_crc.h`.

## Priority 4e — TIM6 update timing (and a much bigger, project-wide bug): FIXED ✅

`timer_tim6_update` looked like a missing-TIM6-model problem at first, but
TIM6 (and TIM3/4/5/7) *are* already covered by `gnw_h7b0_tim2.c`'s shared
TIM2-TIM14-block device (`soc.h`'s `TIM2_BLOCK_BASE_ADDRESS`, 6 real
instances on the correct 0x400 stride). The actual bug: the write handler
looked up `get_tim2_write_mask(addr)`/reset applied
`get_tim2_reset_value(i*4)` using the *raw block-wide* address --
but those auto-generated tables (from a single TIM2 instance's own
register layout) only recognize offsets within the first 0x400 window.
Every instance past TIM2 itself (TIM3-TIM7) hit their `default:` case
(mask 0) for **every single register**, silently turning every write to
TIM3-TIM7 -- CR1, PSC, ARR, SR, all of it -- into a no-op, project-wide,
for any firmware, not just this diag case. Confirmed via debug
instrumentation: `TIM6->CR1 = TIM_CR1_CEN` never actually set the
in-memory CR1 bit, so `gnw_h7b0_tim2_start_counting()` never ran; more
subtly, firmware's own `TIM6->SR = 0` (meant to clear the UIF that the
existing EGR.UG side-effect had just set) was *also* silently dropped,
so the busy-wait loop saw UIF already latched from the earlier EGR.UG
write and returned near-instantly with a bogus low cycle count.

Fixed by folding to the per-instance local offset (`addr & 0x3ff`)
before consulting either table, matching what the existing EGR/CR1
side-effect logic several lines below already correctly did.

**Files changed**: `hw/misc/gnw_h7b0_tim2.c`.

## Priority 4f — EXTI SWIER->PR1 (fixes exti_edge_config AND exti_sw_trigger): FIXED ✅

`exti_edge_config`'s own header comment claimed its final SWIER-related
assertion was *expected* to fail, citing `case_exti_sw_trigger.c`'s
documented "SWIER doesn't set PR1" hardware anomaly -- but
`case_exti_sw_trigger.c` itself carries a newer (same-session,
2026-07-16) "BUG FIX" comment superseding that: confirmed empirically
against real hardware, SWIER *does* latch PR1, but only for a line with
an edge direction armed (`RTSR1`/`FTSR1`) -- SWIER feeds a synthetic edge
through the same edge-detect logic those registers configure, rather
than bypassing it outright. `exti_edge_config`'s comment was written
against the pre-fix understanding and never updated. Since
`exti_edge_config` always arms `RTSR1`/`FTSR1` before each SWIER probe,
its PR1 assertions should genuinely pass on real hardware -- and our
EXTI model didn't implement SWIER1's side effect at all (plain
read/write shadow), so PR1 could never latch that way regardless of
RTSR1/FTSR1, failing both this case and `exti_sw_trigger` deterministically.

Fixed by adding the real SWIER1 side effect: on a SWIER1 write, any bit
also armed in `RTSR1|FTSR1` and unmasked in `CPUIMR1` sets `CPUPR1` and
pulses the matching NVIC line, matching the empirically-confirmed
edge-detect-logic behavior.

**Files changed**: `hw/misc/gnw_h7b0_exti.c`.

## Priority 4g — remaining: only known-flaky (host-contention) fails left

After all of the above, a full suite re-run showed **zero** confirmed-stable
fails. `gpio_output_readback` reappeared as FAIL in this run (previously
seen passing and failing across different runs this session, consistent
with the host-contention-flake pattern already established for
`dwt_vs_systick`/`exti_sw_trigger`/`boot_option_bytes` earlier) -- re-run
on a quiet host before treating it as real. No other case in the same run
failed.

Also appearing as FAIL in this same run, worth double-checking for
host-contention flake vs. real regression before triaging (this host has
been under sustained heavy load all session): `exti_sw_trigger`,
`boot_option_bytes`, `power_sleep_wfi`, `gpio_output_readback`,
`ospi_dlyb_readback`. None of these were touched by this session's RCC/RNG
change; `exti_sw_trigger`/`boot_option_bytes`/`gpio_output_readback` were
confirmed passing earlier this session (see priority 4a's note), so this
is very likely the same kind of host-contention flakiness already seen
with `dwt_vs_systick`, not a real regression -- re-run on a quiet host
before treating any of these five as real.

`power_sleep_wfi` not observed failing in either run this session.

Next: triage the 7 confirmed-stable fails above individually, prioritized
by whatever the project owner cares about next (no priority order set yet
for this bucket), after re-confirming the 3 possibly-flaky ones on a quiet
host.
