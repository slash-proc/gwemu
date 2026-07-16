/* Auto-generated stub for JPEG */
#ifndef HW_MISC_GNW_H7B0_JPEG_H
#define HW_MISC_GNW_H7B0_JPEG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_JPEG "gnw-h7b0-jpeg"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0JpegState, GNW_H7B0_JPEG)

#define GNW_H7B0_JPEG_SIZE 0x1000

struct GnwH7B0JpegState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_JPEG_SIZE / 4];

    GByteArray *inbuf;

    /* Decoded output: a full-resolution Y plane plus Cb/Cr planes
     * subsampled per the image's real SOF0 H/V sampling factors (to
     * match real hardware/firmware's expected total output size --
     * serving full-resolution, unsubsampled chroma made our DOR output
     * roughly 2x the size firmware's own destination buffer expects,
     * which firmware's own polling loop only partially drains, leaving
     * the tail of its buffer stale). DOR reads pop 4 bytes at a time
     * from a flat cursor over y_plane||cb_plane||cr_plane. */
    uint8_t *y_plane;
    uint8_t *cb_plane;
    uint8_t *cr_plane;
    int plane_width;
    int plane_height;
    int chroma_width;
    int chroma_height;
    uint32_t dor_cursor;

    /* Encode-direction (CONFR1.DE clear) state: raw YCbCr pixel bytes
     * accumulated from DIR writes until a full image's worth has
     * arrived (per CONFR3.XSIZE/CONFR1.YSIZE), then real-encoded into
     * encode_out; DOR reads drain encode_out via enc_dor_cursor,
     * mirroring the decode side's y/cb/cr_plane + dor_cursor pattern. */
    GByteArray *encode_out;
    uint32_t enc_dor_cursor;
};

void gnw_h7b0_jpeg_get_last_decoded_size(uint32_t *width, uint32_t *height);
void gnw_h7b0_jpeg_get_last_chroma_size(uint32_t *width, uint32_t *height);

#endif
