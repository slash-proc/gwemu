/*
 * STM32H7B0 OCTOSPI device with real indirect-mode command decoding
 * (Nintendo Game & Watch)
 *
 * Not cycle-accurate -- command/status completion is instant (TCF set
 * the moment CCR/IR/AR is written, no dummy-cycle or clock-rate
 * timing modeled) -- but the indirect-mode command *protocol* is now
 * real: RDID/RDSR/RDCR/SFDP/WREN/RSTEN/RST are decoded and answered
 * as a synthetic Macronix MX25U51245G (64Mbit... no, 64MB/512Mbit,
 * JEDEC C2 25 3A) NOR flash chip -- see retro-go's
 * ../gnw-chainloader/retro-go-sd/Core/Src/gw_flash.c for the real
 * driver's command set and jedec_map[] this was cross-checked
 * against. Picked to match EXTFLASH_SIZE's existing 64M placeholder
 * (gnw_h7b0_soc.h) exactly, so SFDP-reported density and the actual
 * backing region size agree. PP/SE/BE32K/BE64K/CE (0x3E/0x21/0x5C/
 * 0xDC/0x60) and indirect READ (0xEC) now read/modify the same
 * memory-mapped XIP RAM region at EXTFLASH_BASE_ADDRESS the CPU sees
 * (wired up by gnw_h7b0_ospi_set_backing(), called from
 * gnw_h7b0_soc.c) -- so littlefs's block device ops in retro-go's
 * gw_littlefs.c (prog/erase via indirect OSPI_Program()/
 * OSPI_EraseSync(), read via direct memcpy from the mapped region)
 * now actually persist across each other instead of writes being
 * silently dropped while reads always saw the `-device loader`-
 * populated initial content. Only the specific opcodes
 * cmds_quad_32b_mx[] uses (this device's synthetic identity) are
 * handled this way; any other program/erase/read opcode still
 * no-ops/returns zero, since no other identity is emulated.
 *
 * ST HAL's HAL_OSPI_Command() (used by both the no-data and
 * indirect-write paths in
 * sdk/stm32h7xx-hal-driver/Src/stm32h7xx_hal_ospi.c) writes the
 * command config registers (CCR/IR last) then polls SR's TCF bit, and
 * __HAL_OSPI_CLEAR_FLAG() writes FCR to W1C the corresponding SR bit.
 * HAL_OSPI_Receive()/HAL_OSPI_Transmit() (the indirect-read/write
 * data phase) trigger the actual transfer differently: they re-write
 * AR (if the command has an address phase, e.g. an SFDP read) or IR
 * (if not) *after* HAL_OSPI_Command() has already configured -- but
 * not started -- the transaction, then poll SR's FT bit per byte
 * around each DR access, then TC once done. Without TCF/FTF also
 * being set on an AR write, that second trigger path hangs forever
 * (found via a gnw-chainloader boot hanging in OSPI_GetFlashSizeSfdp()
 * -- a genuine silent infinite loop, not a BusFault, since AR is a
 * real backed register and the read just never completes). Every
 * register other than SR/FCR/DR/CCR/IR/AR is a plain read-what-was-
 * written shadow with no side effects.
 *
 * Real IRQ line (OCTOSPI1_IRQn/OCTOSPI2_IRQn), gated by CR's TEIE/
 * TCIE/FTIE/SMIE/TOIE bits against SR -- found via 2026-07-12
 * breakpoint-lockstep tracing: stock Zelda firmware (unlike retro-go's
 * always-polling gw_flash.c driver) uses HAL_OSPI's interrupt-mode
 * completion path for at least one OSPI1 command during boot, busy-
 * waiting on a software completion flag an ISR is expected to set;
 * with no IRQ ever firing, that wait never completes and firmware
 * hits its own "spin forever on HAL error" trap. The line is
 * re-evaluated (raised or lowered) on every SR-affecting write (IR/AR
 * trigger, FCR clear) and on any CR write that changes which SR bits
 * are unmasked, matching real hardware's level-sensitive semantics.
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

#ifndef HW_MISC_GNW_H7B0_OSPI_H
#define HW_MISC_GNW_H7B0_OSPI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_OSPI "gnw-h7b0-ospi"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0OspiState, GNW_H7B0_OSPI)

/*
 * Register offsets/bits below are from
 * sdk/cmsis-device-h7/Include/stm32h7b0xx.h's OCTOSPI_TypeDef and
 * OCTOSPI_SR_x / OCTOSPI_FCR_x bit definitions, cross-checked against
 * STM32H7B0.svd.
 */
#define GNW_H7B0_OSPI_SIZE  0x1000

#define GNW_H7B0_OSPI_SR    0x20
#define OSPI_SR_TEF         (1U << 0)
#define OSPI_SR_TCF         (1U << 1)
#define OSPI_SR_FTF         (1U << 2)
#define OSPI_SR_SMF         (1U << 3)
#define OSPI_SR_TOF         (1U << 4)
#define OSPI_SR_BUSY        (1U << 5)

#define GNW_H7B0_OSPI_FCR   0x24
/* FCR bit positions mirror SR's TEF/TCF/FTF/SMF/TOF exactly. */

#define GNW_H7B0_OSPI_CR    0x00
#define OSPI_CR_TEIE        (1U << 16)
#define OSPI_CR_TCIE        (1U << 17)
#define OSPI_CR_FTIE        (1U << 18)
#define OSPI_CR_SMIE        (1U << 19)
#define OSPI_CR_TOIE        (1U << 20)
#define OSPI_CR_PMM         (1U << 23) /* polling match mode: 0=AND, 1=OR */
#define OSPI_CR_FMODE_SHIFT 28
#define OSPI_CR_FMODE_MASK  (0x3U << OSPI_CR_FMODE_SHIFT)
#define OSPI_FMODE_AUTOPOLL 2U

