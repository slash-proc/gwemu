/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest-single.h"
#include "migration/migration-qmp.h"

#define RCC_BASE        0x58024400U
#define RCC_CR          (RCC_BASE + 0x00U)
#define RCC_CFGR        (RCC_BASE + 0x10U)
#define DWT_CTRL        0xe0001000U
#define DWT_CYCCNT      0xe0001004U
#define DEMCR           0xe000edfcU
#define DEMCR_TRCENA    (1U << 24)
#define RCC_CR_CSION    (1U << 7)
#define RCC_CFGR_SW_CSI 1U
#define RCC_CFGR_SWS_SHIFT 3

static QTestState *start(bool incoming)
{
    return qtest_initf("-machine gnw-h7b0 -audiodev none,id=none %s",
                       incoming ? "-incoming defer" : "");
}

static void enable_dwt(QTestState *qts)
{
    qtest_writel(qts, DEMCR, qtest_readl(qts, DEMCR) | DEMCR_TRCENA);
    qtest_writel(qts, DWT_CTRL, 1);
}

static void test_clock_change_and_reset(void)
{
    QTestState *qts = start(false);
    uint32_t before, after;

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    enable_dwt(qts);
    qtest_clock_step(qts, 1000000);
    before = qtest_readl(qts, DWT_CYCCNT);
    g_assert_cmpuint(before, >, 0);

    /* Switch SYSCLK from HSI to CSI after enabling the second source. */
    qtest_writel(qts, RCC_CR, qtest_readl(qts, RCC_CR) | RCC_CR_CSION);
    qtest_writel(qts, RCC_CFGR, RCC_CFGR_SW_CSI);
    g_assert_cmpuint((qtest_readl(qts, RCC_CFGR) >> RCC_CFGR_SWS_SHIFT) & 7,
                     ==, RCC_CFGR_SW_CSI);
    after = qtest_readl(qts, DWT_CYCCNT);
    g_assert_cmpuint(after, >=, before);
    qtest_clock_step(qts, 1000000);
    after = qtest_readl(qts, DWT_CYCCNT);
    g_assert_cmpuint(after, >, before);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, DWT_CYCCNT), ==, 0);
    qtest_quit(qts);
}

static char *migration_uri(void)
{
    g_autofree char *path = g_strdup_printf("gnw-dwt-%u.sock", (unsigned)getpid());
    g_unlink(path);
    return g_strdup_printf("unix:%s", path);
}

static void test_migration_roundtrip(void)
{
    g_autofree char *uri = migration_uri();
    QTestState *src = start(false);
    QTestState *dst = start(true);
    uint32_t saved;

    qtest_qmp_assert_success(src, "{'execute':'cont'}");
    enable_dwt(src);
    qtest_clock_step(src, 1000000);
    saved = qtest_readl(src, DWT_CYCCNT);
    g_assert_cmpuint(saved, >, 0);

    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    wait_for_migration_complete(dst);
    qtest_qmp_assert_success(dst, "{'execute':'cont'}");
    g_assert_cmphex(qtest_readl(dst, DWT_CYCCNT), ==, saved);
    qtest_clock_step(dst, 1000000);
    g_assert_cmpuint(qtest_readl(dst, DWT_CYCCNT), >, saved);

    qtest_quit(dst);
    qtest_quit(src);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/gnw-h7b0/dwt/clock-change-reset",
                   test_clock_change_and_reset);
    qtest_add_func("/gnw-h7b0/dwt/migration-roundtrip",
                   test_migration_roundtrip);
    return g_test_run();
}
