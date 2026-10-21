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

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "exec/icount.h"
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
#define RCC_CR_CSION        (1U << 7)
#define RCC_CR_CSIRDY       (1U << 8)
#define RCC_CR_PLL1ON       (1U << 24)
#define RCC_CR_PLL1RDY      (1U << 25)
#define RCC_CR_PLL2ON       (1U << 26)
#define RCC_CR_PLL2RDY      (1U << 27)
#define RCC_CR_PLL3ON       (1U << 28)
#define RCC_CR_PLL3RDY      (1U << 29)
#define RCC_CR_HSI48ON      (1U << 12)
#define RCC_CR_HSI48RDY     (1U << 13)

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
#define RCC_BDCR_BDRST      (1U << 16)

/*
 * RCC_RSR (reset status/cause flags) resets to 0x01680000 here --
 * SFTRSTF|PINRSTF|BORRSTF|CDRSTF set, PORRSTF deliberately left clear. A real
 * power-on sets PINRSTF|BORRSTF|CDRSTF|PORRSTF together (0x00E80000, per
 * STM32H7B0.svd/CMSIS, these are not mutually exclusive causes) and a plain
 * zeroed reset (this device's generic behavior otherwise) reads as "no reset
 * cause at all", which real firmware doesn't expect -- found via a retro-go
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
 *
 * SFTRSTF (software-reset flag, bit 24) added on top of the above: a stock
 * firmware image was found gating its entire LTDC/graphics init path on
 * `RCC_RSR & (SFTRSTF|WWDGRSTF)` being nonzero (real disassembly: `tst
 * RCC_RSR, #0x11000000; beq <skip-ltdc-init>`). Real G&W hardware boots
 * through a stage-0 bootloader (see ../../../game-and-watch-stage0) that
 * jumps into application firmware via a software reset; stock firmware
 * expects to see SFTRSTF set as a signal that stage0 already ran and did
 * its own init. We load application firmware directly via -kernel, skipping
 * stage0 entirely, so without SFTRSTF here the image permanently takes the
 * branch meant to run only after that handoff and never initializes LTDC.
 * Setting SFTRSTF unconditionally simulates "this looks like a stage0
 * handoff" without actually chainloading a stage0 image.
 */
#define GNW_H7B0_RCC_RSR        0x0d0
#define RCC_RSR_RESET_VALUE     0x01680000U
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

/* See gnw_h7b0_rcc_add_reset_target(). */
typedef struct GnwH7B0RccResetTarget {
    hwaddr rstr_offset;
    unsigned bit;
    DeviceState *dev;
} GnwH7B0RccResetTarget;

#define GNW_H7B0_RCC_RESET_TARGET_MAX 8

/* APB timer-kernel clock domains observable by the timer models. */
typedef enum GnwH7B0RccTimerClockDomain {
    GNW_H7B0_RCC_TIMER_APB1,
    GNW_H7B0_RCC_TIMER_APB2,
} GnwH7B0RccTimerClockDomain;

typedef void (*GnwH7B0RccTimerClockChangeFn)(void *opaque,
                                              uint32_t old_hz,
                                              uint32_t new_hz);

typedef struct GnwH7B0RccTimerClockObserver {
    GnwH7B0RccTimerClockDomain domain;
    GnwH7B0RccTimerClockChangeFn callback;
    void *opaque;
} GnwH7B0RccTimerClockObserver;

typedef enum GnwH7B0RccTimingDomain {
    GNW_H7B0_RCC_TIMING_CORE,
    GNW_H7B0_RCC_TIMING_APB1,
    GNW_H7B0_RCC_TIMING_APB2,
} GnwH7B0RccTimingDomain;

typedef bool (*GnwH7B0RccTimingPrepareFn)(
    void *opaque, const IcountCycleStamp *before, uint32_t peripheral_hz);
typedef void (*GnwH7B0RccTimingCommitFn)(
    void *opaque, const IcountCycleStamp *before,
    const IcountCycleStamp *after, uint32_t peripheral_hz);
typedef void (*GnwH7B0RccTimingAbortFn)(void *opaque);

