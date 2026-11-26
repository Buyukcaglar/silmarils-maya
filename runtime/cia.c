/*
 * cia.c - the two 8520 CIAs: timers A and B on the E clock (colour clock / 5), the interrupt control register, the
 * time-of-day counters (CIA-A counts vertical syncs, CIA-B horizontal syncs) and the port bits this game reads
 * (CIA-A PRA: the fire buttons and the floppy status lines; bit 1 drives the LED filter).  A timer underflows
 * counter + 1 E clocks after it starts; continuous timers reload from the latch, one-shot timers stop.
 * Portable C.
 */
#include <string.h>

#include "amiga.h"

Cia g_cia[2];

void cia_reset(void)
{
    memset(g_cia, 0, sizeof g_cia);
    g_cia[0].ta.latch = g_cia[0].tb.latch = g_cia[1].ta.latch = g_cia[1].tb.latch = 0xffff;
    g_cia[0].ta.counter = g_cia[0].tb.counter = g_cia[1].ta.counter = g_cia[1].tb.counter = 0xffff;
    g_cia[0].ddra = 0x03;          /* OVL and the LED are outputs */
    g_cia[0].pra = 0x00;           /* LED on: the filter on, as Kickstart leaves it */
    g_cia[1].ddrb = 0xff;
    g_cia[1].prb = 0xff;
    paula_led(1);
}

static void raise(int which, uint8_t bit)
{
    Cia *c = &g_cia[which];
    c->icr_flags |= bit;
    if ((c->icr_mask & bit) && !c->irq) {
        c->irq = 1;
        intreq_set(which == 0 ? 0x0008u : 0x2000u);
    }
}

static int64_t e_now(int64_t cc)
{
    return cc / 5;
}

static void timer_run(int which, CiaTimer *t, int64_t ticks, uint8_t bit)
{
    while (t->running && ticks > 0) {
        if ((int64_t)t->counter + 1 > ticks) {
            t->counter = (uint16_t)(t->counter - ticks);
            return;
        }
        ticks -= (int64_t)t->counter + 1;
        t->counter = t->latch;
        raise(which, bit);
        if (t->oneshot) {
            t->running = 0;
            if (bit == 1) g_cia[which].cra &= (uint8_t)~1u;
            else g_cia[which].crb &= (uint8_t)~1u;
        }
        if (t->latch == 0 && !t->oneshot) {
            ticks %= 1;           /* a zero period: one underflow per E clock; do not loop without end */
        }
    }
}

void cia_run(int64_t cc)
{
    int k;
    int64_t e = e_now(cc);
    for (k = 0; k < 2; k++) {
        Cia *c = &g_cia[k];
        int64_t ticks = e - c->last_e;
        if (ticks <= 0) continue;
        c->last_e = e;
        timer_run(k, &c->ta, ticks, 1);
        if (!(c->crb & 0x40)) {             /* timer B on the E clock (INMODE 0); counting TA's underflows: not used */
            timer_run(k, &c->tb, ticks, 2);
        }
    }
}

int64_t cia_next_event(void)
{
    int64_t best = INT64_MAX;
    int k;
    for (k = 0; k < 2; k++) {
        Cia *c = &g_cia[k];
        if (c->ta.running) {
            int64_t e = (c->last_e + (int64_t)c->ta.counter + 1) * 5;
            if (e < best) best = e;
        }
        if (c->tb.running && !(c->crb & 0x40)) {
            int64_t e = (c->last_e + (int64_t)c->tb.counter + 1) * 5;
            if (e < best) best = e;
        }
    }
    return best;
}

void cia_tod_tick(int which)
{
    Cia *c = &g_cia[which];
    if (c->tod_stopped) return;
    c->tod = (c->tod + 1) & 0xffffffu;
    if (c->tod == c->alarm) raise(which, 4);
}

static uint8_t pra_inputs(void)
{
    /* CIA-A PRA: 7 /FIR1, 6 /FIR0 (the left mouse button), 5 /RDY, 4 /TK0, 3 /WPRO, 2 /CHNG: all inactive (1)
     * except a pressed button */
    uint8_t v = 0xfc;
    if (H.buttons & 1) v &= (uint8_t)~0x40u;
    if (H.fire2) v &= (uint8_t)~0x80u;
    return v;
}

