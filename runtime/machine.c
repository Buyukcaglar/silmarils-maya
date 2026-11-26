/*
 * machine.c - the dispatcher of the recompiled code, the CPU's exceptions and interrupts, the memory routing and
 * the host API (maya.h).  Portable C.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <wchar.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "amiga.h"
#include "maya.h"
#include "tx_gen.h"

M68K M;
uint8_t g_ram[RAM_SIZE + 16];
uint8_t g_iopage[256];
int g_stop;
char g_datadir[1024], g_savedir[1024];

static char g_err[1024];
static uint16_t *g_index;     /* (pc - TX_CODE_LO) / 2 -> region number + 1 */

void rt_log(const char *fmt, ...)
{
#ifdef MAYA_LOG
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
#else
    (void)fmt;
#endif
}

void rt_fatal(const char *fmt, ...)
{
    va_list ap;
    size_t n;
    if (g_stop == 2) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    n = strlen(g_err);
    snprintf(g_err + n, sizeof g_err - n,
             "\n\nPC=$%06X SR=$%04X\nD0-D7 %08X %08X %08X %08X %08X %08X %08X %08X\nA0-A7 %08X %08X %08X %08X %08X %08X %08X %08X",
             (unsigned)M.pc, (unsigned)GET_SR(), M.d[0], M.d[1], M.d[2], M.d[3], M.d[4], M.d[5], M.d[6], M.d[7],
             M.a[0], M.a[1], M.a[2], M.a[3], M.a[4], M.a[5], M.a[6], M.a[7]);
    g_stop = 2;
    M.ev = 0;
}

#ifdef _WIN32
static int widen(const char *s, wchar_t *w, int n)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) > 0;
}
#endif

FILE *rt_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wp[1100], wm[8];
    if (!widen(path, wp, 1100) || !widen(mode, wm, 8)) return NULL;
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}

int rt_exists(const char *path, int *is_dir)
{
#ifdef _WIN32
    wchar_t wp[1100];
    DWORD a;
    if (!widen(path, wp, 1100)) return 0;
    a = GetFileAttributesW(wp);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    if (is_dir) *is_dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return 1;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    return 1;
#endif
}

int rt_mkdir(const char *path)
{
#ifdef _WIN32
    wchar_t wp[1100];
    if (!widen(path, wp, 1100)) return -1;
    return _wmkdir(wp);
#else
    return mkdir(path, 0755);
#endif
}

int rt_remove(const char *path)
{
#ifdef _WIN32
    wchar_t wp[1100];
    if (!widen(path, wp, 1100)) return -1;
    return _wremove(wp);
#else
    return remove(path);
#endif
}

const char *maya_error(void)
{
    return g_err;
}

/* ------------------------------------------------------------------------------------------------ CPU state */

void SET_SR(uint32_t v)
{
    uint8_t s = (v >> 13) & 1;
    SET_CCR(v);
    M.t = (v >> 15) & 1;
    M.ipl = (v >> 8) & 7;
    if (s != M.s) {
        uint32_t t = M.a[7];
        M.a[7] = M.osp;
        M.osp = t;
        M.s = s;
    }
    M.ev = M.cyc;             /* a lowered mask may let a pending interrupt in */
}

void RTE(void)
{
    uint32_t sr = POP16();
    M.pc = POP32();
    SET_SR(sr);
}

void take_exception(int vector, int level)
{
    uint32_t sr = GET_SR();
    if (!M.s) {
        uint32_t t = M.a[7];
        M.a[7] = M.osp;
        M.osp = t;
        M.s = 1;
    }
    M.t = 0;
    if (level >= 0) {
        M.ipl = (uint8_t)level;
    }
    PUSH32(M.pc);
    PUSH16(sr);
    M.pc = RL((uint32_t)vector * 4u);
    M.cyc += 44;
}

void rt_bad_pc(void)
{
    rt_fatal("The recompiled code was entered at $%06X, which is not an instruction start.", (unsigned)M.pc);
}

void rt_exception(int vector)
{
    if (vector == 0 || vector == 4 || vector == 10 || vector == 11) {
        rt_fatal("CPU exception %d (illegal instruction or reset) at $%06X.", vector, (unsigned)M.pc);
        return;
    }
    if (vector == 7 || vector == 5 || vector == 6 || vector >= 32) {
        /* TRAPV, division by zero, CHK, TRAP #n: the PC pushed is the next instruction's for TRAPs; the game
         * installs none of these, so they reach TRAP_FATAL through the vector table */
        take_exception(vector, -1);
        return;
    }
    rt_fatal("CPU exception %d at $%06X.", vector, (unsigned)M.pc);
}

/* ------------------------------------------------------------------------------------------------ memory */

