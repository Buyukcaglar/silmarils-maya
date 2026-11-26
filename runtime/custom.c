/*
 * custom.c - Agnus and Denise as this game uses them (OCS, PAL): the beam, the interrupt and DMA control
 * registers, the copper (run at scanline granularity), and the low-res bitplane display drawn line by line into the
 * display window.  Paula's audio is in paula.c, the blitter in blitter.c.  Portable C.
 */
#include <string.h>

#include "amiga.h"

Custom H;
Frame g_frame;
Frame g_shown;

static int g_fb_top;          /* the display window's first line this frame */

void custom_reset(void)
{
    memset(&H, 0, sizeof H);
    H.diwstrt = 0x2c81;
    H.diwstop = 0x2cc1;
    H.ddfstrt = 0x38;
    H.ddfstop = 0xd0;
    H.copstop = 1;
    H.bzero = 1;
    g_frame.w = g_shown.w = 320;
    g_frame.h = g_shown.h = 200;
    memset(g_frame.px, 0, sizeof g_frame.px);
    memset(g_shown.px, 0, sizeof g_shown.px);
}

void intreq_set(uint16_t bits)
{
    if ((H.intreq & bits) != bits) {
        H.intreq |= bits;
        M.ev = M.cyc;
    }
}

/* ------------------------------------------------------------------------------------------------ display */

static uint32_t rgb12(uint16_t c)
{
    uint32_t r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
    return (r * 17u << 16) | (g * 17u << 8) | (b * 17u);
}

static int diw_vstart(void) { return H.diwstrt >> 8; }
static int diw_vstop(void)
{
    int v = H.diwstop >> 8;
    return (v & 0x80) ? v : v + 0x100;          /* OCS: V8 of the stop is the complement of V7 */
}
static int diw_hstart(void) { return H.diwstrt & 0xff; }
static int diw_hstop(void) { return (H.diwstop & 0xff) + 0x100; }

static void frame_begin(void)
{
    int w = diw_hstop() - diw_hstart();
    int h = diw_vstop() - diw_vstart();
    if (w < 16) w = 16;
    if (w > FB_MAXW) w = FB_MAXW;
    if (h < 16) h = 16;
    if (h > FB_MAXH) h = FB_MAXH;
    g_frame.w = w;
    g_frame.h = h;
    g_fb_top = diw_vstart();
}

