/*
 * STM32H7B0 RCC minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_rcc.h for scope/rationale. Deliberately not the existing
 * hw/misc/stm32_rcc.c device: that model is F4-family register layout
 * (different offsets) and, more importantly, doesn't mirror *ON bits
 * into *RDY bits at all -- reused as-is, real H7B0 firmware's clock-init
 * polling loops would hang forever waiting for a status bit nothing ever
 * sets.
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

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/core/clock.h"
#include "hw/misc/gnw_h7b0_rcc.h"
#include "hw/misc/gnw_h7b0_regs_rcc.h"

static void gnw_h7b0_rcc_update_sysclk_clock(GnwH7B0RccState *s);

static void gnw_h7b0_rcc_reset(DeviceState *dev)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(dev);

    for (int i = 0; i < (GNW_H7B0_RCC_SIZE / 4); i++) {
        s->regs[i] = get_rcc_reset_value(i * 4);
    }
    
    /* Override RSR reset per the rationale in gnw_h7b0_rcc.h */
    s->regs[GNW_H7B0_RCC_RSR >> 2] = RCC_RSR_RESET_VALUE;
}

static uint64_t gnw_h7b0_rcc_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(opaque);

    if (addr >= GNW_H7B0_RCC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_rcc_write(void *opaque, hwaddr addr,
                                uint64_t val64, unsigned int size)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_RCC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    uint32_t mask = get_rcc_write_mask(addr);
    value = (s->regs[addr >> 2] & ~mask) | (value & mask);

    switch (addr) {
    case GNW_H7B0_RCC_CR:
        /*
         * Mirror each *ON bit into its *RDY bit instantly (not
         * cycle-accurate -- real hardware takes some cycles to lock).
         * Good enough to unblock a polling loop, which is this stub's
         * only job.
         */
        if (value & RCC_CR_HSION) {
            value |= RCC_CR_HSIRDY;
        } else {
            value &= ~RCC_CR_HSIRDY;
        }
        if (value & RCC_CR_HSEON) {
            value |= RCC_CR_HSERDY;
        } else {
            value &= ~RCC_CR_HSERDY;
        }
        /*
         * CSION -> CSIRDY, same instant-approximation as every other
         * oscillator here. Found missing entirely 2026-07-13 while
         * investigating Mario CFW running at ~half real-hardware
         * speed: firmware's SystemClock_Config() requests CSI ON
         * (confirmed live) as part of the same HAL_RCC_OscConfig()-
         * style call that also configures PLL1, and PLL1 never
         * reported ready in QEMU across several boot traces --
         * confirmed live that CSION was set (bit 7) while CSIRDY
         * (bit 8) stayed permanently clear, unlike every other
         * oscillator's RDY bit, which all mirror correctly. If
         * firmware's oscillator-enable loop waits on CSIRDY before
         * or interleaved with the PLL1 configuration steps, a CSI
         * readiness wait that can never succeed would plausibly
         * explain the run-to-run-inconsistent PLL1 failure observed
         * this session (sometimes never reaching the PLL1DIVR write,
         * sometimes reaching it with what looked like a corrupted
         * value) -- not yet confirmed as the complete fix, but a
         * real, concrete gap either way.
         */
        if (value & RCC_CR_CSION) {
            value |= RCC_CR_CSIRDY;
        } else {
            value &= ~RCC_CR_CSIRDY;
        }
        if (value & RCC_CR_PLL1ON) {
            value |= RCC_CR_PLL1RDY;
        } else {
            value &= ~RCC_CR_PLL1RDY;
        }
        if (value & RCC_CR_PLL2ON) {
            value |= RCC_CR_PLL2RDY;
        } else {
            value &= ~RCC_CR_PLL2RDY;
        }
        if (value & RCC_CR_PLL3ON) {
            value |= RCC_CR_PLL3RDY;
        } else {
            value &= ~RCC_CR_PLL3RDY;
        }
        s->regs[addr >> 2] = value;
        return;
    case GNW_H7B0_RCC_CFGR:
        /* Mirror SW (requested source) straight into SWS (switch status). */
        value = (value & ~RCC_CFGR_SWS_MASK) |
                (((value & RCC_CFGR_SW_MASK) >> RCC_CFGR_SW_SHIFT)
                 << RCC_CFGR_SWS_SHIFT);
        s->regs[addr >> 2] = value;
        gnw_h7b0_rcc_update_sysclk_clock(s);
        return;
    case GNW_H7B0_RCC_CSR:
        /* Mirror LSION into LSIRDY, same instant-approximation as CR. */
        if (value & RCC_CSR_LSION) {
            value |= RCC_CSR_LSIRDY;
        } else {
            value &= ~RCC_CSR_LSIRDY;
        }
        s->regs[addr >> 2] = value;
        return;
    case GNW_H7B0_RCC_BDCR:
        /*
         * Real G&W hardware may not even populate an LSE crystal, but
         * firmware spins on LSERDY regardless (sometimes with a timeout,
         * sometimes without -- confirmed both in retro-go and in a stock
         * firmware's HAL_RCCEx_PeriphCLKConfig RTCSEL-select path, which
         * clears LSEON to 0 via a BDRST pulse while reprogramming RTCSEL
         * and then waits on LSERDY in that same call), so this must
         * always come ready to avoid a permanent hang -- unconditionally,
         * not just mirroring LSEON, since real firmware's own BDRST
         * pulse legitimately clears LSEON right before this wait.
         */
        value |= RCC_BDCR_LSERDY;
        s->regs[addr >> 2] = value;
        return;
    case GNW_H7B0_RCC_RSR:
        /*
         * Writing RMVF (bit16) clears the *RSTF cause flags on real
         * hardware -- keep RMVF itself readable as written (firmware
         * doesn't rely on it self-clearing here) but zero the rest.
         */
        s->regs[addr >> 2] = value & RCC_RSR_RMVF;
        return;
    case GNW_H7B0_RCC_PLLCKSELR_OFFSET:
    case GNW_H7B0_RCC_PLLCFGR_OFFSET:
    case GNW_H7B0_RCC_PLL1DIVR_OFFSET:
    case GNW_H7B0_RCC_PLL1FRACR_OFFSET:
        /*
         * Otherwise plain read/write shadows (PLL2's half of PLLCKSELR/
         * PLLCFGR included -- gnw_h7b0_rcc_get_pll2p_hz() reads those
         * live, no recompute-and-cache needed there), but any of these
         * can change PLL1's output and thus SYSCLK -- recompute and
         * push the new rate out to the board's Clock.
         */
        s->regs[addr >> 2] = value;
        gnw_h7b0_rcc_update_sysclk_clock(s);
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: offset 0x%"HWADDR_PRIx" is a plain read/write "
                      "shadow, no real clock behavior modeled\n",
                      __func__, addr);
        s->regs[addr >> 2] = value;
    }
}

