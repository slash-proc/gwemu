/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/gnw_h7b0_regs_rcc.h"
#include "libqtest-single.h"

#define MDMA_BASE       0x52000000U
#define RCC_BASE        0x58024400U
#define RCC_RSR         (RCC_BASE + GNW_H7B0_RCC_RSR_OFFSET)
#define RCC_AHB3ENR     (RCC_BASE + GNW_H7B0_RCC_AHB3ENR_OFFSET)
#define RCC_AHB1ENR     (RCC_BASE + GNW_H7B0_RCC_AHB1ENR_OFFSET)
#define CH15_BASE       (MDMA_BASE + 0x40U + 15U * 0x40U)
#define CISR            (CH15_BASE + 0x00U)
#define CIFCR           (CH15_BASE + 0x04U)
#define CCR             (CH15_BASE + 0x0cU)
#define CTCR            (CH15_BASE + 0x10U)
#define CBNDTR          (CH15_BASE + 0x14U)
#define CSAR            (CH15_BASE + 0x18U)
#define CDAR            (CH15_BASE + 0x1cU)

#define SRC             0x240d0000U
#define DST             0x240d8000U
#define LENGTH          0x8000U

#define CCR_EN          (1U << 0)
#define CCR_SWRQ        (1U << 16)
#define CTCR_SINC_WORD  ((2U << 0) | (2U << 8))
#define CTCR_DINC_WORD  ((2U << 2) | (2U << 10))
#define CTCR_SSIZE_WORD (2U << 4)
#define CTCR_DSIZE_WORD (2U << 6)
#define CTCR_TLEN_128   (127U << 18)
#define CTCR_FULL       (3U << 28)
#define CTCR_SWRM       (1U << 30)
#define CTCR_BWM        (1U << 31)
#define CISR_ALL_DONE   0x1eU

static void start(void)
{
    qtest_start("-machine gnw-h7b0 -audiodev none,id=none");
}

static void configure_and_request(bool swrm, bool bwm)
{
    uint32_t ctcr = CTCR_SINC_WORD | CTCR_DINC_WORD |
                    CTCR_SSIZE_WORD | CTCR_DSIZE_WORD |
                    CTCR_TLEN_128 | CTCR_FULL;

    if (swrm) {
        ctcr |= CTCR_SWRM;
    }
    if (bwm) {
        ctcr |= CTCR_BWM;
    }
    writel(CIFCR, 0x1f);
    writel(CCR, 0);
    writel(CTCR, ctcr);
    writel(CBNDTR, LENGTH);
    writel(CSAR, SRC);
    writel(CDAR, DST);
    writel(CCR, CCR_EN | CCR_SWRQ);
}

static void test_full_transfer(bool bwm)
{
    g_autofree uint32_t *source = g_new(uint32_t, LENGTH / 4);
    g_autofree uint32_t *destination = g_new0(uint32_t, LENGTH / 4);

    start();
    for (unsigned i = 0; i < LENGTH / 4; i++) {
        source[i] = 0x51a70000U ^ i;
        destination[i] = 0xa55a0000U ^ i;
    }
    qtest_memwrite(global_qtest, SRC, source, LENGTH);
    qtest_memwrite(global_qtest, DST, destination, LENGTH);

    configure_and_request(true, bwm);
    memset(destination, 0, LENGTH);
    qtest_memread(global_qtest, DST, destination, LENGTH);

    g_assert_cmpmem(destination, LENGTH, source, LENGTH);
    g_assert_cmphex(readl(CBNDTR) & 0x1ffffU, ==, 0);
    g_assert_cmphex(readl(CISR) & 0x1fU, ==, CISR_ALL_DONE);
    g_assert_cmphex(readl(CCR) & (CCR_EN | CCR_SWRQ), ==, 0);
    qtest_end();
}

static void test_full_bwm_clear(void)
{
    test_full_transfer(false);
}

static void test_full_bwm_set(void)
{
    test_full_transfer(true);
}

