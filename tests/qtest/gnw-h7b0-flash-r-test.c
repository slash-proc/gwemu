/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "migration/migration-qmp.h"

#define FLASH_R_BASE 0x52002000U
#define CR1          (FLASH_R_BASE + 0x00cU)
#define SR1          (FLASH_R_BASE + 0x010U)
#define CR2          (FLASH_R_BASE + 0x10cU)
#define SR2          (FLASH_R_BASE + 0x110U)
#define BANK1         0x08000000U
#define BANK2         0x08100000U
#define CR_SER        (1U << 2)
#define CR_START      (1U << 5)
#define SR_QW         (1U << 2)
#define SECTOR_SIZE   8192U
#define SECTOR_NS     11000

static QTestState *start(const char *extra)
{
    return qtest_initf("-machine gnw-h7b0 "
                       "-audiodev none,id=none %s", extra);
}

static QTestState *start_tcg(const char *extra)
{
    return qtest_initf("-machine gnw-h7b0 -accel tcg "
                       "-audiodev none,id=none %s", extra);
}

static void fill(QTestState *qts, uint64_t address, uint8_t value)
{
    qtest_memset(qts, address, value, SECTOR_SIZE);
}

static uint8_t first(QTestState *qts, uint64_t address)
{
    return qtest_readb(qts, address);
}

static void request_sector(QTestState *qts, uint64_t cr)
{
    qtest_writel(qts, cr, CR_SER | CR_START);
}

static void test_default_sync(void)
{
    QTestState *qts = start("");
    fill(qts, BANK1, 0);
    request_sector(qts, CR1);
    g_assert_cmphex(first(qts, BANK1), ==, 0xff);
    g_assert_cmphex(qtest_readl(qts, SR1) & SR_QW, ==, 0);
    qtest_quit(qts);
}

static void assert_single_gate_sync(const char *extra)
{
    QTestState *qts = start(extra);
    fill(qts, BANK1, 0);
    request_sector(qts, CR1);
    g_assert_cmphex(first(qts, BANK1), ==, 0xff);
    g_assert_cmphex(qtest_readl(qts, SR1) & SR_QW, ==, 0);
    qtest_quit(qts);
}

static void test_timing_gate_only_sync(void)
{
    QTestState *qts = start_tcg("-icount shift=0,sleep=off "
                                "-global gnw-h7b0-soc.timing-mode=on");
    fill(qts, BANK1, 0);
    request_sector(qts, CR1);
    g_assert_cmphex(first(qts, BANK1), ==, 0xff);
    g_assert_cmphex(qtest_readl(qts, SR1) & SR_QW, ==, 0);
    qtest_quit(qts);
}

static void test_profile_gate_only_sync(void)
{
    assert_single_gate_sync(
        "-global gnw-h7b0-flash_r.x-test-erase-timing=on");
}

static QTestState *start_timed(void)
{
    /* Keep QTest in control of QEMU_CLOCK_VIRTUAL.  The SoC-wide timing
     * mode requires icount, whose virtual clock advances with guest
     * instructions rather than qtest_clock_step().  The flash device's
     * timing gate exercises the same erase timer without that SoC guard. */
    QTestState *qts = start("-global gnw-h7b0-flash_r.timing-mode=on "
                            "-global gnw-h7b0-flash_r.x-test-erase-timing=on");
    /* QEMU keeps QEMU_CLOCK_VIRTUAL disabled until the VM is continued. */
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    return qts;
}

static void test_pending_and_deadline(void)
{
    QTestState *qts = start_timed();
    int64_t before, after;
    fill(qts, BANK1, 0);
    request_sector(qts, CR1);
    g_assert_cmphex(qtest_readl(qts, SR1) & SR_QW, ==, SR_QW);
    g_assert_cmphex(first(qts, BANK1), ==, 0);
    before = qtest_clock_step(qts, SECTOR_NS - 1);
    g_assert_cmpint(before, ==, SECTOR_NS - 1);
    g_assert_cmphex(first(qts, BANK1), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SR1) & SR_QW, ==, SR_QW);
    after = qtest_clock_step(qts, 1);
    g_assert_cmpint(after, ==, SECTOR_NS);
    g_assert_cmphex(first(qts, BANK1), ==, 0xff);
    g_assert_cmphex(qtest_readl(qts, SR1) & SR_QW, ==, 0);
    qtest_quit(qts);
}

static void test_overlap_rejected(void)
{
    QTestState *qts = start_timed();
    fill(qts, BANK1, 0);
    fill(qts, BANK2, 0);
    request_sector(qts, CR1);
    request_sector(qts, CR2);
    g_assert_cmphex(qtest_readl(qts, SR1) & SR_QW, ==, SR_QW);
    g_assert_cmphex(qtest_readl(qts, SR2) & SR_QW, ==, 0);
    qtest_clock_step(qts, SECTOR_NS);
    g_assert_cmphex(first(qts, BANK1), ==, 0xff);
    g_assert_cmphex(first(qts, BANK2), ==, 0);
    qtest_quit(qts);
}

