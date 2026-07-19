/*
 * STM32H7B0 RTC minimal stub (Nintendo Game & Watch)
 *
 * Not cycle-accurate, no real calendar/clock counting. Purpose: real
 * firmware's HAL_RTC_Init() -> RTC_EnterInitMode() writes ICSR.INIT
 * (bit 7) then polls ICSR.INITF (bit 6) for real hardware to confirm
 * entry into initialization mode, and RTC_ExitInitMode() ->
 * HAL_RTC_WaitForSynchro() then polls ICSR.RSF (bit 5) for the shadow
 * registers to resync -- both bounded-timeout HAL polls that must not
 * time out, or HAL_RTC_Init() returns HAL_TIMEOUT and firmware treats
 * that as fatal (retro-go's Error_Handler()/BSOD, traced via
 * gnw-chainloader/retro-go-sd's Core/Src/main.c MX_RTC_Init()). This
 * was a plain zero-initialized RAM placeholder through Phase 1 (see
 * STATUS.md); INITF/RSF never reading back set is exactly this
 * timeout. Every ICSR write instantly mirrors INITF|RSF|INITS set
 * (same "instant approximation" pattern as RCC/PWR/OSPI/ADC's
 * ON->RDY bits) so the poll never blocks.
 *
 * CR's ALRAE/ALRBE/WUTE enable bits (Alarm A/B, periodic Wakeup Timer)
 * get the same instant treatment: on the 0->1 edge, the matching SR
 * flag (ALRAF/ALRBF/WUTF) is set immediately rather than after a real
 * ALRMAR/WUTR-derived delay, so firmware busy-polling SR (or ICSR's
 * ALRAWF/ALRBWF/WUTWF "write allowed" bits, already instant) for its
 * own alarm/wakeup to fire doesn't hang forever. Found via
 * patched-zelda-bank1.bin stalling completely (no more RTC/RCC writes
 * logged at all) right after a WPR-unlock + CR-write sequence. SCR
 * clears SR bits the same way real hardware does (write 1 to the
 * matching SCR bit); MISR mirrors SR gated by the CR *IE enable bits.
 * No real IRQ is raised to the NVIC -- this only unblocks *polling*
 * loops, not interrupt-driven waits.
 *
 * TR/DR (time/date) are a real ticking calendar, seeded from the
 * host's wall-clock time at reset and advanced via QEMU_CLOCK_VIRTUAL
 * (see gnw_h7b0_rtc_sync_calendar()/_reanchor_calendar() in the .c
 * file) -- added because gnw-chainloader has a real clock display
 * that reads TR/DR and a static/stub value looked obviously wrong. A
 * write to either re-anchors the running calendar to the new value
 * (in case firmware ever sets the clock), but no tested firmware image
 * actually does that -- they only read it.
 *
 * Every other register (SSR, PRER, WUTR, WPR, CALR, SHIFTR,
 * timestamp/alarm value registers, CFGR) is a plain read/write shadow
 * with no side effects.
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

#ifndef HW_MISC_GNW_H7B0_RTC_H
#define HW_MISC_GNW_H7B0_RTC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_RTC "gnw-h7b0-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0RtcState, GNW_H7B0_RTC)

/*
 * Register offsets/bits below are from
 * sdk/cmsis-device-h7/Include/stm32h7b0xx.h's RTC_TypeDef and
 * RTC_ICSR_x bit definitions (TAMP-merged variant, used on H7B0).
 */
#define GNW_H7B0_RTC_SIZE   0x400

#define GNW_H7B0_RTC_ICSR   0x0C
#define RTC_ICSR_INIT       (1U << 7)
#define RTC_ICSR_INITF      (1U << 6)
#define RTC_ICSR_RSF        (1U << 5)
#define RTC_ICSR_INITS      (1U << 4)
#define RTC_ICSR_WUTWF      (1U << 2)
#define RTC_ICSR_ALRBWF     (1U << 1)
#define RTC_ICSR_ALRAWF     (1U << 0)

#define GNW_H7B0_RTC_CR     0x18
#define RTC_CR_WUTIE        (1U << 14)
#define RTC_CR_ALRBIE       (1U << 13)
#define RTC_CR_ALRAIE       (1U << 12)
#define RTC_CR_WUTE         (1U << 10)
#define RTC_CR_ALRBE        (1U << 9)
#define RTC_CR_ALRAE        (1U << 8)

#define GNW_H7B0_RTC_SR     0x50
#define RTC_SR_WUTF         (1U << 2)
#define RTC_SR_ALRBF        (1U << 1)
#define RTC_SR_ALRAF        (1U << 0)

#define GNW_H7B0_RTC_MISR   0x54

#define GNW_H7B0_RTC_SCR    0x5C

struct GnwH7B0RtcState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;              /* RTC_Alarm_IRQn (Alarm A/B only -- the
                                 * wakeup-timer's real IRQ line,
                                 * RTC_WKUP_IRQn, isn't wired anywhere
                                 * else in this SoC model and no tested
                                 * firmware needs it). */
    uint32_t regs[GNW_H7B0_RTC_SIZE / 4];

    /*
     * Real ticking calendar: rtc_base_epoch is the Unix time the
     * calendar read TR/DR as, at host virtual-clock time
     * rtc_base_vclock_ns -- current calendar time is always
     * rtc_base_epoch + (now - rtc_base_vclock_ns). Seeded from the
     * host's wall-clock time at reset (like a battery-backed RTC that
     * was already running) rather than 2000-01-01, since real
     * firmware (gnw-chainloader's clock display) only ever *reads*
     * this calendar -- it has no in-firmware time-set UI in the
     * images tested so far, so there's nothing to seed it from except
     * the host clock.
     */
    time_t rtc_base_epoch;
    int64_t rtc_base_vclock_ns;
};

#endif
