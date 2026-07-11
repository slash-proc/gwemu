/*
 * STM32H7B0 RTC minimal stub (Nintendo Game & Watch)
 *
 * See gnw_h7b0_rtc.h for scope/rationale.
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
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_rtc.h"
#include "hw/misc/gnw_h7b0_regs_rtc.h"

static inline uint32_t rtc_bcd(unsigned int n)
{
    return ((n / 10) << 4) | (n % 10);
}

static inline unsigned int rtc_unbcd(uint32_t v)
{
    return ((v >> 4) & 0xf) * 10 + (v & 0xf);
}

/* Recompute TR/DR from the running calendar (host wall-clock time plus
 * elapsed virtual-clock time since the last read/write) and store the
 * encoded BCD fields back into regs, so gnw_h7b0_rtc_read() just
 * returns whatever's already there. */
static void gnw_h7b0_rtc_sync_calendar(GnwH7B0RtcState *s)
{
    int64_t elapsed_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                          s->rtc_base_vclock_ns;
    time_t now = s->rtc_base_epoch + elapsed_ns / NANOSECONDS_PER_SECOND;
    struct tm tm;
    uint32_t tr, dr;
    unsigned int rtc_wday;

    gmtime_r(&now, &tm);

    tr = (rtc_bcd(tm.tm_sec) & 0x7f)
       | ((rtc_bcd(tm.tm_min) & 0x7f) << 8)
       | ((rtc_bcd(tm.tm_hour) & 0x3f) << 16);

    rtc_wday = tm.tm_wday == 0 ? 7 : (unsigned int)tm.tm_wday;
    dr = (rtc_bcd(tm.tm_mday) & 0x3f)
       | ((rtc_bcd(tm.tm_mon + 1) & 0x1f) << 8)
       | ((rtc_wday & 0x7) << 13)
       | ((rtc_bcd((tm.tm_year + 1900) % 100) & 0xff) << 16);

    s->regs[GNW_H7B0_RTC_TR_OFFSET >> 2] = tr;
    s->regs[GNW_H7B0_RTC_DR_OFFSET >> 2] = dr;
}

/* Inverse of gnw_h7b0_rtc_sync_calendar(): re-derive the epoch base
 * from whatever's currently in TR/DR, so a firmware write to either
 * (setting the clock) re-anchors the running calendar instead of
 * being instantly overwritten by the next read's sync. */
