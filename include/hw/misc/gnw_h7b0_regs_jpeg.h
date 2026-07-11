/* Auto-generated from STM32H7B0.svd for JPEG */
#ifndef GNW_H7B0_REGS_JPEG_H
#define GNW_H7B0_REGS_JPEG_H

#include <stdint.h>

#define GNW_H7B0_JPEG_CONFR0_OFFSET 0x0
#define GNW_H7B0_JPEG_CONFR0_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFR0_WMASK  0x00000001
#define JPEG_CONFR0_START            (1U << 0)

#define GNW_H7B0_JPEG_CONFR1_OFFSET 0x4
#define GNW_H7B0_JPEG_CONFR1_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFR1_WMASK  0xffff01fb
/* CONFR1 bit layout, per STM32H7B0.svd's JPEG_CONFR1 fields. */
#define JPEG_CONFR1_NF_SHIFT         0
#define JPEG_CONFR1_NF_MASK          (0x3U << JPEG_CONFR1_NF_SHIFT)  /* Number of color components - 1 */
#define JPEG_CONFR1_DE               (1U << 3)                       /* Decoding Enable */
#define JPEG_CONFR1_COLORSPACE_SHIFT 4
#define JPEG_CONFR1_COLORSPACE_MASK  (0x3U << JPEG_CONFR1_COLORSPACE_SHIFT)
#define JPEG_CONFR1_NS_SHIFT         6
#define JPEG_CONFR1_NS_MASK          (0x3U << JPEG_CONFR1_NS_SHIFT)  /* Number of components for Scan - 1 */
#define JPEG_CONFR1_HDR              (1U << 8)                       /* Header Processing enable */
#define JPEG_CONFR1_YSIZE_SHIFT      16
#define JPEG_CONFR1_YSIZE_MASK       (0xffffU << JPEG_CONFR1_YSIZE_SHIFT)

#define GNW_H7B0_JPEG_CONFR2_OFFSET 0x8
#define GNW_H7B0_JPEG_CONFR2_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFR2_WMASK  0x03ffffff
#define JPEG_CONFR2_NMCU_SHIFT       0
#define JPEG_CONFR2_NMCU_MASK        (0x3ffffffU << JPEG_CONFR2_NMCU_SHIFT)

#define GNW_H7B0_JPEG_CONFR3_OFFSET 0xc
#define GNW_H7B0_JPEG_CONFR3_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFR3_WMASK  0xffff0000
/* CONFR3 bit layout, per STM32H7B0.svd's JPEG_CONFR3 fields. */
#define JPEG_CONFR3_XSIZE_SHIFT      16
#define JPEG_CONFR3_XSIZE_MASK       (0xffffU << JPEG_CONFR3_XSIZE_SHIFT)

/*
 * CONFRN1-CONFRN4 ("JPEG codec configuration registers 4-7" in the SVD,
 * named CONFRN1..4 here) are the four per-color-component configuration
 * registers. All four share the identical HD/HA/QT/NB/VSF/HSF bit layout,
 * per STM32H7B0.svd's JPEG_CONFRN1 (and identical CONFRN2/3/4) fields.
 */
#define JPEG_CONFRN_HD               (1U << 0)                       /* Huffman DC table select */
#define JPEG_CONFRN_HA               (1U << 1)                       /* Huffman AC table select */
#define JPEG_CONFRN_QT_SHIFT         2
#define JPEG_CONFRN_QT_MASK          (0x3U << JPEG_CONFRN_QT_SHIFT)   /* Quantization table select */
#define JPEG_CONFRN_NB_SHIFT         4
#define JPEG_CONFRN_NB_MASK          (0xfU << JPEG_CONFRN_NB_SHIFT)   /* Number of data units - 1 in MCU */
#define JPEG_CONFRN_VSF_SHIFT        8
#define JPEG_CONFRN_VSF_MASK         (0xfU << JPEG_CONFRN_VSF_SHIFT)  /* Vertical sampling factor */
#define JPEG_CONFRN_HSF_SHIFT        12
#define JPEG_CONFRN_HSF_MASK         (0xfU << JPEG_CONFRN_HSF_SHIFT)  /* Horizontal sampling factor */

#define GNW_H7B0_JPEG_CONFRN1_OFFSET 0x10
#define GNW_H7B0_JPEG_CONFRN1_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFRN1_WMASK  0x0000ffff

