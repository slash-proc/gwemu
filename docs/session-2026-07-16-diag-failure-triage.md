# 2026-07-16 diag-suite failure triage (live tracker, prioritized)

Rewritten in place as status changes — this is the working checklist for
systematically fixing every current `../stm32h7b0-diag` failure, per the
project owner's explicit priority order: **CRC32 → HASH/HMAC → JPEG →
everything else**. Baseline snapshot (main checkout, before this pass):
63 OK / 39 FAIL / 8 UNRUN out of 110 total cases (see
`docs/session-2026-07-16-perf-expert-panel-and-jpeg-encode.md` for how
that baseline was captured).

**Current confirmed state (main checkout, all three priority-1/2 fixes
applied and independently verified by the coordinator): 85 OK / 18 FAIL
(0 genuine UNRUN — one blocked case remains, see JPEG section below;
some residual "UNRUN" count in raw harness output is blank padding slots
beyond the real registered case count, a harness quirk, not a real
gap).**

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

## Priority 4 — everything else (10 remaining fails, NOT YET TRIAGED individually)

Confirmed current fails after re-running the full live suite with CRC32 +
HASH/HMAC + LTDC + JPEG all applied (2026-07-16):
`dac1_output_value`, `dac2_output_value`, `dma1_m2m`, `crypto_crc16_reconfig`,
`timer_tim6_update`, `mdma_m2m`, `crypto_rng_sanity`, `crypto_rng_seed_error`,
`dma2_m2m`, `exti_edge_config`.

`dwt_vs_systick` (previously flagged as possibly a host-contention flake),
`boot_option_bytes`, `gpio_output_readback`, and `exti_sw_trigger` are no
longer failing in this run — consistent with the host-contention-flake
suspicion, not re-fixed by any code change. `power_sleep_wfi` also not
observed failing this run.

Next: triage the 10 confirmed fails above individually, prioritized by
whatever the project owner cares about next (no priority order set yet
for this bucket).
