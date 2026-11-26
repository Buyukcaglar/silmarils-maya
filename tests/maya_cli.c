/*
 * maya_cli.c - a headless runner for testing: boots the game, runs frames, optionally clicks and types, and writes
 * the picture as a BMP and the sound as a WAV.
 *
 *   maya_cli <data dir> <frames> [picture.bmp] [sound.wav] [--click F X Y] [--key F RAW] [--shot F file.bmp]
 *
 * --click F X Y : press the left button at game pixel (X,Y) on frame F (released 5 frames later)
 * --key F RAW   : press the Amiga raw key RAW on frame F (released 5 frames later)
 * --shot F FILE : write the picture of frame F to FILE
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/maya.h"
#include "../runtime/amiga.h"
#include "tx_gen.h"

static void put16(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)(v >> 8 & 255), f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xffff); put16(f, v >> 16); }

static void write_bmp(const char *path)
{
    int w, h, x, y;
    const uint32_t *px = maya_picture(&w, &h);
    FILE *f = fopen(path, "wb");
    if (f == NULL) return;
    fputc('B', f); fputc('M', f);
    put32(f, 54u + (unsigned)(w * h * 4)); put32(f, 0); put32(f, 54);
    put32(f, 40); put32(f, (unsigned)w); put32(f, (unsigned)h); put16(f, 1); put16(f, 32);
    put32(f, 0); put32(f, (unsigned)(w * h * 4)); put32(f, 2835); put32(f, 2835); put32(f, 0); put32(f, 0);
    for (y = h - 1; y >= 0; y--)
        for (x = 0; x < w; x++) put32(f, px[y * w + x]);
    fclose(f);
}

typedef struct Ev { int frame, kind, a, b; const char *file; } Ev;

int main(int argc, char **argv)
{
    char err[512];
    int frames, f, k;
    static int16_t snd[2 * 4096];
    FILE *wav = NULL;
    size_t total = 0;
    Ev ev[256];
    int nev = 0;
    const char *bmp = NULL, *wavp = NULL;
    int mx = 160, my = 100, btn = 0;
    if (argc < 3) {
        fprintf(stderr, "usage: maya_cli <data dir> <frames> [picture.bmp] [sound.wav] [--click F X Y] [--key F RAW] [--shot F file]\n");
        return 2;
    }
    frames = atoi(argv[2]);
    for (k = 3; k < argc; k++) {
        if (strcmp(argv[k], "--click") == 0 && k + 3 < argc && nev < 254) {
            ev[nev].frame = atoi(argv[k + 1]); ev[nev].kind = 1; ev[nev].a = atoi(argv[k + 2]); ev[nev].b = atoi(argv[k + 3]); nev++;
            ev[nev].frame = atoi(argv[k + 1]) + 5; ev[nev].kind = 2; ev[nev].a = atoi(argv[k + 2]); ev[nev].b = atoi(argv[k + 3]); nev++;
            k += 3;
        } else if (strcmp(argv[k], "--key") == 0 && k + 2 < argc && nev < 254) {
            ev[nev].frame = atoi(argv[k + 1]); ev[nev].kind = 3; ev[nev].a = (int)strtol(argv[k + 2], NULL, 0); nev++;
            ev[nev].frame = atoi(argv[k + 1]) + 5; ev[nev].kind = 4; ev[nev].a = (int)strtol(argv[k + 2], NULL, 0); nev++;
            k += 2;
        } else if (strcmp(argv[k], "--joy") == 0 && k + 3 < argc && nev < 254) {
            /* --joy F BITS N: hold the joystick bits from frame F for N frames */
            ev[nev].frame = atoi(argv[k + 1]); ev[nev].kind = 6; ev[nev].a = (int)strtol(argv[k + 2], NULL, 0); nev++;
            ev[nev].frame = atoi(argv[k + 1]) + atoi(argv[k + 3]); ev[nev].kind = 6; ev[nev].a = 0; nev++;
            k += 3;
        } else if (strcmp(argv[k], "--shot") == 0 && k + 2 < argc && nev < 255) {
            ev[nev].frame = atoi(argv[k + 1]); ev[nev].kind = 5; ev[nev].file = argv[k + 2]; nev++;
            k += 2;
        } else if (bmp == NULL) {
            bmp = argv[k];
        } else if (wavp == NULL) {
            wavp = argv[k];
        }
    }
    if (maya_boot(argv[1], "saves_test", err, sizeof err) != 0) {
        fprintf(stderr, "boot: %s\n", err);
        return 1;
    }
    if (wavp) {
        wav = fopen(wavp, "wb");
        if (wav) { for (k = 0; k < 44; k++) fputc(0, wav); }
    }
    for (f = 0; f < frames; f++) {
        size_t n;
        for (k = 0; k < nev; k++) {
            if (ev[k].frame != f) continue;
            if (ev[k].kind == 1) { mx = ev[k].a; my = ev[k].b; maya_mouse(mx, my, btn); btn = 1; maya_mouse(mx, my, btn); }
            if (ev[k].kind == 2) { btn = 0; maya_mouse(mx, my, btn); }
            if (ev[k].kind == 3) maya_key(ev[k].a, 1);
            if (ev[k].kind == 4) maya_key(ev[k].a, 0);
            if (ev[k].kind == 5) write_bmp(ev[k].file);
            if (ev[k].kind == 6) maya_joystick(ev[k].a);
        }
        if (getenv("MAYA_TRACE") && f % 100 == 0)
            fprintf(stderr, "f%d pc=%06X sr=%04X intena=%04X intreq=%04X dmacon=%04X a7=%06X d0=%08X\n", f, (unsigned)M.pc, (unsigned)GET_SR(), H.intena, H.intreq, H.dmacon, (unsigned)M.a[7], (unsigned)M.d[0]);
#ifdef MAYA_PROFILE
        if (f == (getenv("MAYA_PROF0") ? atoi(getenv("MAYA_PROF0")) : 800)) { extern unsigned long g_prof[4096]; memset(g_prof, 0, sizeof g_prof); }
#endif
        { extern int g_alistrace; g_alistrace = getenv("MAYA_ALIS") && f >= atoi(getenv("MAYA_ALIS")) && f < atoi(getenv("MAYA_ALIS")) + 3; }
        if (!maya_run_frame()) {
            const char *e = maya_error();
            fprintf(stderr, "stopped at frame %d: %s\n", f, e && e[0] ? e : "(the game ended)");
            break;
        }
        while ((n = maya_audio(snd, 4096)) > 0) {
            if (wav) fwrite(snd, 4, n, wav);
            total += n;
        }
    }
    if (bmp) write_bmp(bmp);
    if (wav) {
        unsigned bytes = (unsigned)(total * 4);
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav); put32(wav, 36 + bytes); fwrite("WAVEfmt ", 1, 8, wav);
        put32(wav, 16); put16(wav, 1); put16(wav, 2); put32(wav, MAYA_AUDIO_RATE); put32(wav, MAYA_AUDIO_RATE * 4);
        put16(wav, 4); put16(wav, 16); fwrite("data", 1, 4, wav); put32(wav, bytes);
        fclose(wav);
    }
#ifdef MAYA_PROFILE
    {
        extern unsigned long g_prof[4096];
        for (k = 0; k < 25; k++) {
            unsigned j, best = 0;
            for (j = 1; j < tx_region_count; j++) if (g_prof[j] > g_prof[best]) best = j;
            if (!g_prof[best]) break;
            fprintf(stderr, "  region $%06X  %lu\n", tx_regions[best].lo, g_prof[best]);
            g_prof[best] = 0;
        }
    }
#endif
    printf("ran %d frames, %zu audio frames\n", f, total);
    return 0;
}
