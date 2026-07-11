/* Auto-generated stub for JPEG */
#ifndef HW_MISC_GNW_H7B0_JPEG_H
#define HW_MISC_GNW_H7B0_JPEG_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_JPEG "gnw-h7b0-jpeg"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0JpegState, GNW_H7B0_JPEG)

#define GNW_H7B0_JPEG_SIZE 0x1000

struct GnwH7B0JpegState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_JPEG_SIZE / 4];

    GByteArray *inbuf;
    uint8_t *rgb_buffer;
    int rgb_width;
    int rgb_height;
};

uint8_t *gnw_h7b0_jpeg_get_hack_buffer(int *width, int *height);

#endif