static void test_swrq_requires_swrm(void)
{
    g_autofree uint8_t *source = g_malloc(LENGTH);
    g_autofree uint8_t *destination = g_malloc(LENGTH);
    g_autofree uint8_t *actual = g_malloc(LENGTH);

    start();
    memset(source, 0x51, LENGTH);
    memset(destination, 0xa5, LENGTH);
    qtest_memwrite(global_qtest, SRC, source, LENGTH);
    qtest_memwrite(global_qtest, DST, destination, LENGTH);

    configure_and_request(false, true);
    qtest_memread(global_qtest, DST, actual, LENGTH);

    g_assert_cmpmem(actual, LENGTH, destination, LENGTH);
    g_assert_cmphex(readl(CBNDTR) & 0x1ffffU, ==, LENGTH);
    g_assert_cmphex(readl(CISR) & 0x1fU, ==, 0);
    qtest_end();
}

static void test_rcc_clock_enable_offsets(void)
{
    static const struct {
        uint32_t offset;
        uint32_t mask;
        uint32_t rm_offset;
    } clock_regs[] = {
        { GNW_H7B0_RCC_AHB3ENR_OFFSET, GNW_H7B0_RCC_AHB3ENR_WMASK, 0x0d4 },
        { GNW_H7B0_RCC_AHB1ENR_OFFSET, GNW_H7B0_RCC_AHB1ENR_WMASK, 0x0d8 },
        { GNW_H7B0_RCC_AHB2ENR_OFFSET, GNW_H7B0_RCC_AHB2ENR_WMASK, 0x0dc },
        { GNW_H7B0_RCC_AHB4ENR_OFFSET, GNW_H7B0_RCC_AHB4ENR_WMASK, 0x0e0 },
        { GNW_H7B0_RCC_APB3ENR_OFFSET, GNW_H7B0_RCC_APB3ENR_WMASK, 0x0e4 },
        { GNW_H7B0_RCC_APB1LENR_OFFSET, GNW_H7B0_RCC_APB1LENR_WMASK, 0x0e8 },
        { GNW_H7B0_RCC_APB1HENR_OFFSET, GNW_H7B0_RCC_APB1HENR_WMASK, 0x0ec },
        { GNW_H7B0_RCC_APB2ENR_OFFSET, GNW_H7B0_RCC_APB2ENR_WMASK, 0x0f0 },
        { GNW_H7B0_RCC_APB4ENR_OFFSET, GNW_H7B0_RCC_APB4ENR_WMASK, 0x0f4 },
        { GNW_H7B0_RCC_AHB3LPENR_OFFSET, GNW_H7B0_RCC_AHB3LPENR_WMASK, 0x0fc },
        { GNW_H7B0_RCC_AHB1LPENR_OFFSET, GNW_H7B0_RCC_AHB1LPENR_WMASK, 0x100 },
        { GNW_H7B0_RCC_AHB2LPENR_OFFSET, GNW_H7B0_RCC_AHB2LPENR_WMASK, 0x104 },
        { GNW_H7B0_RCC_AHB4LPENR_OFFSET, GNW_H7B0_RCC_AHB4LPENR_WMASK, 0x108 },
        { GNW_H7B0_RCC_APB3LPENR_OFFSET, GNW_H7B0_RCC_APB3LPENR_WMASK, 0x10c },
        { GNW_H7B0_RCC_APB1LLPENR_OFFSET, GNW_H7B0_RCC_APB1LLPENR_WMASK, 0x110 },
        { GNW_H7B0_RCC_APB1HLPENR_OFFSET, GNW_H7B0_RCC_APB1HLPENR_WMASK, 0x114 },
        { GNW_H7B0_RCC_APB2LPENR_OFFSET, GNW_H7B0_RCC_APB2LPENR_WMASK, 0x118 },
        { GNW_H7B0_RCC_APB4LPENR_OFFSET, GNW_H7B0_RCC_APB4LPENR_WMASK, 0x11c },
    };

    start();

    /* RM0455 §8.7.37–8.7.56 and STM32H7B0 CMSIS place RSR at 0xd0,
     * AHB3ENR at 0xd4 (MDMAEN bit 0), then the enable and low-power gate
     * registers at the offsets listed below. */
    g_assert_cmphex(readl(RCC_RSR), ==, 0x01680000U);
    g_assert_cmphex(readl(RCC_AHB3ENR) & 1U, ==, 0U);
    writel(RCC_AHB3ENR, 1U);
    g_assert_cmphex(readl(RCC_AHB3ENR) & 1U, ==, 1U);
    /* The prior generated SVD offsets were displaced by 0x60. Those
     * addresses must not act as aliases for the real RCC gate registers. */
    g_assert_cmphex(readl(RCC_BASE + 0x130U), ==, 0U);
    g_assert_cmphex(readl(RCC_BASE + 0x134U), ==, 0U);

    writel(RCC_AHB1ENR, 1U);
    g_assert_cmphex(readl(RCC_AHB1ENR), ==, 1U);

    for (size_t i = 0; i < ARRAY_SIZE(clock_regs); i++) {
        uint32_t address = RCC_BASE + clock_regs[i].offset;

        g_assert_cmphex(clock_regs[i].offset, ==, clock_regs[i].rm_offset);
        writel(address, clock_regs[i].mask);
        g_assert_cmphex(readl(address), ==, clock_regs[i].mask);
    }
    qtest_end();
}