/*
 * PLL2P output frequency, decoded live from PLLCKSELR/PLL2DIVR/PLL2FRACR --
 * see gnw_h7b0_rcc.h for why (SAI1 sample-rate derivation). Matches
 * HAL_RCCEx_GetPLL2ClockFreq()'s fractional-N formula
 * (stm32h7xx_hal_rcc_ex.c): VCO = (HSE/DIVM2) * (N2_raw + FRACN2/8192 + 1),
 * PLL2P = VCO / (P2_raw + 1). N2_raw/P2_raw are the *raw register fields*
 * (PLL2DIVR bits, unmodified) -- do NOT add 1 when decoding them here: the
 * config side (__HAL_RCC_PLL2_CONFIG) stores (PLL2N - 1)/(PLL2P - 1) into
 * those fields, and this getter's own "+1" is what cancels that back out
 * for N (netting VCO's N term to the real PLL2N with no adjustment needed
 * here at all), while P's "+1" is applied explicitly at the division step
 * below, matching HAL_RCCEx_GetPLL2ClockFreq line-for-line. Getting this
 * backwards (adding an extra +1 to N2 here) silently produced numbers off
 * by roughly N/(N-1) -- confirmed against a live run where real firmware's
 * ACR1.MCKDIV readback only made sense against the *uncorrected* raw-field
 * formula, not the doubly-adjusted one.
 * Returns 0 for invalid/unconfigured dividers (DIVM2 == 0) rather than
 * dividing by zero -- callers fall back to their own default in that case.
 */