typedef struct GnwH7B0RccTimingObserver {
    GnwH7B0RccTimingDomain domain;
    GnwH7B0RccTimingPrepareFn prepare;
    GnwH7B0RccTimingCommitFn commit;
    GnwH7B0RccTimingAbortFn abort;
    void *opaque;
} GnwH7B0RccTimingObserver;

/* ADC1/ADC2 have both an RCC-selected asynchronous kernel input and a
 * synchronous input derived from sys_ck_cpu/HPRE. Notify the shared ADC12
 * block whenever either input rate changes; it selects the active mode and
 * applies its common CCR prescaler. */
typedef void (*GnwH7B0RccAdcClockChangeFn)(void *opaque);

typedef struct GnwH7B0RccAdcClockObserver {
    GnwH7B0RccAdcClockChangeFn callback;
    void *opaque;
} GnwH7B0RccAdcClockObserver;

/* OCTOSPI1/2 share CDCCIP2R.OCTOSPISEL and the per_ck mux. */
typedef void (*GnwH7B0RccOctospiClockChangeFn)(void *opaque,
                                               uint32_t old_hz,
                                               uint32_t new_hz);

typedef struct GnwH7B0RccOctospiClockObserver {
    GnwH7B0RccOctospiClockChangeFn callback;
    void *opaque;
} GnwH7B0RccOctospiClockObserver;

/* `known` separates modeled internal SAI1SEL sources from external I2S_CKIN;
 * `kernel_hz == 0` is an authoritative stopped/reserved source. The APB2
 * peripheral gate is reported separately because a live kernel can be gated
 * at SAI1. */
typedef struct GnwH7B0RccSai1ClockInfo {
    bool known;
    bool enabled;
    uint32_t kernel_hz;
} GnwH7B0RccSai1ClockInfo;

typedef void (*GnwH7B0RccSai1ClockChangeFn)(void *opaque);

typedef struct GnwH7B0RccSai1ClockObserver {
    GnwH7B0RccSai1ClockChangeFn callback;
    void *opaque;
} GnwH7B0RccSai1ClockObserver;

/* AHB3 engine reference clocks; rates do not imply service throughput. */
typedef enum GnwH7B0RccEngine {
    GNW_H7B0_RCC_ENGINE_MDMA,
    GNW_H7B0_RCC_ENGINE_DMA2D,
    GNW_H7B0_RCC_ENGINE_COUNT,
} GnwH7B0RccEngine;

typedef struct GnwH7B0RccEngineClockInfo {
    bool known;
    bool enabled;
    uint32_t reference_hz;
} GnwH7B0RccEngineClockInfo;

typedef void (*GnwH7B0RccEngineClockChangeFn)(
    void *opaque, GnwH7B0RccEngineClockInfo old_info,
    GnwH7B0RccEngineClockInfo new_info);

typedef struct GnwH7B0RccEngineClockObserver {
    GnwH7B0RccEngine engine;
    GnwH7B0RccEngineClockChangeFn callback;
    void *opaque;
} GnwH7B0RccEngineClockObserver;

typedef void (*GnwH7B0RccBackupDomainResetFn)(void *opaque);

#define GNW_H7B0_RCC_ENGINE_CLOCK_OBSERVER_MAX 4

#define GNW_H7B0_RCC_TIMER_CLOCK_OBSERVER_MAX 8
#define GNW_H7B0_RCC_TIMING_OBSERVER_MAX 8
#define GNW_H7B0_RCC_ADC_CLOCK_OBSERVER_MAX 4
#define GNW_H7B0_RCC_OCTOSPI_CLOCK_OBSERVER_MAX 8
#define GNW_H7B0_RCC_SAI1_CLOCK_OBSERVER_MAX 4