#define GNW_H7B0_JPEG_CONFRN2_OFFSET 0x14
#define GNW_H7B0_JPEG_CONFRN2_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFRN2_WMASK  0x0000ffff

#define GNW_H7B0_JPEG_CONFRN3_OFFSET 0x18
#define GNW_H7B0_JPEG_CONFRN3_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFRN3_WMASK  0x0000ffff

#define GNW_H7B0_JPEG_CONFRN4_OFFSET 0x1c
#define GNW_H7B0_JPEG_CONFRN4_RESET  0x00000000
#define GNW_H7B0_JPEG_CONFRN4_WMASK  0x0000ffff

#define GNW_H7B0_JPEG_CR_OFFSET 0x30
#define GNW_H7B0_JPEG_CR_RESET  0x00000000
#define GNW_H7B0_JPEG_CR_WMASK  0x0000787f
/* CR bit layout, per STM32H7B0.svd's JPEG_CR fields. */
#define JPEG_CR_JCEN                 (1U << 0)   /* JPEG Core Enable */
#define JPEG_CR_IFTIE                (1U << 1)   /* Input FIFO Threshold Interrupt Enable */
#define JPEG_CR_IFNFIE               (1U << 2)   /* Input FIFO Not Full Interrupt Enable */
#define JPEG_CR_OFTIE                (1U << 3)   /* Output FIFO Threshold Interrupt Enable */
#define JPEG_CR_OFNEIE               (1U << 4)   /* Output FIFO Not Empty Interrupt Enable */
#define JPEG_CR_EOCIE                (1U << 5)   /* End of Conversion Interrupt Enable */
#define JPEG_CR_HPDIE                (1U << 6)   /* Header Parsing Done Interrupt Enable */
#define JPEG_CR_IDMAEN               (1U << 11)  /* Input DMA Enable */
#define JPEG_CR_ODMAEN               (1U << 12)  /* Output DMA Enable */
#define JPEG_CR_IFF                  (1U << 13)  /* Input FIFO Flush -- pulse, always reads back 0 */
#define JPEG_CR_OFF                  (1U << 14)  /* Output FIFO Flush -- pulse, always reads back 0 */
#define JPEG_CR_FLUSH_PULSE_MASK     (JPEG_CR_IFF | JPEG_CR_OFF)

#define GNW_H7B0_JPEG_SR_OFFSET 0x34
#define GNW_H7B0_JPEG_SR_RESET  0x00000006
#define GNW_H7B0_JPEG_SR_WMASK  0x00000000
/* SR bit positions, per sdk/cmsis-device-h7/Include/stm32h7b0xx.h's JPEG_SR_*. */
#define JPEG_SR_IFTF  (1U << 1)
#define JPEG_SR_IFNFF (1U << 2)
#define JPEG_SR_OFTF  (1U << 3)
#define JPEG_SR_OFNEF (1U << 4)
#define JPEG_SR_EOCF  (1U << 5)
#define JPEG_SR_COF   (1U << 7)

#define GNW_H7B0_JPEG_CFR_OFFSET 0x38
#define GNW_H7B0_JPEG_CFR_RESET  0x00000000
/*
 * 0x60 = CEOCF (bit 5) | CHPDF (bit 6) -- the only two bits the SVD defines
 * as real fields of JPEG_CFR. This is already the full real hardware
 * "clear flag" surface, not a subset: real firmware's __HAL_JPEG_CLEAR_FLAG()
 * macro (sdk/stm32h7xx-hal-driver/Inc/stm32h7xx_hal_jpeg.h) always ANDs
 * whatever flag value it's given -- including its own JPEG_FLAG_ALL
 * (0x000000FE, covering IFTF/IFNFF/OFTF/OFNEF/EOCF/HPDF/COF) -- with
 * (JPEG_FLAG_EOCF | JPEG_FLAG_HPDF) == 0x60 before writing CFR. So every
 * real "clear all flags" call in stm32h7xx_hal_jpeg.c still only ever
 * writes 0x60 to CFR; IFTF/IFNFF/OFTF/OFNEF/COF are FIFO/operation status
 * bits that track live state and were never clearable via CFR on real
 * silicon. Widening this mask to 0xFE would be a fidelity regression, not
 * an improvement -- kept at 0x60 deliberately.
 */
#define GNW_H7B0_JPEG_CFR_WMASK  0x00000060

