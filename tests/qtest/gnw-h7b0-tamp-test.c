/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest-single.h"

#define RCC_BASE        0x58024400U
#define RCC_BDCR        (RCC_BASE + 0x70U)
#define RCC_BDCR_BDRST  (1U << 16)
#define TAMP_BASE       0x58004400U
#define TAMP_CR2        (TAMP_BASE + 0x04U)
#define TAMP_BKP0R      (TAMP_BASE + 0x100U)
#define TAMP_BKP31R     (TAMP_BASE + 0x17cU)

static void test_backup_registers_survive_system_reset(void)
{
    QTestState *qts = qtest_init("-machine gnw-h7b0 -audiodev none,id=none");
    const uint32_t marker = 0x5a454c44; /* "ZELD" */

    /* Keep the existing initial-state convention, then plant a boot marker. */
    g_assert_cmphex(qtest_readl(qts, TAMP_BKP0R), ==, 0x32f2);
    qtest_writel(qts, TAMP_BKP31R, marker);
    qtest_writel(qts, TAMP_CR2, 0x5);

    qtest_system_reset(qts);

    g_assert_cmphex(qtest_readl(qts, TAMP_BKP31R), ==, marker);
    g_assert_cmphex(qtest_readl(qts, TAMP_BKP0R), ==, 0x32f2);
    g_assert_cmphex(qtest_readl(qts, TAMP_CR2), ==, 0);

    /* An explicit RCC backup-domain reset still clears all BKP registers. */
    qtest_writel(qts, RCC_BDCR,
                 qtest_readl(qts, RCC_BDCR) | RCC_BDCR_BDRST);
    g_assert_cmphex(qtest_readl(qts, TAMP_BKP31R), ==, 0);
    g_assert_cmphex(qtest_readl(qts, TAMP_BKP0R), ==, 0);
    qtest_writel(qts, RCC_BDCR,
                 qtest_readl(qts, RCC_BDCR) & ~RCC_BDCR_BDRST);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/gnw-h7b0/tamp/backup-reset",
                   test_backup_registers_survive_system_reset);
    return g_test_run();
}
