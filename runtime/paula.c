/*
 * paula.c - Paula's four audio DMA channels at colour-clock accuracy, the audio interrupts, and the output: stereo
 * (channels 0 and 3 left, 1 and 2 right) at PAULA_RATE through the A500's filters (a fixed one-pole low-pass at
 * 4,421 Hz, the LED filter while CIA-A PRA bit 1 is 0: a two-pole low-pass at 3,091 Hz, Q 0.66, and a 5.2 Hz
 * high-pass).  The sample timing and filter values follow Civ64's pamiga_paula.c (same author, MIT); the channel
 * state machine adds the interrupts and AUDxDAT.  Portable C.
 *
 * A channel plays 8-bit signed samples, two to a word, each lasting AUDxPER colour clocks.  Turning its DMA on
 * loads the pointer and length latches and raises its interrupt (the block started: the player may write the next
 * block's latches); when the length runs out the latches load again and the interrupt is raised again.
 */
#include <math.h>
#include <string.h>

#include "amiga.h"

#define TICKS_PER_CC ((int64_t)PAULA_RATE)       /* time unit: a colour clock is PAULA_RATE ticks */
#define TICKS_PER_OUT ((int64_t)CC_HZ)           /* an output sample is CC_HZ ticks */
#define RING 16384u                              /* stereo frames buffered */

typedef struct Chan {
    uint32_t lc, pt;
    uint16_t len, per, vol;
    uint32_t left;           /* words still to fetch in this block */
    uint16_t word;
    int half;                /* 0: the high byte plays, 1 the low */
    int on;
    int8_t sample;
    int64_t rem;             /* ticks until the next sample */
} Chan;

static Chan g_ch[4];
static int64_t g_pos;        /* ticks into the current output sample */
static int64_t g_area[2];    /* sum of level x ticks */
static int16_t g_ring[RING * 2];
static size_t g_head, g_count;
static int g_led;
/* filters */
static double g_lp[2], g_l1[2], g_l2[2], g_m1[2], g_m2[2], g_hpi[2], g_hpo[2];
static double g_lp_a, g_led_b0, g_led_b1, g_led_b2, g_led_a1, g_led_a2, g_hp_a;

static uint16_t chip_word(uint32_t a)
{
    a &= CHIP_SIZE - 2;
    return (uint16_t)((g_ram[a] << 8) | g_ram[a + 1]);
}

static int64_t per_ticks(const Chan *c)
{
    uint32_t p = c->per;
    if (p < 64) p = 64;            /* below the DMA's limit a period reads its fetches late: treat as 64 */
    return (int64_t)p * TICKS_PER_CC;
}

static void chan_fetch(int k)
{
    Chan *c = &g_ch[k];
    if (c->left == 0) {
        c->pt = c->lc;
        c->left = c->len ? c->len : 65536u;
        intreq_set((uint16_t)(0x0080u << k));
    }
    c->word = chip_word(c->pt);
    c->pt += 2;
    c->left--;
}

static void chan_start(int k)
{
    Chan *c = &g_ch[k];
    c->on = 1;
    c->left = 0;
    chan_fetch(k);                 /* loads the latches, raises the interrupt */
    c->half = 0;
    c->sample = (int8_t)(c->word >> 8);
    c->rem = per_ticks(c);
}

void paula_reset(void)
{
    double fs = PAULA_RATE;
    double w, cs, al, a0;
    memset(g_ch, 0, sizeof g_ch);
    g_pos = 0;
    g_area[0] = g_area[1] = 0;
    g_head = g_count = 0;
    memset(g_lp, 0, sizeof g_lp); memset(g_l1, 0, sizeof g_l1); memset(g_l2, 0, sizeof g_l2);
    memset(g_m1, 0, sizeof g_m1); memset(g_m2, 0, sizeof g_m2);
    memset(g_hpi, 0, sizeof g_hpi); memset(g_hpo, 0, sizeof g_hpo);
    g_led = 1;
    g_lp_a = 1.0 - exp(-2.0 * 3.14159265358979 * 4421.0 / fs);
    w = 2.0 * 3.14159265358979 * 3091.0 / fs;
    cs = cos(w);
    al = sin(w) / (2.0 * 0.66);
    a0 = 1.0 + al;
    g_led_b0 = (1.0 - cs) / 2.0 / a0;
    g_led_b1 = (1.0 - cs) / a0;
    g_led_b2 = (1.0 - cs) / 2.0 / a0;
    g_led_a1 = -2.0 * cs / a0;
    g_led_a2 = (1.0 - al) / a0;
    g_hp_a = exp(-2.0 * 3.14159265358979 * 5.2 / fs);
}

