/*
 * gnw-h7b0 headless session recorder.
 *
 * Armed by GNW_RECORD=<basename> (entirely inert otherwise): a
 * QEMU_CLOCK_VIRTUAL timer at GNW_RECORD_FPS (default 60) forces a
 * console update and appends the current frame as raw pixels to
 * <basename>.frames, plus a one-line <basename>.meta describing the
 * stream (width/height/fps/pix_fmt) for the ffmpeg mux that the Docker
 * entrypoint runs after QEMU exits. Audio is captured separately by the
 * stock `wav` audiodev -- both run on the virtual clock, so a
 * constant-frame-rate raw stream and the wav are in sync by
 * construction (duplicated frames during guest stalls are correct
 * behavior, and under -icount the recording still plays back at true
 * speed regardless of how fast the host ran it).
 *
 * Whole-session only by design (v1 scope decision): recording starts at
 * machine start and ends at exit. No mid-run start/stop.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "hw/display/gnw_h7b0_recorder.h"

static FILE *rec_file;
static QEMUTimer *rec_timer;
static QemuConsole *rec_con;
static int64_t rec_period_ns;
static int rec_w, rec_h;
static char *rec_basename;
static uint64_t rec_nframes;

static void gnw_h7b0_recorder_write_meta(void)
{
    g_autofree char *meta_path = g_strdup_printf("%s.meta", rec_basename);
    FILE *f = fopen(meta_path, "w");

    if (f) {
        fprintf(f, "width=%d\nheight=%d\nfps=%lld\npix_fmt=bgr0\n",
                rec_w, rec_h,
                (long long)(NANOSECONDS_PER_SECOND / rec_period_ns));
        fclose(f);
    }
}

static void gnw_h7b0_recorder_tick(void *opaque)
{
    DisplaySurface *surface;

    graphic_hw_update(rec_con);
    surface = qemu_console_surface(rec_con);

    if (surface) {
        int w = surface_width(surface), h = surface_height(surface);

        if (rec_w == 0) {
            /* First frame fixes the stream geometry. */
            rec_w = w;
            rec_h = h;
            gnw_h7b0_recorder_write_meta();
        }
        if (w == rec_w && h == rec_h &&
            surface_bytes_per_pixel(surface) == 4) {
            /* x8r8g8b8 little-endian == ffmpeg pix_fmt bgr0, row by row
             * to honor stride. */
            for (int y = 0; y < h; y++) {
                fwrite(surface_data(surface) +
                           (size_t)y * surface_stride(surface),
                       (size_t)w * 4, 1, rec_file);
            }
        } else {
            /* Geometry changed mid-run (mode switch): repeat nothing,
             * emit black to keep CFR cadence honest. */
            g_autofree uint8_t *black = g_malloc0((size_t)rec_w * 4);
            for (int y = 0; y < rec_h; y++) {
                fwrite(black, (size_t)rec_w * 4, 1, rec_file);
            }
        }
        rec_nframes++;
    }

    timer_mod(rec_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + rec_period_ns);
}

void gnw_h7b0_recorder_init(QemuConsole *con)
{
    const char *basename = getenv("GNW_RECORD");
    const char *fps_env = getenv("GNW_RECORD_FPS");
    long fps = 60;

    if (!basename || rec_timer) {
        return;
    }
    if (fps_env) {
        fps = atol(fps_env);
        if (fps < 1 || fps > 240) {
            error_report("gnw-recorder: bad GNW_RECORD_FPS \"%s\"", fps_env);
            exit(1);
        }
    }

    g_autofree char *frames_path = g_strdup_printf("%s.frames", basename);
    rec_file = fopen(frames_path, "wb");
    if (!rec_file) {
        error_report("gnw-recorder: cannot open %s: %s", frames_path,
                     strerror(errno));
        exit(1);
    }

    rec_basename = g_strdup(basename);
    rec_con = con;
    rec_period_ns = NANOSECONDS_PER_SECOND / fps;
    rec_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gnw_h7b0_recorder_tick,
                             NULL);
    timer_mod(rec_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + rec_period_ns);

    fprintf(stderr, "gnw-recorder: recording to %s.frames at %ldfps "
            "(virtual clock)\n", basename, fps);
}
