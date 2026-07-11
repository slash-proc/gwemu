/* Auto-generated from STM32H7B0.svd for SPI1 */
#ifndef GNW_H7B0_REGS_SPI1_H
#define GNW_H7B0_REGS_SPI1_H

#include <stdint.h>

#define GNW_H7B0_SPI1_CR1_OFFSET 0x0
#define GNW_H7B0_SPI1_CR1_RESET  0x00000000
#define GNW_H7B0_SPI1_CR1_WMASK  0x0000fd01

#define GNW_H7B0_SPI1_CR2_OFFSET 0x4
#define GNW_H7B0_SPI1_CR2_RESET  0x00000000
#define GNW_H7B0_SPI1_CR2_WMASK  0x0000ffff

#define GNW_H7B0_SPI1_CFG1_OFFSET 0x8
#define GNW_H7B0_SPI1_CFG1_RESET  0x00070007
#define GNW_H7B0_SPI1_CFG1_WMASK  0x705fdfff

#define GNW_H7B0_SPI1_CFG2_OFFSET 0xc
#define GNW_H7B0_SPI1_CFG2_RESET  0x00000000
#define GNW_H7B0_SPI1_CFG2_WMASK  0xf7fe80ff

#define GNW_H7B0_SPI1_IER_OFFSET 0x10
#define GNW_H7B0_SPI1_IER_RESET  0x00000000
#define GNW_H7B0_SPI1_IER_WMASK  0x000007f9

#define GNW_H7B0_SPI1_SR_OFFSET 0x14
#define GNW_H7B0_SPI1_SR_RESET  0x00001002
#define GNW_H7B0_SPI1_SR_WMASK  0x00000000

#define GNW_H7B0_SPI1_IFCR_OFFSET 0x18
#define GNW_H7B0_SPI1_IFCR_RESET  0x00000000
#define GNW_H7B0_SPI1_IFCR_WMASK  0x00000ff8

#define GNW_H7B0_SPI1_TXDR_OFFSET 0x20
#define GNW_H7B0_SPI1_TXDR_RESET  0x00000000
#define GNW_H7B0_SPI1_TXDR_WMASK  0xffffffff

#define GNW_H7B0_SPI1_RXDR_OFFSET 0x30
#define GNW_H7B0_SPI1_RXDR_RESET  0x00000000
#define GNW_H7B0_SPI1_RXDR_WMASK  0x00000000

#define GNW_H7B0_SPI1_CRCPOLY_OFFSET 0x40
#define GNW_H7B0_SPI1_CRCPOLY_RESET  0x00000107
#define GNW_H7B0_SPI1_CRCPOLY_WMASK  0xffffffff

#define GNW_H7B0_SPI1_TXCRC_OFFSET 0x44
#define GNW_H7B0_SPI1_TXCRC_RESET  0x00000000
#define GNW_H7B0_SPI1_TXCRC_WMASK  0xffffffff

#define GNW_H7B0_SPI1_RXCRC_OFFSET 0x48
#define GNW_H7B0_SPI1_RXCRC_RESET  0x00000000
#define GNW_H7B0_SPI1_RXCRC_WMASK  0xffffffff

#define GNW_H7B0_SPI1_UDRDR_OFFSET 0x4c
#define GNW_H7B0_SPI1_UDRDR_RESET  0x00000000
#define GNW_H7B0_SPI1_UDRDR_WMASK  0xffffffff

#define GNW_H7B0_SPI1_CGFR_OFFSET 0x50
#define GNW_H7B0_SPI1_CGFR_RESET  0x00000000
#define GNW_H7B0_SPI1_CGFR_WMASK  0x03ff7fbf

static inline uint32_t get_spi_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_SPI1_CR1_OFFSET:
            return GNW_H7B0_SPI1_CR1_WMASK;
        case GNW_H7B0_SPI1_CR2_OFFSET:
            return GNW_H7B0_SPI1_CR2_WMASK;
        case GNW_H7B0_SPI1_CFG1_OFFSET:
            return GNW_H7B0_SPI1_CFG1_WMASK;
        case GNW_H7B0_SPI1_CFG2_OFFSET:
            return GNW_H7B0_SPI1_CFG2_WMASK;
        case GNW_H7B0_SPI1_IER_OFFSET:
            return GNW_H7B0_SPI1_IER_WMASK;
        case GNW_H7B0_SPI1_SR_OFFSET:
            return GNW_H7B0_SPI1_SR_WMASK;
        case GNW_H7B0_SPI1_IFCR_OFFSET:
            return GNW_H7B0_SPI1_IFCR_WMASK;
        case GNW_H7B0_SPI1_TXDR_OFFSET:
            return GNW_H7B0_SPI1_TXDR_WMASK;
        case GNW_H7B0_SPI1_RXDR_OFFSET:
            return GNW_H7B0_SPI1_RXDR_WMASK;
        case GNW_H7B0_SPI1_CRCPOLY_OFFSET:
            return GNW_H7B0_SPI1_CRCPOLY_WMASK;
        case GNW_H7B0_SPI1_TXCRC_OFFSET:
            return GNW_H7B0_SPI1_TXCRC_WMASK;
        case GNW_H7B0_SPI1_RXCRC_OFFSET:
            return GNW_H7B0_SPI1_RXCRC_WMASK;
        case GNW_H7B0_SPI1_UDRDR_OFFSET:
            return GNW_H7B0_SPI1_UDRDR_WMASK;
        case GNW_H7B0_SPI1_CGFR_OFFSET:
            return GNW_H7B0_SPI1_CGFR_WMASK;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_spi_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_SPI1_CR1_OFFSET:
            return GNW_H7B0_SPI1_CR1_RESET;
        case GNW_H7B0_SPI1_CR2_OFFSET:
            return GNW_H7B0_SPI1_CR2_RESET;
        case GNW_H7B0_SPI1_CFG1_OFFSET:
            return GNW_H7B0_SPI1_CFG1_RESET;
        case GNW_H7B0_SPI1_CFG2_OFFSET:
            return GNW_H7B0_SPI1_CFG2_RESET;
        case GNW_H7B0_SPI1_IER_OFFSET:
            return GNW_H7B0_SPI1_IER_RESET;
        case GNW_H7B0_SPI1_SR_OFFSET:
            return GNW_H7B0_SPI1_SR_RESET;
        case GNW_H7B0_SPI1_IFCR_OFFSET:
            return GNW_H7B0_SPI1_IFCR_RESET;
        case GNW_H7B0_SPI1_TXDR_OFFSET:
            return GNW_H7B0_SPI1_TXDR_RESET;
        case GNW_H7B0_SPI1_RXDR_OFFSET:
            return GNW_H7B0_SPI1_RXDR_RESET;
        case GNW_H7B0_SPI1_CRCPOLY_OFFSET:
            return GNW_H7B0_SPI1_CRCPOLY_RESET;
        case GNW_H7B0_SPI1_TXCRC_OFFSET:
            return GNW_H7B0_SPI1_TXCRC_RESET;
        case GNW_H7B0_SPI1_RXCRC_OFFSET:
            return GNW_H7B0_SPI1_RXCRC_RESET;
        case GNW_H7B0_SPI1_UDRDR_OFFSET:
            return GNW_H7B0_SPI1_UDRDR_RESET;
        case GNW_H7B0_SPI1_CGFR_OFFSET:
            return GNW_H7B0_SPI1_CGFR_RESET;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_SPI1_H */
