/*
 * gnw-h7b0 timeline script engine -- see gnw_timeline.h for rationale.
 *
 * Script format (line-based; '#' starts a comment; blank lines ignored):
 *
 *   0:02.0   hold game+left 3.0     # chord held for 3.0s
 *   0:14     press a                # down + 100ms + up
 *   @18720   press a+b              # vblank-frame addressing
 *   5:12.5   screenshot menu.png    # PNG into $GNW_OUT (default cwd)
 *   6:00     quit
 *
 * Timestamps are guest (virtual-clock) time as [MM:]SS[.fff]; "@N" is
 * the Nth LTDC vblank since machine start (counted here, notified by
 * gnw_h7b0_ltdc_vblank_tick()). Actions: press/hold/release with
 * '+'-joined button names (pause game time a b left down right up pwr
 * start select -- same table as the keyboard mapping), screenshot,
 * quit.
 *
 * Time-addressed entries run off a QEMU_CLOCK_VIRTUAL timer chain (same
 * pattern as GNW_AUTO_INPUT, which predates this and remains as
 * one-liner sugar); frame-addressed entries fire from the vblank
 * notifier. Both contexts hold the BQL.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "system/runstate.h"
#include "ui/console.h"
#include "hw/misc/gnw_h7b0_gpio.h"
#include "hw/misc/gnw_timeline.h"

typedef enum GnwTlKind {
    GNW_TL_BUTTON,      /* set one button up/down */
    GNW_TL_SCREENSHOT,
    GNW_TL_QUIT,
} GnwTlKind;

typedef struct GnwTlEvent {
    bool by_frame;
    int64_t ns;         /* valid when !by_frame */
    uint64_t frame;     /* valid when by_frame */
    GnwTlKind kind;
    int btn;            /* GNW_TL_BUTTON */
    bool down;          /* GNW_TL_BUTTON */
    char *path;         /* GNW_TL_SCREENSHOT */
} GnwTlEvent;

static GnwTlEvent *tl_events;
static int tl_nevents;
static int tl_next_time;        /* next unfired index in the time list */
static GnwTlEvent *tl_fevents;  /* frame-addressed, sorted by frame */
static int tl_nfevents;
static int tl_next_frame;
static uint64_t tl_frame_count;
static QEMUTimer *tl_timer;
static GnwH7B0GpioState *tl_gpio;
static const char *tl_out_dir;

/* 100ms, matching GNW_AUTO_INPUT's historical tap duration. */
#define GNW_TL_TAP_NS (100 * 1000000LL)

static void gnw_timeline_exec(const GnwTlEvent *ev)
{
    switch (ev->kind) {
    case GNW_TL_BUTTON:
        gnw_h7b0_gpio_inject_button(tl_gpio, ev->btn, ev->down);
        break;
    case GNW_TL_SCREENSHOT: {
        Error *err = NULL;
        g_autofree char *path = g_path_is_absolute(ev->path)
            ? g_strdup(ev->path)
            : g_build_filename(tl_out_dir ?: ".", ev->path, NULL);
        if (!gwemu_screendump_png(path, &err)) {
            error_reportf_err(err, "gnw-timeline: screenshot %s: ", path);
        } else {
            fprintf(stderr, "gnw-timeline: screenshot %s\n", path);
        }
        break;
    }
    case GNW_TL_QUIT:
        fprintf(stderr, "gnw-timeline: quit\n");
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_QMP_QUIT);
        break;
    }
}

static void gnw_timeline_timer_cb(void *opaque)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    while (tl_next_time < tl_nevents && tl_events[tl_next_time].ns <= now) {
        gnw_timeline_exec(&tl_events[tl_next_time]);
        tl_next_time++;
    }
    if (tl_next_time < tl_nevents) {
        timer_mod(tl_timer, tl_events[tl_next_time].ns);
    }
}

void gnw_timeline_notify_vblank(void)
{
    if (!tl_fevents) {
        return;
    }
    tl_frame_count++;
    while (tl_next_frame < tl_nfevents &&
           tl_fevents[tl_next_frame].frame <= tl_frame_count) {
        gnw_timeline_exec(&tl_fevents[tl_next_frame]);
        tl_next_frame++;
    }
}

