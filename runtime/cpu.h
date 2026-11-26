/*
 * cpu.h - the MC68000 state and the operations the generated code (tools/m68k/gen_c.py) is written in.
 *
 * The state is one global, M.  Memory is a flat 16 MB array (24-bit addresses); the pages of the custom chips
 * ($DFxxxx) and the CIAs ($BFxxxx) go to the hardware model instead (io_read / io_write).  Condition codes are kept
 * as separate 0/1 bytes.  Portable C.
 */
#ifndef MAYA_CPU_H
#define MAYA_CPU_H

#include <stdint.h>

typedef struct M68K {
    uint32_t d[8], a[8];   /* a[7] is the active stack pointer */
    uint32_t pc;
    uint32_t osp;          /* the inactive stack pointer: USP while supervisor, SSP while user */
    uint8_t x, n, z, v, c; /* condition codes, each 0 or 1 */
    uint8_t s, t, ipl;     /* supervisor bit, trace bit (kept, not acted on), interrupt mask 0..7 */
    int64_t cyc;           /* CPU clock cycles since boot */
    int64_t ev;            /* the cycle count at which rt_event() must run */
} M68K;

extern M68K M;

#define RAM_SIZE 0x1000000u
extern uint8_t g_ram[RAM_SIZE + 16];
extern uint8_t g_iopage[256];

uint32_t io_read(uint32_t a, int size);
void io_write(uint32_t a, uint32_t v, int size);

static inline uint32_t RB(uint32_t a)
{
    a &= 0xffffffu;
    if (g_iopage[a >> 16]) return io_read(a, 1);
    return g_ram[a];
}

static inline uint32_t RW(uint32_t a)
{
    a &= 0xffffffu;
    if (g_iopage[a >> 16]) return io_read(a, 2);
    return ((uint32_t)g_ram[a] << 8) | g_ram[a + 1];
}

static inline uint32_t RL(uint32_t a)
{
    return (RW(a) << 16) | RW(a + 2);
}

static inline void WB(uint32_t a, uint32_t v)
{
    a &= 0xffffffu;
    if (g_iopage[a >> 16]) { io_write(a, v & 0xffu, 1); return; }
    g_ram[a] = (uint8_t)v;
}

static inline void WW(uint32_t a, uint32_t v)
{
    a &= 0xffffffu;
    if (g_iopage[a >> 16]) { io_write(a, v & 0xffffu, 2); return; }
    g_ram[a] = (uint8_t)(v >> 8);
    g_ram[a + 1] = (uint8_t)v;
}

static inline void WL(uint32_t a, uint32_t v)
{
    WW(a, v >> 16);
    WW(a + 2, v);
}

static inline void PUSH32(uint32_t v) { M.a[7] -= 4; WL(M.a[7], v); }
static inline void PUSH16(uint32_t v) { M.a[7] -= 2; WW(M.a[7], v); }
static inline uint32_t POP32(void) { uint32_t v = RL(M.a[7]); M.a[7] += 4; return v; }
static inline uint32_t POP16(void) { uint32_t v = RW(M.a[7]); M.a[7] += 2; return v; }

#define SZMASK(sz) ((sz) == 1 ? 0xffu : (sz) == 2 ? 0xffffu : 0xffffffffu)
#define SZSHIFT(sz) ((unsigned)(sz) * 8u - 1u)

static inline uint32_t GET_CCR(void)
{
    return ((uint32_t)M.x << 4) | ((uint32_t)M.n << 3) | ((uint32_t)M.z << 2) | ((uint32_t)M.v << 1) | M.c;
}

static inline uint32_t GET_SR(void)
{
    return ((uint32_t)M.t << 15) | ((uint32_t)M.s << 13) | ((uint32_t)M.ipl << 8) | GET_CCR();
}

static inline void SET_CCR(uint32_t v)
{
    M.x = (v >> 4) & 1; M.n = (v >> 3) & 1; M.z = (v >> 2) & 1; M.v = (v >> 1) & 1; M.c = v & 1;
}

void SET_SR(uint32_t v);      /* machine.c: switches stacks on a change of S, rechecks interrupts */
void RTE(void);
void rt_bad_pc(void);         /* entered a region at an address that is not one of its instructions */
void rt_exception(int vector);

static inline void FLG_LOGIC(uint32_t r, int sz)
{
    r &= SZMASK(sz);
    M.n = (uint8_t)((r >> SZSHIFT(sz)) & 1);
    M.z = r == 0;
    M.v = 0;
    M.c = 0;
}

