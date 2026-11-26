/*
 * hle.c - the Amiga OS as T.X uses it, emulated at the library-call level (docs/DECISIONS.md D9): exec (libraries,
 * memory, the task, SuperState, keyboard.device I/O), dos (files in the game's folder, saves in the save folder),
 * graphics (the few RastPort calls of the error screens), intuition (the game's screen and window, the Workbench
 * screen whose bitmap the game borrows), and the OS's interrupt handlers, including the CIA-B server the WHDLoad
 * slave adds (patches_maya.py).  Library calls land on base - offset; the dispatcher sends them here.
 * Portable C.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#endif

#include "amiga.h"
#include "tx_gen.h"

#if TX_EXTER_INT != HLE_EXTER_INT
#error "patches_maya.EXTER_INT and HLE_EXTER_INT differ"
#endif

uint8_t g_keymatrix[16];

static uint32_t g_hle_top;            /* the next free byte of HLE_DATA */
static uint32_t g_wbscreen, g_screen, g_window, g_task;
static uint32_t g_argstr;

/* ------------------------------------------------------------------------------------------------ helpers */

static void w8(uint32_t a, uint32_t v) { g_ram[a & 0xffffffu] = (uint8_t)v; }
static void w16(uint32_t a, uint32_t v) { w8(a, v >> 8); w8(a + 1, v); }
static void w32(uint32_t a, uint32_t v) { w16(a, v >> 16); w16(a + 2, v); }
static uint32_t r8(uint32_t a) { return g_ram[a & 0xffffffu]; }
static uint32_t r16(uint32_t a) { return (r8(a) << 8) | r8(a + 1); }
static uint32_t r32(uint32_t a) { return (r16(a) << 16) | r16(a + 2); }

static uint32_t hle_struct(uint32_t size)
{
    uint32_t a = g_hle_top;
    g_hle_top = (g_hle_top + size + 7u) & ~7u;
    if (g_hle_top > HLE_DATA_TOP) {
        rt_fatal("The OS emulation ran out of structure space.");
        return HLE_DATA;
    }
    memset(g_ram + a, 0, size);
    return a;
}

static void cstr(uint32_t a, char *out, size_t n)
{
    size_t k = 0;
    while (k + 1 < n) {
        uint8_t c = (uint8_t)r8(a + (uint32_t)k);
        if (c == 0) break;
        out[k++] = (char)c;
    }
    out[k] = 0;
}

/* ------------------------------------------------------------------------------------------------ memory */

typedef struct Span { uint32_t lo, hi; } Span;
static Span g_free[1024];
static int g_nfree;

static void heap_init(uint32_t lo, uint32_t hi)
{
    g_nfree = 1;
    g_free[0].lo = lo;
    g_free[0].hi = hi;
}

static uint32_t heap_alloc(uint32_t size)
{
    int k;
    size = (size + 7u) & ~7u;
    for (k = 0; k < g_nfree; k++) {
        if (g_free[k].hi - g_free[k].lo >= size) {
            uint32_t a = g_free[k].lo;
            g_free[k].lo += size;
            if (g_free[k].lo == g_free[k].hi) {
                memmove(&g_free[k], &g_free[k + 1], (size_t)(g_nfree - k - 1) * sizeof g_free[0]);
                g_nfree--;
            }
            return a;
        }
    }
    return 0;
}

static void heap_free(uint32_t a, uint32_t size)
{
    int k;
    uint32_t hi;
    size = (size + 7u) & ~7u;
    if (a == 0 || size == 0) return;
    hi = a + size;
    for (k = 0; k < g_nfree && g_free[k].lo < a; k++) {
    }
    if (g_nfree >= (int)(sizeof g_free / sizeof g_free[0])) return;
    memmove(&g_free[k + 1], &g_free[k], (size_t)(g_nfree - k) * sizeof g_free[0]);
    g_free[k].lo = a;
    g_free[k].hi = hi;
    g_nfree++;
    if (k + 1 < g_nfree && g_free[k].hi == g_free[k + 1].lo) {      /* merge with the next */
        g_free[k].hi = g_free[k + 1].hi;
        memmove(&g_free[k + 1], &g_free[k + 2], (size_t)(g_nfree - k - 2) * sizeof g_free[0]);
        g_nfree--;
    }
    if (k > 0 && g_free[k - 1].hi == g_free[k].lo) {                 /* and with the previous */
        g_free[k - 1].hi = g_free[k].hi;
        memmove(&g_free[k], &g_free[k + 1], (size_t)(g_nfree - k - 1) * sizeof g_free[0]);
        g_nfree--;
    }
}

