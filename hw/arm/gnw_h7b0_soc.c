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
    object_initialize_child(obj, "ltdc", &s->ltdc, TYPE_GNW_H7B0_LTDC);
    object_initialize_child(obj, "dma2d", &s->dma2d, TYPE_GNW_H7B0_DMA2D);
    object_initialize_child(obj, "spi2", &s->spi2, TYPE_GNW_H7B0_SPI);
    object_initialize_child(obj, "spi1", &s->spi1, TYPE_GNW_H7B0_SPI);
    /* SPI1 is the "Tim" dedicated-pin SD-card mod path; SPI2 is not
     * (LCD-panel init commands) -- see gnw_h7b0_spi.h. */
    qdev_prop_set_bit(DEVICE(&s->spi1), "sd-card", true);
    object_initialize_child(obj, "rtc", &s->rtc, TYPE_GNW_H7B0_RTC);
    object_initialize_child(obj, "crc", &s->crc, TYPE_GNW_H7B0_CRC);
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_GNW_H7B0_GPIO);
    object_initialize_child(obj, "dbgmcu", &s->dbgmcu, TYPE_GNW_H7B0_DBGMCU);
    object_initialize_child(obj, "dwt", &s->dwt, TYPE_GNW_H7B0_DWT);
    object_initialize_child(obj, "flash_r", &s->flash_r, TYPE_GNW_H7B0_FLASH_R);
    object_initialize_child(obj, "fmc", &s->fmc, TYPE_GNW_H7B0_FMC);
    object_initialize_child(obj, "crs", &s->crs, TYPE_GNW_H7B0_CRS);
    object_initialize_child(obj, "octospim", &s->octospim, TYPE_GNW_H7B0_OCTOSPIM);
    object_initialize_child(obj, "exti", &s->exti, TYPE_GNW_H7B0_EXTI);
    object_initialize_child(obj, "syscfg", &s->syscfg, TYPE_GNW_H7B0_SYSCFG);
    object_initialize_child(obj, "dma", &s->dma, TYPE_GNW_H7B0_DMA);
    object_initialize_child(obj, "sai1", &s->sai1, TYPE_GNW_H7B0_SAI1);
    object_initialize_child(obj, "dac1", &s->dac1, TYPE_GNW_H7B0_DAC);
    object_initialize_child(obj, "dac2", &s->dac2, TYPE_GNW_H7B0_DAC);
    object_initialize_child(obj, "tim1", &s->tim1, TYPE_GNW_H7B0_TIM1);
    object_initialize_child(obj, "jpeg", &s->jpeg, TYPE_GNW_H7B0_JPEG);
    object_initialize_child(obj, "tamp", &s->tamp, TYPE_GNW_H7B0_TAMP);
    object_initialize_child(obj, "wwdg", &s->wwdg, TYPE_GNW_H7B0_WWDG);
    object_initialize_child(obj, "tim2", &s->tim2, TYPE_GNW_H7B0_TIM2);


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
    INIT_RAM_REGION(uid, "GNW_H7B0.uid", UID_BASE_ADDRESS, UID_SIZE);
    {
        /*
         * Seed a fixed synthetic 96-bit UID (HAL_GetUIDw0/1/2() read
         * this as three consecutive words at UID_BASE) -- see
         * gnw_h7b0_soc.h's UID_BASE_ADDRESS comment for why this
         * region exists at all. Value is arbitrary; nothing depends
         * on matching real silicon.
         */
        uint32_t *uid_ptr = memory_region_get_ram_ptr(&s->uid);

        uid_ptr[0] = 0x47575145; /* "GWQE" */
        uid_ptr[1] = 0x4d55004d; /* "MU\0M" */
        uid_ptr[2] = 0x00000001;
    }
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
    /*
     * Flash controller registers (FLASH_ACR etc, distinct from the
     * memory-mapped flash content above) -- same plain-RAM-placeholder
     * rationale as DBGMCU just above: real firmware polls FLASH_ACR
     * wait-state-ready bits during clock init and BusFaults without
     * backing memory here.
     */
    /* FMC (external memory controller) -- same plain-RAM rationale. */
    /* CRS (HSI48 clock recovery/auto-trim) -- plain RAM placeholder. */
    /* OCTOSPI IO manager -- plain RAM placeholder (pin-mux only). */
    /* EXTI + SYSCFG -- plain RAM placeholder. */
    /* DMA1 + DMA2 + DMAMUX1 -- plain RAM placeholder. */
    /* SAI1 -- real device (gnw_h7b0_sai1.c): opens an AUD_* voice and
     * snoops DMA1 Stream0 transfer-complete ticks for PCM data. */
    /* DAC1/DAC2 -- plain RAM placeholders (previously entirely unmapped). */
    /* TIM1 -- plain RAM placeholder (previously entirely unmapped). */
    /* JPEG -- plain RAM placeholder (previously entirely unmapped). */
    /* TAMP -- plain RAM placeholder (previously entirely unmapped). */
    /* WWDG -- plain RAM placeholder (previously entirely unmapped). */
    /*
     * TIM2/3/4/5/6/7/12/13/14 -- plain RAM placeholder (previously
     * entirely unmapped).
     */
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
    /*
     * DWT (0xe0001000) is otherwise unconditionally RAZ/WI inside
     * armv7m.c's own container -- see gnw_h7b0_dwt.h for why real
     * firmware needs a real DWT_CYCCNT here. s->dwt's MemoryRegion is
     * already valid at this point (set up in its instance_init via
     * object_initialize_child, independent of when it's realized).
     */
    object_property_set_link(OBJECT(&s->armv7m), "dwt-mr",
                              OBJECT(&s->dwt.mmio), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rcc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rcc), 0, RCC_BASE_ADDRESS);
    /* s->sysclk is wired by the board (gnw_h7b0.c) before realize, same
     * as the sai1_set_dma/sai1_set_rcc pattern below -- lets RCC push
     * live PLL1/SYSCLK recomputes out to the ARMv7M cpuclk/refclk and
     * anything else downstream, instead of the Clock staying pinned at
     * its pre-realize bootstrap value forever. */
    gnw_h7b0_rcc_set_sysclk(&s->rcc, s->sysclk);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pwr), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pwr), 0, PWR_BASE_ADDRESS);

    /*
     * flash-size ties the OSPI device's synthetic JEDEC/SFDP identity
     * to the same 64M placeholder EXTFLASH_SIZE uses for the
     * memory-mapped XIP window below, so density reported over the
     * command protocol and the actual backing region size agree.
     */
    qdev_prop_set_uint64(DEVICE(&s->octospi1), "flash-size", EXTFLASH_SIZE);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->octospi1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->octospi1), 0, OCTOSPI1_BASE_ADDRESS);
    /*
     * Real hardware's single external NOR is wired to OCTOSPI1; give
     * it a host pointer into the same extflash RAM region the CPU
     * sees over XIP, so indirect-mode PP/erase/READ (used by retro-go's
     * littlefs block device, gw_littlefs.c) actually persist instead
     * of being silently dropped -- see gnw_h7b0_ospi.h.
     */
    gnw_h7b0_ospi_set_backing(&s->octospi1,
                               memory_region_get_ram_ptr(&s->extflash));

    qdev_prop_set_uint64(DEVICE(&s->octospi2), "flash-size", EXTFLASH_SIZE);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->octospi2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->octospi2), 0, OCTOSPI2_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->adc), 0, ADC_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->adc), 0,
                        qdev_get_gpio_in(armv7m, ADC_IRQn));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ltdc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ltdc), 0, LTDC_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ltdc), 0,
                        qdev_get_gpio_in(armv7m, LTDC_IRQn));
    gnw_h7b0_ltdc_set_rcc(&s->ltdc, &s->rcc);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dma2d), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dma2d), 0, DMA2D_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma2d), 0,
                        qdev_get_gpio_in(armv7m, DMA2D_IRQn));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->spi2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spi2), 0, SPI2_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->spi1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spi1), 0, SPI1_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rtc), 0, RTC_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->crc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->crc), 0, CRC_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio), 0, GPIO_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dbgmcu), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dbgmcu), 0, DBGMCU_BASE_ADDRESS);

    /*
     * s->dwt is NOT mapped via sysbus_mmio_map/system_memory -- it's
     * wired directly into armv7m's own container (see the "dwt-mr"
     * link property set before armv7m's realize above), since that's
     * the only way to actually override armv7m.c's own RAZ/WI default
     * for the CPU-architected 0xe0001000 DWT address. Still needs its
     * own realize for reset/vmstate registration.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dwt), errp)) {
        return;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->flash_r), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->flash_r), 0, FLASH_R_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->fmc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->fmc), 0, FMC_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->crs), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->crs), 0, CRS_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->octospim), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->octospim), 0, OCTOSPIM_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->exti), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->exti), 0, EXTI_SYSCFG_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->syscfg), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->syscfg), 0, EXTI_SYSCFG_BASE_ADDRESS + 0x400);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dma), 0, DMA_BASE_ADDRESS);
    {
        /* DMA1 Stream0-6 (11-17), Stream7 (47); DMA2 Stream0-4 (56-60),
         * Stream5-7 (68-70) -- see stm32h7b0xx.h's IRQn_Type, not a
         * contiguous range. */
        static const int dma_stream_irqn[GNW_H7B0_DMA_STREAM_COUNT] = {
            11, 12, 13, 14, 15, 16, 17, 47,
            56, 57, 58, 59, 60, 68, 69, 70,
        };
        for (int i = 0; i < GNW_H7B0_DMA_STREAM_COUNT; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), i,
                                qdev_get_gpio_in(armv7m, dma_stream_irqn[i]));
        }
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sai1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sai1), 0, SAI1_BASE_ADDRESS);
    gnw_h7b0_sai1_set_dma(&s->sai1, &s->dma);
    gnw_h7b0_sai1_set_rcc(&s->sai1, &s->rcc);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dac1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dac1), 0, DAC1_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dac2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dac2), 0, DAC2_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->tim1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tim1), 0, TIM1_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->jpeg), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->jpeg), 0, JPEG_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->tamp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tamp), 0, TAMP_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->wwdg), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->wwdg), 0, WWDG_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->tim2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tim2), 0, TIM2_BLOCK_BASE_ADDRESS);
    gnw_h7b0_tim2_set_rcc(&s->tim2, &s->rcc);

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