struct GnwH7B0RccState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_RCC_SIZE / 4];
    /* FRACR is a writable shadow; hardware latches FRACN on FRACEN 0->1. */
    uint32_t pll1_fracn_latched;
    uint32_t pll2_fracn_latched;
    uint32_t pll3_fracn_latched;
    GnwH7B0RccResetTarget reset_targets[GNW_H7B0_RCC_RESET_TARGET_MAX];
    int n_reset_targets;
    GnwH7B0RccTimerClockObserver
        timer_clock_observers[GNW_H7B0_RCC_TIMER_CLOCK_OBSERVER_MAX];
    int n_timer_clock_observers;
    GnwH7B0RccTimingObserver
        timing_observers[GNW_H7B0_RCC_TIMING_OBSERVER_MAX];
    int n_timing_observers;
    GnwH7B0RccAdcClockObserver
        adc_clock_observers[GNW_H7B0_RCC_ADC_CLOCK_OBSERVER_MAX];
    int n_adc_clock_observers;
    GnwH7B0RccOctospiClockObserver
        octospi_clock_observers[GNW_H7B0_RCC_OCTOSPI_CLOCK_OBSERVER_MAX];
    int n_octospi_clock_observers;
    GnwH7B0RccSai1ClockObserver
        sai1_clock_observers[GNW_H7B0_RCC_SAI1_CLOCK_OBSERVER_MAX];
    int n_sai1_clock_observers;

    GnwH7B0RccEngineClockObserver
        engine_clock_observers[GNW_H7B0_RCC_ENGINE_CLOCK_OBSERVER_MAX];
    int n_engine_clock_observers;

    GnwH7B0RccBackupDomainResetFn backup_domain_reset;
    void *backup_domain_reset_opaque;

    /* Not owned; set by the SoC so RCC writes can update the Cortex-M core
     * clock and SysTick's HCLK/8 reference clock independently. May be NULL
     * in standalone RCC tests. */
    Clock *sysclk;
    Clock *systick_refclk;
};

uint32_t gnw_h7b0_rcc_get_pll2p_hz(GnwH7B0RccState *s);
/* True while RCC_APB2ENR.SAI1EN is set, i.e. SAI1 is clocked at all. */
bool gnw_h7b0_rcc_sai1_clock_enabled(GnwH7B0RccState *s);
/* Return the effective SAI1 kernel frequency for modeled internal mux
 * sources. Supported but stopped sources return true with *kernel_hz == 0;
 * unmodeled external sources return false, distinguishing UNKNOWN from
 * STOPPED for timing consumers. */
bool gnw_h7b0_rcc_get_sai1_clock_info(GnwH7B0RccState *s,
                                      uint32_t *kernel_hz);
void gnw_h7b0_rcc_add_sai1_clock_observer(
    GnwH7B0RccState *s, GnwH7B0RccSai1ClockChangeFn callback,
    void *opaque);
/*
 * Register a device to be held in reset by one of RCC's peripheral-reset
 * (xxxRSTR) bits: writing the bit to 1 asserts the peripheral's reset,
 * which on real silicon returns all of its registers to their reset
 * values (RM0455 RCC, "peripheral reset registers").
 *
 * This is not academic. `HAL_DeInit()` force-resets *every* bus, and
 * retro-go calls it on the way out of a game, immediately before
 * jumping back to the launcher (odroid_system.c's boot path). Without
 * it, a DMA stream armed by the game survives into the freshly started
 * launcher -- whose HAL handles are zeroed .bss -- and the first
 * half-transfer interrupt it raises can never be acknowledged.
 */
void gnw_h7b0_rcc_add_reset_target(GnwH7B0RccState *s, hwaddr rstr_offset,
                                    unsigned bit, DeviceState *dev);
void gnw_h7b0_rcc_set_backup_domain_reset(
    GnwH7B0RccState *s, GnwH7B0RccBackupDomainResetFn callback,
    void *opaque);
/* Register a callback for a changed APB timer kernel clock. The callback is
 * invoked synchronously after RCC registers are updated, only when the
 * selected domain's effective kernel frequency changes. */
void gnw_h7b0_rcc_add_timer_clock_observer(
    GnwH7B0RccState *s, GnwH7B0RccTimerClockDomain domain,
    GnwH7B0RccTimerClockChangeFn callback, void *opaque);