uint32_t hle_alloc_chip(uint32_t size, int clear)
{
    uint32_t a = heap_alloc(size);
    if (a && clear) memset(g_ram + a, 0, size);
    return a;
}

static uint32_t avail(uint32_t flags)
{
    uint32_t best = 0, total = 0;
    int k;
    for (k = 0; k < g_nfree; k++) {
        uint32_t n = g_free[k].hi - g_free[k].lo;
        total += n;
        if (n > best) best = n;
    }
    if (flags & 0x20000u) {
        /* MEMF_LARGEST: at most 512 KB, as on the Amigas the game was made for (DECISIONS.md D13) */
        return best > 0x80000u ? 0x80000u : best;
    }
    return total;
}

/* ------------------------------------------------------------------------------------------------ files */

/* an open file; cow: the game's own file, opened read-only, copied to the save folder on its first write */
typedef struct HFile { FILE *f; int used, cow; char name[256]; } HFile;
static HFile g_files[16];

static int split_volume(const char *name, const char **path)
{
    const char *colon = strchr(name, ':');
    if (colon == NULL) {
        *path = name;
        return 1;
    }
    *path = colon + 1;
    {
        size_t n = (size_t)(colon - name);
        char vol[32];
        size_t k;
        if (n >= sizeof vol) return 0;
        for (k = 0; k < n; k++) vol[k] = (char)((name[k] >= 'a' && name[k] <= 'z') ? name[k] - 32 : name[k]);
        vol[n] = 0;
        /* WHDLoad's setup: DF0: is the game's folder; no other drive exists */
        return n == 0 || strcmp(vol, "DF0") == 0 || strcmp(vol, "SYS") == 0;
    }
}

static void join(char *out, size_t n, const char *dir, const char *path)
{
    size_t k;
    snprintf(out, n, "%s/%s", dir, path);
    for (k = 0; out[k]; k++) {
#ifdef _WIN32
        if (out[k] == '/') out[k] = '\\';
#endif
    }
}

#ifndef _WIN32
/* the file's real name in dir when only the case differs (macOS volumes may be case-sensitive) */
static void fix_case(char *full)
{
    char dir[1100];
    char *slash = strrchr(full, '/');
    DIR *d;
    struct dirent *e;
    struct stat st;
    if (stat(full, &st) == 0 || slash == NULL) return;
    memcpy(dir, full, (size_t)(slash - full));
    dir[slash - full] = 0;
    d = opendir(dir);
    if (d == NULL) return;
    while ((e = readdir(d)) != NULL) {
        if (strcasecmp(e->d_name, slash + 1) == 0) {
            strcpy(slash + 1, e->d_name);
            break;
        }
    }
    closedir(d);
}
#else
static void fix_case(char *full) { (void)full; }
#endif

static int exists(const char *p, int *is_dir)
{
    return rt_exists(p, is_dir);
}

static void make_savedir(void)
{
    rt_mkdir(g_savedir);
}

/* the host path to read an Amiga name from: the save folder first, then the game's folder */
static int read_path(const char *name, char *out, size_t n)
{
    const char *path;
    if (!split_volume(name, &path)) return 0;
    join(out, n, g_savedir, path);
    fix_case(out);
    if (path[0] && exists(out, NULL)) return 1;
    join(out, n, g_datadir, path);
    fix_case(out);
    return 1;
}

static int write_path(const char *name, char *out, size_t n)
{
    const char *path;
    if (!split_volume(name, &path) || path[0] == 0) return 0;
    make_savedir();
    join(out, n, g_savedir, path);
    fix_case(out);
    return 1;
}

