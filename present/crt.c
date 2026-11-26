/*
 * crt.c - the "CRT scanlines" filter (present.h), ported from the user's Civ64 port (present/crt.c there, MIT,
 * Onur BUYUKCAGLAR; docs/HOST-DESIGN.md).
 *
 * The filter is DOSBox Pure Unleashed's CRT filter with "Only Scanlines", scanline intensity "Normal gaps",
 * "Sharpest", no curvature, no mask and no border.  The math follows that filter's fragment shader
 * (fragment_shader_crt_src in DOSBox Pure Unleashed's main.cpp, (C) Bernhard Schelling, GPL-2.0-or-later),
 * reimplemented in portable C; no shader code is copied.  With ScanlineThinness T = 0.65 a row at distance a
 * (0 .. 0.5 scanlines) from a scanline's centre has the weight
 *   w = s(a T) + s((1 - a) T), s(x) = cos(min(0.5, x) 2 pi) / 2 + 1/2   (clamped to 1)
 * and the tonal curve midOut = 0.18 / (1.5 - T), pMidIn = 0.18, ty = (midOut - pMidIn) / ((1 - pMidIn) midOut),
 * tz = (pMidIn - pMidIn midOut) / (midOut - pMidIn midOut); every channel of a pixel whose brightest channel is p
 * (linear light) is multiplied by w f(p) / p with f(p) = p / (p ty + tz).
 *
 * No resampling: each scaled pixel keeps its value and takes its image row's weight.  Double scan: 2 scanlines a
 * source row.  Grid rule: where P = output rows per scanline would hide the gaps (not P >= 2.25 and not
 * 1.4 <= P < 1.75), the scanlines lie on a fixed grid of two output rows (centre on even rows, gap on odd).  The
 * scanlines are spaced over the full-screen image height, so a window shows the full screen's look.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "present.h"

#define CRT_T 0.65                /* ScanlineThinness: 0.5 + 0.05 x "Normal gaps" (3) */
#define CRT_PI2 6.28318530717958

static int g_luts;
static uint16_t g_dec[256];       /* sRGB byte -> linear, 0..65535 */
static uint8_t g_enc[65536];      /* linear -> sRGB byte */
static uint32_t g_fac[PRESENT_CRT_LEVELS][256];   /* 16.16: w f(p) / p for level w and brightest byte m */

static double from_srgb(double c) { return pow((c + 0.055) / 1.055, 2.4); }
static double to_srgb(double c) { return 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

static void luts(void)
{
    double mid_out = 0.18 / (1.5 - CRT_T);
    double mid_in = 0.18;
    double ty = (mid_out - mid_in) / ((1.0 - mid_in) * mid_out);
    double tz = (mid_in - mid_in * mid_out) / (mid_out - mid_in * mid_out);
    int i, m;
    if (g_luts) return;
    for (i = 0; i < 256; i++) {
        double v = floor(from_srgb((double)i / 255.0) * 65535.0 + 0.5);
        g_dec[i] = (uint16_t)(v < 0.0 ? 0.0 : (v > 65535.0 ? 65535.0 : v));
    }
    for (i = 0; i < 65536; i++) {
        double v = floor(to_srgb((double)i / 65535.0) * 255.0 + 0.5);
        g_enc[i] = (uint8_t)(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v));
    }
    for (i = 0; i < PRESENT_CRT_LEVELS; i++) {
        double w = (double)i / (double)(PRESENT_CRT_LEVELS - 1);
        for (m = 0; m < 256; m++) {
            double p = w * (double)g_dec[m] / 65535.0;
            double g;
            p = p > 1.0 / (256.0 * 65536.0) ? p : 1.0 / (256.0 * 65536.0);
            g = w / (p * ty + tz);
            g_fac[i][m] = (uint32_t)floor(g * 65536.0 + 0.5);
        }
    }
    g_luts = 1;
}

void present_crt_free(PresentCrt *c)
{
    free(c->lvl);
    memset(c, 0, sizeof *c);
}

static double scan(double x)
{
    return cos((x < 0.5 ? x : 0.5) * CRT_PI2) * 0.5 + 0.5;
}

static uint8_t level(double a)
{
    double w = scan(a * CRT_T) + scan((1.0 - a) * CRT_T);
    return (uint8_t)floor((w > 1.0 ? 1.0 : w) * (double)(PRESENT_CRT_LEVELS - 1) + 0.5);
}

int present_crt_grid_rule(int32_t sh, int32_t lines)
{
    int64_t s = sh;
    int64_t n = lines < 1 ? 1 : lines;
    if (4 * s >= 9 * n || (5 * s >= 7 * n && 4 * s < 7 * n)) return 0;
    return 1;
}

int32_t present_crt_scanlines(int32_t rows)
{
    return 2 * rows;
}

void present_crt_set(PresentCrt *c, const PresentLayout *L, int32_t gw, int32_t gh, int32_t ph)
{
    int32_t e, lines;
    present_crt_free(c);
    luts();
    c->L = *L;
    c->gw = gw < 1 ? 1 : gw;
    c->gh = gh < 1 ? 1 : gh;
    c->ph = ph < 1 ? L->sh : ph;
    lines = present_crt_scanlines(c->gh);
    c->grid = present_crt_grid_rule(c->ph, lines);
    c->lvl = (uint8_t *)malloc((size_t)(L->sh > 0 ? L->sh : 1));
    if (c->lvl == NULL) return;
    for (e = 0; e < L->sh; e++) {
        double py = ((double)e + 0.5) * (double)lines / (double)c->ph;
        double a = fabs(py - (floor(py) + 0.5));
        if (c->grid) a = (e & 1) != 0 ? 0.5 : 0.0;
        c->lvl[e] = level(a);
    }
    c->valid = 1;
}

int present_crt_matches(const PresentCrt *c, const PresentLayout *L, int32_t ph)
{
    return c->valid && memcmp(&c->L, L, sizeof *L) == 0 && c->ph == (ph < 1 ? L->sh : ph);
}

int present_crt_level(const PresentCrt *c, int32_t e)
{
    return c->valid && e >= 0 && e < c->L.sh ? (int)c->lvl[e] : -1;
}

void present_crt_apply(const PresentCrt *c, uint32_t *client)
{
    const PresentLayout *L = &c->L;
    size_t cw = (size_t)L->cw;
    int32_t e, x;
    if (!c->valid) return;
    for (e = 0; e < L->sh; e++) {
        uint32_t *row = client + (size_t)(e + L->oy) * cw + L->ox;
        const uint32_t *fac = g_fac[c->lvl[e]];
        uint32_t last_in = row[0] ^ 1u, last_out = 0u;
        for (x = 0; x < L->sw; x++) {
            uint32_t p = row[x];
            if (p != last_in) {
                uint32_t rr = (p >> 16) & 0xFFu, gg = (p >> 8) & 0xFFu, bb = p & 0xFFu;
                uint32_t m = rr > gg ? rr : gg, f, v;
                m = m > bb ? m : bb;
                f = fac[m];
                v = (uint32_t)(((uint64_t)g_dec[rr] * f) >> 16);
                rr = g_enc[v > 65535u ? 65535u : v];
                v = (uint32_t)(((uint64_t)g_dec[gg] * f) >> 16);
                gg = g_enc[v > 65535u ? 65535u : v];
                v = (uint32_t)(((uint64_t)g_dec[bb] * f) >> 16);
                bb = g_enc[v > 65535u ? 65535u : v];
                last_in = p;
                last_out = (rr << 16) | (gg << 8) | bb;
            }
            row[x] = last_out;
        }
    }
}
