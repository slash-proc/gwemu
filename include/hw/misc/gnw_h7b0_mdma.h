/*
 * QEMU model of the STM32H7B0 MDMA (master DMA) controller.
 *
 * Real (if synchronous) SW-triggered memory-to-memory channel transfers --
 * enough to satisfy game-and-watch-retro-go-sd/diag firmware that uses
 * HAL_MDMA_Start()+HAL_MDMA_PollForTransfer() with MDMA_REQUEST_SW. Register-
 * request-triggered / linked-list transfers are not modeled (no known
 * firmware here uses them).
 */
#ifndef HW_MISC_GNW_H7B0_MDMA_H
#define HW_MISC_GNW_H7B0_MDMA_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_MDMA "gnw-h7b0-mdma"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0MdmaState, GNW_H7B0_MDMA)

#define GNW_H7B0_MDMA_SIZE 0x1000
#define GNW_H7B0_MDMA_CHANNEL_COUNT 16

typedef struct GnwH7B0MdmaTimingProfile {
    uint32_t identity;
    bool eligible;
    uint32_t setup_cycles;
    uint32_t cycles_per_byte_num;
    uint32_t cycles_per_byte_den;
    uint32_t service_bytes;
    /* Optional exact evidence-domain representation.  When setup_num is
     * nonzero all phase needs use setup_num + bytes * cycles_per_byte_num
     * over cycles_per_byte_den, accumulated at reference_hz. */
    uint64_t setup_num;
    uint32_t reference_hz;
    uint32_t required_clock_hz;
} GnwH7B0MdmaTimingProfile;

typedef struct GnwH7B0MdmaChannelCtx {
    struct GnwH7B0MdmaState *s;
    uint8_t channel;
} GnwH7B0MdmaChannelCtx;

struct GnwH7B0MdmaState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    uint32_t regs[GNW_H7B0_MDMA_SIZE / 4];
    bool timing_mode;
    bool migration_timing_mode;
    bool clock_known;
    bool clock_gate_enabled;
    uint32_t clock_hz;
    GnwH7B0MdmaTimingProfile profile;
    uint32_t migration_profile_identity;
    uint64_t unsupported_count;
    bool active[GNW_H7B0_MDMA_CHANNEL_COUNT];
    bool external_paced[GNW_H7B0_MDMA_CHANNEL_COUNT];
    bool first_phase[GNW_H7B0_MDMA_CHANNEL_COUNT];
    bool large_profile[GNW_H7B0_MDMA_CHANNEL_COUNT];
    uint32_t source[GNW_H7B0_MDMA_CHANNEL_COUNT];
    uint32_t destination[GNW_H7B0_MDMA_CHANNEL_COUNT];
    uint32_t remaining[GNW_H7B0_MDMA_CHANNEL_COUNT];
    uint32_t phase_bytes[GNW_H7B0_MDMA_CHANNEL_COUNT];
    uint64_t phase_need[GNW_H7B0_MDMA_CHANNEL_COUNT];
    uint64_t phase_progress[GNW_H7B0_MDMA_CHANNEL_COUNT];
    uint32_t phase_subcycle[GNW_H7B0_MDMA_CHANNEL_COUNT];
    int64_t last_update_ns[GNW_H7B0_MDMA_CHANNEL_COUNT];
    int64_t migration_clock_ns;
    QEMUTimer *timer[GNW_H7B0_MDMA_CHANNEL_COUNT];
    GnwH7B0MdmaChannelCtx ctx[GNW_H7B0_MDMA_CHANNEL_COUNT];
    bool test_profile;
    bool mdma_axi1_v1_profile;
    bool dma_engine_successor_profile;
    bool bus_v6_request_path;
    char *dma_engine_successor_receipt_sha256;
    bool test_clock_known;
    bool test_clock_enabled;
    uint32_t test_clock_hz;
    uint32_t test_required_clock_hz;
    /* Read-only comparison diagnostics: matching V37 CPU loads by position
     * within the active paced MDMA service phase. */
    uint64_t v37_bwm_phase_loads[4];
};

void gnw_h7b0_mdma_set_timing_profile(GnwH7B0MdmaState *s,
                                       const GnwH7B0MdmaTimingProfile *profile);
void gnw_h7b0_mdma_set_clock_info(GnwH7B0MdmaState *s, bool known,
                                   uint32_t reference_hz,
                                   bool gate_enabled);
uint32_t gnw_h7b0_mdma_contends_for_range(void *opaque, uint64_t address,
                                          unsigned size);
uint32_t gnw_h7b0_mdma_large_contends_for_range(void *opaque,
                                                uint64_t address,
                                                unsigned size);
uint32_t gnw_h7b0_mdma_v37_bwm_contends_for_range(void *opaque,
                                                   uint64_t address,
                                                   unsigned size,
                                                   int64_t guest_time_ns);
bool gnw_h7b0_mdma_bus_v6_request(GnwH7B0MdmaState *s, uint32_t request);

#endif