/* Automatic status-polling registers (RM0455 OCTOSPI_PSMKR/PSMAR). */
#define GNW_H7B0_OSPI_PSMKR 0x80
#define GNW_H7B0_OSPI_PSMAR 0x88

#define GNW_H7B0_OSPI_DLR   0x40
#define GNW_H7B0_OSPI_AR    0x48
#define GNW_H7B0_OSPI_DR    0x50
#define GNW_H7B0_OSPI_CCR   0x100
#define GNW_H7B0_OSPI_IR    0x110

/*
 * NOR flash command opcodes this device decodes -- see
 * ../gnw-chainloader/retro-go-sd/Core/Src/gw_flash.c's cmds_quad_32b_mx[]
 * (the config matched by the synthetic JEDEC ID below).
 */
#define OSPI_CMD_WRSR   0x01
#define OSPI_CMD_RDSR   0x05
#define OSPI_CMD_WREN   0x06
#define OSPI_CMD_RDCR   0x15
#define OSPI_CMD_RSTEN  0x66
#define OSPI_CMD_RST    0x99
#define OSPI_CMD_RDID   0x9F
#define OSPI_CMD_CE     0x60
#define OSPI_CMD_SFDP   0x5A

/*
 * PP/erase/READ opcodes for cmds_quad_32b_mx[] specifically (the
 * config table gw_flash.c's jedec_map[] selects for this device's
 * synthetic Macronix identity, which is >16MB and thus 32-bit-address
 * quad commands) -- see gnw_h7b0_ospi_set_backing() in gnw_h7b0_ospi.c
 * for how these now actually read/modify the memory-mapped XIP region
 * instead of being dropped.
 */
#define OSPI_CMD_PP     0x3E /* 4PP4B */
#define OSPI_CMD_SE     0x21 /* Sector Erase, 4KB */
#define OSPI_CMD_BE32K  0x5C /* Block Erase, 32KB */
#define OSPI_CMD_BE64K  0xDC /* Block Erase, 64KB */
#define OSPI_CMD_READ   0xEC /* 4READ4B */

/*
 * CCR's ADSIZE[1:0] field (bits 13:12, per STM32H7B0.svd / CMSIS
 * OCTOSPI_CCR_ADSIZE) -- 0=8-bit, 1=16-bit, 2=24-bit, 3=32-bit address
 * phase. AR itself always holds the full numeric address regardless of
 * ADSIZE (ADSIZE only controls how many bytes real hardware clocks out
 * over the wire), so nothing here changes how pending_addr is
 * interpreted -- this is purely so gnw_h7b0_ospi_write() can flag a
 * command whose firmware-configured ADSIZE doesn't match what this
 * device's synthetic identity is documented (in the header comment
 * above) to use, instead of silently trusting that assumption forever.
 */
#define OSPI_CCR_ADSIZE_SHIFT  12
#define OSPI_CCR_ADSIZE_MASK   (0x3U << OSPI_CCR_ADSIZE_SHIFT)
#define OSPI_ADSIZE_8B   0
#define OSPI_ADSIZE_16B  1
#define OSPI_ADSIZE_24B  2
#define OSPI_ADSIZE_32B  3

/* Status register bits (generic SPI-NOR: WIP/WEL/.../QE at bit 6). */
#define OSPI_FLASH_SR_WIP  (1U << 0)
#define OSPI_FLASH_SR_WEL  (1U << 1)
#define OSPI_FLASH_SR_QE   (1U << 6)

/*
 * Synthetic chip identity: Macronix MX25U51245G, 64MB/512Mbit, quad
 * mode always-on (QE hardwired set in RDSR so the MX/ISSI init path
 * in gw_flash.c's init_mx_issi() skips its WRSR round-trip). Matches
 * EXTFLASH_SIZE's existing 64M placeholder (gnw_h7b0_soc.h) so the
 * SFDP-reported density and the real backing region size agree.
 */
#define OSPI_FLASH_JEDEC_MANUF  0xC2
#define OSPI_FLASH_JEDEC_TYPE   0x25
#define OSPI_FLASH_JEDEC_DENS   0x3A
#define OSPI_FLASH_DEFAULT_SIZE (64 * 1024 * 1024)

#define OSPI_SFDP_TABLE_PTR  0x20

struct GnwH7B0OspiState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    uint32_t regs[GNW_H7B0_OSPI_SIZE / 4];
    /* See gnw_h7b0_stub_log.h -- one-shot LOG_UNIMP per plain-shadow offset. */
    bool logged_unimp[GNW_H7B0_OSPI_SIZE / 4];

    /* Indirect-mode command decode state, latched at trigger time. */
    uint8_t  pending_instr;
    uint32_t pending_addr;
    uint32_t dr_pos;
    bool     wel;

    uint64_t flash_size;

    /*
     * Host pointer into the memory-mapped XIP RAM region (extflash in
     * gnw_h7b0_soc.c), wired up post-realize via
     * gnw_h7b0_ospi_set_backing() so indirect-mode PP/erase/READ
     * actually persist to -- and read back from -- the same bytes the
     * CPU sees over XIP. NULL (all indirect writes dropped, reads
     * return 0xFF) until wired.
     */
    uint8_t *backing;
};

void gnw_h7b0_ospi_set_backing(GnwH7B0OspiState *s, void *backing);

#endif
