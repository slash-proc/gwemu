# H7B0 clock-tree findings

Hard-won facts about this board's real clock tree, from live gdb tracing
of real firmware (game-and-watch-retro-go-sd) against this repo's RCC
model this session. Written so the next peripheral that needs a real
clock frequency (ADC timing, a new timer-driven device, further SPI/OSPI
baud-rate fidelity) can consult this instead of re-deriving or re-guessing
from scratch — three of the four findings below were confirmed *bugs*
caused by exactly that re-guessing pattern.

## 1. The real oscillator is HSI (64MHz), not HSE (24MHz)

Real firmware's `SystemClock_Config` (game-and-watch-retro-go-sd's
`Core/Src/main.c`) sets `RCC_OscInitStruct.OscillatorType =
RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_LSE` and
`RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI` — HSE is never
enabled. `HSI_VALUE` is 64000000 (`Core/Inc/stm32h7xx_hal_conf.h`). This
board has no external crystal in active use: despite every PLL2 M/N/P
register and HAL struct field being conventionally named after "HSE" in
ST's headers, the oscillator actually feeding PLL2 (and the whole clock
tree) is the internal 64MHz HSI.

This was a real, confirmed bug: `hw/misc/gnw_h7b0_rcc.c`'s
`gnw_h7b0_rcc_get_pll2p_hz()` originally hardcoded `HSE_VALUE=24000000`
unconditionally, producing audio sample rates off by a consistent factor
of 64/24 = 8/3. Confirmed via live gdb tracing of real firmware's own
freshly-computed `hsai_BlockA1.Init.Mckdiv` against two independent real
PLL2 configurations (a HAL-auto-computed 48000Hz case for NES, and
Celeste's 22050Hz-target PLL2 config) — both only reconciled with real
firmware's actual on-hardware Mckdiv once HSI=64MHz was used as the
oscillator input instead of HSE=24MHz.

Fix (already applied): `gnw_h7b0_rcc_get_pll2p_hz()` decodes
`PLLCKSELR`'s `PLLSRC` field (bits[1:0]; 0=HSI, 1=CSI, 2=HSE per
`RCC_PLLCKSELR_PLLSRC_HSI/CSI/HSE` in `include/hw/misc/gnw_h7b0_rcc.h`)
and picks `GNW_H7B0_RCC_HSI_HZ` (64000000) or `GNW_H7B0_RCC_HSE_HZ`
(24000000) accordingly, defaulting to HSI for the reserved/CSI encoding
too since this firmware never exercises those paths.

## 2. PLL2 VCO/PLL2P formula

Verified bit-exact against real firmware's own compiled float math
(`HAL_RCCEx_GetPLL2ClockFreq`, `stm32h7xx_hal_rcc_ex.c`), via
objdump/gdb disassembly of a matching-build ELF
(`retro-go-temp/elf/gw_retro_go_bank1.elf`, later a freshly self-built
`game-and-watch-retro-go-sd/build/gw_retro_go.elf`):

```
VCO    = (osc_hz / DIVM2) * (N2_raw + FRACN2/8192 + 1)
PLL2P  = VCO / (P2_raw + 1)
```

- `osc_hz` is HSI or HSE per finding #1.
- `DIVM2` is `PLLCKSELR`'s DIVM2 field (bits[17:12]), used as-is.
- `N2_raw`/`P2_raw` are `PLL2DIVR`'s raw N2 (bits[8:0]) / P2 (bits[15:9])
  register **field values**, not the "struct" value passed to
  `HAL_RCCEx_PeriphCLKConfig`. `__HAL_RCC_PLL2_CONFIG`
  (`stm32h7xx_hal_rcc_ex.h`) stores `(PLL2N-1)`/`(PLL2P-1)` into these
  fields; the getter's own "+1" terms exactly cancel that back out for N
  (net: use `N2_raw` directly, no adjustment), while P's "+1" is applied
  explicitly at the division step. Getting this backwards — adding an
  extra +1 to N2 during decode — was a separate confirmed bug this
  session, silently producing numbers off by roughly N/(N-1), independent
  of and in addition to the HSI/HSE bug above.