/* the save-folder path of an Amiga name, without creating the folder */
static int write_path_peek(const char *name, char *out, size_t n)
{
    const char *path;
    if (!split_volume(name, &path) || path[0] == 0) return 0;
    join(out, n, g_savedir, path);
    fix_case(out);
    return 1;
}

/* the first write to a game file opened read-only: copy it into the save folder and continue there */
static int cow_detach(HFile *h)
{
    char p[1300];
    long pos = ftell(h->f), len;
    FILE *o;
    unsigned char *buf;
    if (!write_path(h->name, p, sizeof p)) return 0;
    fseek(h->f, 0, SEEK_END);
    len = ftell(h->f);
    buf = (unsigned char *)malloc(len > 0 ? (size_t)len : 1);
    if (buf == NULL) return 0;
    fseek(h->f, 0, SEEK_SET);
    len = (long)fread(buf, 1, (size_t)len, h->f);
    o = rt_fopen(p, "w+b");
    if (o == NULL) { free(buf); return 0; }
    fwrite(buf, 1, (size_t)len, o);
    free(buf);
    fclose(h->f);
    h->f = o;
    h->cow = 0;
    fseek(o, pos, SEEK_SET);
    rt_log("dos copy-on-write \"%s\" -> %s", h->name, p);
    return 1;
}

static uint32_t dos_open(uint32_t name_a, uint32_t mode)
{
    char name[256], p[1300];
    FILE *f = NULL;
    int k, cow = 0;
    cstr(name_a, name, sizeof name);
    for (k = 0; k < 16 && g_files[k].used; k++) {
    }
    if (k == 16) return 0;
    if (mode == 1006) {                                  /* MODE_NEWFILE */
        if (write_path(name, p, sizeof p)) f = rt_fopen(p, "wb");
    } else {
        /* MODE_OLDFILE (1005), and MODE_READWRITE (1004: in Kickstart 1.x "an old file with an exclusive lock"),
         * which T.X uses for every read: an existing file, never created, that may be written.  A copy in the save
         * folder is opened for update; the game's own files are opened for reading only, and copied into the save
         * folder when the game first writes to them (decision D14): the game folder is never written. */
        char sp[1300];
        int in_save = write_path_peek(name, sp, sizeof sp) && exists(sp, NULL);
        if (in_save) {
            strcpy(p, sp);
            f = rt_fopen(p, "r+b");
        } else if (read_path(name, p, sizeof p)) {
            f = rt_fopen(p, "rb");
            cow = f != NULL;
        }
    }
    rt_log("dos Open \"%s\" %u -> %s", name, (unsigned)mode, f ? p : "(fail)");
    if (f == NULL) return 0;
    g_files[k].f = f;
    g_files[k].used = 1;
    g_files[k].cow = cow;
    snprintf(g_files[k].name, sizeof g_files[k].name, "%s", name);
    return 0x100u + (uint32_t)k;
}

static HFile *dos_file(uint32_t fh)
{
    if (fh < 0x100u || fh >= 0x110u || !g_files[fh - 0x100u].used) return NULL;
    return &g_files[fh - 0x100u];
}

static void info_fill(uint32_t a)
{
    /* as the slave's Info() replacement: validated, DOS\0, 1,758 blocks of 488 bytes, 5 used, in use */
    memset(g_ram + (a & 0xffffffu), 0, 36);
    w32(a + 8, 0x52);
    w32(a + 12, 0x6de);
    w32(a + 16, 5);
    w32(a + 20, 0x1e8);
    w32(a + 24, 0x444f5300u);
    w32(a + 32, 1);
}

