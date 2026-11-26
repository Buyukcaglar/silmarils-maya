/*
 * layout.c - where the picture goes on the client and the nearest-neighbour scaling to it (present.h).
 * Portable C.
 */
#include <stdlib.h>
#include <string.h>

#include "present.h"

int present_rect_empty(const PresentRect *r)
{
    return r->r <= r->l || r->b <= r->t;
}

void present_rect_clip(PresentRect *r, int32_t w, int32_t h)
{
    if (r->l < 0) r->l = 0;
    if (r->t < 0) r->t = 0;
    if (r->r > w) r->r = w;
    if (r->b > h) r->b = h;
}

void present_layout(PresentLayout *L, int32_t cw, int32_t ch, int32_t fw, int32_t fh, int mode, int aspect43)
{
    /* the frame's shown height per 1x: fh, or fh * 6 / 5 for 4:3 (320 x 200 -> 320 x 240) */
    int64_t num = aspect43 ? 6 : 1, den = aspect43 ? 5 : 1;
    int32_t sw = 0, sh = 0;
    memset(L, 0, sizeof *L);
    L->cw = cw < 1 ? 1 : cw;
    L->ch = ch < 1 ? 1 : ch;
    L->fw = fw < 1 ? 1 : fw;
    L->fh = fh < 1 ? 1 : fh;
    if (mode == PRESENT_INTEGER) {
        int32_t k = L->cw / L->fw;
        while (k > 0 && (int64_t)L->fh * k * num / den > L->ch) k--;
        if (k > 0) {
            sw = L->fw * k;
            sh = (int32_t)((int64_t)L->fh * k * num / den);
        }
    }
    if (sw == 0) {
        /* fit: the frame's aspect fw : fh * num / den */
        int64_t aw = (int64_t)L->fw * den, ah = (int64_t)L->fh * num;
        if ((int64_t)L->cw * ah <= (int64_t)L->ch * aw) {
            sw = L->cw;
            sh = (int32_t)(((int64_t)L->cw * ah + aw / 2) / aw);
        } else {
            sh = L->ch;
            sw = (int32_t)(((int64_t)L->ch * aw + ah / 2) / ah);
        }
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
    }
    L->sw = sw;
    L->sh = sh;
    L->ox = (L->cw - sw) / 2;
    L->oy = (L->ch - sh) / 2;
}

void present_scale(const PresentLayout *L, const uint32_t *frame, uint32_t *client)
{
    static int32_t *xmap;
    static int32_t xmap_n;
    int32_t x, y, last_sy = -1;
    size_t cw = (size_t)L->cw;
    if (xmap_n < L->sw) {
        free(xmap);
        xmap = (int32_t *)malloc((size_t)L->sw * sizeof *xmap);
        xmap_n = xmap ? L->sw : 0;
        if (xmap == NULL) return;
    }
    for (x = 0; x < L->sw; x++) xmap[x] = (int32_t)(((2 * (int64_t)x + 1) * L->fw) / (2 * (int64_t)L->sw));
    /* the border */
    for (y = 0; y < L->oy; y++) memset(client + (size_t)y * cw, 0, cw * 4);
    for (y = L->oy + L->sh; y < L->ch; y++) memset(client + (size_t)y * cw, 0, cw * 4);
    for (y = 0; y < L->sh; y++) {
        uint32_t *row = client + (size_t)(y + L->oy) * cw;
        int32_t sy = (int32_t)(((2 * (int64_t)y + 1) * L->fh) / (2 * (int64_t)L->sh));
        if (L->ox > 0) memset(row, 0, (size_t)L->ox * 4);
        if (L->ox + L->sw < L->cw) memset(row + L->ox + L->sw, 0, (size_t)(L->cw - L->ox - L->sw) * 4);
        if (sy == last_sy) {
            memcpy(row + L->ox, row - cw + L->ox, (size_t)L->sw * 4);
        } else {
            const uint32_t *src = frame + (size_t)sy * (size_t)L->fw;
            uint32_t *d = row + L->ox;
            for (x = 0; x < L->sw; x++) d[x] = src[xmap[x]];
        }
        last_sy = sy;
    }
}