static inline uint32_t ADD(uint32_t s, uint32_t d, int sz)
{
    uint32_t m = SZMASK(sz);
    unsigned sh = SZSHIFT(sz);
    uint64_t t;
    uint32_t r;
    s &= m; d &= m;
    t = (uint64_t)s + d;
    r = (uint32_t)t & m;
    M.c = M.x = (uint8_t)((t >> (sh + 1)) & 1);
    M.v = (uint8_t)((((s ^ r) & (d ^ r)) >> sh) & 1);
    M.n = (uint8_t)((r >> sh) & 1);
    M.z = r == 0;
    return r;
}

/* d - s */
static inline uint32_t SUB(uint32_t s, uint32_t d, int sz)
{
    uint32_t m = SZMASK(sz);
    unsigned sh = SZSHIFT(sz);
    uint32_t r;
    s &= m; d &= m;
    r = (d - s) & m;
    M.c = M.x = s > d;
    M.v = (uint8_t)((((s ^ d) & (r ^ d)) >> sh) & 1);
    M.n = (uint8_t)((r >> sh) & 1);
    M.z = r == 0;
    return r;
}

static inline void CMP(uint32_t s, uint32_t d, int sz)
{
    uint32_t m = SZMASK(sz);
    unsigned sh = SZSHIFT(sz);
    uint32_t r;
    s &= m; d &= m;
    r = (d - s) & m;
    M.c = s > d;
    M.v = (uint8_t)((((s ^ d) & (r ^ d)) >> sh) & 1);
    M.n = (uint8_t)((r >> sh) & 1);
    M.z = r == 0;
}

static inline uint32_t ADDX(uint32_t s, uint32_t d, int sz)
{
    uint32_t m = SZMASK(sz);
    unsigned sh = SZSHIFT(sz);
    uint64_t t;
    uint32_t r;
    s &= m; d &= m;
    t = (uint64_t)s + d + M.x;
    r = (uint32_t)t & m;
    M.c = M.x = (uint8_t)((t >> (sh + 1)) & 1);
    M.v = (uint8_t)((((s ^ r) & (d ^ r)) >> sh) & 1);
    M.n = (uint8_t)((r >> sh) & 1);
    if (r != 0) M.z = 0;
    return r;
}

static inline uint32_t SUBX(uint32_t s, uint32_t d, int sz)
{
    uint32_t m = SZMASK(sz);
    unsigned sh = SZSHIFT(sz);
    uint32_t r;
    s &= m; d &= m;
    r = (d - s - M.x) & m;
    M.c = M.x = ((uint64_t)s + M.x) > d;
    M.v = (uint8_t)((((s ^ d) & (r ^ d)) >> sh) & 1);
    M.n = (uint8_t)((r >> sh) & 1);
    if (r != 0) M.z = 0;
    return r;
}

static inline uint32_t ABCD(uint32_t s, uint32_t d)
{
    unsigned lo = (s & 15) + (d & 15) + M.x;
    unsigned r = (s & 0xf0) + (d & 0xf0) + lo;
    if (lo > 9) r += 6;
    if (r > 0x99) { r += 0x60; M.c = M.x = 1; } else { M.c = M.x = 0; }
    r &= 0xff;
    if (r) M.z = 0;
    M.n = (uint8_t)(r >> 7);
    return r;
}

static inline uint32_t SBCD(uint32_t s, uint32_t d)
{
    int lo = (int)(d & 15) - (int)(s & 15) - M.x;
    int r = (int)(d & 0xf0) - (int)(s & 0xf0) + lo;
    if (lo < 0) r -= 6;
    if (r < 0) { r -= 0x60; M.c = M.x = 1; } else { M.c = M.x = 0; }
    r &= 0xff;
    if (r) M.z = 0;
    M.n = (uint8_t)(r >> 7);
    return (uint32_t)r;
}

static inline void FLG_NZ(uint32_t r, int sz)
{
    r &= SZMASK(sz);
    M.n = (uint8_t)((r >> SZSHIFT(sz)) & 1);
    M.z = r == 0;
}

static inline uint32_t ASL(uint32_t v, unsigned cnt, int sz)
{
    uint32_t m = SZMASK(sz);
    unsigned sh = SZSHIFT(sz);
    unsigned k;
    uint8_t vf = 0, c = 0;
    v &= m;
    for (k = 0; k < cnt; k++) {
        uint32_t msb = (v >> sh) & 1;
        c = (uint8_t)msb;
        v = (v << 1) & m;
        if (((v >> sh) & 1) != msb) vf = 1;
    }
    if (cnt) M.x = c;
    M.c = c; M.v = vf; FLG_NZ(v, sz);
    return v;
}

