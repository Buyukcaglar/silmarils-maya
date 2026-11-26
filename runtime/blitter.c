/*
 * blitter.c - the OCS blitter, run to completion when BLTSIZE is written: area mode (four channels, shifts,
 * first/last word masks, descending mode, inclusive/exclusive fill) and line mode.  The blitter is never busy
 * afterwards (DMACONR BBUSY 0) and raises its interrupt at once.  Portable C.
 */
#include "amiga.h"

static uint16_t cw(uint32_t a)
{
    a &= CHIP_SIZE - 2;
    return (uint16_t)((g_ram[a] << 8) | g_ram[a + 1]);
}

static void cwr(uint32_t a, uint16_t v)
{
    a &= CHIP_SIZE - 2;
    g_ram[a] = (uint8_t)(v >> 8);
    g_ram[a + 1] = (uint8_t)v;
}

static uint16_t minterm(uint8_t lf, uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t r = 0;
    if (lf & 0x01) r |= (uint16_t)(~a & ~b & ~c);
    if (lf & 0x02) r |= (uint16_t)(~a & ~b & c);
    if (lf & 0x04) r |= (uint16_t)(~a & b & ~c);
    if (lf & 0x08) r |= (uint16_t)(~a & b & c);
    if (lf & 0x10) r |= (uint16_t)(a & ~b & ~c);
    if (lf & 0x20) r |= (uint16_t)(a & ~b & c);
    if (lf & 0x40) r |= (uint16_t)(a & b & ~c);
    if (lf & 0x80) r |= (uint16_t)(a & b & c);
    return r;
}

static void blit_area(int w, int h)
{
    uint16_t con0 = H.bltcon0, con1 = H.bltcon1;
    int usea = con0 & 0x0800, useb = con0 & 0x0400, usec = con0 & 0x0200, used = con0 & 0x0100;
    int ash = con0 >> 12, bsh = con1 >> 12;
    int desc = con1 & 2;
    int fill = con1 & 0x18;
    int efe = con1 & 0x10;
    uint8_t lf = (uint8_t)con0;
    int32_t step = desc ? -2 : 2;
    uint32_t apt = H.bltapt, bpt = H.bltbpt, cpt = H.bltcpt, dpt = H.bltdpt;
    uint16_t aold = 0, bold = 0;
    uint16_t adat = H.bltadat, bdat = H.bltbdat, cdat = H.bltcdat;
    int x, y;
    int zero = 1;
    for (y = 0; y < h; y++) {
        int fc = (con1 & 4) != 0;            /* FCI: the fill carry at the start of a row */
        for (x = 0; x < w; x++) {
            uint16_t a, b, d, am;
            if (usea) { adat = cw(apt); apt += (uint32_t)step; }
            if (useb) { bdat = cw(bpt); bpt += (uint32_t)step; }
            if (usec) { cdat = cw(cpt); cpt += (uint32_t)step; }
            am = adat;
            if (x == 0) am &= H.bltafwm;
            if (x == w - 1) am &= H.bltalwm;
            if (!desc) {
                a = (uint16_t)((((uint32_t)aold << 16) | am) >> ash);
                b = (uint16_t)((((uint32_t)bold << 16) | bdat) >> bsh);
            } else {
                a = (uint16_t)((((uint32_t)am << 16) | aold) >> (16 - ash));
                b = (uint16_t)((((uint32_t)bdat << 16) | bold) >> (16 - bsh));
            }
            aold = am;
            bold = bdat;
            d = minterm(lf, a, b, cdat);
            if (fill) {
                uint16_t out = 0;
                int bit;
                for (bit = 0; bit < 16; bit++) {
                    uint16_t m = (uint16_t)(1u << bit);
                    int s = (d & m) != 0;
                    if (efe) {
                        int o = fc ^ s;
                        if (o) out |= m;
                        if (s) fc = !fc;
                    } else {
                        if (s) fc = !fc;
                        if (fc || s) out |= m;
                    }
                }
                d = out;
            }
            if (d) zero = 0;
            if (used) { cwr(dpt, d); dpt += (uint32_t)step; }
        }
        if (usea) apt += (uint32_t)(int32_t)(desc ? -H.bltamod : H.bltamod);
        if (useb) bpt += (uint32_t)(int32_t)(desc ? -H.bltbmod : H.bltbmod);
        if (usec) cpt += (uint32_t)(int32_t)(desc ? -H.bltcmod : H.bltcmod);
        if (used) dpt += (uint32_t)(int32_t)(desc ? -H.bltdmod : H.bltdmod);
    }
    H.bltapt = apt; H.bltbpt = bpt; H.bltcpt = cpt; H.bltdpt = dpt;
    H.bltadat = adat; H.bltbdat = bdat; H.bltcdat = cdat;
    H.bzero = zero;
}