void gnw_h7b0_rcc_add_timing_observer(
    GnwH7B0RccState *s, GnwH7B0RccTimingDomain domain,
    GnwH7B0RccTimingPrepareFn prepare,
    GnwH7B0RccTimingCommitFn commit, GnwH7B0RccTimingAbortFn abort,
    void *opaque);
void gnw_h7b0_rcc_add_adc_clock_observer(
    GnwH7B0RccState *s, GnwH7B0RccAdcClockChangeFn callback, void *opaque);
/* Return the effective shared OCTOSPI kernel frequency, or zero when its
 * selected source/output is stopped or reserved. */
uint32_t gnw_h7b0_rcc_get_octospi_kernel_hz(GnwH7B0RccState *s);
/* Called synchronously after a cold register change changes effective rate;
 * migration post-load deliberately rebuilds consumers without notification. */
void gnw_h7b0_rcc_add_octospi_clock_observer(
    GnwH7B0RccState *s, GnwH7B0RccOctospiClockChangeFn callback,
    void *opaque);
uint32_t gnw_h7b0_rcc_get_pll1p_hz(GnwH7B0RccState *s);
uint32_t gnw_h7b0_rcc_get_pll3r_hz(GnwH7B0RccState *s);
uint32_t gnw_h7b0_rcc_get_sysclk_hz(GnwH7B0RccState *s);
/* SYSCLK divided by RCC.CDCFGR1.CDCPRE (Cortex-M7 core frequency). */
uint32_t gnw_h7b0_rcc_get_core_hz(GnwH7B0RccState *s);
/* Core frequency divided by RCC.CDCFGR1.HPRE (AHB/HCLK frequency). */
uint32_t gnw_h7b0_rcc_get_hclk_hz(GnwH7B0RccState *s);
GnwH7B0RccEngineClockInfo gnw_h7b0_rcc_engine_clock_info(
    GnwH7B0RccState *s, GnwH7B0RccEngine engine);
void gnw_h7b0_rcc_add_engine_clock_observer(
    GnwH7B0RccState *s, GnwH7B0RccEngine engine,
    GnwH7B0RccEngineClockChangeFn callback, void *opaque);

/* TIM2-TIM7 kernel clock (APB1 prescaler + the x2/x4 timer rule applied,
 * NOT simply HCLK) -- see the definition for why this distinction is
 * load-bearing for stock-firmware audio. */
uint32_t gnw_h7b0_rcc_get_timer_ker_hz(GnwH7B0RccState *s);
/* TIM1/TIM8 timer kernel clock (APB2 prescaler + TIMPRE rule). */
uint32_t gnw_h7b0_rcc_get_timer_apb2_ker_hz(GnwH7B0RccState *s);
uint32_t gnw_h7b0_rcc_get_sai1_kernel_hz(GnwH7B0RccState *s);
/* RCC-selected ADC asynchronous source and effective ADC12 kernel rate.
 * `common_ccr` is ADC12_COMMON.CCR; the helper applies CKMODE/PRESC and the
 * fixed analog-path divide-by-two from RM0455. */
uint32_t gnw_h7b0_rcc_get_adc_source_hz(GnwH7B0RccState *s);
uint32_t gnw_h7b0_rcc_get_adc_kernel_hz(GnwH7B0RccState *s,
                                        uint32_t common_ccr);
/* As above, but return false when the selected mux/prescaler is not modeled;
 * a supported source that is stopped is known and returns true with 0 Hz. */
bool gnw_h7b0_rcc_get_adc_clock_info(GnwH7B0RccState *s,
                                     uint32_t common_ccr,
                                     uint32_t *kernel_hz);
void gnw_h7b0_rcc_set_sysclk(GnwH7B0RccState *s, Clock *sysclk);
/* Establish the initial icount cycle rate before timing consumers initialize.
 * RCC remains the sole owner of subsequent cycle-clock boundaries. */
void gnw_h7b0_rcc_init_timing_clock(GnwH7B0RccState *s);
void gnw_h7b0_rcc_set_systick_refclk(GnwH7B0RccState *s,
                                     Clock *systick_refclk);

#endif
