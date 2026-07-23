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
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include "hw/arm/gnw_h7b0_soc.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/system.h"
#include "hw/misc/unimp.h"

static void gnw_h7b0_soc_initfn(Object *obj)
{
    GnwH7B0State *s = GNW_H7B0_SOC(obj);

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);
    object_initialize_child(obj, "rcc", &s->rcc, TYPE_GNW_H7B0_RCC);
    object_initialize_child(obj, "pwr", &s->pwr, TYPE_GNW_H7B0_PWR);
    object_initialize_child(obj, "octospi1", &s->octospi1, TYPE_GNW_H7B0_OSPI);
    object_initialize_child(obj, "octospi2", &s->octospi2, TYPE_GNW_H7B0_OSPI);
    object_initialize_child(obj, "adc", &s->adc, TYPE_GNW_H7B0_ADC);
    object_initialize_child(obj, "lptim1", &s->lptim1, TYPE_GNW_H7B0_LPTIM1);
    object_initialize_child(obj, "ltdc", &s->ltdc, TYPE_GNW_H7B0_LTDC);
    object_initialize_child(obj, "dma2d", &s->dma2d, TYPE_GNW_H7B0_DMA2D);
    object_initialize_child(obj, "spi2", &s->spi2, TYPE_GNW_H7B0_SPI);
    object_initialize_child(obj, "spi1", &s->spi1, TYPE_GNW_H7B0_SPI);
    /* SPI1 is the "Tim" dedicated-pin SD-card mod path; SPI2 is not
     * (LCD-panel init commands) -- see gnw_h7b0_spi.h. */
    qdev_prop_set_bit(DEVICE(&s->spi1), "sd-card", true);
    object_initialize_child(obj, "rtc", &s->rtc, TYPE_GNW_H7B0_RTC);
    object_initialize_child(obj, "crc", &s->crc, TYPE_GNW_H7B0_CRC);
    object_initialize_child(obj, "hash", &s->hash, TYPE_GNW_H7B0_HASH);
    object_initialize_child(obj, "mdma", &s->mdma, TYPE_GNW_H7B0_MDMA);
    object_initialize_child(obj, "rng", &s->rng, TYPE_GNW_H7B0_RNG);
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_GNW_H7B0_GPIO);
    object_initialize_child(obj, "dbgmcu", &s->dbgmcu, TYPE_GNW_H7B0_DBGMCU);
    object_initialize_child(obj, "dwt", &s->dwt, TYPE_GNW_H7B0_DWT);
    object_initialize_child(obj, "flash_r", &s->flash_r, TYPE_GNW_H7B0_FLASH_R);
    object_initialize_child(obj, "fmc", &s->fmc, TYPE_GNW_H7B0_FMC);
    object_initialize_child(obj, "crs", &s->crs, TYPE_GNW_H7B0_CRS);
    object_initialize_child(obj, "octospim", &s->octospim, TYPE_GNW_H7B0_OCTOSPIM);
    object_initialize_child(obj, "otfdec1", &s->otfdec1, TYPE_GNW_H7B0_OTFDEC);
    object_initialize_child(obj, "otfdec2", &s->otfdec2, TYPE_GNW_H7B0_OTFDEC);
    object_initialize_child(obj, "cryp", &s->cryp, TYPE_GNW_H7B0_CRYP);
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
    object_initialize_child(obj, "iwdg", &s->iwdg, TYPE_GNW_H7B0_IWDG);
    object_initialize_child(obj, "lpuart1", &s->lpuart1, TYPE_GNW_H7B0_LPUART1);
    object_initialize_child(obj, "usart1", &s->usart1, TYPE_GNW_H7B0_USART1);


    s->sysclk = qdev_init_clock_in(DEVICE(s), "sysclk", NULL, NULL, 0);
}

/*
 * Backs `field` directly by `image_path` (created/truncated to `size` if it
 * doesn't already exist yet) when non-NULL/non-empty, so guest writes land
 * in that file live instead of anonymous memory -- see the bank1_image/
 * bank2_image/extflash_image doc comment in gnw_h7b0_soc.h. Falls back to
 * plain anonymous RAM (today's behavior, seeded only via `-device loader`)
 * otherwise.
 *
 * memory_region_init_ram_from_file() is CONFIG_POSIX-only in QEMU itself
 * (it's mmap-based) -- genuinely unavailable on Windows via QEMU's own
 * generic memory-backend machinery (backends/hostmem-file.c does the same
 * #ifndef CONFIG_POSIX: error_setg(...) refusal). On _WIN32 we get real
 * persistence anyway via a direct CreateFileMapping()/MapViewOfFile() host
 * pointer, wrapped as guest RAM with memory_region_init_ram_ptr() -- the
 * same portable "wrap an existing host pointer" primitive this codebase
 * already relies on for gnw_h7b0_otfdec.c's decrypt-overlay buffers. Only
 * a truly unknown non-POSIX, non-Windows host falls back to ephemeral RAM,
 * loudly (warn_report(), not silence): persistence quietly not working
 * would be a much worse surprise than an explicit "this won't persist".
 */