static void call_dos(int lvo)
{
    switch (lvo) {
    case -30: M.d[0] = dos_open(M.d[1], M.d[2]); break;                 /* Open */
    case -36: {                                                          /* Close */
        HFile *h = dos_file(M.d[1]);
        if (h) { fclose(h->f); h->used = 0; }
        M.d[0] = (uint32_t)-1;
        break;
    }
    case -42: {                                                          /* Read */
        HFile *h = dos_file(M.d[1]);
        uint32_t buf = M.d[2] & 0xffffffu, len = M.d[3];
        if (h == NULL) { M.d[0] = (uint32_t)-1; break; }
        if (buf + len > RAM_SIZE) len = RAM_SIZE - buf;
        M.d[0] = (uint32_t)fread(g_ram + buf, 1, len, h->f);
        rt_log("dos Read %x %u bytes -> %d", (unsigned)M.d[1], (unsigned)len, (int)M.d[0]);
        break;
    }
    case -48: {                                                          /* Write */
        HFile *h = dos_file(M.d[1]);
        uint32_t buf = M.d[2] & 0xffffffu, len = M.d[3];
        if (h == NULL || (h->cow && !cow_detach(h))) { M.d[0] = (uint32_t)-1; break; }
        if (buf + len > RAM_SIZE) len = RAM_SIZE - buf;
        M.d[0] = (uint32_t)fwrite(g_ram + buf, 1, len, h->f);
        fflush(h->f);
        rt_log("dos Write %x %u bytes -> %d", (unsigned)M.d[1], (unsigned)len, (int)M.d[0]);
        break;
    }
    case -66: {                                                          /* Seek */
        HFile *h = dos_file(M.d[1]);
        long old;
        int whence = (int32_t)M.d[3] < 0 ? SEEK_SET : (int32_t)M.d[3] > 0 ? SEEK_END : SEEK_CUR;
        if (h == NULL) { M.d[0] = (uint32_t)-1; break; }
        old = ftell(h->f);
        M.d[0] = fseek(h->f, (long)(int32_t)M.d[2], whence) == 0 ? (uint32_t)old : (uint32_t)-1;
        rt_log("dos Seek %x %d %d -> %d", (unsigned)M.d[1], (int)M.d[2], (int)M.d[3], (int)M.d[0]);
        break;
    }
    case -72: {                                                          /* DeleteFile */
        char name[256], p[1300];
        cstr(M.d[1], name, sizeof name);
        M.d[0] = (write_path(name, p, sizeof p) && rt_remove(p) == 0) ? (uint32_t)-1 : 0;
        break;
    }
    case -84: {                                                          /* Lock */
        char name[256], p[1300];
        const char *path;
        cstr(M.d[1], name, sizeof name);
        M.d[0] = 0;
        if (split_volume(name, &path)) {
            if (path[0] == 0) {
                M.d[0] = 0x200u;                                         /* the volume's root */
            } else if (read_path(name, p, sizeof p) && exists(p, NULL)) {
                M.d[0] = 0x201u;
            }
        }
        rt_log("dos Lock \"%s\" -> %x", name, (unsigned)M.d[0]);
        break;
    }
    case -90: break;                                                     /* UnLock */
    case -114: info_fill(M.d[2]); M.d[0] = (uint32_t)-1; break;          /* Info */
    case -132: M.d[0] = 0; break;                                        /* IoErr */
    default: rt_fatal("dos.library call %d is not emulated.", lvo); break;
    }
}

/* ------------------------------------------------------------------------------------------------ graphics */

static void rp_pixel(uint32_t rp, int x, int y, int pen)
{
    uint32_t bm = r32(rp + 4);
    int bpr, rows, depth, p;
    if (bm == 0) return;
    bpr = (int)r16(bm);
    rows = (int)r16(bm + 2);
    depth = (int)r8(bm + 5);
    if (x < 0 || y < 0 || x >= bpr * 8 || y >= rows) return;
    for (p = 0; p < depth && p < 8; p++) {
        uint32_t pl = r32(bm + 8 + 4u * (uint32_t)p);
        uint32_t a = pl + (uint32_t)(y * bpr + (x >> 3));
        uint8_t m = (uint8_t)(0x80u >> (x & 7));
        if (pl == 0) continue;
        if (pen & (1 << p)) w8(a, r8(a) | m);
        else w8(a, r8(a) & (uint32_t)~m);
    }
}