uint32_t io_read(uint32_t a, int size)
{
    if ((a & 0xff0000u) == 0xdf0000u) {
        if ((a & 0xfff000u) != 0xdff000u) {
            return 0;
        }
        if (size == 1) {
            uint32_t w = custom_read(a & 0x1feu);
            return (a & 1) ? (w & 0xffu) : (w >> 8);
        }
        return custom_read(a & 0x1feu);
    }
    if ((a & 0xff0000u) == 0xbf0000u) {
        int reg = (a >> 8) & 15;
        uint32_t va = 0xff, vb = 0xff;
        if (!(a & 0x1000u)) va = cia_read(0, reg);
        if (!(a & 0x2000u)) vb = cia_read(1, reg);
        if (size == 1) {
            return (a & 1) ? va : vb;
        }
        return (vb << 8) | va;
    }
    return 0;
}

void io_write(uint32_t a, uint32_t v, int size)
{
    if ((a & 0xff0000u) == 0xdf0000u) {
        if ((a & 0xfff000u) != 0xdff000u) {
            return;
        }
        if (size == 1) {
            v = (v & 0xffu) * 0x101u;     /* a byte write puts the byte on both halves of the bus */
        }
        custom_write(a & 0x1feu, v);
        return;
    }
    if ((a & 0xff0000u) == 0xbf0000u) {
        int reg = (a >> 8) & 15;
        if (size == 1) {
            if ((a & 1) && !(a & 0x1000u)) cia_write(0, reg, v);
            if (!(a & 1) && !(a & 0x2000u)) cia_write(1, reg, v);
        } else {
            if (!(a & 0x1000u)) cia_write(0, reg, v & 0xffu);
            if (!(a & 0x2000u)) cia_write(1, reg, v >> 8);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ events */

int pending_level(void)
{
    uint16_t act;
    if (!(H.intena & 0x4000u)) {
        return 0;
    }
    act = H.intreq & H.intena;
    if (act & 0x2000u) return 6;
    if (act & 0x1800u) return 5;
    if (act & 0x0780u) return 4;
    if (act & 0x0070u) return 3;
    if (act & 0x0008u) return 2;
    if (act & 0x0007u) return 1;
    return 0;
}

static void rt_event(void)
{
    int lv;
    hw_sync(M.cyc);
    lv = pending_level();
    if (lv > M.ipl) {
        take_exception(24 + lv, lv);
    }
    M.ev = hw_next_event();
}

#ifdef MAYA_PROFILE
unsigned long g_prof[4096];
#endif
int g_alistrace;
int g_mustrace;
long g_frame_no;

static void dispatch(void)
{
    uint32_t pc = M.pc & 0xffffffu;
    if (pc >= TX_CODE_LO && pc < TX_CODE_HI) {
        uint16_t k = g_index[(pc - TX_CODE_LO) >> 1];
        if (k != 0 && !(pc & 1)) {
#ifdef MAYA_PROFILE
            g_prof[k - 1]++;
#endif
#ifdef MAYA_ALISTRACE
            if (pc == 0x4200au && g_alistrace)
                fprintf(stderr, "  alis a3=%06X op=%02X %02X %02X %02X d7=%08X\n", (unsigned)M.a[3], (unsigned)RB(M.a[3]),
                        (unsigned)RB(M.a[3] + 1), (unsigned)RB(M.a[3] + 2), (unsigned)RB(M.a[3] + 3), (unsigned)M.d[7]);
#endif
#ifdef MAYA_MUSTRACE
            {
                if (pc == 0x49290u || pc == 0x49390u || pc == 0x49180u || pc == 0x4913cu || pc == 0x491eeu ||
                    pc == 0x456d4u || pc == 0x456e0u || pc == 0x4905au || pc == 0x4910cu || pc == 0x491a8u ||
                    pc == 0x491bau || pc == 0x491ceu)
                    fprintf(stderr, "[%ld] call %06X d0=%08X d1=%08X a0=%06X a1=%06X a2=%06X | ptr=%06X dur=%04X mode=%02X 9450=%04X sr=%04X\n",
                            g_frame_no, (unsigned)pc, (unsigned)M.d[0], (unsigned)M.d[1], (unsigned)M.a[0],
                            (unsigned)M.a[1], (unsigned)M.a[2], (unsigned)RL(0x49a42), (unsigned)RW(0x49a4e),
                            (unsigned)RB(0x49a50), (unsigned)RW(0x49450), (unsigned)GET_SR());
                if (pc == 0x49590u && g_mustrace)
                    fprintf(stderr, "[%ld] tick ptr=%06X dur=%04X mode=%02X next=%02X %02X %02X %02X\n", g_frame_no,
                            (unsigned)RL(0x49a42), (unsigned)RW(0x49a4e), (unsigned)RB(0x49a50),
                            (unsigned)RB(RL(0x49a42)), (unsigned)RB(RL(0x49a42) + 1), (unsigned)RB(RL(0x49a42) + 2),
                            (unsigned)RB(RL(0x49a42) + 3));
            }
#endif
            tx_regions[k - 1].fn();
            return;
        }
    }
    if (pc >= HLE_BASE && pc < HLE_TOP) {
        hle_exec(pc);
        return;
    }
    rt_fatal("The game jumped to $%06X, which is not recompiled code.", (unsigned)pc);
}

/* ------------------------------------------------------------------------------------------------ host API */

int maya_is_data_dir(const char *dir)
{
    char p[1100];
    FILE *f;
    snprintf(p, sizeof p, "%s/MAIN.CO", dir);
    f = rt_fopen(p, "rb");
    if (f == NULL) {
        snprintf(p, sizeof p, "%s/main.co", dir);
        f = rt_fopen(p, "rb");
    }
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

int maya_boot(const char *datadir, const char *savedir, char *err, size_t errlen)
{
    unsigned k;
    if (!maya_is_data_dir(datadir)) {
        snprintf(err, errlen, "The folder \"%s\" does not hold the game's files (MAIN.CO not found).", datadir);
        return 1;
    }
    snprintf(g_datadir, sizeof g_datadir, "%s", datadir);
    snprintf(g_savedir, sizeof g_savedir, "%s", savedir);
    memset(g_ram, 0, sizeof g_ram);
    memset(&M, 0, sizeof M);
    memcpy(g_ram + TX_IMAGE_BASE, tx_image, TX_IMAGE_SIZE);
    memset(g_iopage, 0, sizeof g_iopage);
    g_iopage[0xbf] = 1;
    g_iopage[0xdf] = 1;
    g_iopage[0xdc] = 1;       /* the clock chip: reads 0 */
    free(g_index);
    g_index = (uint16_t *)calloc((TX_CODE_HI - TX_CODE_LO) / 2 + 1, sizeof *g_index);
    if (g_index == NULL) {
        snprintf(err, errlen, "Out of memory.");
        return 1;
    }
    for (k = 0; k < tx_region_count; k++) {
        uint32_t a;
        for (a = tx_regions[k].lo; a < tx_regions[k].hi; a += 2) {
            g_index[(a - TX_CODE_LO) >> 1] = (uint16_t)(k + 1);
        }
    }
    g_stop = 0;
    g_err[0] = 0;
    custom_reset();
    cia_reset();
    paula_reset();
    hle_boot();               /* vectors, ExecBase, libraries, heap, the CPU's start state */
    M.ev = 0;
    return 0;
}

int maya_run_frame(void)
{
    H.frame_done = 0;
    while (!g_stop) {
        if (M.cyc >= M.ev) {
            rt_event();
            if (H.frame_done) {
                break;
            }
            continue;
        }
        dispatch();
    }
    return g_stop == 0;
}

const uint32_t *maya_picture(int *w, int *h)
{
    *w = g_shown.w;
    *h = g_shown.h;
    return g_shown.px;
}

size_t maya_audio(int16_t *out, size_t frames)
{
    return paula_take(out, frames);
}

void maya_mouse(int x, int y, int buttons)
{
    int dx = x - H.mouse_x, dy = y - H.mouse_y;
    /* the hardware counters as a mouse moving the same distance would give (low-res pixels: 1 count each) */
    uint8_t cx = (uint8_t)((H.joy0dat & 0xff) + dx);
    uint8_t cy = (uint8_t)((H.joy0dat >> 8) + dy);
    H.joy0dat = (uint16_t)((cy << 8) | cx);
    H.mouse_x = x;
    H.mouse_y = y;
    H.buttons = buttons;
}

void maya_key(int raw, int down)
{
    if (raw < 0 || raw > 127) {
        return;
    }
    if (down) {
        g_keymatrix[raw >> 3] |= (uint8_t)(1u << (raw & 7));
    } else {
        g_keymatrix[raw >> 3] &= (uint8_t)~(1u << (raw & 7));
    }
}

void maya_joystick(int bits)
{
    /* JOY1DAT: right sets bit 1, left bit 9; down is bit 1 xor bit 0, up bit 9 xor bit 8 */
    int r = (bits & 8) != 0, l = (bits & 4) != 0, d = (bits & 2) != 0, u = (bits & 1) != 0;
    int b1 = r, b9 = l;
    int b0 = d ^ b1, b8 = u ^ b9;
    H.joy1dat = (uint16_t)((b9 << 9) | (b8 << 8) | (b1 << 1) | b0);
    H.fire2 = (bits & 16) != 0;
}

void maya_keys_release(void)
{
    memset(g_keymatrix, 0, sizeof g_keymatrix);
}
