/*
 * STM32H7B0 SoC (Nintendo Game & Watch), Phase 0 skeleton
 *
 * Bare Cortex-M7 + RAM only, no peripherals yet. See gnw_h7b0_soc.h and
 * ../../docs/roadmap.md.
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
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "hw/arm/gnw_h7b0_soc.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties.h"

static void gnw_h7b0_soc_initfn(Object *obj)
{
    GnwH7B0State *s = GNW_H7B0_SOC(obj);

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);

    s->sysclk = qdev_init_clock_in(DEVICE(s), "sysclk", NULL, NULL, 0);
}

static void gnw_h7b0_soc_realize(DeviceState *dev_soc, Error **errp)
{
    GnwH7B0State *s = GNW_H7B0_SOC(dev_soc);
    MemoryRegion *system_memory = get_system_memory();
    DeviceState *armv7m;
    Error *err = NULL;

    if (!clock_has_source(s->sysclk)) {
        error_setg(errp, "sysclk clock must be wired up by the board code");
        return;
    }

    memory_region_init_ram(&s->dtcm, OBJECT(dev_soc), "GNW_H7B0.dtcm",
                            DTCM_SIZE, &err);
    if (err != NULL) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(system_memory, DTCM_BASE_ADDRESS, &s->dtcm);

    memory_region_init_ram(&s->axisram, OBJECT(dev_soc), "GNW_H7B0.axisram",
                            AXISRAM_SIZE, &err);
    if (err != NULL) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(system_memory, AXISRAM_BASE_ADDRESS,
                                 &s->axisram);

    /*
     * TEMPORARY (Phase 0 only): Cortex-M reset always reads initial
     * SP/PC from address 0x0. Real hardware maps flash there via
     * BOOT/OSPI XIP; until Phase 1 adds a real flash/QSPI model, alias
     * AXI SRAM at 0x0 so a kernel image loaded into AXI SRAM is also
     * reachable at the CPU's hardwired reset vector address. Remove once
     * Phase 1's flash model provides a real address-0 mapping.
     */
    memory_region_init_alias(&s->axisram_alias, OBJECT(dev_soc),
                              "GNW_H7B0.axisram.alias", &s->axisram, 0,
                              AXISRAM_SIZE);
    memory_region_add_subregion(system_memory, 0, &s->axisram_alias);

    armv7m = DEVICE(&s->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", 96);
    qdev_prop_set_uint8(armv7m, "num-prio-bits", 4);
    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m7"));
    qdev_prop_set_bit(armv7m, "enable-bitband", true);
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    qdev_connect_clock_in(armv7m, "refclk", s->sysclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                              OBJECT(system_memory), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }

    /*
     * Peripherals (RCC, DMA2D, GPIO, USART, flash/QSPI boot) are added in
     * later phases; see ../../docs/roadmap.md.
     */
}

static void gnw_h7b0_soc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = gnw_h7b0_soc_realize;
}

static const TypeInfo gnw_h7b0_soc_info = {
    .name          = TYPE_GNW_H7B0_SOC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0State),
    .instance_init = gnw_h7b0_soc_initfn,
    .class_init    = gnw_h7b0_soc_class_init,
};

static void gnw_h7b0_soc_types(void)
{
    type_register_static(&gnw_h7b0_soc_info);
}

type_init(gnw_h7b0_soc_types)
