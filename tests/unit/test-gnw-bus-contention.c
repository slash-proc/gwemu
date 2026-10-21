/* Focused scope tests for the v37 BWM comparison-only victim predicate. */
#include "qemu/osdep.h"
#include "exec/gnw-cycle-model.h"

static GnwCycleInsnMetadata valid_metadata(void)
{
    return (GnwCycleInsnMetadata) {
        .encoding = 0x6824,
        .gpr_reads = 1U << 4,
        .gpr_writes = 1U << 4,
        .address_reads = 1U << 4,
        .size = 2,
        .flags = GNW_CYCLE_META_MEMORY,
    };
}

static GnwCycleMemoryObservation valid_observation(void)
{
    return (GnwCycleMemoryObservation) {
        .physical_address = 0x240c0000,
        .kind = GNW_CYCLE_MEMORY_LOAD,
        .size = 4,
        .access_count = 1,
        .translation_valid = true,
    };
}

static void test_v37_bwm_exact_scope(void)
{
    GnwCycleInsnMetadata metadata = valid_metadata();
    GnwCycleMemoryObservation observation = valid_observation();

    g_assert_true(gnw_m7_bus_contention_v37_bwm_load_eligible(
        &metadata, &observation));

    metadata.encoding = 0x6825;
    g_assert_false(gnw_m7_bus_contention_v37_bwm_load_eligible(
        &metadata, &observation));
    metadata = valid_metadata();

    observation.size = 2;
    g_assert_false(gnw_m7_bus_contention_v37_bwm_load_eligible(
        &metadata, &observation));
    observation = valid_observation();

    observation.physical_address = 0x240d0000;
    g_assert_false(gnw_m7_bus_contention_v37_bwm_load_eligible(
        &metadata, &observation));
    observation.physical_address = 0x240c0002;
    g_assert_false(gnw_m7_bus_contention_v37_bwm_load_eligible(
        &metadata, &observation));
    observation = valid_observation();

    observation.access_count = 2;
    g_assert_false(gnw_m7_bus_contention_v37_bwm_load_eligible(
        &metadata, &observation));
    observation = valid_observation();
    observation.access_flags = 1;
    g_assert_false(gnw_m7_bus_contention_v37_bwm_load_eligible(
        &metadata, &observation));
    observation = valid_observation();
    observation.translation_valid = false;
    g_assert_false(gnw_m7_bus_contention_v37_bwm_load_eligible(
        &metadata, &observation));
    observation = valid_observation();

    metadata.flags |= GNW_CYCLE_META_SYSTEM;
    g_assert_false(gnw_m7_bus_contention_v37_bwm_load_eligible(
        &metadata, &observation));
}

static void test_v37_bwm_charge_preserves_half_cycles(void)
{
    uint64_t fraction = 0;
    uint64_t cycles = 0;

    g_assert_cmpuint(GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_NUMERATOR, ==, 9);
    g_assert_cmpuint(GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_DENOMINATOR, ==, 2);

    /* One match is 4 cycles plus one half-cycle carried forward; two
     * consecutive matches must total exactly 9 cycles, not 8 or 10.
     */
    for (unsigned i = 0; i < 2; i++) {
        uint64_t total = fraction +
            GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_NUMERATOR;
        cycles += total /
            GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_DENOMINATOR;
        fraction = total %
            GNW_M7_BUS_CONTENTION_V37_BWM_COMPARE_DENOMINATOR;
        if (i == 0) {
            g_assert_cmpuint(cycles, ==, 4);
            g_assert_cmpuint(fraction, ==, 1);
        }
    }

    g_assert_cmpuint(cycles, ==, 9);
    g_assert_cmpuint(fraction, ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/gnw/bus-contention/v37-bwm-exact-scope",
                    test_v37_bwm_exact_scope);
    g_test_add_func("/gnw/bus-contention/v37-bwm-half-cycle-charge",
                    test_v37_bwm_charge_preserves_half_cycles);
    return g_test_run();
}
