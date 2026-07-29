/*
 * QTest testcase for the ARMv7-M MPU region registers in the NVIC.
 *
 * Covers what happens when the guest selects an MPU region that the CPU
 * does not implement, which the architecture leaves UNPREDICTABLE. QEMU
 * discards such a write; the point of these tests is that discarding it
 * must not let the following MPU_RASR write -- which takes its region
 * number solely from MPU_RNR -- land on whatever region happened to be
 * selected beforehand.
 *
 * Run against mps2-an385 (Cortex-M3, 8 MPU regions).
 *
 * This code is licensed under the GPL version 2 or later.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MPU_TYPE 0xe000ed90
#define MPU_RNR  0xe000ed98
#define MPU_RBAR 0xe000ed9c
#define MPU_RASR 0xe000eda0

/* Region 2, base 0x20000000, 1KB, full access, enabled. */
#define BASE_R2  0x20000000
#define RASR_R2  0x03000013

/* An RBAR value naming region 8, which mps2-an385's CPU does not implement. */
#define RBAR_VALID  (1 << 4)

static uint32_t read_region_rbar(QTestState *qts, uint32_t region)
{
    qtest_writel(qts, MPU_RNR, region);
    return qtest_readl(qts, MPU_RBAR);
}

static uint32_t read_region_rasr(QTestState *qts, uint32_t region)
{
    qtest_writel(qts, MPU_RNR, region);
    return qtest_readl(qts, MPU_RASR);
}

/* Program region 2 the ordinary way, so we have a known state to disturb. */
static void setup_region2(QTestState *qts)
{
    qtest_writel(qts, MPU_RNR, 2);
    qtest_writel(qts, MPU_RBAR, BASE_R2);
    qtest_writel(qts, MPU_RASR, RASR_R2);

    g_assert_cmphex(read_region_rbar(qts, 2), ==, BASE_R2 | 2);
    g_assert_cmphex(read_region_rasr(qts, 2), ==, RASR_R2);
}

/* The CPU should report 8 implemented regions, which the rest of this relies on. */
static void test_mpu_type(void)
{
    QTestState *qts = qtest_init("-machine mps2-an385");

    g_assert_cmpuint((qtest_readl(qts, MPU_TYPE) >> 8) & 0xff, ==, 8);

    qtest_quit(qts);
}

/* Baseline: an in-range RBAR write with VALID set retargets MPU_RNR. */
static void test_rbar_valid_in_range(void)
{
    QTestState *qts = qtest_init("-machine mps2-an385");

    setup_region2(qts);

    /* Select region 5 via RBAR.VALID, then write RASR without touching RNR. */
    qtest_writel(qts, MPU_RBAR, 0x60000000 | RBAR_VALID | 5);
    qtest_writel(qts, MPU_RASR, 0x03000029);

    g_assert_cmphex(qtest_readl(qts, MPU_RNR), ==, 5);
    g_assert_cmphex(read_region_rbar(qts, 5), ==, 0x60000000 | 5);
    g_assert_cmphex(read_region_rasr(qts, 5), ==, 0x03000029);

    /* Region 2 must be untouched. */
    g_assert_cmphex(read_region_rbar(qts, 2), ==, BASE_R2 | 2);
    g_assert_cmphex(read_region_rasr(qts, 2), ==, RASR_R2);

    qtest_quit(qts);
}

/*
 * An RBAR write naming an unimplemented region must not leave the
 * following RASR write to fall through onto the previously selected
 * region. Real firmware hits this by passing an unaligned address
 * straight to MPU_RBAR, so the low bits are accidentally VALID|REGION.
 */
static void test_rbar_valid_out_of_range(void)
{
    QTestState *qts = qtest_init("-machine mps2-an385");

    setup_region2(qts);

    /* Region 4 is untouched from reset, and is what a stale RNR would hit. */
    qtest_writel(qts, MPU_RNR, 4);
    qtest_writel(qts, MPU_RBAR, 0x40000000);
    qtest_writel(qts, MPU_RASR, 0x03000021);

    /* Now name region 8, which does not exist, and follow it with a RASR. */
    qtest_writel(qts, MPU_RBAR, 0x08000000 | RBAR_VALID | 8);
    qtest_writel(qts, MPU_RASR, 0x1000000f);

    /* Neither the named region nor the stale one may have changed. */
    g_assert_cmphex(read_region_rbar(qts, 4), ==, 0x40000000 | 4);
    g_assert_cmphex(read_region_rasr(qts, 4), ==, 0x03000021);
    g_assert_cmphex(read_region_rbar(qts, 2), ==, BASE_R2 | 2);
    g_assert_cmphex(read_region_rasr(qts, 2), ==, RASR_R2);

    /* A subsequent valid selection must work again. */
    qtest_writel(qts, MPU_RNR, 2);
    qtest_writel(qts, MPU_RASR, 0x03000015);
    g_assert_cmphex(read_region_rasr(qts, 2), ==, 0x03000015);

    qtest_quit(qts);
}

/* The same hazard reached via MPU_RNR rather than MPU_RBAR.VALID. */
static void test_rnr_out_of_range(void)
{
    QTestState *qts = qtest_init("-machine mps2-an385");

    setup_region2(qts);

    qtest_writel(qts, MPU_RNR, 9);
    qtest_writel(qts, MPU_RBAR, 0x08000000);
    qtest_writel(qts, MPU_RASR, 0x1000000f);

    g_assert_cmphex(read_region_rbar(qts, 2), ==, BASE_R2 | 2);
    g_assert_cmphex(read_region_rasr(qts, 2), ==, RASR_R2);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/armv7m-mpu/type", test_mpu_type);
    qtest_add_func("/armv7m-mpu/rbar-valid-in-range", test_rbar_valid_in_range);
    qtest_add_func("/armv7m-mpu/rbar-valid-out-of-range",
                   test_rbar_valid_out_of_range);
    qtest_add_func("/armv7m-mpu/rnr-out-of-range", test_rnr_out_of_range);

    return g_test_run();
}