static void call_gfx(int lvo)
{
    uint32_t rp = M.a[1];
    switch (lvo) {
    case -60: {                                                          /* Text */
        char s[256];
        uint32_t n = M.d[0] & 0xffff;
        if (n > 255) n = 255;
        memcpy(s, g_ram + (M.a[0] & 0xffffffu), n);
        s[n] = 0;
        rt_log("graphics Text \"%s\"", s);
        w16(rp + 36, r16(rp + 36) + 8 * n);
        break;
    }
    case -234: {                                                         /* SetRast */
        uint32_t bm = r32(rp + 4);
        int p;
        if (bm == 0) break;
        for (p = 0; p < (int)r8(bm + 5) && p < 8; p++) {
            uint32_t pl = r32(bm + 8 + 4u * (uint32_t)p);
            uint32_t n = r16(bm) * r16(bm + 2);
            if (pl && pl + n <= CHIP_SIZE) memset(g_ram + pl, (M.d[0] >> p) & 1 ? 0xff : 0, n);
        }
        break;
    }
    case -240: w16(rp + 36, M.d[0]); w16(rp + 38, M.d[1]); break;         /* Move */
    case -246: {                                                         /* Draw */
        int x0 = (int16_t)r16(rp + 36), y0 = (int16_t)r16(rp + 38);
        int x1 = (int16_t)M.d[0], y1 = (int16_t)M.d[1];
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int e = dx + dy;
        int pen = (int)r8(rp + 25);
        for (;;) {
            int e2;
            rp_pixel(rp, x0, y0, pen);
            if (x0 == x1 && y0 == y1) break;
            e2 = 2 * e;
            if (e2 >= dy) { e += dy; x0 += sx; }
            if (e2 <= dx) { e += dx; y0 += sy; }
        }
        w16(rp + 36, M.d[0]); w16(rp + 38, M.d[1]);
        break;
    }
    case -288: break;                                                    /* SetRGB4: the game's copper sets colours */
    case -306: {                                                         /* RectFill */
        int x, y, pen = (int)r8(rp + 25);
        for (y = (int16_t)M.d[1]; y <= (int16_t)M.d[3]; y++)
            for (x = (int16_t)M.d[0]; x <= (int16_t)M.d[2]; x++) rp_pixel(rp, x, y, pen);
        break;
    }
    case -324: rp_pixel(rp, (int16_t)M.d[0], (int16_t)M.d[1], (int)r8(rp + 25)); M.d[0] = 0; break;   /* WritePixel */
    case -342: w8(rp + 25, M.d[0]); break;                                /* SetAPen */
    case -348: w8(rp + 26, M.d[0]); break;                                /* SetBPen */
    case -354: w8(rp + 28, M.d[0]); break;                                /* SetDrMd */
    default: rt_fatal("graphics.library call %d is not emulated.", lvo); break;
    }
}

/* ------------------------------------------------------------------------------------------------ intuition */

static uint32_t make_screen(int w, int h, int depth, uint32_t planes0, int contiguous)
{
    uint32_t s = hle_struct(400);
    uint32_t bm = s + 184;
    int bpr = ((w + 15) / 16) * 2;
    int p;
    w16(s + 12, (uint32_t)w);
    w16(s + 14, (uint32_t)h);
    w32(s + 84 + 4, bm);                 /* the RastPort's BitMap */
    w8(s + 84 + 25, 1);                  /* FgPen */
    w8(s + 84 + 28, 1);                  /* DrawMode JAM2 */
    w16(bm, (uint32_t)bpr);
    w16(bm + 2, (uint32_t)h);
    w8(bm + 5, (uint32_t)depth);
    for (p = 0; p < depth; p++) {
        uint32_t pl = contiguous ? planes0 + (uint32_t)(p * bpr * h) : hle_alloc_chip((uint32_t)(bpr * h), 1);
        w32(bm + 8 + 4u * (uint32_t)p, pl);
    }
    return s;
}