#ifdef _WIN32
static bool gnw_h7b0_init_ram_from_file_win32(MemoryRegion *mr, Object *owner,
                                               const char *name, uint64_t size,
                                               const char *image_path,
                                               Error **errp)
{
    HANDLE file = CreateFileA(image_path, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        error_setg(errp, "%s: CreateFileA(\"%s\") failed (error %lu)",
                   name, image_path, (unsigned long)GetLastError());
        return false;
    }

    LARGE_INTEGER cur_size;
    if (!GetFileSizeEx(file, &cur_size)) {
        error_setg(errp, "%s: GetFileSizeEx(\"%s\") failed (error %lu)",
                   name, image_path, (unsigned long)GetLastError());
        CloseHandle(file);
        return false;
    }
    /* Created/truncated to `size` if it doesn't already exist yet or is
     * shorter -- matches memory_region_init_ram_from_file()'s documented
     * behavior on POSIX. Never shrink an existing longer file. */
    if ((uint64_t)cur_size.QuadPart < size) {
        LARGE_INTEGER new_size;
        new_size.QuadPart = (LONGLONG)size;
        if (!SetFilePointerEx(file, new_size, NULL, FILE_BEGIN) ||
            !SetEndOfFile(file)) {
            error_setg(errp, "%s: extending \"%s\" to %" PRIu64
                       " bytes failed (error %lu)", name, image_path, size,
                       (unsigned long)GetLastError());
            CloseHandle(file);
            return false;
        }
    }

    HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READWRITE,
                                         (DWORD)(size >> 32),
                                         (DWORD)(size & 0xFFFFFFFFu), NULL);
    if (mapping == NULL) {
        error_setg(errp, "%s: CreateFileMappingA(\"%s\") failed (error %lu)",
                   name, image_path, (unsigned long)GetLastError());
        CloseHandle(file);
        return false;
    }

    void *ptr = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                               0, 0, size);
    if (ptr == NULL) {
        error_setg(errp, "%s: MapViewOfFile(\"%s\") failed (error %lu)",
                   name, image_path, (unsigned long)GetLastError());
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }

    /* The mapping/file HANDLEs are intentionally never closed here: the
     * view stays valid as long as they're open, this region needs to
     * stay live for the device's/process's whole lifetime (same "leak
     * until process exit" convention gnw_h7b0_otfdec.c already uses for
     * its own host-pointer-backed regions), and Windows keeps the
     * underlying mapping alive via the still-open view regardless. */
    memory_region_init_ram_ptr(mr, owner, name, size, ptr);
    return true;
}
#endif

