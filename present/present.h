/*
 * present.h - the game's picture on a host window's client area: layout (integer scaling or fit, square or 4:3
 * pixels), nearest-neighbour scaling and the "CRT scanlines" filter (crt.c).  Portable C, shared by the Windows and
 * the macOS hosts.  The layout and filter follow the user's Civ64 port (docs/HOST-DESIGN.md).
 */
#ifndef MAYA_PRESENT_H
#define MAYA_PRESENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PresentRect {
    int32_t l, t, r, b;
} PresentRect;

/* a picture of fw x fh pixels shown at sw x sh at (ox, oy) on a cw x ch client */
typedef struct PresentLayout {
    int32_t cw, ch;       /* the client */
    int32_t fw, fh;       /* the frame */
    int32_t ox, oy;       /* the image's top-left corner */
    int32_t sw, sh;       /* the image's size */
} PresentLayout;

enum { PRESENT_INTEGER = 0, PRESENT_FIT = 1 };

/* The layout of an fw x fh frame on a cw x ch client.  mode: PRESENT_INTEGER (the largest whole multiple that fits;
 * fit when not even 1x fits) or PRESENT_FIT (the largest size of the frame's aspect).  aspect43: the picture is
 * shown 4:3 (pixels 6:5 tall, as a PAL Amiga's 320 x 200 on a 4:3 monitor's area) instead of square pixels. */
void present_layout(PresentLayout *L, int32_t cw, int32_t ch, int32_t fw, int32_t fh, int mode, int aspect43);

int present_rect_empty(const PresentRect *r);
void present_rect_clip(PresentRect *r, int32_t w, int32_t h);

/* The client image (L.cw x L.ch XRGB, top-down): the frame scaled into the image rectangle, black around it. */
void present_scale(const PresentLayout *L, const uint32_t *frame, uint32_t *client);

/* ---- the CRT scanlines filter (crt.c) ---- */

#define PRESENT_CRT_LEVELS 256     /* the scan weight's steps (w = level / 255) */

typedef struct PresentCrt {
    int valid;
    PresentLayout L;       /* the layout the tables belong to */
    int32_t gw, gh;        /* the source grid: the scanlines' count is 2 * gh (double scan) */
    uint8_t *lvl;          /* per image row: its scan weight's level (L.sh entries) */
    int grid;              /* the scanlines on the two-row output grid (present_crt_grid_rule) */
    int32_t ph;            /* the output rows the scanlines are spaced over */
} PresentCrt;

/* The filter's tables for layout L over a gw x gh source; the scanlines spaced as over ph output rows (the image
 * height the frame has full screen on the window's monitor, so a window shows the full screen's period and
 * darkness; ph < 1: L.sh).  A crt starts zeroed. */
void present_crt_set(PresentCrt *c, const PresentLayout *L, int32_t gw, int32_t gh, int32_t ph);
void present_crt_free(PresentCrt *c);
/* 1 when c was set for this layout and period */
int present_crt_matches(const PresentCrt *c, const PresentLayout *L, int32_t ph);
/* Filters the image rows of the client image in place; the border is left unchanged. */
void present_crt_apply(const PresentCrt *c, uint32_t *client);
/* the scanlines of a source of `rows` rows: two a row (double scan) */
int32_t present_crt_scanlines(int32_t rows);
int present_crt_grid_rule(int32_t sh, int32_t lines);
int present_crt_level(const PresentCrt *c, int32_t e);

#ifdef __cplusplus
}
#endif

#endif