static void call_intuition(int lvo)
{
    switch (lvo) {
    case -198: {                                                         /* OpenScreen */
        uint32_t ns = M.a[0];
        int w = (int16_t)r16(ns + 4), h = (int16_t)r16(ns + 6), depth = (int16_t)r16(ns + 8);
        if (w <= 0) w = 320;
        if (h <= 0) h = 200;
        if (depth <= 0 || depth > 6) depth = 4;
        g_screen = make_screen(w, h, depth, 0, 0);
        w16(g_screen + 8, r16(ns));
        w16(g_screen + 10, r16(ns + 2));
        M.d[0] = g_screen;
        break;
    }
    case -204: {                                                         /* OpenWindow */
        uint32_t win = hle_struct(160);
        w16(win + 8, r16(M.a[0] + 4));
        w16(win + 10, r16(M.a[0] + 6));
        w32(win + 46, g_screen);
        w32(win + 50, g_screen + 84);
        g_window = win;
        M.d[0] = win;
        break;
    }
    case -66: case -72: break;                                           /* CloseScreen, CloseWindow */
    case -162: {                                                         /* MoveScreen */
        uint32_t s = M.a[0];
        w16(s + 8, r16(s + 8) + M.d[0]);
        w16(s + 10, r16(s + 10) + M.d[1]);
        break;
    }
    case -270: case -60: break;                                          /* SetPointer, ClearPointer */
    default: rt_fatal("intuition.library call %d is not emulated.", lvo); break;
    }
}

/* ------------------------------------------------------------------------------------------------ exec */

static uint32_t open_library(uint32_t name_a)
{
    char name[64];
    cstr(name_a, name, sizeof name);
    if (strcmp(name, "exec.library") == 0) return LIB_EXEC;
    if (strcmp(name, "dos.library") == 0) return LIB_DOS;
    if (strcmp(name, "graphics.library") == 0) return LIB_GFX;
    if (strcmp(name, "intuition.library") == 0) return LIB_INTUITION;
    rt_log("exec OpenLibrary \"%s\": not available", name);
    return 0;
}

static void call_exec(int lvo)
{
    switch (lvo) {
    case -120: case -126: case -132: case -138: break;                   /* Disable Enable Forbid Permit */
    case -150: {                                                         /* SuperState */
        if (M.s) { M.d[0] = 0; break; }
        M.d[0] = M.osp;                  /* the system stack; the caller's stack becomes the supervisor's */
        M.osp = M.a[7];
        M.s = 1;
        break;
    }
    case -156:                                                           /* UserState */
        if (M.s) {
            M.osp = M.d[0];
            M.s = 0;
            M.ev = M.cyc;
        }
        break;
    case -168: {                                                         /* AddIntServer */
        rt_log("exec AddIntServer %u $%06X", (unsigned)M.d[0], (unsigned)M.a[1]);
        break;
    }
    case -198: {                                                         /* AllocMem */
        uint32_t a = heap_alloc(M.d[0]);
        if (a && (M.d[1] & 0x10000u)) memset(g_ram + a, 0, (M.d[0] + 7u) & ~7u);
        rt_log("exec AllocMem %u $%X -> $%06X", (unsigned)M.d[0], (unsigned)M.d[1], (unsigned)a);
        M.d[0] = M.d[0] ? a : 0;
        break;
    }
    case -210: heap_free(M.a[1], M.d[0]); break;                         /* FreeMem */
    case -216: M.d[0] = avail(M.d[1]); break;                            /* AvailMem */
    case -294: M.d[0] = g_task; break;                                   /* FindTask */
    case -354: case -360: break;                                         /* AddPort RemPort */
    case -408: case -552: M.d[0] = open_library(M.a[1]); break;          /* OldOpenLibrary OpenLibrary */
    case -414: break;                                                    /* CloseLibrary */
    case -444: {                                                         /* OpenDevice */
        char name[64];
        uint32_t io = M.a[1];
        cstr(M.a[0], name, sizeof name);
        if (strcmp(name, "keyboard.device") == 0) {
            w32(io + 20, LIB_KEYBOARD);
            w32(io + 24, 0);
            w8(io + 31, 0);
            M.d[0] = 0;
        } else {
            w8(io + 31, 0xff);
            M.d[0] = 0xff;
        }
        break;
    }
    case -450: break;                                                    /* CloseDevice */
    case -456: case -462: {                                              /* DoIO SendIO */
        uint32_t io = M.a[1];
        uint32_t cmd = r16(io + 28);
        if (r32(io + 20) == LIB_KEYBOARD && cmd == 10) {                 /* KBD_READMATRIX */
            uint32_t n = r32(io + 36), data = r32(io + 40), k;
            if (n > 16) n = 16;
            for (k = 0; k < n; k++) w8(data + k, g_keymatrix[k]);
            w32(io + 32, n);
            w8(io + 31, 0);
            M.d[0] = 0;
        } else {
            w8(io + 31, 0xfd);                                           /* IOERR_NOCMD */
            M.d[0] = 0xfffffffdu;
        }
        break;
    }
    default: rt_fatal("exec.library call %d is not emulated.", lvo); break;
    }
}

