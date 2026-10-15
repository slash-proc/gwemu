/*
 * NOR parts supported by retro-go's Game & Watch OSPI driver.
 * Keep this table in sync with game-and-watch-retro-go-sd/Core/Src/gw_flash.c.
 */
#ifndef HW_MISC_GNW_H7B0_NOR_H
#define HW_MISC_GNW_H7B0_NOR_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum GnwNorCommandProfile {
    GNW_NOR_PROFILE_MX_24,
    GNW_NOR_PROFILE_MX_32,
    GNW_NOR_PROFILE_MX_32_54,
    GNW_NOR_PROFILE_SPANSION_32,
    GNW_NOR_PROFILE_ISSI_24,
    GNW_NOR_PROFILE_WINBOND_24,
    GNW_NOR_PROFILE_WINBOND_32,
} GnwNorCommandProfile;

typedef struct GnwNorChip {
    const char *key;       /* Stable profile/CLI key: lowercase JEDEC bytes. */
    const char *name;
    uint64_t size_bytes;
    uint8_t jedec[3];
    GnwNorCommandProfile profile;
} GnwNorChip;

#define GNW_NOR_MIB (1024ULL * 1024ULL)
#define GNW_NOR_CHIP_TABLE(X) \
    X("c2-25-34", "MX25U8035F",      1, 0xC2, 0x25, 0x34, GNW_NOR_PROFILE_MX_24) \
    X("c2-25-36", "MX25U3232F",      4, 0xC2, 0x25, 0x36, GNW_NOR_PROFILE_MX_24) \
    X("c2-25-37", "MX25U6432F",      8, 0xC2, 0x25, 0x37, GNW_NOR_PROFILE_MX_24) \
    X("c2-25-38", "MX25U1283xF",    16, 0xC2, 0x25, 0x38, GNW_NOR_PROFILE_MX_24) \
    X("c2-20-18", "MX25L12873",     16, 0xC2, 0x20, 0x18, GNW_NOR_PROFILE_MX_24) \
    X("c2-25-39", "MX25U25635F",    32, 0xC2, 0x25, 0x39, GNW_NOR_PROFILE_MX_32) \
    X("c2-25-3a", "MX25U51245G",    64, 0xC2, 0x25, 0x3A, GNW_NOR_PROFILE_MX_32) \
    X("c2-24-3a", "MX25U51245G",    64, 0xC2, 0x24, 0x3A, GNW_NOR_PROFILE_MX_32) \
    X("c2-20-1a", "MX25L51245G",    64, 0xC2, 0x20, 0x1A, GNW_NOR_PROFILE_MX_32) \
    X("c2-95-3a", "MX25U51245G-54", 64, 0xC2, 0x95, 0x3A, GNW_NOR_PROFILE_MX_32_54) \
    X("c2-25-3b", "MX66U1G45G",    128, 0xC2, 0x25, 0x3B, GNW_NOR_PROFILE_MX_32) \
    X("c2-25-3c", "MX66U2G45G",    256, 0xC2, 0x25, 0x3C, GNW_NOR_PROFILE_MX_32) \
    X("01-02-20", "S25FS512S",       64, 0x01, 0x02, 0x20, GNW_NOR_PROFILE_SPANSION_32) \
    X("34-2b-1a", "S25FS512S",       64, 0x34, 0x2B, 0x1A, GNW_NOR_PROFILE_SPANSION_32) \
    X("9d-70-18", "IS25WP128F",      16, 0x9D, 0x70, 0x18, GNW_NOR_PROFILE_ISSI_24) \
    X("ef-60-18", "W25Q128JW-Q/N",  16, 0xEF, 0x60, 0x18, GNW_NOR_PROFILE_WINBOND_24) \
    X("ef-80-18", "W25Q128JW-M",    16, 0xEF, 0x80, 0x18, GNW_NOR_PROFILE_WINBOND_24) \
    X("ef-60-20", "W25Q512NW-Q/N",  64, 0xEF, 0x60, 0x20, GNW_NOR_PROFILE_WINBOND_32) \
    X("ef-80-20", "W25Q512NW-M",    64, 0xEF, 0x80, 0x20, GNW_NOR_PROFILE_WINBOND_32)

#define GNW_NOR_CHIP_ROW(key_, name_, mib_, mfg_, type_, dens_, profile_) \
    { key_, name_, (uint64_t)(mib_) * GNW_NOR_MIB, \
      { mfg_, type_, dens_ }, profile_ },
static const GnwNorChip gnw_nor_chips[] = {
    GNW_NOR_CHIP_TABLE(GNW_NOR_CHIP_ROW)
};
#undef GNW_NOR_CHIP_ROW

#define GNW_NOR_CHIP_COUNT (sizeof(gnw_nor_chips) / sizeof(gnw_nor_chips[0]))
#define GNW_NOR_DEFAULT_CHIP_KEY "c2-25-3a"

static inline const GnwNorChip *gnw_nor_chip_by_key(const char *key)
{
    if (key) {
        for (unsigned i = 0; i < GNW_NOR_CHIP_COUNT; i++) {
            if (!strcmp(key, gnw_nor_chips[i].key)) {
                return &gnw_nor_chips[i];
            }
        }
    }
    return NULL;
}

#endif