/*
 * PLLCKSELR.PLLSRC (bits[1:0]) is shared between PLL1 and PLL2 -- one
 * oscillator mux feeds both PLLs' input dividers. Factored out so
 * gnw_h7b0_rcc_get_pll1p_hz() doesn't duplicate gnw_h7b0_rcc_get_pll2p_hz()'s
 * switch statement.
 */
static uint32_t gnw_h7b0_rcc_get_osc_hz(GnwH7B0RccState *s)
{
    uint32_t sel = s->regs[GNW_H7B0_RCC_PLLCKSELR_OFFSET >> 2];
    uint32_t pllsrc = (sel & RCC_PLLCKSELR_PLLSRC_MASK) >> RCC_PLLCKSELR_PLLSRC_SHIFT;

    switch (pllsrc) {
    case RCC_PLLCKSELR_PLLSRC_HSE:
        return GNW_H7B0_RCC_HSE_HZ;
    case RCC_PLLCKSELR_PLLSRC_HSI:
    default:
        /* This firmware always selects HSI (main.c's SystemClock_Config
         * never enables RCC_OSCILLATORTYPE_HSE) -- treat CSI/reserved
         * encodings the same as HSI rather than adding a third rarely
         * (never, for this firmware) exercised oscillator value. */
        return GNW_H7B0_RCC_HSI_HZ;
    }
}

uint32_t gnw_h7b0_rcc_get_pll2p_hz(GnwH7B0RccState *s)
{
    uint32_t sel = s->regs[GNW_H7B0_RCC_PLLCKSELR_OFFSET >> 2];
    uint32_t cfgr = s->regs[GNW_H7B0_RCC_PLLCFGR_OFFSET >> 2];
    uint32_t divr = s->regs[GNW_H7B0_RCC_PLL2DIVR_OFFSET >> 2];
    uint32_t fracr = s->regs[GNW_H7B0_RCC_PLL2FRACR_OFFSET >> 2];

    uint32_t divm2 = (sel & RCC_PLLCKSELR_DIVM2_MASK) >> RCC_PLLCKSELR_DIVM2_SHIFT;
    uint32_t n2_raw = (divr & RCC_PLL2DIVR_N2_MASK) >> RCC_PLL2DIVR_N2_SHIFT;
    uint32_t p2_raw = (divr & RCC_PLL2DIVR_P2_MASK) >> RCC_PLL2DIVR_P2_SHIFT;
    uint32_t fracn2 = (cfgr & RCC_PLLCFGR_PLL2FRACEN) ?
        ((fracr & RCC_PLL2FRACR_FRACN2_MASK) >> RCC_PLL2FRACR_FRACN2_SHIFT) : 0;
    uint32_t osc_hz = gnw_h7b0_rcc_get_osc_hz(s);

    if (divm2 == 0) {
        return 0;
    }

    /* VCO = osc * (N2_raw + FRACN2/8192 + 1) / DIVM2 -- numerator/
     * denominator kept as 64-bit integers scaled by 8192 to avoid
     * floating point. */
    uint64_t numerator = (uint64_t)osc_hz *
                          (n2_raw * 8192ULL + fracn2 + 8192ULL);
    uint64_t denominator = (uint64_t)divm2 * 8192ULL;
    uint64_t vco = numerator / denominator;

    return (uint32_t)(vco / (p2_raw + 1));
}

/*
 * PLL1P output frequency -- same VCO/output formula and same N/P raw-field
 * handling as gnw_h7b0_rcc_get_pll2p_hz() (see its comment for why N gets
 * no extra +1 here beyond the one already in the VCO term), just PLL1's
 * own registers/fields (DIVM1, PLL1FRACEN, PLL1DIVR, PLL1FRACR) instead of
 * PLL2's. This is the CPU/SYSCLK PLL -- see gnw_h7b0_rcc_get_sysclk_hz().
 */