/* ------------------------------------------------------------------------------------------------ traps */

static void push_regs(void)
{
    int k;
    for (k = 14; k >= 0; k--) PUSH32(k < 8 ? M.d[k] : M.a[k - 8]);
}

static void pop_regs(void)
{
    int k;
    for (k = 0; k < 15; k++) {
        uint32_t v = POP32();
        if (k < 8) M.d[k] = v; else M.a[k - 8] = v;
    }
}

static void ack(uint16_t bits)
{
    custom_write(0x09c, bits);
}

static void trap(int k)
{
    switch (k) {
    case TRAP_EXIT:
        g_stop = 1;
        break;
    case TRAP_RTS:
        M.pc = POP32();
        break;
    case TRAP_FATAL:
        rt_fatal("An unexpected CPU exception (stacked PC $%06X).", (unsigned)RL(M.a[7] + 2));
        break;
    case TRAP_L1: ack(H.intreq & 0x0007u); RTE(); break;
    case TRAP_L2: cia_read(0, 0xd); ack(0x0008u); RTE(); break;
    case TRAP_L3: ack(H.intreq & 0x0070u); RTE(); break;
    case TRAP_L4: ack(H.intreq & 0x0780u); RTE(); break;
    case TRAP_L5: ack(H.intreq & 0x1800u); RTE(); break;
    case TRAP_L7: RTE(); break;
    case TRAP_L6: {
        uint32_t code = r32(HLE_EXTER_INT + 18);
        push_regs();
        cia_read(1, 0xd);                /* the ciab.resource's server: acknowledge CIA-B */
        if (code != 0 && (H.intena & 0x2000u)) {
            M.a[1] = r32(HLE_EXTER_INT + 14);
            M.a[5] = code;
            M.a[0] = 0xdff000u;
            M.a[6] = LIB_EXEC;
            M.d[1] = (uint32_t)(H.intena & H.intreq);
            PUSH32(HLE_TRAP(TRAP_L6_DONE));
            M.pc = code;
            break;
        }
    }
        /* fall through: no server */
    case TRAP_L6_DONE:
        pop_regs();
        ack(0x2000u);
        RTE();
        break;
    default:
        rt_fatal("Unknown OS trap %d.", k);
        break;
    }
}

void hle_exec(uint32_t pc)
{
    M.cyc += 100;
    if (pc < HLE_TRAP(TRAP_COUNT)) {
        trap((int)((pc - HLE_BASE) >> 1));
        return;
    }
    {
        static const uint32_t bases[] = {LIB_EXEC, LIB_DOS, LIB_GFX, LIB_INTUITION};
        int b;
        for (b = 0; b < 4; b++) {
            if (pc < bases[b] && pc >= bases[b] - 0x1000u) {
                int lvo = -(int)(bases[b] - pc);
                uint32_t ret = POP32();
                M.pc = ret;              /* the call returns as rts would; SuperState and friends may change S */
                switch (b) {
                case 0: call_exec(lvo); break;
                case 1: call_dos(lvo); break;
                case 2: call_gfx(lvo); break;
                default: call_intuition(lvo); break;
                }
                if (g_stop == 2) M.pc = pc;
                return;
            }
        }
    }
    rt_fatal("The game called $%06X in the OS area, which is not emulated.", (unsigned)pc);
}