static void render_line(int line)
{
    int row = line - g_fb_top;
    int w = g_frame.w;
    uint32_t *dst;
    int x;
    int planes;
    int bitmap_on;
    if (row < 0 || row >= g_frame.h) {
        return;
    }
    dst = g_frame.px + (size_t)row * w;
    planes = (H.bplcon0 >> 12) & 7;
    if (planes > 6) planes = 6;
    bitmap_on = (H.dmacon & 0x0300u) == 0x0300u && planes > 0 && line >= diw_vstart() && line < diw_vstop();
    if (!bitmap_on) {
        uint32_t bg = rgb12(H.color[0]);
        for (x = 0; x < w; x++) {
            dst[x] = bg;
        }
        return;
    }
    {
        int words = ((int)((H.ddfstop & 0xfc) - (H.ddfstrt & 0xfc)) >> 3) + 1;
        int first = 2 * (H.ddfstrt & 0xfc) + 17;   /* the first fetched pixel's horizontal position */
        int hs = diw_hstart();
        uint8_t idx[FB_MAXW];
        int p;
        if (words < 1) words = 1;
        if (words > 32) words = 32;
        memset(idx, 0, sizeof idx);
        for (p = 0; p < planes; p++) {
            uint32_t pt = H.bplpt[p] & (CHIP_SIZE - 2);
            int delay = (p & 1) ? ((H.bplcon1 >> 4) & 15) : (H.bplcon1 & 15);
            int start = first + delay - hs;          /* the column of the first fetched pixel */
            int k;
            for (k = 0; k < words; k++) {
                uint32_t a = (pt + 2u * (uint32_t)k) & (CHIP_SIZE - 2);
                uint32_t wd = ((uint32_t)g_ram[a] << 8) | g_ram[a + 1];
                int b;
                if (wd == 0) {
                    continue;
                }
                for (b = 0; b < 16; b++) {
                    int col = start + k * 16 + b;
                    if (col >= 0 && col < w && (wd & (0x8000u >> b))) {
                        idx[col] |= (uint8_t)(1u << p);
                    }
                }
            }
            H.bplpt[p] = pt + 2u * (uint32_t)words + (uint32_t)(int32_t)((p & 1) ? H.bpl2mod : H.bpl1mod);
        }
        for (x = 0; x < w; x++) {
            dst[x] = rgb12(H.color[idx[x] & 31]);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ copper */

static void copper_restart(void)
{
    H.coppc = H.cop1lc;
    H.copwait = 0;
    H.copstop = 0;
}

static void copper_line(int line)
{
    int n;
    if ((H.dmacon & 0x0280u) != 0x0280u || H.copstop) {
        return;
    }
    for (n = 0; n < 512; n++) {
        if (H.copwait) {
            int vp = line & 0xff;
            int wv = H.copw1 >> 8, wh = H.copw1 & 0xfe;
            int vm = ((H.copw2 >> 8) & 0x7f) | 0x80, hm = H.copw2 & 0xfe;
            int cv = vp & vm, tv = wv & vm;
            int ok = cv > tv || (cv == tv && (wh & hm) <= 0xe2);
            if (line >= 256 && tv >= 0x80 && cv < 0x80) {
                ok = 0;          /* lines 256+: only the low 8 bits compare (the PAL WAIT $FFDF trick) */
            }
            if (!ok) {
                return;
            }
            H.copwait = 0;
        }
        {
            uint32_t a = H.coppc & (CHIP_SIZE - 2);
            uint16_t w1 = (uint16_t)((g_ram[a] << 8) | g_ram[a + 1]);
            uint16_t w2 = (uint16_t)((g_ram[a + 2] << 8) | g_ram[a + 3]);
            H.coppc += 4;
            if (!(w1 & 1)) {
                uint32_t reg = w1 & 0x1feu;
                if (reg < 0x40u || (reg < 0x80u && !H.cdang)) {
                    H.copstop = 1;
                    return;
                }
                custom_write(reg, w2);
                if (H.copstop) {
                    return;
                }
                continue;
            }
            if (!(w2 & 1)) {          /* WAIT */
                H.copw1 = w1;
                H.copw2 = w2;
                H.copwait = 1;
                continue;
            }
            {                         /* SKIP: skip the next instruction when the beam is past the position */
                int vp = line & 0xff;
                int vm = ((w2 >> 8) & 0x7f) | 0x80;
                if ((vp & vm) >= ((w1 >> 8) & vm)) {
                    H.coppc += 4;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------------ time */

static void vblank(void)
{
    memcpy(&g_shown, &g_frame, sizeof g_shown);
    H.frames++;
    H.frame_done = 1;
    intreq_set(0x0020u);                 /* VERTB */
    cia_tod_tick(0);                     /* CIA-A TOD counts vertical syncs */
    copper_restart();
    hle_frame();
    frame_begin();
}

static void new_line(void)
{
    H.line++;
    if (H.line >= FRAME_LINES) {
        H.line = 0;
        vblank();
    }
    H.line_start = H.cc;
    cia_tod_tick(1);                     /* CIA-B TOD counts horizontal syncs */
    copper_line(H.line);
    render_line(H.line);
}

void hw_sync(int64_t cyc)
{
    int64_t t = cyc >> 1;
    while (H.cc < t) {
        int64_t stop = H.line_start + LINE_CC;
        int64_t ce = cia_next_event();
        if (ce < stop) stop = ce;
        if (t < stop) stop = t;
        if (stop <= H.cc) stop = H.cc + 1;
        paula_run(stop - H.cc);
        H.cc = stop;
        cia_run(stop);
        if (H.cc >= H.line_start + LINE_CC) {
            new_line();
        }
    }
}

int64_t hw_next_event(void)
{
    int64_t e = H.line_start + LINE_CC;
    int64_t ce = cia_next_event();
    if (ce < e) e = ce;
    return e * 2;
}

/* ------------------------------------------------------------------------------------------------ registers */

static uint16_t vhpos(void)
{
    int h = (int)(H.cc - H.line_start);
    if (h > 0xe2) h = 0xe2;
    return (uint16_t)(((H.line & 0xff) << 8) | (h & 0xff));
}

uint32_t custom_read(uint32_t reg)
{
    hw_sync(M.cyc);
    switch (reg) {
    case 0x002: return (H.dmacon & 0x07ffu) | (H.bzero ? 0x2000u : 0);
    case 0x004: return 0x8000u | ((H.line >> 8) & 1);   /* LOF (long frames), V8; OCS PAL Agnus id 0 */
    case 0x006: return vhpos();
    case 0x00a: return H.joy0dat;
    case 0x00c: return H.joy1dat;
    case 0x010: return H.adkcon;
    case 0x012: case 0x014: return 0;
    case 0x016: return (H.buttons & 2) ? 0xbb00u : 0xff00u;   /* POTGOR: right button pulls DATLY low */
    case 0x018: return 0;
    case 0x01a: return 0;
    case 0x01c: return H.intena;
    case 0x01e: return H.intreq;
    default: return 0xffffu;     /* a write-only register reads as noise; all ones */
    }
}

void custom_write(uint32_t reg, uint32_t v)
{
    v &= 0xffffu;
    hw_sync(M.cyc);
    H.regs[(reg >> 1) & 0xff] = (uint16_t)v;
    if (reg >= 0x0a0 && reg < 0x0e0) {
        paula_write(reg, v);
        return;
    }
    if (reg >= 0x0e0 && reg < 0x0f8) {
        int p = (int)((reg - 0x0e0) >> 2);
        if (reg & 2) H.bplpt[p] = (H.bplpt[p] & 0xffff0000u) | (v & 0xfffeu);
        else H.bplpt[p] = (H.bplpt[p] & 0xffffu) | ((v & 0x1fu) << 16);
        return;
    }
    if (reg >= 0x180 && reg < 0x1c0) {
        H.color[(reg - 0x180) >> 1] = (uint16_t)(v & 0x0fffu);
        return;
    }
    switch (reg) {
    case 0x02e: H.cdang = (v & 2) != 0; break;
    case 0x034: break;                   /* POTGO */
    case 0x036: H.joy0dat = H.joy1dat = (uint16_t)v; break;   /* JOYTEST */
    case 0x040: H.bltcon0 = (uint16_t)v; break;
    case 0x042: H.bltcon1 = (uint16_t)v; break;
    case 0x044: H.bltafwm = (uint16_t)v; break;
    case 0x046: H.bltalwm = (uint16_t)v; break;
    case 0x048: H.bltcpt = (H.bltcpt & 0xffffu) | ((v & 0x1fu) << 16); break;
    case 0x04a: H.bltcpt = (H.bltcpt & 0xffff0000u) | (v & 0xfffeu); break;
    case 0x04c: H.bltbpt = (H.bltbpt & 0xffffu) | ((v & 0x1fu) << 16); break;
    case 0x04e: H.bltbpt = (H.bltbpt & 0xffff0000u) | (v & 0xfffeu); break;
    case 0x050: H.bltapt = (H.bltapt & 0xffffu) | ((v & 0x1fu) << 16); break;
    case 0x052: H.bltapt = (H.bltapt & 0xffff0000u) | (v & 0xfffeu); break;
    case 0x054: H.bltdpt = (H.bltdpt & 0xffffu) | ((v & 0x1fu) << 16); break;
    case 0x056: H.bltdpt = (H.bltdpt & 0xffff0000u) | (v & 0xfffeu); break;
    case 0x058: blitter_start(); break;
    case 0x060: H.bltcmod = (int16_t)(v & 0xfffeu); break;
    case 0x062: H.bltbmod = (int16_t)(v & 0xfffeu); break;
    case 0x064: H.bltamod = (int16_t)(v & 0xfffeu); break;
    case 0x066: H.bltdmod = (int16_t)(v & 0xfffeu); break;
    case 0x070: H.bltcdat = (uint16_t)v; break;
    case 0x072: H.bltbdat = (uint16_t)v; break;
    case 0x074: H.bltadat = (uint16_t)v; break;
    case 0x080: H.cop1lc = (H.cop1lc & 0xffffu) | ((v & 0x1fu) << 16); break;
    case 0x082: H.cop1lc = (H.cop1lc & 0xffff0000u) | (v & 0xfffeu); break;
    case 0x084: H.cop2lc = (H.cop2lc & 0xffffu) | ((v & 0x1fu) << 16); break;
    case 0x086: H.cop2lc = (H.cop2lc & 0xffff0000u) | (v & 0xfffeu); break;
    case 0x088: H.coppc = H.cop1lc; H.copwait = 0; H.copstop = 0; break;
    case 0x08a: H.coppc = H.cop2lc; H.copwait = 0; H.copstop = 0; break;
    case 0x08e: H.diwstrt = (uint16_t)v; break;
    case 0x090: H.diwstop = (uint16_t)v; break;
    case 0x092: H.ddfstrt = (uint16_t)(v & 0xfc); break;
    case 0x094: H.ddfstop = (uint16_t)(v & 0xfc); break;
    case 0x096: {
        uint16_t old = H.dmacon;
        if (v & 0x8000u) H.dmacon |= (uint16_t)(v & 0x07ffu);
        else H.dmacon &= (uint16_t)~(v & 0x07ffu);
        paula_dmacon(old, H.dmacon);
        break;
    }
    case 0x09a:
        if (v & 0x8000u) H.intena |= (uint16_t)(v & 0x7fffu);
        else H.intena &= (uint16_t)~(v & 0x7fffu);
        M.ev = M.cyc;
        break;
    case 0x09c:
        if (v & 0x8000u) H.intreq |= (uint16_t)(v & 0x7fffu);
        else H.intreq &= (uint16_t)~(v & 0x7fffu);
        if (g_cia[0].irq) H.intreq |= 0x0008u;      /* the CIAs' lines are levels: still asserted, set again */
        if (g_cia[1].irq) H.intreq |= 0x2000u;
        M.ev = M.cyc;
        break;
    case 0x09e:
        if (v & 0x8000u) H.adkcon |= (uint16_t)(v & 0x7fffu);
        else H.adkcon &= (uint16_t)~(v & 0x7fffu);
        break;
    case 0x100: H.bplcon0 = (uint16_t)v; break;
    case 0x102: H.bplcon1 = (uint16_t)v; break;
    case 0x104: H.bplcon2 = (uint16_t)v; break;
    case 0x108: H.bpl1mod = (int16_t)(v & 0xfffeu); break;
    case 0x10a: H.bpl2mod = (int16_t)(v & 0xfffeu); break;
    default: break;
    }
}
