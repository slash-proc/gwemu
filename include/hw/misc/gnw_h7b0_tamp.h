/* Auto-generated stub for TAMP */
#ifndef HW_MISC_GNW_H7B0_TAMP_H
#define HW_MISC_GNW_H7B0_TAMP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_TAMP "gnw-h7b0-tamp"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0TampState, GNW_H7B0_TAMP)

#define GNW_H7B0_TAMP_SIZE 0x400

struct GnwH7B0TampState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_TAMP_SIZE / 4];
    bool backup_domain_initialized;
};

/* Clear BKP0R..BKP31R as an explicit RCC backup-domain reset would. */
void gnw_h7b0_tamp_backup_domain_reset(GnwH7B0TampState *s);

#endif
