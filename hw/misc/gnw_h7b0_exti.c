/*
 * EXTI, Nintendo Game & Watch STM32H7B0.
 *
 * See gnw_h7b0_exti.h for scope/rationale.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_exti.h"
#include "hw/misc/gnw_h7b0_regs_exti.h"

#define EXTI_RTSR1   (GNW_H7B0_EXTI_RTSR1_OFFSET >> 2)
#define EXTI_FTSR1   (GNW_H7B0_EXTI_FTSR1_OFFSET >> 2)
#define EXTI_SWIER1  (GNW_H7B0_EXTI_SWIER1_OFFSET >> 2)
#define EXTI_CPUIMR1 (GNW_H7B0_EXTI_CPUIMR1_OFFSET >> 2)
#define EXTI_CPUPR1  (GNW_H7B0_EXTI_CPUPR1_OFFSET >> 2)

/* Real NVIC IRQ grouping for EXTI lines 0-15: lines 0-4 each get their own
 * IRQ, lines 5-9 share EXTI9_5, lines 10-15 share EXTI15_10. Lines 16-21
 * are internal (PVD, RTC, USB wakeup, ...) with their own dedicated IRQs
 * elsewhere in the vector table, not modeled here since nothing we've
 * found needs them yet. */
static int line_to_irq_index(int line)
{
    if (line >= 0 && line <= 4) {
        return line;
    }
    if (line >= 5 && line <= 9) {
        return 5;
    }
    if (line >= 10 && line <= 15) {
        return 6;
    }
    return -1;
}

void gnw_h7b0_exti_set_line(GnwH7B0ExtiState *s, int line, bool level)
{
    bool old_level;

    if (line < 0 || line >= (int)ARRAY_SIZE(s->line_level)) {
        return;
    }
    old_level = s->line_level[line];
    s->line_level[line] = level;
    if (old_level == level) {
        return;
    }

    bool rising = !old_level && level;
    bool falling = old_level && !level;
    bool rtsr = (s->regs[EXTI_RTSR1] >> line) & 1;
    bool ftsr = (s->regs[EXTI_FTSR1] >> line) & 1;

    if (!((rising && rtsr) || (falling && ftsr))) {
        return;
    }
    if (!((s->regs[EXTI_CPUIMR1] >> line) & 1)) {
        return;
    }

    s->regs[EXTI_CPUPR1] |= (1u << line);

    int idx = line_to_irq_index(line);
    if (idx >= 0) {
        qemu_irq_pulse(s->irq[idx]);
    }
}

static void gnw_h7b0_exti_reset(DeviceState *dev)
{
    GnwH7B0ExtiState *s = GNW_H7B0_EXTI(dev);
    for (int i = 0; i < (GNW_H7B0_EXTI_SIZE / 4); i++) {
        s->regs[i] = get_exti_reset_value(i * 4);
    }
    memset(s->line_level, 0, sizeof(s->line_level));
}

static uint64_t gnw_h7b0_exti_read(void *opaque, hwaddr addr, unsigned int size)
{
    GnwH7B0ExtiState *s = GNW_H7B0_EXTI(opaque);
    if (addr >= GNW_H7B0_EXTI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return 0;
    }
    return s->regs[addr >> 2];
}

static void gnw_h7b0_exti_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{
    GnwH7B0ExtiState *s = GNW_H7B0_EXTI(opaque);
    if (addr >= GNW_H7B0_EXTI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        return;
    }

    /* CPUPR1 (and its D2/D3 aliases we don't separately model) is
     * write-1-to-clear, like every other STM32 pending-flags register --
     * not a plain read/write shadow. */
    if ((addr >> 2) == EXTI_CPUPR1) {
        s->regs[EXTI_CPUPR1] &= ~(uint32_t)val64;
        return;
    }

    uint32_t mask = get_exti_write_mask(addr);
    s->regs[addr >> 2] = (s->regs[addr >> 2] & ~mask) | ((uint32_t)val64 & mask);

    /*
     * SWIER1 (software interrupt event register): each set bit is
     * meant to force that line's pending bit, but NOT unconditionally
     * -- confirmed empirically against real hardware (this repo has no
     * Reference Manual): with no edge direction selected at all, SWIER1
     * never sets PR1, for every line tried. Setting RTSR1 (or FTSR1)
     * for the line, even with no real physical edge to detect, is what
     * makes it latch -- SWIER turns out to feed a synthetic edge
     * through the same edge-detect logic RTSR/FTSR configure, rather
     * than bypassing it outright. See
     * stm32h7b0-diag/fw/cases/case_exti_sw_trigger.c's 2026-07-16 "BUG
     * FIX" header comment for the full empirical trail this is modeled
     * on. Previously unimplemented entirely (SWIER1 was a plain
     * read/write shadow with no side effect), which meant PR1 could
     * never be set this way at all, regardless of RTSR1/FTSR1 --
     * failing stm32h7b0-diag's exti_sw_trigger/exti_edge_config cases
     * (both of which expect PR1 to latch once an edge direction is
     * armed) even though the RTSR1/FTSR1 register round-trip itself was
     * already correct.
     */
    if ((addr >> 2) == EXTI_SWIER1) {
        uint32_t armed = s->regs[EXTI_RTSR1] | s->regs[EXTI_FTSR1];
        uint32_t fired = (uint32_t)val64 & armed & (s->regs[EXTI_CPUIMR1]);

        s->regs[EXTI_CPUPR1] |= fired;
        while (fired) {
            int line = ctz32(fired);
            int idx = line_to_irq_index(line);
            if (idx >= 0) {
                qemu_irq_pulse(s->irq[idx]);
            }
            fired &= fired - 1;
        }
    }
}

static const MemoryRegionOps gnw_h7b0_exti_ops = {
    .read = gnw_h7b0_exti_read,
    .write = gnw_h7b0_exti_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void gnw_h7b0_exti_init(Object *obj)
{
    GnwH7B0ExtiState *s = GNW_H7B0_EXTI(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_exti_ops, s, TYPE_GNW_H7B0_EXTI, GNW_H7B0_EXTI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    for (int i = 0; i < GNW_H7B0_EXTI_NUM_IRQ_OUTPUTS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
}

static const VMStateDescription vmstate_gnw_h7b0_exti = {
    .name = TYPE_GNW_H7B0_EXTI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0ExtiState, GNW_H7B0_EXTI_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void gnw_h7b0_exti_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_exti;
    device_class_set_legacy_reset(dc, gnw_h7b0_exti_reset);
}

static const TypeInfo gnw_h7b0_exti_info = {
    .name          = TYPE_GNW_H7B0_EXTI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0ExtiState),
    .instance_init = gnw_h7b0_exti_init,
    .class_init    = gnw_h7b0_exti_class_init,
};

static void gnw_h7b0_exti_register_types(void)
{
    type_register_static(&gnw_h7b0_exti_info);
}
type_init(gnw_h7b0_exti_register_types)