void hle_frame(void)
{
    /* intuition's view of the mouse, in the game's screen (it reads MouseX at 18 and the next word as Y) */
    if (g_screen) {
        w16(g_screen + 16, (uint32_t)H.mouse_y);
        w16(g_screen + 18, (uint32_t)H.mouse_x);
        w16(g_screen + 20, (uint32_t)H.mouse_y);
    }
}

/* ------------------------------------------------------------------------------------------------ boot */

static void make_library(uint32_t base, int version)
{
    w8(base + 8, 9);                     /* NT_LIBRARY */
    w16(base + 20, (uint32_t)version);
    w16(base + 22, 0);
}

void hle_boot(void)
{
    uint32_t a;
    int k;
    memset(g_keymatrix, 0, sizeof g_keymatrix);
    for (k = 0; k < 16; k++) {
        if (g_files[k].used) fclose(g_files[k].f);
        g_files[k].used = 0;
    }
    g_hle_top = HLE_DATA;
    g_screen = g_window = 0;
    heap_init((TX_IMAGE_BASE + TX_IMAGE_SIZE + 0xfffu) & ~0xfffu, HEAP_HI);

    /* vectors: exceptions to the fatal trap, the autovectors to the OS's handlers */
    for (k = 2; k < 64; k++) w32((uint32_t)k * 4u, HLE_TRAP(TRAP_FATAL));
    for (k = 1; k <= 7; k++) w32(0x60u + 4u * (uint32_t)k, HLE_TRAP(TRAP_L1 + k - 1));
    w32(4, LIB_EXEC);

    make_library(LIB_EXEC, 34);
    w16(LIB_EXEC + 34, 34);              /* SoftVer */
    w16(LIB_EXEC + 296, 0);              /* AttnFlags: a 68000 */
    w8(LIB_EXEC + 530, 50);              /* VBlankFrequency */
    w8(LIB_EXEC + 531, 50);              /* PowerSupplyFrequency */
    make_library(LIB_DOS, 34);
    make_library(LIB_GFX, 34);
    w16(LIB_GFX + 206, 4);               /* DisplayFlags: PAL */
    make_library(LIB_INTUITION, 34);
    g_task = hle_struct(92);
    w8(g_task + 8, 13);                  /* NT_PROCESS */
    w32(LIB_EXEC + 276, g_task);

    /* the Workbench screen the game starts from: 640 x 256, two planes, contiguous (the game uses its bitmap's
     * 40,000 bytes from plane 0 as buffers) */
    a = hle_alloc_chip(0xa000, 1);
    g_wbscreen = make_screen(640, 256, 2, a, 1);
    w32(LIB_INTUITION + 56, g_wbscreen); /* ActiveScreen */
    w32(LIB_INTUITION + 60, g_wbscreen); /* FirstScreen */

    /* the slave's EXTER server (AddIntServer(INTB_EXTER)): is_Code an rts until the game sets its handler */
    w8(HLE_EXTER_INT + 8, 2);            /* NT_INTERRUPT */
    w32(HLE_EXTER_INT + 18, HLE_TRAP(TRAP_RTS));
    w32(LIB_EXEC + 84 + 13 * 12, HLE_EXTER_INT);

    /* the OS's copper list: nothing but the end */
    a = hle_alloc_chip(8, 1);
    w32(a, 0xfffffffeu);
    H.cop1lc = a;
    H.coppc = a;
    H.copstop = 0;
    H.dmacon = 0x03f0u;                  /* DMAEN BPLEN COPEN BLTEN SPREN DSKEN */
    H.intena = 0x6028u;                  /* as the slave leaves it: INTEN EXTER VERTB PORTS */

    g_argstr = hle_struct(4);
    w8(g_argstr, '\n');

    /* the CPU at the game's entry, as the slave calls it: jsr 4(seglist) with d0 = 1, a0 = "\n" */
    M.s = 0;
    M.a[7] = USP_TOP;
    M.osp = SSP_TOP;
    PUSH32(HLE_TRAP(TRAP_EXIT));
    M.d[0] = 1;
    M.a[0] = g_argstr;
    M.a[1] = TX_IMAGE_BASE + 8 - 4;
    M.pc = TX_ENTRY;
    M.ipl = 0;
}