static bool gnw_h7b0_init_ram_or_file(MemoryRegion *mr, Object *owner,
                                       const char *name, uint64_t size,
                                       const char *image_path, Error **errp)
{
    if (image_path != NULL && image_path[0] != '\0') {
#ifdef CONFIG_POSIX
        return memory_region_init_ram_from_file(mr, owner, name, size, 0,
                                                 RAM_SHARED, image_path, 0,
                                                 errp);
#elif defined(_WIN32)
        return gnw_h7b0_init_ram_from_file_win32(mr, owner, name, size,
                                                  image_path, errp);
#else
        warn_report("persistent flash-image backing isn't supported on "
                    "this host -- \"%s\" will NOT persist writes, falling "
                    "back to ephemeral RAM for %s", image_path, name);
#endif
    }
    memory_region_init_ram(mr, owner, name, size, errp);
    return *errp == NULL;
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

#define INIT_RAM_OR_FILE_REGION(field, name, base, size, image_path) \
    do { \
        if (!gnw_h7b0_init_ram_or_file(&s->field, OBJECT(dev_soc), name, \
                                        size, (image_path), &err)) { \
            error_propagate(errp, err); \
            return; \
        } \
        memory_region_add_subregion(system_memory, base, &s->field); \
    } while (0)

    INIT_RAM_REGION(itcm, "GNW_H7B0.itcm", ITCM_BASE_ADDRESS, ITCM_SIZE);
    INIT_RAM_REGION(dtcm, "GNW_H7B0.dtcm", DTCM_BASE_ADDRESS, DTCM_SIZE);
    /*
     * Real H7B0 AXI SRAM is one genuinely contiguous ~1MB block (RM0455);
     * AXISRAM1/2/3 are just documentation/sizing subdivisions of it, not
     * separate hardware blocks -- confirmed no gap between them
     * (AXISRAM1_BASE+SIZE == AXISRAM2_BASE, and likewise for 2->3).
     * Modeling them as three separate MemoryRegion objects made any
     * guest buffer straddling one of those artificial boundaries
     * unreachable via a single memory_region_find() call -- found via
     * live profiling when the LTDC non-VBR fallback's RAM dirty-bitmap
     * tracking (gnw_h7b0_ltdc_fb_dirty_check_and_clear()) silently
     * failed to bind on every call for a framebuffer that happened to
     * span AXISRAM1/AXISRAM2. One region for the whole span is a more
     * faithful model of the real contiguous hardware, not just a
     * workaround, and fixes this for any future code with the same
     * assumption.
     */
    INIT_RAM_REGION(axisram1, "GNW_H7B0.axisram1", AXISRAM1_BASE_ADDRESS,
                     AXISRAM1_SIZE + AXISRAM2_SIZE + AXISRAM3_SIZE);
    INIT_RAM_REGION(ahbsram1, "GNW_H7B0.ahbsram1", AHBSRAM1_BASE_ADDRESS,
                     AHBSRAM1_SIZE);
    INIT_RAM_REGION(ahbsram2, "GNW_H7B0.ahbsram2", AHBSRAM2_BASE_ADDRESS,
                     AHBSRAM2_SIZE);
    INIT_RAM_REGION(srdsram, "GNW_H7B0.srdsram", SRDSRAM_BASE_ADDRESS,
                     SRDSRAM_SIZE);
    INIT_RAM_REGION(bkpsram, "GNW_H7B0.bkpsram", BKPSRAM_BASE_ADDRESS,
                     BKPSRAM_SIZE);
    INIT_RAM_OR_FILE_REGION(flash_bank1, "GNW_H7B0.flash_bank1",
                             FLASH_BANK1_BASE_ADDRESS, FLASH_BANK_SIZE,
                             s->bank1_image);
    INIT_RAM_OR_FILE_REGION(flash_bank2, "GNW_H7B0.flash_bank2",
                             FLASH_BANK2_BASE_ADDRESS, FLASH_BANK_SIZE,
                             s->bank2_image);
    INIT_RAM_OR_FILE_REGION(extflash, "GNW_H7B0.extflash", EXTFLASH_BASE_ADDRESS,
                             EXTFLASH_SIZE, s->extflash_image);
    INIT_RAM_REGION(uid, "GNW_H7B0.uid", UID_BASE_ADDRESS, UID_SIZE);

    /*
     * Unimplemented devices to prevent BusFaults on gnwmanager payload,
     * a not-yet-modeled peripheral read/write, or a reset-register
     * snapshot tool (scripts/snapshot_registers.py) walking every
     * STM32H7B0.svd peripheral. Every SVD peripheral base address not
     * already covered by a real device model below is stubbed here so
     * accesses log instead of faulting.
     */
    create_unimplemented_device("SPI3", 0x40003c00, 0x400);
    create_unimplemented_device("SPDIFRX", 0x40004000, 0x400);
    create_unimplemented_device("USART2", 0x40004400, 0x400);
    create_unimplemented_device("USART3", 0x40004800, 0x400);
    create_unimplemented_device("UART4", 0x40004c00, 0x400);
    create_unimplemented_device("UART5", 0x40005000, 0x400);
    create_unimplemented_device("I2C1", 0x40005400, 0x400);
    create_unimplemented_device("I2C2", 0x40005800, 0x400);
    create_unimplemented_device("I2C3", 0x40005c00, 0x400);
    create_unimplemented_device("CEC", 0x40006c00, 0x400);
    create_unimplemented_device("UART7", 0x40007800, 0x400);
    create_unimplemented_device("UART8", 0x40007c00, 0x400);
    create_unimplemented_device("SWPMI", 0x40008800, 0x400);
    create_unimplemented_device("OPAMP", 0x40009000, 0x400);
    create_unimplemented_device("MDIOS", 0x40009400, 0x400);
    create_unimplemented_device("TT_FDCAN", 0x4000a000, 0x400);
    create_unimplemented_device("FDCAN", 0x4000a400, 0x400);
    create_unimplemented_device("CAN_CCU", 0x4000a800, 0x400);
    create_unimplemented_device("TIM8", 0x40010400, 0x400);
    create_unimplemented_device("USART6", 0x40011400, 0x400);
    create_unimplemented_device("USART9", 0x40011800, 0x400);
    create_unimplemented_device("USART10", 0x40011c00, 0x400);
    create_unimplemented_device("SPI4", 0x40013400, 0x400);
    create_unimplemented_device("TIM15", 0x40014000, 0x400);
    create_unimplemented_device("TIM16", 0x40014400, 0x400);
    create_unimplemented_device("TIM17", 0x40014800, 0x400);
    create_unimplemented_device("SPI5", 0x40015000, 0x400);
    create_unimplemented_device("SAI2", 0x40015c00, 0x400);
    create_unimplemented_device("HRTIM_Master", 0x40017400, 0x80);
    create_unimplemented_device("HRTIM_TIMA", 0x40017480, 0x80);
    create_unimplemented_device("HRTIM_TIMB", 0x40017500, 0x80);
    create_unimplemented_device("HRTIM_TIMC", 0x40017580, 0x80);
    create_unimplemented_device("HRTIM_TIMD", 0x40017600, 0x80);
    create_unimplemented_device("HRTIM_TIME", 0x40017680, 0x80);
    create_unimplemented_device("HRTIM_Common", 0x40017780, 0x80);
    create_unimplemented_device("DFSDM1", 0x40017800, 0x4bc);
    create_unimplemented_device("DMAMUX1", 0x40020800, 0x400);
    create_unimplemented_device("OTG1_HS_GLOBAL", 0x40040000, 0x400);
    create_unimplemented_device("OTG1_HS_HOST", 0x40040400, 0x400);
    create_unimplemented_device("OTG1_HS_DEVICE", 0x40040800, 0x400);
    create_unimplemented_device("OTG1_HS_PWRCLK", 0x40040e00, 0x3f200);
    create_unimplemented_device("DCMI", 0x48020000, 0x400);
    create_unimplemented_device("PSSI", 0x48020400, 0x6b);
    create_unimplemented_device("HSEM", 0x48020800, 0x400);
    create_unimplemented_device("SDMMC2", 0x48022400, 0x400);
    create_unimplemented_device("DELAY_Block_SDMMC2", 0x48022800, 0x400);
    create_unimplemented_device("BDMA1", 0x48022c00, 0x400);
    create_unimplemented_device("AXI", 0x51000000, 0x100000);
    create_unimplemented_device("Delay_Block_OCTOSPI1", 0x52006000, 0x400);
    create_unimplemented_device("SDMMC1", 0x52007000, 0x3fd);
    create_unimplemented_device("DELAY_Block_SDMMC1", 0x52008000, 0x400);
    create_unimplemented_device("RAMECC", 0x52009000, 0x400);
    create_unimplemented_device("Delay_Block_OCTOSPI2", 0x5200b000, 0x400);
    create_unimplemented_device("SPI6", 0x58001400, 0x400);
    create_unimplemented_device("I2C4", 0x58001c00, 0x400);
    create_unimplemented_device("LPTIM2", 0x58002400, 0x400);
    create_unimplemented_device("LPTIM3", 0x58002800, 0x400);
    create_unimplemented_device("COMP1", 0x58003800, 0x400);
    create_unimplemented_device("VREFBUF", 0x58003c00, 0x400);
    create_unimplemented_device("DFSDM2", 0x58006c00, 0x400);
    create_unimplemented_device("BDMA2", 0x58025400, 0x400);
    create_unimplemented_device("DMAMUX2", 0x58025800, 0x400);

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
    /*
     * Was 96 -- too small for OCTOSPI2_IRQn (150), added when wiring
     * up real OCTOSPI1/2 IRQ lines (see gnw_h7b0_ospi.h). NVIC's
     * num-irq must be a multiple of 32; 160 is the smallest multiple
     * covering every IRQn this SoC model currently uses.
     */
    qdev_prop_set_uint32(armv7m, "num-irq", 160);
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
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->octospi1), 0,
                        qdev_get_gpio_in(armv7m, OCTOSPI1_IRQn));
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
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->octospi2), 0,
                        qdev_get_gpio_in(armv7m, OCTOSPI2_IRQn));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->adc), 0, ADC_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->adc), 0,
                        qdev_get_gpio_in(armv7m, ADC_IRQn));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->lptim1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->lptim1), 0, LPTIM1_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->lptim1), 0,
                        qdev_get_gpio_in(armv7m, LPTIM1_IRQn));

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
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc), 0,
                        qdev_get_gpio_in(armv7m, RTC_Alarm_IRQn));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->crc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->crc), 0, CRC_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->hash), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->hash), 0, HASH_BASE_ADDRESS);
    /* HASH_RNG shared line (NVIC IRQ 80, see STM32H7B0.svd) -- RNG has no
     * error-condition IRQ source in this synchronous model (see
     * gnw_h7b0_rng.c), so this line is HASH's alone for now. */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->hash), 0, qdev_get_gpio_in(armv7m, 80));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mdma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->mdma), 0, MDMA_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rng), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rng), 0, RNG_BASE_ADDRESS);

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
    gnw_h7b0_flash_r_set_banks(&s->flash_r, &s->flash_bank1, &s->flash_bank2);

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

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->otfdec1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->otfdec1), 0, OTFDEC1_BASE_ADDRESS);
    gnw_h7b0_otfdec_set_extflash(&s->otfdec1, system_memory, &s->extflash,
                                  EXTFLASH_BASE_ADDRESS, EXTFLASH_SIZE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->otfdec2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->otfdec2), 0, OTFDEC2_BASE_ADDRESS);
    gnw_h7b0_otfdec_set_extflash(&s->otfdec2, system_memory, &s->extflash,
                                  EXTFLASH_BASE_ADDRESS, EXTFLASH_SIZE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->cryp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->cryp), 0, CRYP_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->cryp), 0, qdev_get_gpio_in(armv7m, 79));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->exti), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->exti), 0, EXTI_SYSCFG_BASE_ADDRESS);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 0,
                        qdev_get_gpio_in(armv7m, EXTI0_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 1,
                        qdev_get_gpio_in(armv7m, EXTI1_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 2,
                        qdev_get_gpio_in(armv7m, EXTI2_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 3,
                        qdev_get_gpio_in(armv7m, EXTI3_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 4,
                        qdev_get_gpio_in(armv7m, EXTI4_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 5,
                        qdev_get_gpio_in(armv7m, EXTI9_5_IRQn));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->exti), 6,
                        qdev_get_gpio_in(armv7m, EXTI15_10_IRQn));
    /* GPIO's PA0-release logic needs to notify EXTI of a real edge -- see
     * gnw_h7b0_gpio.c. Both devices already exist and are realized by
     * this point; this just wires the pointer, not a QOM property, since
     * GPIO -> EXTI isn't a real hardware bus relationship worth modeling
     * more formally for one internal notification. */
    s->gpio.exti = &s->exti;
    s->gpio.syscfg = &s->syscfg;

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
    gnw_h7b0_hash_set_dma(&s->hash, &s->dma);
    gnw_h7b0_adc_set_dma(&s->adc, &s->dma);

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
    gnw_h7b0_tim1_set_rcc(&s->tim1, &s->rcc);

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

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->iwdg), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->iwdg), 0, IWDG_BASE_ADDRESS);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->lpuart1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->lpuart1), 0, LPUART1_BASE_ADDRESS);

    /* Homebrew firmware uses USART1 as its printf console; bind it to
     * serial port 0 so -serial stdio/file: just works. */
    qdev_prop_set_chr(DEVICE(&s->usart1), "chardev", serial_hd(0));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->usart1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->usart1), 0, USART1_BASE_ADDRESS);

    /*
     * Remaining peripherals (DMA2D, GPIO, USART, real flash/QSPI boot)
     * are added in later phases; see ../../docs/roadmap.md.
     */
}

static const Property gnw_h7b0_soc_properties[] = {
    /*
     * Optional file-backed persistence for the flash regions -- e.g.
     * -global gnw-h7b0-soc.extflash-image=backup/qemu-images/zelda-extflash.bin
     * makes guest writes (flashing/erasing) land in that file live, instead
     * of only in anonymous RAM seeded once at boot by `-device loader`. See
     * the doc comment on GnwH7B0State's bank1_image/bank2_image/
     * extflash_image fields in gnw_h7b0_soc.h.
     */
    DEFINE_PROP_STRING("bank1-image", GnwH7B0State, bank1_image),
    DEFINE_PROP_STRING("bank2-image", GnwH7B0State, bank2_image),
    DEFINE_PROP_STRING("extflash-image", GnwH7B0State, extflash_image),
};

static void gnw_h7b0_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = gnw_h7b0_soc_realize;
    device_class_set_props(dc, gnw_h7b0_soc_properties);
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