#define GNW_H7B0_JPEG_DIR_OFFSET 0x40
#define GNW_H7B0_JPEG_DIR_RESET  0x00000000
#define GNW_H7B0_JPEG_DIR_WMASK  0xffffffff

#define GNW_H7B0_JPEG_DOR_OFFSET 0x44
#define GNW_H7B0_JPEG_DOR_RESET  0x00000000
#define GNW_H7B0_JPEG_DOR_WMASK  0x00000000

static inline uint32_t get_jpeg_write_mask(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_JPEG_CONFR0_OFFSET:
            return GNW_H7B0_JPEG_CONFR0_WMASK;
        case GNW_H7B0_JPEG_CONFR1_OFFSET:
            return GNW_H7B0_JPEG_CONFR1_WMASK;
        case GNW_H7B0_JPEG_CONFR2_OFFSET:
            return GNW_H7B0_JPEG_CONFR2_WMASK;
        case GNW_H7B0_JPEG_CONFR3_OFFSET:
            return GNW_H7B0_JPEG_CONFR3_WMASK;
        case GNW_H7B0_JPEG_CONFRN1_OFFSET:
            return GNW_H7B0_JPEG_CONFRN1_WMASK;
        case GNW_H7B0_JPEG_CONFRN2_OFFSET:
            return GNW_H7B0_JPEG_CONFRN2_WMASK;
        case GNW_H7B0_JPEG_CONFRN3_OFFSET:
            return GNW_H7B0_JPEG_CONFRN3_WMASK;
        case GNW_H7B0_JPEG_CONFRN4_OFFSET:
            return GNW_H7B0_JPEG_CONFRN4_WMASK;
        case GNW_H7B0_JPEG_CR_OFFSET:
            return GNW_H7B0_JPEG_CR_WMASK;
        case GNW_H7B0_JPEG_SR_OFFSET:
            return GNW_H7B0_JPEG_SR_WMASK;
        case GNW_H7B0_JPEG_CFR_OFFSET:
            return GNW_H7B0_JPEG_CFR_WMASK;
        case GNW_H7B0_JPEG_DIR_OFFSET:
            return GNW_H7B0_JPEG_DIR_WMASK;
        case GNW_H7B0_JPEG_DOR_OFFSET:
            return GNW_H7B0_JPEG_DOR_WMASK;
        default:
            return 0x00000000; /* Read-only or unmapped by default */
    }
}

static inline uint32_t get_jpeg_reset_value(uint32_t offset) {
    switch (offset) {
        case GNW_H7B0_JPEG_CONFR0_OFFSET:
            return GNW_H7B0_JPEG_CONFR0_RESET;
        case GNW_H7B0_JPEG_CONFR1_OFFSET:
            return GNW_H7B0_JPEG_CONFR1_RESET;
        case GNW_H7B0_JPEG_CONFR2_OFFSET:
            return GNW_H7B0_JPEG_CONFR2_RESET;
        case GNW_H7B0_JPEG_CONFR3_OFFSET:
            return GNW_H7B0_JPEG_CONFR3_RESET;
        case GNW_H7B0_JPEG_CONFRN1_OFFSET:
            return GNW_H7B0_JPEG_CONFRN1_RESET;
        case GNW_H7B0_JPEG_CONFRN2_OFFSET:
            return GNW_H7B0_JPEG_CONFRN2_RESET;
        case GNW_H7B0_JPEG_CONFRN3_OFFSET:
            return GNW_H7B0_JPEG_CONFRN3_RESET;
        case GNW_H7B0_JPEG_CONFRN4_OFFSET:
            return GNW_H7B0_JPEG_CONFRN4_RESET;
        case GNW_H7B0_JPEG_CR_OFFSET:
            return GNW_H7B0_JPEG_CR_RESET;
        case GNW_H7B0_JPEG_SR_OFFSET:
            return GNW_H7B0_JPEG_SR_RESET;
        case GNW_H7B0_JPEG_CFR_OFFSET:
            return GNW_H7B0_JPEG_CFR_RESET;
        case GNW_H7B0_JPEG_DIR_OFFSET:
            return GNW_H7B0_JPEG_DIR_RESET;
        case GNW_H7B0_JPEG_DOR_OFFSET:
            return GNW_H7B0_JPEG_DOR_RESET;
        default:
            return 0x00000000;
    }
}

#endif /* GNW_H7B0_REGS_JPEG_H */