uint32_t cia_read(int which, int reg)
{
    Cia *c = &g_cia[which];
    hw_sync(M.cyc);
    switch (reg) {
    case 0x0:
        if (which == 0) return (uint32_t)((pra_inputs() & ~c->ddra) | (c->pra & c->ddra));
        return (uint32_t)((0xff & ~c->ddra) | (c->pra & c->ddra));
    case 0x1: return (uint32_t)((0xff & ~c->ddrb) | (c->prb & c->ddrb));
    case 0x2: return c->ddra;
    case 0x3: return c->ddrb;
    case 0x4: return c->ta.counter & 0xff;
    case 0x5: return c->ta.counter >> 8;
    case 0x6: return c->tb.counter & 0xff;
    case 0x7: return c->tb.counter >> 8;
    case 0x8: {
        uint32_t t = c->tod_latched ? c->tod_latch : c->tod;
        c->tod_latched = 0;
        return t & 0xff;
    }
    case 0x9: return ((c->tod_latched ? c->tod_latch : c->tod) >> 8) & 0xff;
    case 0xa:
        c->tod_latched = 1;
        c->tod_latch = c->tod;
        return (c->tod >> 16) & 0xff;
    case 0xc: return c->sdr;
    case 0xd: {
        uint8_t v = c->icr_flags;
        if (c->irq) v |= 0x80;
        c->icr_flags = 0;
        c->irq = 0;
        return v;
    }
    case 0xe: return c->cra;
    case 0xf: return c->crb;
    default: return 0xff;
    }
}

static void timer_ctrl(Cia *c, CiaTimer *t, uint8_t v, int is_a)
{
    t->oneshot = (v & 0x08) != 0;
    if (v & 0x10) t->counter = t->latch;      /* LOAD strobe */
    t->running = (v & 0x01) != 0;
    if (is_a) c->cra = (uint8_t)(v & ~0x10u);
    else c->crb = (uint8_t)(v & ~0x10u);
}

static void timer_hi(Cia *c, CiaTimer *t, uint8_t v, int is_a)
{
    t->latch = (uint16_t)((t->latch & 0xff) | (v << 8));
    if (!t->running) {
        t->counter = t->latch;
    }
    if (t->oneshot) {               /* one-shot: writing the high byte loads and starts the timer */
        t->counter = t->latch;
        t->running = 1;
        if (is_a) c->cra |= 1; else c->crb |= 1;
    }
}

void cia_write(int which, int reg, uint32_t val)
{
    Cia *c = &g_cia[which];
    uint8_t v = (uint8_t)val;
    hw_sync(M.cyc);
    switch (reg) {
    case 0x0:
        c->pra = v;
        if (which == 0) paula_led(((v | ~c->ddra) & 2) == 0);
        break;
    case 0x1: c->prb = v; break;
    case 0x2:
        c->ddra = v;
        if (which == 0) paula_led(((c->pra | ~v) & 2) == 0);
        break;
    case 0x3: c->ddrb = v; break;
    case 0x4: c->ta.latch = (uint16_t)((c->ta.latch & 0xff00) | v); break;
    case 0x5: timer_hi(c, &c->ta, v, 1); M.ev = M.cyc; break;
    case 0x6: c->tb.latch = (uint16_t)((c->tb.latch & 0xff00) | v); break;
    case 0x7: timer_hi(c, &c->tb, v, 0); M.ev = M.cyc; break;
    case 0x8:
        if (c->crb & 0x80) c->alarm = (c->alarm & 0xffff00u) | v;
        else { c->tod = (c->tod & 0xffff00u) | v; c->tod_stopped = 0; }
        break;
    case 0x9:
        if (c->crb & 0x80) c->alarm = (c->alarm & 0xff00ffu) | ((uint32_t)v << 8);
        else c->tod = (c->tod & 0xff00ffu) | ((uint32_t)v << 8);
        break;
    case 0xa:
        if (c->crb & 0x80) c->alarm = (c->alarm & 0x00ffffu) | ((uint32_t)v << 16);
        else { c->tod = (c->tod & 0x00ffffu) | ((uint32_t)v << 16); c->tod_stopped = 1; }
        break;
    case 0xc: c->sdr = v; break;
    case 0xd:
        if (v & 0x80) c->icr_mask |= (uint8_t)(v & 0x1f);
        else c->icr_mask &= (uint8_t)~(v & 0x1f);
        if ((c->icr_flags & c->icr_mask) && !c->irq) {
            c->irq = 1;
            intreq_set(which == 0 ? 0x0008u : 0x2000u);
        }
        break;
    case 0xe: timer_ctrl(c, &c->ta, v, 1); M.ev = M.cyc; break;
    case 0xf: timer_ctrl(c, &c->tb, v, 0); M.ev = M.cyc; break;
    default: break;
    }
}