static void gnw_h7b0_rtc_reanchor_calendar(GnwH7B0RtcState *s)
{
    uint32_t tr = s->regs[GNW_H7B0_RTC_TR_OFFSET >> 2];
    uint32_t dr = s->regs[GNW_H7B0_RTC_DR_OFFSET >> 2];
    struct tm tm = { 0 };

    tm.tm_sec = rtc_unbcd(tr & 0x7f);
    tm.tm_min = rtc_unbcd((tr >> 8) & 0x7f);
    tm.tm_hour = rtc_unbcd((tr >> 16) & 0x3f);
    tm.tm_mday = rtc_unbcd(dr & 0x3f);
    tm.tm_mon = rtc_unbcd((dr >> 8) & 0x1f) - 1;
    tm.tm_year = rtc_unbcd((dr >> 16) & 0xff) + 100;

    s->rtc_base_epoch = timegm(&tm);
    s->rtc_base_vclock_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void gnw_h7b0_rtc_reset(DeviceState *dev)
{
    GnwH7B0RtcState *s = GNW_H7B0_RTC(dev);

    for (int i = 0; i < (GNW_H7B0_RTC_SIZE / 4); i++) {
        s->regs[i] = get_rtc_reset_value(i * 4);
    }
    /* Calendar reads as already-initialized/synchronized from reset,
     * same rationale as RCC_RSR's non-zero reset value: a plain-zero
     * reset reads as "never initialized", which is not what real
     * firmware expects to see on any boot after the very first. */
    s->regs[GNW_H7B0_RTC_ICSR >> 2] = RTC_ICSR_RSF | RTC_ICSR_INITS;

    s->rtc_base_epoch = time(NULL);
    s->rtc_base_vclock_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    gnw_h7b0_rtc_sync_calendar(s);
}

static uint64_t gnw_h7b0_rtc_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0RtcState *s = GNW_H7B0_RTC(opaque);

    if (addr >= GNW_H7B0_RTC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    if (addr == GNW_H7B0_RTC_TR_OFFSET || addr == GNW_H7B0_RTC_DR_OFFSET) {
        gnw_h7b0_rtc_sync_calendar(s);
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_rtc_write(void *opaque, hwaddr addr,
                                uint64_t val64, unsigned int size)
{
    GnwH7B0RtcState *s = GNW_H7B0_RTC(opaque);
    uint32_t value = val64;

    if (addr >= GNW_H7B0_RTC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    uint32_t mask = get_rtc_write_mask(addr);
    value = (s->regs[addr >> 2] & ~mask) | (value & mask);

    switch (addr) {
    case GNW_H7B0_RTC_ICSR:
        /*
         * HAL_RTC_WaitForSynchro() writes ICSR to clear RSF before
         * polling it back set; RTC_EnterInitMode() sets INIT and
         * polls INITF. Real hardware sets INITF a few RTCCLK cycles
         * after INIT is requested, and RSF once shadow registers
         * resync -- approximated here as instant, same pattern as
         * RCC/PWR/OSPI/ADC's ON->RDY bits. INITS (calendar has been
         * initialized at least once) stays set once seen.
         *
         * WUTWF/ALRBWF/ALRAWF (bits 2/1/0) are also part of ICSR's reset
         * value (0x7, all set) but were getting silently zeroed by this
         * handler on the very first ICSR write, since only INIT/INITF/
         * RSF/INITS were ever preserved. HAL_RTC_SetAlarm_IT() polls
         * ALRAWF before reprogramming ALRMAR; once zeroed, that wait
         * never ends. Found via a silent hang in a patched stock-firmware
         * image (no debug symbols -- disassembly-only, an endless WPR/
         * CR/SCR unlock-wait-retry alarm-set sequence with LTDC never
         * getting configured). These write-allowed flags are true
         * whenever the corresponding alarm/wakeup-timer is disabled;
         * approximated as always true, same "instant ready" simplification
         * as the rest of this register.
         */
        s->regs[addr >> 2] = (value & RTC_ICSR_INIT)
                              | RTC_ICSR_INITF | RTC_ICSR_RSF | RTC_ICSR_INITS
                              | RTC_ICSR_WUTWF | RTC_ICSR_ALRBWF
                              | RTC_ICSR_ALRAWF;
        return;
    case GNW_H7B0_RTC_CR:
    {
        uint32_t old_cr = s->regs[addr >> 2];
        uint32_t sr = s->regs[GNW_H7B0_RTC_SR >> 2];

        s->regs[addr >> 2] = value;

        if ((value & RTC_CR_ALRAE) && !(old_cr & RTC_CR_ALRAE)) {
            sr |= RTC_SR_ALRAF;
        }
        if ((value & RTC_CR_ALRBE) && !(old_cr & RTC_CR_ALRBE)) {
            sr |= RTC_SR_ALRBF;
        }
        if ((value & RTC_CR_WUTE) && !(old_cr & RTC_CR_WUTE)) {
            sr |= RTC_SR_WUTF;
        }
        s->regs[GNW_H7B0_RTC_SR >> 2] = sr;
        s->regs[GNW_H7B0_RTC_MISR >> 2] =
            (sr & RTC_SR_ALRAF & ((value & RTC_CR_ALRAIE) ? ~0u : 0))
          | (sr & RTC_SR_ALRBF & ((value & RTC_CR_ALRBIE) ? ~0u : 0))
          | (sr & RTC_SR_WUTF  & ((value & RTC_CR_WUTIE)  ? ~0u : 0));
        return;
    }
    case GNW_H7B0_RTC_SCR:
        s->regs[GNW_H7B0_RTC_SR >> 2] &= ~value;
        s->regs[GNW_H7B0_RTC_MISR >> 2] &= ~value;
        return;
    case GNW_H7B0_RTC_TR_OFFSET:
    case GNW_H7B0_RTC_DR_OFFSET:
        s->regs[addr >> 2] = value;
        gnw_h7b0_rtc_reanchor_calendar(s);
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: offset 0x%"HWADDR_PRIx" is a plain read/write "
                      "shadow, no real calendar/clock modeled\n",
                      __func__, addr);
        s->regs[addr >> 2] = value;
    }
}

static const MemoryRegionOps gnw_h7b0_rtc_ops = {
    .read = gnw_h7b0_rtc_read,
    .write = gnw_h7b0_rtc_write,
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

static void gnw_h7b0_rtc_init(Object *obj)
{
    GnwH7B0RtcState *s = GNW_H7B0_RTC(obj);

    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_rtc_ops, s,
                           TYPE_GNW_H7B0_RTC, GNW_H7B0_RTC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription vmstate_gnw_h7b0_rtc = {
    .name = TYPE_GNW_H7B0_RTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0RtcState, GNW_H7B0_RTC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_rtc;
    device_class_set_legacy_reset(dc, gnw_h7b0_rtc_reset);
}

static const TypeInfo gnw_h7b0_rtc_info = {
    .name          = TYPE_GNW_H7B0_RTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0RtcState),
    .instance_init = gnw_h7b0_rtc_init,
    .class_init    = gnw_h7b0_rtc_class_init,
};

static void gnw_h7b0_rtc_register_types(void)
{
    type_register_static(&gnw_h7b0_rtc_info);
}

type_init(gnw_h7b0_rtc_register_types)
