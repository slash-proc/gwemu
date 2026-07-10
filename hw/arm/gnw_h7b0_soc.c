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
    object_initialize_child(obj, "rcc", &s->rcc, TYPE_GNW_H7B0_RCC);
    object_initialize_child(obj, "pwr", &s->pwr, TYPE_GNW_H7B0_PWR);
    object_initialize_child(obj, "octospi1", &s->octospi1, TYPE_GNW_H7B0_OSPI);
    object_initialize_child(obj, "octospi2", &s->octospi2, TYPE_GNW_H7B0_OSPI);
    object_initialize_child(obj, "adc", &s->adc, TYPE_GNW_H7B0_ADC);

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
    /*
     * DBGMCU (CoreSight debug unit): not modeled as a real device, same
     * "plain RAM placeholder" treatment as extflash above. Real firmware
     * (ST HAL's DBGMCU_CR sleep/stop/standby-debug-enable helpers) reads
     * and read-modifies-writes this during early boot; without *some*
     * backing memory here those accesses BusFault, and this SoC's fault
     * handlers don't recover from that gracefully. A RAM stub is enough
     * to let boot past that point -- real semantics (e.g. IDC's fixed
     * chip-ID reset value) are future work if something depends on them.
     */
    INIT_RAM_REGION(dbgmcu, "GNW_H7B0.dbgmcu", DBGMCU_BASE_ADDRESS,
                     DBGMCU_SIZE);
    /*
     * Flash controller registers (FLASH_ACR etc, distinct from the
     * memory-mapped flash content above) -- same plain-RAM-placeholder
     * rationale as DBGMCU just above: real firmware polls FLASH_ACR
     * wait-state-ready bits during clock init and BusFaults without
     * backing memory here.
     */
    INIT_RAM_REGION(flash_r, "GNW_H7B0.flash_r", FLASH_R_BASE_ADDRESS,
                     FLASH_R_SIZE);
    /* FMC (external memory controller) -- same plain-RAM rationale. */
    INIT_RAM_REGION(fmc, "GNW_H7B0.fmc", FMC_BASE_ADDRESS, FMC_SIZE);
    /*
     * GPIOA-K -- plain RAM placeholder, not a real GPIO model yet
     * (Phase 4). See the GPIO_SIZE comment in gnw_h7b0_soc.h.
     */
    INIT_RAM_REGION(gpio, "GNW_H7B0.gpio", GPIO_BASE_ADDRESS, GPIO_SIZE);
    /* CRS (HSI48 clock recovery/auto-trim) -- plain RAM placeholder. */
    INIT_RAM_REGION(crs, "GNW_H7B0.crs", CRS_BASE_ADDRESS, CRS_SIZE);
    /* OCTOSPI IO manager -- plain RAM placeholder (pin-mux only). */
    INIT_RAM_REGION(octospim, "GNW_H7B0.octospim", OCTOSPIM_BASE_ADDRESS,
                     OCTOSPIM_SIZE);
    /* SPI2 (SD-card mod path) -- plain RAM placeholder. */
    INIT_RAM_REGION(spi2, "GNW_H7B0.spi2", SPI2_BASE_ADDRESS, SPI2_SIZE);
#undef INIT_RAM_REGION

    armv7m = DEVICE(&s->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", 96);
    qdev_prop_set_uint8(armv7m, "num-prio-bits", 4);
    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m7"));
    qdev_prop_set_bit(armv7m, "enable-bitband", true);
    /*
     * Real hardware boots from flash bank 1 at 0x08000000 via BOOT_ADD
     * option-byte address-0 remap (cold boot) or a debug probe directly
     * setting VTOR/SP/PC there (the gnwmanager dev-flow path retro-go's
     * own linker script targets -- see STM32H7B0VBTx_FLASH.ld, whose
     * .isr_vector lands at FLASH's origin, not ITCM's). QEMU's ARMv7M
     * container exposes exactly this indirection via init-nsvtor: it's
     * the initial value of VTOR, which is where cpu_reset() reads the
     * initial SP/PC vector table from. (Cortex-M7 has no TrustZone-M,
     * so it's the "ns" -- non-secure, i.e. only -- variant that applies;
     * "init-svtor" is a no-op property on this core, silently ignored by
     * armv7m.c's object_property_find guard -- do not use it here.)
     * Pointing it at flash bank 1 models that remap without needing a
     * fake alias memory region.
     */
    qdev_prop_set_uint32(armv7m, "init-nsvtor", FLASH_BANK1_BASE_ADDRESS);
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    qdev_connect_clock_in(armv7m, "refclk", s->sysclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                              OBJECT(system_memory), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rcc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rcc), 0, RCC_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pwr), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pwr), 0, PWR_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->octospi1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->octospi1), 0, OCTOSPI1_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->octospi2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->octospi2), 0, OCTOSPI2_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->adc), 0, ADC_BASE_ADDRESS);

    /*
     * Remaining peripherals (DMA2D, GPIO, USART, real flash/QSPI boot)
     * are added in later phases; see ../../docs/roadmap.md.
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
