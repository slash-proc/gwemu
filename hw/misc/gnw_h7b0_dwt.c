/*
 * ARMv7-M DWT unit -- real CYCCNT only (Nintendo Game & Watch)
 *
 * See gnw_h7b0_dwt.h for scope/rationale.
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
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "exec/icount.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/gnw_h7b0_dwt.h"

static void gnw_h7b0_dwt_reanchor(GnwH7B0DwtState *s, int64_t now_ns);

static uint64_t gnw_h7b0_dwt_elapsed_cycles(GnwH7B0DwtState *s,
                                             int64_t now)
{
    int64_t elapsed_ns = now - s->enabled_since_ns;

    if (elapsed_ns <= 0 || !s->frequency_hz) {
        return 0;
    }
    return muldiv64(elapsed_ns, s->frequency_hz, NANOSECONDS_PER_SECOND);
}

static uint64_t gnw_h7b0_dwt_cyccnt(GnwH7B0DwtState *s)
{
    int64_t now_ns;

    if (!s->enabled) {
        return s->base_count;
    }

    if (s->timing_mode) {
        IcountCycleStamp stamp;
        GnwH7B0CyclePhase phase = s->cycle_phase;
        uint64_t ticks;

        icount_get_cycle_stamp(&stamp);
        if (!gnw_h7b0_cycle_phase_settle(&phase, stamp, s->frequency_hz,
                                          &ticks)) {
            return s->base_count;
        }
        return s->base_count + ticks;
    }
    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return s->base_count + gnw_h7b0_dwt_elapsed_cycles(s, now_ns);
}

static void gnw_h7b0_dwt_set_effective_enable(GnwH7B0DwtState *s,
                                               bool enabled)
{
    if (enabled == s->enabled) {
        return;
    }
    s->base_count = gnw_h7b0_dwt_cyccnt(s);
    if (enabled) {
        gnw_h7b0_dwt_reanchor(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        if (s->timing_mode) {
            IcountCycleStamp stamp;
            icount_get_cycle_stamp(&stamp);
            g_assert(gnw_h7b0_cycle_phase_init(&s->cycle_phase, stamp));
        }
    }
    s->enabled = enabled;
}

static void gnw_h7b0_dwt_trace_enable(void *opaque, int n, int level)
{
    GnwH7B0DwtState *s = opaque;

    s->trace_enabled = level;
    gnw_h7b0_dwt_set_effective_enable(
        s, s->trace_enabled && (s->regs[GNW_H7B0_DWT_CTRL_OFFSET / 4] &
                                DWT_CTRL_CYCCNTENA));
}

static void gnw_h7b0_dwt_reanchor(GnwH7B0DwtState *s, int64_t now_ns)
{
    s->enabled_since_ns = now_ns;
    if (s->timing_mode) {
        s->enabled_since_cycles =
            icount_get_cycle_count(&s->enabled_since_ns);
    }
}

static void gnw_h7b0_dwt_clock_update(void *opaque, ClockEvent event)
{
    GnwH7B0DwtState *s = opaque;
    int64_t now = 0;

    /*
     * ClockUpdate is delivered after the input period changes. Account the
     * interval using frequency_hz (the old rate), then start a new interval
     * at the new rate. This keeps CYCCNT continuous across RCC clock changes.
     */
    if (s->enabled && !s->timing_mode) {
        now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->base_count += gnw_h7b0_dwt_elapsed_cycles(s, now);
        gnw_h7b0_dwt_reanchor(s, now);
    }
    s->frequency_hz = clock_get_hz(s->sysclk);
}

static bool gnw_h7b0_dwt_timing_prepare(
    void *opaque, const IcountCycleStamp *before, uint32_t old_hz)
{
    GnwH7B0DwtState *s = opaque;
    uint64_t ticks;

    s->pending_cycle_phase = s->cycle_phase;
    s->pending_base_count = s->base_count;
    if (s->enabled &&
        !gnw_h7b0_cycle_phase_settle(&s->pending_cycle_phase, *before,
                                      old_hz, &ticks)) {
        return false;
    }
    if (s->enabled) {
        s->pending_base_count += ticks;
    } else {
        /* Keep the anchor current while disabled so an enable starts clean. */
        if (!gnw_h7b0_cycle_phase_settle(&s->pending_cycle_phase, *before,
                                          0, &ticks)) {
            return false;
        }
    }
    s->pending_phase_valid = true;
    return true;
}

static void gnw_h7b0_dwt_timing_commit(
    void *opaque, const IcountCycleStamp *before,
    const IcountCycleStamp *after, uint32_t new_hz)
{
    GnwH7B0DwtState *s = opaque;

    g_assert(s->pending_phase_valid);
    g_assert(gnw_h7b0_cycle_phase_rebase(&s->pending_cycle_phase,
                                          *before, *after));
    s->cycle_phase = s->pending_cycle_phase;
    s->base_count = s->pending_base_count;
    s->frequency_hz = new_hz;
    s->pending_phase_valid = false;
}