/* "[MM:]SS[.fff]" -> ns; returns -1 on parse failure. */
static int64_t gnw_timeline_parse_time(const char *tok)
{
    double mins = 0, secs = 0;
    const char *colon = strchr(tok, ':');
    char *end;

    if (colon) {
        mins = g_ascii_strtod(tok, &end);
        if (end != colon || mins < 0) {
            return -1;
        }
        tok = colon + 1;
    }
    secs = g_ascii_strtod(tok, &end);
    if (*end != '\0' || secs < 0 || (colon && secs >= 60)) {
        return -1;
    }
    return (int64_t)((mins * 60.0 + secs) * 1e9);
}

/* "a+b+left" -> button indices; returns count or -1 on unknown name. */
static int gnw_timeline_parse_buttons(const char *tok, int *btns, int max)
{
    g_auto(GStrv) parts = g_strsplit(tok, "+", -1);
    int n = 0;

    for (char **p = parts; *p; p++) {
        int btn = gnw_h7b0_gpio_button_from_name(*p);
        if (btn < 0 || n >= max) {
            return -1;
        }
        btns[n++] = btn;
    }
    return n ?: -1;
}

static int gnw_timeline_cmp_time(const void *a, const void *b)
{
    const GnwTlEvent *ea = a, *eb = b;
    return ea->ns < eb->ns ? -1 : ea->ns > eb->ns ? 1 : 0;
}

static int gnw_timeline_cmp_frame(const void *a, const void *b)
{
    const GnwTlEvent *ea = a, *eb = b;
    return ea->frame < eb->frame ? -1 : ea->frame > eb->frame ? 1 : 0;
}

static void gnw_timeline_add(GArray *arr, bool by_frame, int64_t ns,
                             uint64_t frame, GnwTlKind kind, int btn,
                             bool down, const char *path)
{
    GnwTlEvent ev = {
        .by_frame = by_frame,
        .ns = ns,
        .frame = frame,
        .kind = kind,
        .btn = btn,
        .down = down,
        .path = path ? g_strdup(path) : NULL,
    };
    g_array_append_val(arr, ev);
}

