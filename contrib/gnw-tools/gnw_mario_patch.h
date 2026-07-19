#ifndef GNW_MARIO_PATCH_H
#define GNW_MARIO_PATCH_H

#include <stdbool.h>
#include "gnw_cfw_engine.h"

/* Runs mario's patch() sequence against an already-initialized GnwDevice
 * (internal/external/compressed_memory/lookup/rwdata all set up, int_pos
 * already seeded to internal's empty_offset -- see the gnw-make-cfw-images
 * driver for that setup). On failure returns false and sets *out_err
 * (malloc'd, caller frees) to a human-readable message. */
bool gnw_mario_patch(GnwDevice *d, char **out_err);

#endif
