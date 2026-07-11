/*
 * STM32H7B0 RCC minimal stub (Nintendo Game & Watch)
 *
 * Not cycle-accurate, not a full RCC model. Purpose: real firmware's
 * clock-init code writes an *ON bit then polls the matching *RDY bit in
 * RCC_CR (and SW then polls SWS in RCC_CFGR, LSION then polls LSIRDY
 * in RCC_CSR, and LSEON then polls LSERDY in RCC_BDCR) and must not
 * hang forever doing so -- see
 * docs/roadmap.md Phase 1. Every other register is a plain
 * read-what-was-written shadow with no side effects.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_MISC_GNW_H7B0_RCC_H
#define HW_MISC_GNW_H7B0_RCC_H

#include "hw/sysbus.h"
#include "hw/clock.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_RCC "gnw-h7b0-rcc"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0RccState, GNW_H7B0_RCC)

/*
 * Register offsets/bits below are from
 * sdk/cmsis-device-h7/Include/stm32h7b0xx.h's RCC_TypeDef and RCC_CR_x /
 * RCC_CFGR_x bit definitions (run scripts/fetch-sdk.sh if sdk/ is
 * missing), cross-checked against RM0455's register boundary table
 * (RCC at 0x58024400-0x580247FF) and STM32H7B0.svd.
 */
#define GNW_H7B0_RCC_SIZE   0x400

#define GNW_H7B0_RCC_CR     0x00
#define RCC_CR_HSION        (1U << 0)
#define RCC_CR_HSIRDY       (1U << 2)
#define RCC_CR_HSEON        (1U << 16)
#define RCC_CR_HSERDY       (1U << 17)
#define RCC_CR_PLL1ON       (1U << 24)
#define RCC_CR_PLL1RDY      (1U << 25)
#define RCC_CR_PLL2ON       (1U << 26)
#define RCC_CR_PLL2RDY      (1U << 27)
#define RCC_CR_PLL3ON       (1U << 28)
#define RCC_CR_PLL3RDY      (1U << 29)

#define GNW_H7B0_RCC_CFGR   0x10
#define RCC_CFGR_SW_SHIFT   0
#define RCC_CFGR_SW_MASK    (0x7U << RCC_CFGR_SW_SHIFT)
#define RCC_CFGR_SWS_SHIFT  3
#define RCC_CFGR_SWS_MASK   (0x7U << RCC_CFGR_SWS_SHIFT)

#define GNW_H7B0_RCC_CSR    0x74
#define RCC_CSR_LSION       (1U << 0)
#define RCC_CSR_LSIRDY      (1U << 1)

#define GNW_H7B0_RCC_BDCR   0x70
#define RCC_BDCR_LSEON      (1U << 0)
#define RCC_BDCR_LSERDY     (1U << 1)

/*
 * RCC_RSR (reset status/cause flags) resets to 0x00680000 here --
 * PINRSTF|BORRSTF|CDRSTF set, PORRSTF deliberately left clear. A real
 * power-on sets all four together (0x00E80000, per STM32H7B0.svd/CMSIS,
 * these are not mutually exclusive causes) and a plain zeroed reset
 * (this device's generic behavior otherwise) reads as "no reset cause
 * at all", which real firmware doesn't expect -- found via a retro-go
 * boot's reset-cause check falling through to an "Boot from
 * brownout?" fallback path that isn't meant for a normal boot.
 * PORRSTF specifically must stay clear: gnw-chainloader's stub_main()
 * treats PORRSTF as "genuine cold power-on" and deliberately parks in
 * WFI forever (SLEEPDEEP standby, waiting on the physical WKUP1 button
 * pin we don't model) to prevent auto-boot on USB plug-in. Real
 * hardware only sees a true POR once, at first power-up; QEMU resets
 * to this value on every single launch, so leaving PORRSTF set turned
 * every emulated boot into that permanent standby trap. PINRSTF (pin
 * reset) is a more accurate stand-in for "board was just reset/
 * relaunched", which is what every QEMU boot actually is.
 */
#define GNW_H7B0_RCC_RSR        0x130
#define RCC_RSR_RESET_VALUE     0x00680000U
#define RCC_RSR_RMVF            (1U << 16)