static void test_byte_offset_is_not_word_increment(void)
{
    g_autofree uint32_t *source = g_new(uint32_t, LENGTH / 4);
    g_autofree uint32_t *destination = g_new(uint32_t, LENGTH / 4);
    uint32_t verified = 0;
    uint32_t byte_offset_ctcr = (2U << 0) | (2U << 2) |
                                CTCR_SSIZE_WORD | CTCR_DSIZE_WORD |
                                CTCR_TLEN_128 | CTCR_FULL |
                                CTCR_SWRM | CTCR_BWM;

    start();
    for (unsigned i = 0; i < LENGTH / 4; i++) {
        source[i] = 0x51a70000U ^ i;
        destination[i] = 0xa55a0000U ^ i;
    }
    qtest_memwrite(global_qtest, SRC, source, LENGTH);
    qtest_memwrite(global_qtest, DST, destination, LENGTH);
    writel(CTCR, byte_offset_ctcr);
    writel(CBNDTR, LENGTH);
    writel(CSAR, SRC);
    writel(CDAR, DST);
    writel(CCR, CCR_EN | CCR_SWRQ);
    qtest_memread(global_qtest, DST, destination, LENGTH);

    for (unsigned i = 0; i < LENGTH / 4; i++) {
        verified += destination[i] == source[i];
    }
    g_assert_cmpuint(verified, ==, 2048);
    g_assert_cmphex(readl(CBNDTR) & 0x1ffffU, ==, 0);
    qtest_end();
}

int main(int argc, char **argv)
{
    g_autofree char *scratch = g_build_filename(GNW_TIMING_PROJECT_ROOT,
                                                ".timing-work", "tmp", NULL);

    g_assert_cmpint(g_mkdir_with_parents(scratch, 0700), ==, 0);
    g_assert_true(g_setenv("TMPDIR", scratch, true));
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/gnw-h7b0/mdma/full-bwm-clear", test_full_bwm_clear);
    qtest_add_func("/gnw-h7b0/mdma/full-bwm-set", test_full_bwm_set);
    qtest_add_func("/gnw-h7b0/mdma/swrq-requires-swrm",
                   test_swrq_requires_swrm);
    qtest_add_func("/gnw-h7b0/mdma/rcc-clock-enable-offsets",
                   test_rcc_clock_enable_offsets);
    qtest_add_func("/gnw-h7b0/mdma/byte-offset-not-word-increment",
                   test_byte_offset_is_not_word_increment);
    return g_test_run();
}