/* Line mode as the Hardware Reference Manual sets it up: BLTAPTL the error term (4dy - 2dx), BLTAMOD 4(dy - dx),
 * BLTBMOD 4dy, BLTSIZE height = the length, BLTADAT $8000, BLTBDAT the texture, C and D the start's word, ASH the
 * start's bit.  Octant bits in BLTCON1: SUD (4), SUL (3), AUL (2); SING (1) draws one pixel a row. */
static void blit_line(int len)
{
    uint16_t con0 = H.bltcon0, con1 = H.bltcon1;
    uint8_t lf = (uint8_t)con0;
    int sud = (con1 >> 4) & 1, sul = (con1 >> 3) & 1, aul = (con1 >> 2) & 1;
    int sing = (con1 >> 1) & 1;
    int ash = con0 >> 12;
    int bsh = con1 >> 12;
    int16_t err = (int16_t)(H.bltapt & 0xffff);
    int sign = (con1 & 0x40) != 0;
    uint32_t cpt = H.bltcpt;
    uint16_t tex = H.bltbdat;
    int row_drawn = 0;
    int k;
    int zero = 1;
    tex = (uint16_t)((tex << bsh) | (tex >> ((16 - bsh) & 15)));
    for (k = 0; k < len; k++) {
        uint16_t a = (uint16_t)(H.bltadat >> ash);
        uint16_t b = (uint16_t)((tex >> 15) & 1 ? 0xffff : 0);
        uint16_t c = cw(cpt);
        uint16_t d;
        if (sing && row_drawn) a = 0;
        d = minterm(lf, a, b, c);
        if (d) zero = 0;
        cwr(cpt, d);
        row_drawn = 1;
        tex = (uint16_t)((tex << 1) | (tex >> 15));
        /* the step.  SUD ("sometimes up or down") 1: the move the error decides is vertical and the move made
         * every step horizontal; 0 the other way round.  SUL gives the sometimes move's direction (1 up/left),
         * AUL the always move's. */
        {
            int sometimes = !sign;         /* decided on the error before this step's update */
            err = (int16_t)(err + (sometimes ? H.bltamod : H.bltbmod));
            if (sometimes) {
                if (sud) {
                    cpt += (uint32_t)(int32_t)(sul ? -H.bltcmod : H.bltcmod);
                    row_drawn = 0;
                } else if (sul) {
                    if (--ash < 0) { ash = 15; cpt -= 2; }
                } else {
                    if (++ash > 15) { ash = 0; cpt += 2; }
                }
            }
            if (sud) {
                if (aul) { if (--ash < 0) { ash = 15; cpt -= 2; } }
                else { if (++ash > 15) { ash = 0; cpt += 2; } }
            } else {
                cpt += (uint32_t)(int32_t)(aul ? -H.bltcmod : H.bltcmod);
                row_drawn = 0;
            }
            sign = err < 0;
        }
    }
    H.bltcpt = H.bltdpt = cpt;
    H.bltapt = (H.bltapt & 0xffff0000u) | (uint16_t)err;
    H.bltcon0 = (uint16_t)((H.bltcon0 & 0x0fffu) | ((unsigned)ash << 12));
    H.bltcon1 = (uint16_t)((H.bltcon1 & ~0x40u) | (sign ? 0x40u : 0));
    H.bzero = zero;
}

void blitter_start(void)
{
    uint16_t size = H.regs[0x58 >> 1];
    int h = size >> 6, w = size & 63;
    if (h == 0) h = 1024;
    if (w == 0) w = 64;
    if (H.bltcon1 & 1) {
        blit_line(h);
    } else {
        blit_area(w, h);
    }
    M.cyc += 4;
    intreq_set(0x0040u);      /* BLIT */
}