- `FRACN2` is `PLL2FRACR`'s 13-bit field (bits[15:3]), only applied if
  `PLLCFGR`'s `PLL2FRACEN` bit (bit 4) is set.

Implementation: `hw/misc/gnw_h7b0_rcc.c`'s `gnw_h7b0_rcc_get_pll2p_hz()`.

SAI1's kernel clock is PLL2P when `CDCCIP1R`'s `SAI1SEL` field selects
PLL2 — but `SAI1SEL`'s PLL2 encoding is value **1**
(`RCC_SAI1CLKSOURCE_PLL2 == RCC_CDCCIP1R_SAI1SEL_0`), **not** 0 (0 is
`RCC_SAI1CLKSOURCE_PLL`, i.e. pll1_q_ck). This was also a confirmed bug
this session (inverted mux assumption), now fixed in
`gnw_h7b0_rcc_get_sai1_kernel_hz()` (`RCC_CDCCIP1R_SAI1SEL_PLL2` is
defined as 1 in `include/hw/misc/gnw_h7b0_rcc.h`).

## 3. TIM2–TIM14's real kernel clock equals HCLK (340512000 Hz), not an arbitrary guess

Real firmware's `SystemClock_Config` sets `AHBCLKDivider=RCC_HCLK_DIV1`
(HCLK=SYSCLK, no division) and `APB1CLKDivider=RCC_APB1_DIV2`
(PCLK1=HCLK/2). STM32H7's timer kernel clock uses the "x2" rule whenever
its APBx prescaler isn't /1 (`timer_clk = 2 * PCLK1` when the APB1
prescaler is > 1), which exactly cancels the /2. Net: TIM2's real kernel
clock = HCLK = SYSCLK (AHB is /1) = the same `SYSCLK_FRQ` constant
(`340512000ULL`) already used in `hw/arm/gnw_h7b0.c` — that constant was
itself reverse-engineered by reading a live `SystemCoreClock` value out
of guest RAM after boot (see that file's own comment above its
definition).

`hw/misc/gnw_h7b0_tim2.c`'s `GNW_H7B0_TIM2_APPROX_CLK_HZ` was previously
hardcoded to a guessed `200000000`; now corrected to `340512000ULL`
(same file, with a comment explaining the AHB/APB1-x2 derivation above
it).

## 4. General lesson: no real, shared clock-tree model exists yet

Every finding above — the SAI1SEL mux inversion, the PLL2 N-divider
off-by-one, the wrong-oscillator assumption, TIM2's wrong flat-clock
guess — is the same root architectural gap: this codebase has no real,
comprehensive RCC clock-tree model. Individual peripherals (SAI1/DMA
audio pacing, TIM2) each independently invented their own
hardcoded-but-"reasonable-sounding" clock assumption instead of
consulting one real, shared source of truth.

Current state:
- Only PLL2 (feeding SAI1) has real decode logic today —
  `gnw_h7b0_rcc_get_pll2p_hz()` / `gnw_h7b0_rcc_get_sai1_kernel_hz()` in
  `hw/misc/gnw_h7b0_rcc.c`.
- HCLK/PCLK1/PCLK2 and PLL1/PLL3 outputs are **not** modeled as live
  getters anywhere. TIM2's fix hardcodes the already-known-correct HCLK
  value as a constant rather than deriving it from RCC registers — fine
  today because HCLK is never reprogrammed at runtime by this firmware
  (AHB/APB prescalers are set once at boot in `SystemClock_Config`,
  confirmed by reading it; unlike PLL2, which firmware DOES reprogram
  per-game-core for audio). A future peripheral that needs a
  runtime-variable HCLK/PCLK (or PLL1/PLL3 output) will need a real
  getter, not another hardcoded constant.

**When adding real-clock-dependent behavior to a new peripheral**: check
here first for an existing getter; if none exists, decode the relevant
RCC registers the way `gnw_h7b0_rcc_get_pll2p_hz()` does (raw
register-derived, cross-checked against the actual HAL getter's compiled
math via gdb/objdump on a real firmware ELF) rather than hardcoding a
frequency that merely "sounds about right" — that pattern produced every
bug in this document.