void gnw_timeline_init(GnwH7B0GpioState *gpio)
{
    const char *fname = getenv("GNW_TIMELINE");
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    GError *gerr = NULL;

    if (!fname) {
        return;
    }
    if (!g_file_get_contents(fname, &contents, NULL, &gerr)) {
        error_report("gnw-timeline: cannot read %s: %s", fname,
                     gerr->message);
        exit(1);
    }

    tl_gpio = gpio;
    tl_out_dir = getenv("GNW_OUT");

    g_autoptr(GArray) tev = g_array_new(false, false, sizeof(GnwTlEvent));
    g_autoptr(GArray) fev = g_array_new(false, false, sizeof(GnwTlEvent));

    lines = g_strsplit(contents, "\n", -1);
    for (int lineno = 1; lines[lineno - 1]; lineno++) {
        char *line = lines[lineno - 1];
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        g_strstrip(line);
        if (!*line) {
            continue;
        }

        g_auto(GStrv) tok = g_strsplit_set(line, " \t", -1);
        /* g_strsplit_set leaves empty strings for repeated separators --
         * compact them. */
        int ntok = 0;
        for (char **p = tok; *p; p++) {
            if (**p) {
                tok[ntok++] = *p;
            } else {
                g_free(*p);
            }
        }
        tok[ntok] = NULL;

        bool by_frame = false;
        int64_t ns = -1;
        uint64_t frame = 0;

        if (ntok < 2) {
            goto bad;
        }
        if (tok[0][0] == '@') {
            char *end;
            frame = g_ascii_strtoull(tok[0] + 1, &end, 10);
            if (*end != '\0' || end == tok[0] + 1) {
                goto bad;
            }
            by_frame = true;
        } else {
            ns = gnw_timeline_parse_time(tok[0]);
            if (ns < 0) {
                goto bad;
            }
        }

        GArray *arr = by_frame ? fev : tev;
        const char *act = tok[1];
        int btns[8], nbtns;

        if (!strcmp(act, "press") || !strcmp(act, "hold") ||
            !strcmp(act, "release")) {
            if (ntok < 3) {
                goto bad;
            }
            nbtns = gnw_timeline_parse_buttons(tok[2], btns,
                                               ARRAY_SIZE(btns));
            if (nbtns < 0) {
                goto bad;
            }
            int64_t up_delta_ns = -1; /* -1: no matching release */
            bool down = strcmp(act, "release") != 0;
            if (!strcmp(act, "press")) {
                up_delta_ns = GNW_TL_TAP_NS;
            } else if (!strcmp(act, "hold")) {
                if (ntok < 4) {
                    goto bad;
                }
                char *end;
                double dur = g_ascii_strtod(tok[3], &end);
                if (*end != '\0' || dur <= 0) {
                    goto bad;
                }
                up_delta_ns = (int64_t)(dur * 1e9);
            }
            for (int i = 0; i < nbtns; i++) {
                gnw_timeline_add(arr, by_frame, ns, frame, GNW_TL_BUTTON,
                                 btns[i], down, NULL);
                if (down && up_delta_ns >= 0) {
                    /*
                     * The matching release is always time-addressed --
                     * even for an @frame press, "hold for 3.0s" means
                     * guest seconds, and mixing wouldn't be observable
                     * anyway (the guest polls level, not edges). For a
                     * frame-addressed press the release time can't be
                     * computed at parse time; approximate by scheduling
                     * it from the press via a second frame event is
                     * overkill -- instead frame presses get their
                     * release scheduled when the press fires (see
                     * exec-side note below). To keep v1 simple, frame-
                     * addressed press/hold releases are converted to a
                     * frame count at 60Hz nominal. Precision-critical
                     * users can write explicit press/release pairs.
                     */
                    if (by_frame) {
                        uint64_t rel_frames =
                            MAX(1, (uint64_t)(up_delta_ns / 16666667LL));
                        gnw_timeline_add(arr, true, -1, frame + rel_frames,
                                         GNW_TL_BUTTON, btns[i], false,
                                         NULL);
                    } else {
                        gnw_timeline_add(arr, false, ns + up_delta_ns, 0,
                                         GNW_TL_BUTTON, btns[i], false,
                                         NULL);
                    }
                }
            }
        } else if (!strcmp(act, "screenshot")) {
            if (ntok < 3) {
                goto bad;
            }
            gnw_timeline_add(arr, by_frame, ns, frame, GNW_TL_SCREENSHOT,
                             0, false, tok[2]);
        } else if (!strcmp(act, "quit")) {
            gnw_timeline_add(arr, by_frame, ns, frame, GNW_TL_QUIT,
                             0, false, NULL);
        } else {
            goto bad;
        }
        continue;

bad:
        error_report("gnw-timeline: %s:%d: parse error: \"%s\"", fname,
                     lineno, line);
        exit(1);
    }

    tl_nevents = tev->len;
    tl_events = (GnwTlEvent *)g_array_free(g_steal_pointer(&tev), false);
    qsort(tl_events, tl_nevents, sizeof(GnwTlEvent), gnw_timeline_cmp_time);

    tl_nfevents = fev->len;
    tl_fevents = (GnwTlEvent *)g_array_free(g_steal_pointer(&fev), false);
    qsort(tl_fevents, tl_nfevents, sizeof(GnwTlEvent),
          gnw_timeline_cmp_frame);
    if (!tl_nfevents) {
        g_free(tl_fevents);
        tl_fevents = NULL;
    }

    if (tl_nevents) {
        tl_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gnw_timeline_timer_cb,
                                NULL);
        timer_mod(tl_timer, tl_events[0].ns);
    }

    fprintf(stderr, "gnw-timeline: %s armed (%d time + %d frame events)\n",
            fname, tl_nevents, tl_nfevents);
}