uint32_t gnw_h7b0_rcc_get_pll1p_hz(GnwH7B0RccState *s)
{
    uint32_t sel = s->regs[GNW_H7B0_RCC_PLLCKSELR_OFFSET >> 2];
    uint32_t cfgr = s->regs[GNW_H7B0_RCC_PLLCFGR_OFFSET >> 2];
    uint32_t divr = s->regs[GNW_H7B0_RCC_PLL1DIVR_OFFSET >> 2];
    uint32_t fracr = s->regs[GNW_H7B0_RCC_PLL1FRACR_OFFSET >> 2];

    uint32_t divm1 = (sel & RCC_PLLCKSELR_DIVM1_MASK) >> RCC_PLLCKSELR_DIVM1_SHIFT;
    uint32_t n1_raw = (divr & RCC_PLL1DIVR_N1_MASK) >> RCC_PLL1DIVR_N1_SHIFT;
    uint32_t p1_raw = (divr & RCC_PLL1DIVR_P1_MASK) >> RCC_PLL1DIVR_P1_SHIFT;
    uint32_t fracn1 = (cfgr & RCC_PLLCFGR_PLL1FRACEN) ?
        ((fracr & RCC_PLL1FRACR_FRACN1_MASK) >> RCC_PLL1FRACR_FRACN1_SHIFT) : 0;
    uint32_t osc_hz = gnw_h7b0_rcc_get_osc_hz(s);

    if (divm1 == 0) {
        return 0;
    }

    uint64_t numerator = (uint64_t)osc_hz *
                          (n1_raw * 8192ULL + fracn1 + 8192ULL);
    uint64_t denominator = (uint64_t)divm1 * 8192ULL;
    uint64_t vco = numerator / denominator;

    return (uint32_t)(vco / (p1_raw + 1));
}

/*
 * PLL3R output frequency (pll3_r_ck) -- LTDC's pixel clock, hardwired to
 * this PLL output with no clock-source mux (unlike SAI1/ADC). Same VCO/
 * output formula and same N-raw-field handling as gnw_h7b0_rcc_get_pll1p_hz()/
 * gnw_h7b0_rcc_get_pll2p_hz() (see the latter's comment for why N gets no
 * extra +1 beyond the one already folded into the VCO term); R's own "+1"
 * is applied explicitly at the division step below, same as P for PLL1/
 * PLL2. Used by gnw_h7b0_ltdc.c to derive the real vblank rate instead of
 * assuming a fixed 60Hz.
 */
uint32_t gnw_h7b0_rcc_get_pll3r_hz(GnwH7B0RccState *s)
{
    uint32_t sel = s->regs[GNW_H7B0_RCC_PLLCKSELR_OFFSET >> 2];
    uint32_t cfgr = s->regs[GNW_H7B0_RCC_PLLCFGR_OFFSET >> 2];
    uint32_t divr = s->regs[GNW_H7B0_RCC_PLL3DIVR_OFFSET >> 2];
    uint32_t fracr = s->regs[GNW_H7B0_RCC_PLL3FRACR_OFFSET >> 2];

    uint32_t divm3 = (sel & RCC_PLLCKSELR_DIVM3_MASK) >> RCC_PLLCKSELR_DIVM3_SHIFT;
    uint32_t n3_raw = (divr & RCC_PLL3DIVR_N3_MASK) >> RCC_PLL3DIVR_N3_SHIFT;
    uint32_t r3_raw = (divr & RCC_PLL3DIVR_R3_MASK) >> RCC_PLL3DIVR_R3_SHIFT;
    uint32_t fracn3 = (cfgr & RCC_PLLCFGR_PLL3FRACEN) ?
        ((fracr & RCC_PLL3FRACR_FRACN3_MASK) >> RCC_PLL3FRACR_FRACN3_SHIFT) : 0;
    uint32_t osc_hz = gnw_h7b0_rcc_get_osc_hz(s);

    if (divm3 == 0) {
        return 0;
    }

    uint64_t numerator = (uint64_t)osc_hz *
                          (n3_raw * 8192ULL + fracn3 + 8192ULL);
    uint64_t denominator = (uint64_t)divm3 * 8192ULL;
    uint64_t vco = numerator / denominator;

    return (uint32_t)(vco / (r3_raw + 1));
}

/*
 * Effective SYSCLK, respecting CFGR.SWS -- real firmware's overclock
 * sequence briefly drops SW to HSI before reprogramming PLL1, then
 * switches back (see gnw_h7b0_rcc.h's plan-doc reference); RCC already
 * mirrors SW into SWS instantly in gnw_h7b0_rcc_write(), so this getter
 * naturally reflects that transient with no special-casing.
 */