static void gnw_h7b0_dwt_timing_abort(void *opaque)
{
    GnwH7B0DwtState *s = opaque;

    s->pending_phase_valid = false;
}

void gnw_h7b0_dwt_set_rcc(GnwH7B0DwtState *s, GnwH7B0RccState *rcc)
{
    if (s->timing_mode) {
        gnw_h7b0_rcc_add_timing_observer(
            rcc, GNW_H7B0_RCC_TIMING_CORE,
            gnw_h7b0_dwt_timing_prepare,
            gnw_h7b0_dwt_timing_commit,
            gnw_h7b0_dwt_timing_abort, s);
    }
}

static void gnw_h7b0_dwt_reset(DeviceState *dev)
{
    GnwH7B0DwtState *s = GNW_H7B0_DWT(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->base_count = 0;
    s->enabled_since_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->enabled_since_cycles = s->timing_mode ?
        icount_get_cycle_count(&s->enabled_since_ns) : 0;
    s->enabled = false;
    s->trace_enabled = false;
    s->frequency_hz = s->sysclk ? clock_get_hz(s->sysclk) : 0;
    if (s->timing_mode && icount_enabled()) {
        IcountCycleStamp stamp;

        icount_get_cycle_stamp(&stamp);
        if (stamp.core_hz) {
            g_assert(gnw_h7b0_cycle_phase_init(&s->cycle_phase, stamp));
        }
    }
}

static int gnw_h7b0_dwt_pre_save(void *opaque)
{
    GnwH7B0DwtState *s = opaque;

    /*
     * An RCC timing transaction snapshots the old phase in
     * pending_cycle_phase and publishes it only from timing_commit().  A
     * migration stream taken between prepare and commit could otherwise mix
     * the live pre-change phase with clock state from either side of the
     * transaction.  TIM1/TIM2 enforce the same fail-closed boundary.
     */
    if (s->pending_phase_valid) {
        return -EINVAL;
    }

    if (s->enabled && s->timing_mode) {
        IcountCycleStamp stamp;
        uint64_t ticks;

        icount_get_cycle_stamp(&stamp);
        if (!gnw_h7b0_cycle_phase_settle(&s->cycle_phase, stamp,
                                          s->frequency_hz, &ticks)) {
            return -EINVAL;
        }
        s->base_count += ticks;
        gnw_h7b0_dwt_reanchor(
            s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    } else if (s->enabled) {
        s->base_count = gnw_h7b0_dwt_cyccnt(s);
        gnw_h7b0_dwt_reanchor(
            s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    s->migration_timing_mode = s->timing_mode;
    return 0;
}

static bool gnw_h7b0_dwt_post_load(void *opaque, int version_id, Error **errp)
{
    GnwH7B0DwtState *s = opaque;

    if (version_id >= 5 &&
        s->migration_timing_mode != s->timing_mode) {
        error_setg(errp,
                   "DWT timing mode differs between migration endpoints");
        return false;
    }

    if (version_id < 2) {
        s->frequency_hz = s->sysclk ? clock_get_hz(s->sysclk) : 0;
    }
    s->enabled_since_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->enabled_since_cycles = s->timing_mode ?
        icount_get_cycle_count(&s->enabled_since_ns) : 0;
    if (s->timing_mode) {
        IcountCycleStamp current;

        icount_get_cycle_stamp(&current);
        if (version_id >= 4 && s->enabled) {
            if (!gnw_h7b0_cycle_phase_stamp_equal(
                    &s->cycle_phase.stamp, &current)) {
                error_setg(errp,
                           "DWT cycle timeline differs from migration stream");
                return false;
            }
        } else if (!gnw_h7b0_cycle_phase_init(&s->cycle_phase, current)) {
            error_setg(errp,
                       "DWT cannot initialize the destination cycle timeline");
            return false;
        }
    }
    /*
     * `enabled` predates DEMCR migration and is a derived run state.  In
     * particular, an old stream can contain enabled=true without a DEMCR
     * subsection.  Re-establish the architectural gate instead of trusting
     * that legacy derived value.
     */
    gnw_h7b0_dwt_set_effective_enable(
        s, s->trace_enabled && (s->regs[GNW_H7B0_DWT_CTRL_OFFSET / 4] &
                                DWT_CTRL_CYCCNTENA));
    return true;
}

static const VMStateDescription vmstate_gnw_h7b0_dwt = {
    .name = TYPE_GNW_H7B0_DWT,
    .version_id = 5,
    .minimum_version_id = 1,
    .pre_save = gnw_h7b0_dwt_pre_save,
    .post_load_errp = gnw_h7b0_dwt_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0DwtState, GNW_H7B0_DWT_SIZE / 4),
        VMSTATE_UINT64(base_count, GnwH7B0DwtState),
        VMSTATE_INT64(enabled_since_ns, GnwH7B0DwtState),
        VMSTATE_BOOL(enabled, GnwH7B0DwtState),
        VMSTATE_UINT64_V(frequency_hz, GnwH7B0DwtState, 2),
        VMSTATE_INT64_V(enabled_since_cycles, GnwH7B0DwtState, 3),
        VMSTATE_INT64_V(cycle_phase.stamp.modeled_cycles,
                        GnwH7B0DwtState, 4),
        VMSTATE_INT64_V(cycle_phase.stamp.idle_ns,
                        GnwH7B0DwtState, 4),
        VMSTATE_UINT64_V(cycle_phase.stamp.core_hz,
                         GnwH7B0DwtState, 4),
        VMSTATE_UINT64_V(cycle_phase.stamp.generation,
                         GnwH7B0DwtState, 4),
        VMSTATE_UINT64_V(cycle_phase.fraction_q64,
                         GnwH7B0DwtState, 4),
        VMSTATE_BOOL_V(migration_timing_mode, GnwH7B0DwtState, 5),
        VMSTATE_END_OF_LIST()
    }
};

static uint64_t gnw_h7b0_dwt_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0DwtState *s = GNW_H7B0_DWT(opaque);
    hwaddr reg = addr & ~3ULL;
    unsigned int shift = (addr & 3) * 8;
    uint32_t value;

    if (addr >= GNW_H7B0_DWT_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }
    if (reg == GNW_H7B0_DWT_CYCCNT_OFFSET) {
        value = gnw_h7b0_dwt_cyccnt(s);
        /* Test-only producer callback, deliberately after clock observation. */
        icount_test_account_mmio_read();
    } else {
        value = s->regs[reg >> 2];
    }
    return extract32(value, shift, size * 8);
}

static void gnw_h7b0_dwt_write(void *opaque, hwaddr addr, uint64_t val64,
                                unsigned int size)
{
    GnwH7B0DwtState *s = GNW_H7B0_DWT(opaque);
    hwaddr reg = addr & ~3ULL;
    unsigned int shift = (addr & 3) * 8;
    uint32_t value;

    if (addr >= GNW_H7B0_DWT_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    if (size == 4) {
        value = val64;
    } else {
        uint32_t old = reg == GNW_H7B0_DWT_CYCCNT_OFFSET ?
            gnw_h7b0_dwt_cyccnt(s) : s->regs[reg >> 2];

        value = deposit32(old, shift, size * 8, val64);
    }

    switch (reg) {
    case GNW_H7B0_DWT_CYCCNT_OFFSET:
        /*
         * common_emu_enable_dwt_cycles() writes 0 here to reset the
         * counter at the start of each measured loop; real hardware
         * lets CYCCNT be written to any value and keeps counting from
         * there.
         */
        s->base_count = value;
        gnw_h7b0_dwt_reanchor(
            s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        if (s->timing_mode) {
            IcountCycleStamp stamp;

            icount_get_cycle_stamp(&stamp);
            g_assert(gnw_h7b0_cycle_phase_init(&s->cycle_phase, stamp));
        }
        return;
    case GNW_H7B0_DWT_CTRL_OFFSET:
        s->regs[reg >> 2] = value;
        gnw_h7b0_dwt_set_effective_enable(
            s, s->trace_enabled && (value & DWT_CTRL_CYCCNTENA));
        return;
    default:
        s->regs[reg >> 2] = value;
        return;
    }
}

static const MemoryRegionOps gnw_h7b0_dwt_ops = {
    .read = gnw_h7b0_dwt_read,
    .write = gnw_h7b0_dwt_write,
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

static void gnw_h7b0_dwt_init(Object *obj)
{
    GnwH7B0DwtState *s = GNW_H7B0_DWT(obj);

    s->sysclk = qdev_init_clock_in(DEVICE(s), "sysclk",
                                   gnw_h7b0_dwt_clock_update, s, ClockUpdate);
    qdev_init_gpio_in_named(DEVICE(s), gnw_h7b0_dwt_trace_enable,
                            "trace-enable", 1);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_dwt_ops, s,
                           TYPE_GNW_H7B0_DWT, GNW_H7B0_DWT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const Property gnw_h7b0_dwt_properties[] = {
    DEFINE_PROP_BOOL("timing-mode", GnwH7B0DwtState, timing_mode, false),
};

static void gnw_h7b0_dwt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_gnw_h7b0_dwt;
    device_class_set_props(dc, gnw_h7b0_dwt_properties);
    device_class_set_legacy_reset(dc, gnw_h7b0_dwt_reset);
}

static const TypeInfo gnw_h7b0_dwt_info = {
    .name          = TYPE_GNW_H7B0_DWT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0DwtState),
    .instance_init = gnw_h7b0_dwt_init,
    .class_init    = gnw_h7b0_dwt_class_init,
};

static void gnw_h7b0_dwt_register_types(void)
{
    type_register_static(&gnw_h7b0_dwt_info);
}

type_init(gnw_h7b0_dwt_register_types)