/*
 * PLL2 fractional-N synthesis, decoded on demand by
 * gnw_h7b0_rcc_get_pll2p_hz()/gnw_h7b0_rcc_get_sai1_kernel_hz() -- added so
 * gnw_h7b0_sai1.c can derive the *actual* SAI1 sample rate firmware
 * reconfigures per game core (see docs/... audio jank investigation:
 * odroid_audio.c's set_audio_frequency() reprograms these registers at
 * runtime to hit each core's native rate, e.g. 22050/32768/53050/etc, not
 * just the default 48kHz set up at boot). Bit positions from
 * sdk/cmsis-device-h7/Include/stm32h7b0xx.h's RCC_PLLCKSELR_, RCC_PLLCFGR_,
 * RCC_PLL2DIVR_, RCC_PLL2FRACR_, RCC_CDCCIP1R_ macros. These offsets
 * (GNW_H7B0_RCC_PLLCKSELR_OFFSET etc) are already defined in
 * gnw_h7b0_regs_rcc.h as plain shadow registers -- only decode logic is new
 * here, no new register storage.
 */
#define RCC_PLLCKSELR_DIVM1_SHIFT   4
#define RCC_PLLCKSELR_DIVM1_MASK    (0x3fU << RCC_PLLCKSELR_DIVM1_SHIFT)
#define RCC_PLLCKSELR_DIVM2_SHIFT   12
#define RCC_PLLCKSELR_DIVM2_MASK    (0x3fU << RCC_PLLCKSELR_DIVM2_SHIFT)
#define RCC_PLLCKSELR_DIVM3_SHIFT   20
#define RCC_PLLCKSELR_DIVM3_MASK    (0x3fU << RCC_PLLCKSELR_DIVM3_SHIFT)
#define RCC_PLLCKSELR_PLLSRC_SHIFT  0
#define RCC_PLLCKSELR_PLLSRC_MASK   (0x3U << RCC_PLLCKSELR_PLLSRC_SHIFT)
#define RCC_PLLCKSELR_PLLSRC_HSI    0
#define RCC_PLLCKSELR_PLLSRC_CSI    1
#define RCC_PLLCKSELR_PLLSRC_HSE    2

#define RCC_PLLCFGR_PLL1FRACEN      (1U << 0)
#define RCC_PLLCFGR_PLL2FRACEN      (1U << 4)
#define RCC_PLLCFGR_PLL3FRACEN      (1U << 8)

#define RCC_PLL1DIVR_N1_SHIFT       0
#define RCC_PLL1DIVR_N1_MASK        (0x1ffU << RCC_PLL1DIVR_N1_SHIFT)
#define RCC_PLL1DIVR_P1_SHIFT       9
#define RCC_PLL1DIVR_P1_MASK        (0x7fU << RCC_PLL1DIVR_P1_SHIFT)

#define RCC_PLL1FRACR_FRACN1_SHIFT  3
#define RCC_PLL1FRACR_FRACN1_MASK   (0x1fffU << RCC_PLL1FRACR_FRACN1_SHIFT)

#define RCC_PLL2DIVR_N2_SHIFT       0
#define RCC_PLL2DIVR_N2_MASK        (0x1ffU << RCC_PLL2DIVR_N2_SHIFT)
#define RCC_PLL2DIVR_P2_SHIFT       9
#define RCC_PLL2DIVR_P2_MASK        (0x7fU << RCC_PLL2DIVR_P2_SHIFT)

#define RCC_PLL2FRACR_FRACN2_SHIFT  3
#define RCC_PLL2FRACR_FRACN2_MASK   (0x1fffU << RCC_PLL2FRACR_FRACN2_SHIFT)

/*
 * PLL3 -- same fractional-N family as PLL1/PLL2 (see
 * gnw_h7b0_rcc_get_pll2p_hz()'s comment for the shared formula/raw-field
 * rationale), added so gnw_h7b0_ltdc.c can derive the LTDC pixel clock
 * (pll3_r_ck -- LTDC has no clock-source mux, it's hardwired to PLL3R
 * unlike SAI1/ADC) instead of assuming a fixed vblank rate. Needs R, not
 * P: PLL3DIVR's R3 field sits at a different bit offset than P1/P2 (wider
 * field, higher offset), since LTDC needs the third PLL output, the first
 * of the three actually decoded by this device.
 */
#define RCC_PLL3DIVR_N3_SHIFT       0
#define RCC_PLL3DIVR_N3_MASK        (0x1ffU << RCC_PLL3DIVR_N3_SHIFT)
#define RCC_PLL3DIVR_R3_SHIFT       24
#define RCC_PLL3DIVR_R3_MASK        (0x7fU << RCC_PLL3DIVR_R3_SHIFT)