static inline uint32_t ASR(uint32_t v, unsigned cnt, int sz)
{
    uint32_t m = SZMASK(sz);
    unsigned sh = SZSHIFT(sz);
    uint32_t sign;
    unsigned k;
    uint8_t c = 0;
    v &= m;
    sign = v & (1u << sh);
    for (k = 0; k < cnt; k++) { c = v & 1; v = (v >> 1) | sign; }
    if (cnt) M.x = c;
    M.c = c; M.v = 0; FLG_NZ(v, sz);
    return v;
}

static inline uint32_t LSL(uint32_t v, unsigned cnt, int sz)
{
    uint32_t m = SZMASK(sz);
    unsigned sh = SZSHIFT(sz);
    unsigned k;
    uint8_t c = 0;
    v &= m;
    for (k = 0; k < cnt; k++) { c = (v >> sh) & 1; v = (v << 1) & m; }
    if (cnt) M.x = c;
    M.c = c; M.v = 0; FLG_NZ(v, sz);
    return v;
}

static inline uint32_t LSR(uint32_t v, unsigned cnt, int sz)
{
    uint32_t m = SZMASK(sz);
    unsigned k;
    uint8_t c = 0;
    v &= m;
    for (k = 0; k < cnt; k++) { c = v & 1; v >>= 1; }
    if (cnt) M.x = c;
    M.c = c; M.v = 0; FLG_NZ(v, sz);
    return v;
}

static inline uint32_t ROL(uint32_t v, unsigned cnt, int sz)
{
    uint32_t m = SZMASK(sz);
    unsigned sh = SZSHIFT(sz);
    unsigned k;
    uint8_t c = 0;
    v &= m;
    for (k = 0; k < cnt; k++) { c = (v >> sh) & 1; v = ((v << 1) | c) & m; }
    M.c = c; M.v = 0; FLG_NZ(v, sz);
    return v;
}

static inline uint32_t ROR(uint32_t v, unsigned cnt, int sz)
{
    uint32_t m = SZMASK(sz);
    unsigned sh = SZSHIFT(sz);
    unsigned k;
    uint8_t c = 0;
    v &= m;
    for (k = 0; k < cnt; k++) { c = v & 1; v = (v >> 1) | ((uint32_t)c << sh); }
    M.c = c; M.v = 0; FLG_NZ(v, sz);
    return v;
}

static inline uint32_t ROXL(uint32_t v, unsigned cnt, int sz)
{
    uint32_t m = SZMASK(sz);
    unsigned sh = SZSHIFT(sz);
    unsigned k;
    v &= m;
    for (k = 0; k < cnt; k++) { uint8_t c = (v >> sh) & 1; v = ((v << 1) | M.x) & m; M.x = c; }
    M.c = M.x; M.v = 0; FLG_NZ(v, sz);
    return v;
}

static inline uint32_t ROXR(uint32_t v, unsigned cnt, int sz)
{
    uint32_t m = SZMASK(sz);
    unsigned sh = SZSHIFT(sz);
    unsigned k;
    v &= m;
    for (k = 0; k < cnt; k++) { uint8_t c = v & 1; v = (v >> 1) | ((uint32_t)M.x << sh); M.x = c; }
    M.c = M.x; M.v = 0; FLG_NZ(v, sz);
    return v;
}

/* 0: division by zero (the caller raises the exception) */
static inline int DIVU(int r, uint32_t s)
{
    uint32_t d = M.d[r], q, rem;
    s &= 0xffffu;
    if (s == 0) return 0;
    q = d / s;
    rem = d % s;
    M.c = 0;
    if (q > 0xffffu) { M.v = 1; M.n = 1; return 1; }
    M.d[r] = (rem << 16) | q;
    M.n = (uint8_t)((q >> 15) & 1); M.z = q == 0; M.v = 0;
    return 1;
}

static inline int DIVS(int r, uint32_t s)
{
    int32_t d = (int32_t)M.d[r], q, rem;
    int32_t sv = (int16_t)(s & 0xffffu);
    if (sv == 0) return 0;
    M.c = 0;
    if (d == INT32_MIN && sv == -1) { M.v = 1; M.n = 1; return 1; }
    q = d / sv;
    rem = d % sv;
    if (q < -32768 || q > 32767) { M.v = 1; M.n = 1; return 1; }
    M.d[r] = ((uint32_t)(rem & 0xffff) << 16) | ((uint32_t)q & 0xffffu);
    M.n = q < 0; M.z = q == 0; M.v = 0;
    return 1;
}

/* 0: out of bounds (the caller raises the exception) */
static inline int CHK(int r, uint32_t s)
{
    int16_t v = (int16_t)M.d[r];
    int16_t b = (int16_t)s;
    if (v < 0) { M.n = 1; return 0; }
    if (v > b) { M.n = 0; return 0; }
    return 1;
}

#endif
