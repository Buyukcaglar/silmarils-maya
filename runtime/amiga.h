/*
 * amiga.h - the emulated machine inside the runtime: memory map, chipset state, OS emulation (HLE) hooks.
 * Shared by machine.c, custom.c, blitter.c, paula.c, cia.c, hle.c and hooks.c.  Portable C.
 *
 * Memory map (24-bit):
 *   $000000-$1FFFFF  chip RAM (2 MB): vectors, T.X at $40000 (tx_gen.h), the AllocMem heap, the stacks
 *   $BFxxxx          CIA-A (odd bytes) and CIA-B (even bytes)
 *   $DFF000-$DFF1FF  custom chips
 *   $F00000-$F7FFFF  the OS emulation: trap addresses, library bases and the OS's structures (RAM-backed)
 */
#ifndef MAYA_AMIGA_H
#define MAYA_AMIGA_H

#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

#define CHIP_SIZE       0x200000u
#define HEAP_HI         0x1E0000u
#define SSP_TOP         0x1F0000u   /* supervisor stack (interrupts) */
#define USP_TOP         0x1FFFF0u   /* the game's stack */

#define HLE_BASE        0xF00000u
#define HLE_TOP         0xF80000u
#define HLE_TRAP(k)     (HLE_BASE + 2u * (uint32_t)(k))   /* trap k: an address the dispatcher runs as C */
#define HLE_EXTER_INT   0xF01000u   /* the slave's CIA-B Interrupt structure (patches_maya.EXTER_INT) */
#define HLE_DATA        0xF02000u   /* the OS's structures, bump-allocated */
#define HLE_DATA_TOP    0xF10000u
#define LIB_EXEC        0xF20000u   /* library bases; base-$1000..base-1 are their jump tables (LVOs) */
#define LIB_DOS         0xF30000u
#define LIB_GFX         0xF40000u
#define LIB_INTUITION   0xF50000u
#define LIB_KEYBOARD    0xF60000u

/* traps */
enum {
    TRAP_EXIT = 1,        /* the game's return from its entry: quit */
    TRAP_RTS,             /* a do-nothing routine */
    TRAP_FATAL,           /* an unexpected CPU exception vector */
    TRAP_L1, TRAP_L2, TRAP_L3, TRAP_L4, TRAP_L5, TRAP_L6, TRAP_L7,   /* the OS's interrupt handlers */
    TRAP_L6_DONE,         /* a CIA-B server returned */
    TRAP_COUNT
};

/* PAL timing */
#define CPU_HZ          7093790
#define CC_HZ           3546895     /* colour clocks */
#define LINE_CC         227
#define FRAME_LINES     313

/* custom chip state */
typedef struct Custom {
    int64_t cc;               /* colour clocks since boot, synced to M.cyc / 2 */
    int64_t line_start;       /* cc at which the current line began */
    int line;                 /* 0 .. FRAME_LINES-1 */
    int frame_done;
    uint64_t frames;
    uint16_t intena, intreq, dmacon, adkcon;
    uint16_t regs[256];       /* the last value written to each register (word index) */
    /* copper */
    uint32_t cop1lc, cop2lc, coppc;
    int copwait, copstop;
    uint16_t copw1, copw2;
    int cdang;
    /* display */
    uint32_t bplpt[6];
    uint16_t bplcon0, bplcon1, bplcon2, diwstrt, diwstop, ddfstrt, ddfstop;
    int16_t bpl1mod, bpl2mod;
    uint16_t color[32];
    /* blitter */
    uint16_t bltcon0, bltcon1, bltafwm, bltalwm, bltadat, bltbdat, bltcdat;
    int16_t bltamod, bltbmod, bltcmod, bltdmod;
    uint32_t bltapt, bltbpt, bltcpt, bltdpt;
    int bzero;
    /* input */
    uint16_t joy0dat, joy1dat;
    int mouse_x, mouse_y, buttons;     /* game pixels; bit 0 left, bit 1 right */
    int fire2;                         /* the joystick's fire button (port 2) */
} Custom;

extern Custom H;

/* the frame the display produces: the display window, XRGB */
#define FB_MAXW 384
#define FB_MAXH 300
typedef struct Frame {
    int w, h;                 /* the display window's size this frame */
    uint32_t px[FB_MAXW * FB_MAXH];
} Frame;
extern Frame g_frame;         /* being drawn */
extern Frame g_shown;         /* the last complete frame */

void custom_reset(void);
uint32_t custom_read(uint32_t reg);           /* reg: byte offset $000..$1FE */
void custom_write(uint32_t reg, uint32_t v);
void hw_sync(int64_t cyc);
int64_t hw_next_event(void);                  /* in CPU cycles */
void intreq_set(uint16_t bits);
void blitter_start(void);
int pending_level(void);

/* CIAs */
typedef struct CiaTimer {
    uint16_t latch, counter;
    int running, oneshot;
} CiaTimer;
typedef struct Cia {
    uint8_t pra, prb, ddra, ddrb, sdr, icr_flags, icr_mask, cra, crb;
    CiaTimer ta, tb;
    uint32_t tod, tod_latch, alarm;
    int tod_latched, tod_stopped;
    int irq;                  /* the interrupt line is asserted */
    int64_t last_e;           /* E clock ticks done */
} Cia;
extern Cia g_cia[2];          /* 0: CIA-A, 1: CIA-B */
void cia_reset(void);
uint32_t cia_read(int which, int reg);
void cia_write(int which, int reg, uint32_t v);
void cia_run(int64_t cc);                     /* to colour clock cc */
int64_t cia_next_event(void);                 /* the colour clock of the next timer underflow, or INT64_MAX */
void cia_tod_tick(int which);

/* Paula audio */
void paula_reset(void);
void paula_write(uint32_t reg, uint32_t v);   /* $0A0..$0DF */
void paula_dmacon(uint16_t old, uint16_t now);
void paula_run(int64_t cc);                   /* colour clocks to run */
void paula_led(int on);
size_t paula_take(int16_t *out, size_t frames);   /* stereo frames at PAULA_RATE */
#define PAULA_RATE 48000

/* OS emulation */
void hle_boot(void);
void hle_exec(uint32_t pc);                  /* M.pc is inside HLE_BASE..HLE_TOP */
void hle_frame(void);                        /* at every vertical blank: mouse into the OS's structures */
uint32_t hle_alloc_chip(uint32_t size, int clear);
extern uint8_t g_keymatrix[16];

/* machine.c */
extern int g_stop;                           /* 1 exited, 2 fatal */
void rt_fatal(const char *fmt, ...);
void rt_log(const char *fmt, ...);
void take_exception(int vector, int level);
extern char g_datadir[1024], g_savedir[1024];

/* file system calls on UTF-8 paths (machine.c; on Windows through the wide-character API) */
#include <stdio.h>
FILE *rt_fopen(const char *path, const char *mode);
int rt_exists(const char *path, int *is_dir);
int rt_mkdir(const char *path);
int rt_remove(const char *path);

#endif