#define RCC_PLL3FRACR_FRACN3_SHIFT  3
#define RCC_PLL3FRACR_FRACN3_MASK   (0x1fffU << RCC_PLL3FRACR_FRACN3_SHIFT)

/* CFGR.SWS (bits[6:3]) selected clock, decoded by
 * gnw_h7b0_rcc_get_sysclk_hz() -- see gnw_h7b0_rcc.c. */
#define RCC_CFGR_SWS_HSI            0
#define RCC_CFGR_SWS_CSI            1
#define RCC_CFGR_SWS_HSE            2
#define RCC_CFGR_SWS_PLL1           3

#define RCC_CDCCIP1R_SAI1SEL_SHIFT  0
#define RCC_CDCCIP1R_SAI1SEL_MASK   (0x7U << RCC_CDCCIP1R_SAI1SEL_SHIFT)
/* Per stm32h7xx_hal_rcc_ex.h: RCC_SAI1CLKSOURCE_PLL2 ==
 * RCC_CDCCIP1R_SAI1SEL_0 (value 1) -- SAI1SEL==0 is actually
 * RCC_SAI1CLKSOURCE_PLL (pll1_q_ck), not PLL2. */
#define RCC_CDCCIP1R_SAI1SEL_PLL2   1

/* Oscillator values for this board -- see game-and-watch-retro-go-sd's
 * Core/Inc/stm32h7xx_hal_conf.h HSE_VALUE/HSI_VALUE. Fixed, never
 * reprogrammed at runtime (only PLL2's own dividers/FRACN are).
 *
 * Firmware's SystemClock_Config (main.c) sets
 * RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI and never enables
 * RCC_OSCILLATORTYPE_HSE at all -- this board runs its whole clock tree,
 * PLL2 included, off the internal 64MHz HSI, not an external crystal.
 * Assuming HSE unconditionally here (an easy assumption given "HSE" is in
 * every PLL2M/N/P register/variable name) previously produced a PLL2P
 * that was off by exactly HSI_VALUE/HSE_VALUE = 64/24 = 8/3 from real
 * firmware's own computed SAI kernel clock -- confirmed by gdb-tracing
 * HAL_SAI_Init's freshly-*computed* hsai->Init.Mckdiv live (not the
 * struct's stale leftover value from a prior call) against two
 * independent real PLL2 configs and solving for what oscillator input
 * makes the real Mckdiv the formula predicts: both landed on HSE_HZ=64MHz. */
#define GNW_H7B0_RCC_HSE_HZ  24000000U
#define GNW_H7B0_RCC_HSI_HZ  64000000U
/* CSI's default per ST parts (CSI_VALUE in stm32h7xx_hal_conf.h) -- this
 * firmware never selects it (SystemClock_Config only ever uses HSI, see
 * gnw_h7b0_rcc_get_pll2p_hz()'s PLLSRC comment), included only so
 * gnw_h7b0_rcc_get_sysclk_hz()'s CFGR.SWS decode is complete/correct if a
 * guest ever did switch to it transiently. */
#define GNW_H7B0_RCC_CSI_HZ  4000000U

struct GnwH7B0RccState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_RCC_SIZE / 4];

    /* Not owned; set by the board via gnw_h7b0_rcc_set_sysclk() so PLL1/
     * SYSCLK register writes can push a live recompute out through the
     * existing sysclk Clock -> ARMv7M cpuclk/refclk wiring (gnw_h7b0.c/
     * gnw_h7b0_soc.c already own and connect that Clock; RCC only gets a
     * pointer to push updates into it, same as sai1_set_rcc()'s pattern
     * for the reverse direction). May be NULL (e.g. in unit tests that
     * instantiate RCC standalone) -- callers must check before use. */
    Clock *sysclk;
};

uint32_t gnw_h7b0_rcc_get_pll2p_hz(GnwH7B0RccState *s);
uint32_t gnw_h7b0_rcc_get_pll1p_hz(GnwH7B0RccState *s);
uint32_t gnw_h7b0_rcc_get_pll3r_hz(GnwH7B0RccState *s);
uint32_t gnw_h7b0_rcc_get_sysclk_hz(GnwH7B0RccState *s);
uint32_t gnw_h7b0_rcc_get_hclk_hz(GnwH7B0RccState *s);
uint32_t gnw_h7b0_rcc_get_sai1_kernel_hz(GnwH7B0RccState *s);
void gnw_h7b0_rcc_set_sysclk(GnwH7B0RccState *s, Clock *sysclk);

#endif