static char *migration_uri(const char *name)
{
    g_autofree char *path = g_strdup_printf(
        "gnw-flash-%s-%u.sock", name, (unsigned)getpid());

    g_unlink(path);
    return g_strdup_printf("unix:%s", path);
}

static QTestState *start_migration_peer(bool profile, bool incoming)
{
    return qtest_initf(
        "-machine gnw-h7b0 -audiodev none,id=none "
        "-global gnw-h7b0-flash_r.timing-mode=on %s %s",
        profile ?
        "-global gnw-h7b0-flash_r.x-test-erase-timing=on" : "",
        incoming ? "-incoming defer" : "");
}

static void finish_peer(QTestState *qts)
{
    qtest_quit(qts);
}

static void test_migration_preserves_remaining(void)
{
    g_autofree char *uri = migration_uri("success");
    QTestState *src = start_migration_peer(true, false);
    QTestState *dst = start_migration_peer(true, true);

    qtest_qmp_assert_success(src, "{'execute':'cont'}");
    fill(src, BANK1, 0);
    request_sector(src, CR1);
    qtest_clock_step(src, 4000);
    g_assert_cmphex(qtest_readl(src, SR1) & SR_QW, ==, SR_QW);

    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    wait_for_migration_complete(dst);

    /* The qtest accelerator clock is process-local and is not VMState.
     * Align it to the source clock before checking the absolute timer. */
    qtest_qmp_assert_success(dst, "{'execute':'cont'}");
    g_assert_cmpint(qtest_clock_set(dst, 4000), ==, 4000);
    g_assert_cmphex(qtest_readl(dst, SR1) & SR_QW, ==, SR_QW);
    g_assert_cmphex(first(dst, BANK1), ==, 0);
    g_assert_cmpint(qtest_clock_step(dst, 6999), ==, 10999);
    g_assert_cmphex(first(dst, BANK1), ==, 0);
    g_assert_cmphex(qtest_readl(dst, SR1) & SR_QW, ==, SR_QW);
    g_assert_cmpint(qtest_clock_step_next(dst), ==, 11000);
    g_assert_cmphex(first(dst, BANK1), ==, 0xff);
    g_assert_cmphex(qtest_readl(dst, SR1) & SR_QW, ==, 0);

    finish_peer(dst);
    finish_peer(src);
}

static void test_migration_gate_mismatch_cancels(void)
{
    g_autofree char *uri = migration_uri("mismatch");
    QTestState *src = start_migration_peer(true, false);
    QTestState *dst = start_migration_peer(false, true);

    qtest_qmp_assert_success(src, "{'execute':'cont'}");
    fill(src, BANK1, 0);
    request_sector(src, CR1);
    qtest_clock_step(src, 4000);

    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_fail(src, true);

    /* A rejected stream must not retain an armed erase. Resume and cross the
     * imported deadline to prove cleanup, not merely migration rejection. */
    qtest_qmp_assert_success(dst, "{'execute':'cont'}");
    g_assert_cmpint(qtest_clock_step(dst, SECTOR_NS * 2), ==,
                    SECTOR_NS * 2);
    g_assert_cmphex(qtest_readl(dst, SR1) & SR_QW, ==, 0);
    g_assert_cmphex(first(dst, BANK1), ==, 0);

    finish_peer(dst);
    finish_peer(src);
}

static void test_reset_cancels(void)
{
    QTestState *qts = start_timed();
    fill(qts, BANK1, 0);
    request_sector(qts, CR1);
    g_assert_cmphex(qtest_readl(qts, SR1) & SR_QW, ==, SR_QW);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, SR1) & SR_QW, ==, 0);
    qtest_clock_step(qts, SECTOR_NS * 2);
    g_assert_cmphex(first(qts, BANK1), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/gnw-h7b0/flash-r/default-sync", test_default_sync);
    qtest_add_func("/gnw-h7b0/flash-r/timing-gate-only-sync",
                   test_timing_gate_only_sync);
    qtest_add_func("/gnw-h7b0/flash-r/profile-gate-only-sync",
                   test_profile_gate_only_sync);
    qtest_add_func("/gnw-h7b0/flash-r/pending-deadline",
                   test_pending_and_deadline);
    qtest_add_func("/gnw-h7b0/flash-r/overlap-rejected",
                   test_overlap_rejected);
    qtest_add_func("/gnw-h7b0/flash-r/reset-cancels", test_reset_cancels);
    qtest_add_func("/gnw-h7b0/flash-r/migration-preserves-remaining",
                   test_migration_preserves_remaining);
    qtest_add_func("/gnw-h7b0/flash-r/migration-gate-mismatch-cancels",
                   test_migration_gate_mismatch_cancels);
    return g_test_run();
}