uint32_t gnw_h7b0_rcc_get_sysclk_hz(GnwH7B0RccState *s)
{
    uint32_t cfgr = s->regs[GNW_H7B0_RCC_CFGR >> 2];
    uint32_t sws = (cfgr & RCC_CFGR_SWS_MASK) >> RCC_CFGR_SWS_SHIFT;

    switch (sws) {
    case RCC_CFGR_SWS_HSI:
        return GNW_H7B0_RCC_HSI_HZ;
    case RCC_CFGR_SWS_CSI:
        return GNW_H7B0_RCC_CSI_HZ;
    case RCC_CFGR_SWS_HSE:
        return GNW_H7B0_RCC_HSE_HZ;
    case RCC_CFGR_SWS_PLL1:
    default:
        return gnw_h7b0_rcc_get_pll1p_hz(s);
    }
}

/*
 * HCLK = SYSCLK for this firmware (Core/Src/main.c's AHBCLKDivider is
 * always RCC_HCLK_DIV1 across all 3 OC levels) -- deliberate
 * simplification, not an oversight; revisit if a firmware config that
 * actually divides HCLK ever needs modeling.
 */
uint32_t gnw_h7b0_rcc_get_hclk_hz(GnwH7B0RccState *s)
{
    return gnw_h7b0_rcc_get_sysclk_hz(s);
}

void gnw_h7b0_rcc_set_sysclk(GnwH7B0RccState *s, Clock *sysclk)
{
    s->sysclk = sysclk;
}

/*
 * Push a fresh SYSCLK recompute out to the board's sysclk Clock, same
 * synchronous recompute-on-write idiom hw/misc/stm32l4x5_rcc.c uses for
 * its own PLL/mux clocks -- called from every write case that can change
 * the effective SYSCLK (CFGR's SW, or any PLL1 register). No timers/
 * deferred work: guest register write -> clock_update_hz() -> automatic
 * propagation to cpuclk/refclk (and SysTick's existing ClockCallback),
 * all inline in the MMIO write handler.
 */
static void gnw_h7b0_rcc_update_sysclk_clock(GnwH7B0RccState *s)
{
    if (s->sysclk) {
        clock_update_hz(s->sysclk, gnw_h7b0_rcc_get_sysclk_hz(s));
    }
}

uint32_t gnw_h7b0_rcc_get_sai1_kernel_hz(GnwH7B0RccState *s)
{
    uint32_t ccip1r = s->regs[GNW_H7B0_RCC_CDCCIP1R_OFFSET >> 2];
    uint32_t sai1sel = (ccip1r & RCC_CDCCIP1R_SAI1SEL_MASK) >>
                        RCC_CDCCIP1R_SAI1SEL_SHIFT;

    if (sai1sel == RCC_CDCCIP1R_SAI1SEL_PLL2) {
        return gnw_h7b0_rcc_get_pll2p_hz(s);
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: SAI1SEL=%u not PLL2 -- not a path this firmware uses, "
                  "not modeled\n", __func__, sai1sel);
    return 0;
}

static const MemoryRegionOps gnw_h7b0_rcc_ops = {
    .read = gnw_h7b0_rcc_read,
    .write = gnw_h7b0_rcc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void gnw_h7b0_rcc_init(Object *obj)
{
    GnwH7B0RccState *s = GNW_H7B0_RCC(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_rcc_ops, s,
                           TYPE_GNW_H7B0_RCC, GNW_H7B0_RCC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_rcc = {
    .name = TYPE_GNW_H7B0_RCC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0RccState, GNW_H7B0_RCC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_rcc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_rcc;
    device_class_set_legacy_reset(dc, gnw_h7b0_rcc_reset);
}

static const TypeInfo gnw_h7b0_rcc_info = {
    .name          = TYPE_GNW_H7B0_RCC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0RccState),
    .instance_init = gnw_h7b0_rcc_init,
    .class_init    = gnw_h7b0_rcc_class_init,
};

static void gnw_h7b0_rcc_register_types(void)
{
    type_register_static(&gnw_h7b0_rcc_info);
}

type_init(gnw_h7b0_rcc_register_types)
