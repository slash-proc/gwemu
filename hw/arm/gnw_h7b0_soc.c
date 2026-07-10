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

#define INIT_RAM_REGION(field, name, base, size) \
    do { \
        memory_region_init_ram(&s->field, OBJECT(dev_soc), name, size, &err); \
        if (err != NULL) { \
            error_propagate(errp, err); \
            return; \
        } \
        memory_region_add_subregion(system_memory, base, &s->field); \
    } while (0)

    /*
     * ITCM sits at 0x0, the Cortex-M's hardwired reset vector-fetch
     * address, and is genuinely RAM on real hardware (not a flash
     * mirror/alias) -- so a kernel loaded here for bring-up testing is
     * architecturally correct, not a hack, as long as it fits in 64K.
     * OPEN QUESTION (see docs/roadmap.md Phase 1): real firmware bigger
     * than 64K boots from flash bank 1 via BOOT_ADD option-byte
     * selection, which is a real address-0 remap distinct from ITCM's
     * own fixed mapping -- not yet modeled here. Revisit once real
     * (not test-kernel) firmware boot is attempted.
     */
    INIT_RAM_REGION(itcm, "GNW_H7B0.itcm", ITCM_BASE_ADDRESS, ITCM_SIZE);
    INIT_RAM_REGION(dtcm, "GNW_H7B0.dtcm", DTCM_BASE_ADDRESS, DTCM_SIZE);
    INIT_RAM_REGION(axisram1, "GNW_H7B0.axisram1", AXISRAM1_BASE_ADDRESS,
                     AXISRAM1_SIZE);
    INIT_RAM_REGION(axisram2, "GNW_H7B0.axisram2", AXISRAM2_BASE_ADDRESS,
                     AXISRAM2_SIZE);
    INIT_RAM_REGION(axisram3, "GNW_H7B0.axisram3", AXISRAM3_BASE_ADDRESS,
                     AXISRAM3_SIZE);
    INIT_RAM_REGION(ahbsram1, "GNW_H7B0.ahbsram1", AHBSRAM1_BASE_ADDRESS,
                     AHBSRAM1_SIZE);
    INIT_RAM_REGION(ahbsram2, "GNW_H7B0.ahbsram2", AHBSRAM2_BASE_ADDRESS,
                     AHBSRAM2_SIZE);
    INIT_RAM_REGION(srdsram, "GNW_H7B0.srdsram", SRDSRAM_BASE_ADDRESS,
                     SRDSRAM_SIZE);
    INIT_RAM_REGION(bkpsram, "GNW_H7B0.bkpsram", BKPSRAM_BASE_ADDRESS,
                     BKPSRAM_SIZE);
    INIT_RAM_REGION(flash_bank1, "GNW_H7B0.flash_bank1",
                     FLASH_BANK1_BASE_ADDRESS, FLASH_BANK_SIZE);
    INIT_RAM_REGION(flash_bank2, "GNW_H7B0.flash_bank2",
                     FLASH_BANK2_BASE_ADDRESS, FLASH_BANK_SIZE);
    INIT_RAM_REGION(extflash, "GNW_H7B0.extflash", EXTFLASH_BASE_ADDRESS,
                     EXTFLASH_SIZE);

#undef INIT_RAM_REGION

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