void paula_led(int on)
{
    g_led = on;
}

void paula_write(uint32_t reg, uint32_t v)
{
    int k = (int)((reg - 0x0a0) >> 4);
    Chan *c = &g_ch[k];
    switch (reg & 0x0e) {
    case 0x0: c->lc = (c->lc & 0xffffu) | ((v & 0x1fu) << 16); break;
    case 0x2: c->lc = (c->lc & 0xffff0000u) | (v & 0xfffeu); break;
    case 0x4: c->len = (uint16_t)v; break;
    case 0x6: c->per = (uint16_t)v; break;
    case 0x8: c->vol = (uint16_t)((v & 0x40) ? 64 : (v & 0x3f)); break;
    case 0xa:
        if (!c->on) {              /* CPU-driven output: the word plays once */
            c->word = (uint16_t)v;
            c->sample = (int8_t)(v >> 8);
        }
        break;
    default: break;
    }
}

void paula_dmacon(uint16_t old, uint16_t now)
{
    int k;
    int was_master = (old & 0x0200u) != 0, is_master = (now & 0x0200u) != 0;
    for (k = 0; k < 4; k++) {
        int was = was_master && (old & (1u << k));
        int is = is_master && (now & (1u << k));
        if (is && !was) {
            chan_start(k);
        } else if (!is && was) {
            g_ch[k].on = 0;
            g_ch[k].sample = 0;
        }
    }
}

static void out_sample(void)
{
    int s;
    for (s = 0; s < 2; s++) {
        double x = (double)g_area[s] / (double)TICKS_PER_OUT;    /* -16,384 .. 16,256 (two channels) */
        double y;
        g_lp[s] += g_lp_a * (x - g_lp[s]);
        y = g_lp[s];
        if (g_led) {
            double z = g_led_b0 * y + g_led_b1 * g_l1[s] + g_led_b2 * g_l2[s] - g_led_a1 * g_m1[s] - g_led_a2 * g_m2[s];
            g_l2[s] = g_l1[s]; g_l1[s] = y;
            g_m2[s] = g_m1[s]; g_m1[s] = z;
            y = z;
        }
        {
            double hp = g_hp_a * (g_hpo[s] + y - g_hpi[s]);
            g_hpi[s] = y;
            g_hpo[s] = hp;
            y = hp * 1.9;
        }
        if (y > 32767.0) y = 32767.0;
        if (y < -32768.0) y = -32768.0;
        if (g_count < RING) {
            size_t at = (g_head + g_count) % RING;
            g_ring[at * 2 + (size_t)s] = (int16_t)y;
        }
        g_area[s] = 0;
    }
    if (g_count < RING) {
        g_count++;
    }
}

void paula_run(int64_t cc)
{
    int64_t t = cc * TICKS_PER_CC;
    while (t > 0) {
        int64_t step = TICKS_PER_OUT - g_pos;
        int k;
        int32_t lvl[2];
        for (k = 0; k < 4; k++) {
            if (g_ch[k].on && g_ch[k].rem < step) step = g_ch[k].rem;
        }
        if (step > t) step = t;
        if (step < 1) step = 1;
        lvl[0] = (int32_t)g_ch[0].sample * g_ch[0].vol + (int32_t)g_ch[3].sample * g_ch[3].vol;
        lvl[1] = (int32_t)g_ch[1].sample * g_ch[1].vol + (int32_t)g_ch[2].sample * g_ch[2].vol;
        g_area[0] += (int64_t)lvl[0] * step;
        g_area[1] += (int64_t)lvl[1] * step;
        g_pos += step;
        t -= step;
        if (g_pos >= TICKS_PER_OUT) {
            g_pos -= TICKS_PER_OUT;
            out_sample();
        }
        for (k = 0; k < 4; k++) {
            Chan *c = &g_ch[k];
            if (!c->on) continue;
            c->rem -= step;
            if (c->rem <= 0) {
                if (c->half == 0) {
                    c->half = 1;
                    c->sample = (int8_t)(c->word & 0xff);
                } else {
                    chan_fetch(k);
                    c->half = 0;
                    c->sample = (int8_t)(c->word >> 8);
                }
                c->rem += per_ticks(c);
            }
        }
    }
}

size_t paula_take(int16_t *out, size_t frames)
{
    size_t n = frames < g_count ? frames : g_count;
    size_t k;
    for (k = 0; k < n; k++) {
        size_t at = (g_head + k) % RING;
        out[2 * k] = g_ring[at * 2];
        out[2 * k + 1] = g_ring[at * 2 + 1];
    }
    g_head = (g_head + n) % RING;
    g_count -= n;
    return n;
}
