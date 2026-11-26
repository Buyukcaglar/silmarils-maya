/*
 * maya_macos.m - the macOS host of the recompiled "Le Fetiche Maya" (docs/HOST-DESIGN.md, docs/MACOS-PORT.md,
 * docs/DECISIONS.md).  The counterpart of host/win32/maya_win32.c, with the same game API (runtime/maya.h) and
 * picture pipeline (present/present.h).
 *
 * One native window.  The menu bar holds the host's commands: the application menu (About, Quit), and a View menu
 * with Enter Full Screen (Ctrl+Cmd+F), Integer Scaling / Fit to Window, 4:3 Monitor Aspect and CRT Scanlines.
 * AppKit follows the system's light or dark appearance.  The game runs on the main thread: a 4 ms timer runs a frame
 * whenever the sound output needs more (AudioQueue, about 64 ms kept ahead), or on the PAL frame clock when there is
 * no sound device.  The timer runs in the common run loop modes, so the game keeps going while the window is moved
 * or resized and while a menu is open; the About box pauses it.
 *
 * Input: every key goes to the game as the Amiga key in its place (letters, "?" and "/" by the character they type,
 * D17); the arrow keys and the numeric keypad also drive the joystick in port 2, the Control keys, keypad 0 and
 * keypad Enter are its fire button (D15); a game controller too (GameController framework).  The Command key is
 * the Mac's and never goes to the game.  The mouse is the Amiga mouse in port 1 (D16).
 *
 * Settings (maya64.cfg) and saves (saves/) are in ~/Library/Application Support/Le Fetiche Maya 64bit (D24); the
 * window's frame is kept by AppKit.  Command line: --data <folder>, --crt 0|1, --aspect 4:3|square (the last two
 * for this run only; the menu's choices are saved).
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <AudioToolbox/AudioToolbox.h>
#import <GameController/GameController.h>

#include <mach/mach_time.h>
#include <os/lock.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../present/present.h"
#include "../../runtime/maya.h"
#include "host_config.h"

/* ------------------------------------------------------------------------------------------------ state */

typedef struct Cfg {
    char data[1024];          /* UTF-8 */
    int fullscreen;
    int fit, aspect43, crt;
} Cfg;

static Cfg g_cfg;
static char g_cfgpath[1200];
static char g_savedir[1200];
static int g_aspect_file = -1;    /* the file's aspect while --aspect overrides it for this run */
static int g_crt_file = -1;       /* the file's CRT setting while --crt overrides it for this run */
static int g_pause;               /* > 0: the game waits (the About box, a message) */
static int g_minimized;
static int g_ended;
static int g_booted;
static int g_full;

static NSWindow *g_window;
static NSView *g_view;
static CALayer *g_image;          /* the picture, placed at the layout's image rectangle */
static NSPanel *g_about;
static NSTimer *g_timer;
static id g_activity;

/* the picture */
static uint32_t *g_client;        /* the scaled and filtered image (CRT only): sw x sh */
static size_t g_client_px;
static PresentLayout g_layout;    /* in backing pixels */
static PresentCrt g_crt;
static CGColorSpaceRef g_srgb;

/* input */
static uint8_t g_amiga[128];      /* per macOS virtual key code: the Amiga raw key, 0xff none */
static uint8_t g_sent[128];       /* per key code: the Amiga raw key sent + 1, 0 none */
static uint8_t g_jkey[128];       /* per key code: the joystick bits it holds */
static int g_kjoy, g_pjoy;        /* the joystick from the keys, from a game controller */
static int g_mbuttons;
static double g_last_mouse;

static long g_frames;             /* game frames run */
static _Atomic long g_underruns;  /* sound buffers that ran dry (filled with silence) */

#ifdef MAYA_DEVTEST
static void devtest_init(void);
static void devtest_frame(long f);
static void devtest_present(double ms);
#endif

/* ------------------------------------------------------------------------------------------------ utilities */

static NSString *ns(const char *s) { return [NSString stringWithUTF8String:s] ?: @""; }

static double now_s(void) { return CACurrentMediaTime(); }

static void message(NSString *text, NSAlertStyle style)
{
    NSAlert *a = [[NSAlert alloc] init];
    a.alertStyle = style;
    a.messageText = ns(HOST_CAPTION);
    a.informativeText = text;
    [a addButtonWithTitle:@"OK"];
    g_pause++;
    [a runModal];
    g_pause--;
}

static int data_ok(NSString *dir)
{
    const char *s = dir.fileSystemRepresentation;
    return s != NULL && s[0] && maya_is_data_dir(s);
}

/* a folder that holds the game, or one with a "data" or "Maya/data" folder that does: the game's folder */
static NSString *data_find(NSString *dir)
{
    NSString *t;
    if (dir.length == 0) return nil;
    dir = dir.stringByExpandingTildeInPath;
    if (data_ok(dir)) return dir;
    t = [dir stringByAppendingPathComponent:@"data"];
    if (data_ok(t)) return t;
    t = [[dir stringByAppendingPathComponent:@"Maya"] stringByAppendingPathComponent:@"data"];
    if (data_ok(t)) return t;
    return nil;
}

/* ------------------------------------------------------------------------------------------------ settings */

static void cfg_load(void)
{
    FILE *f = fopen(g_cfgpath, "rb");
    char line[1200];
    memset(&g_cfg, 0, sizeof g_cfg);
    if (f == NULL) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *v, *e;
        if (eq == NULL) continue;
        *eq = 0;
        v = eq + 1;
        e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        if (strcmp(line, "data") == 0) snprintf(g_cfg.data, sizeof g_cfg.data, "%s", v);
        else if (strcmp(line, "fullscreen") == 0) g_cfg.fullscreen = atoi(v) != 0;
        else if (strcmp(line, "scale") == 0) g_cfg.fit = strcmp(v, "fit") == 0;
        else if (strcmp(line, "aspect") == 0) g_cfg.aspect43 = strcmp(v, "4:3") == 0;
        else if (strcmp(line, "crt") == 0) g_cfg.crt = atoi(v) != 0;
    }
    fclose(f);
}

static void cfg_save(void)
{
    FILE *f = fopen(g_cfgpath, "wb");
    if (f == NULL) return;
    fprintf(f, "# " HOST_PRODUCT " " HOST_VERSION_STR " settings\n");
    fprintf(f, "data=%s\n", g_cfg.data);
    fprintf(f, "fullscreen=%d\n", g_full);
    fprintf(f, "scale=%s\naspect=%s\ncrt=%d\n", g_cfg.fit ? "fit" : "integer",
            (g_aspect_file >= 0 ? g_aspect_file : g_cfg.aspect43) ? "4:3" : "square",
            g_crt_file >= 0 ? g_crt_file : g_cfg.crt);
    fclose(f);
}

/* ~/Library/Application Support/Le Fetiche Maya 64bit/{maya64.cfg,saves} (D24) */
static void paths_init(void)
{
    NSURL *base = [NSFileManager.defaultManager URLForDirectory:NSApplicationSupportDirectory
                                                       inDomain:NSUserDomainMask appropriateForURL:nil create:YES error:nil];
    NSString *dir = [(base ? base.path : [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support"])
        stringByAppendingPathComponent:@HOST_SUPPORT_NAME];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    snprintf(g_cfgpath, sizeof g_cfgpath, "%s/%s", dir.fileSystemRepresentation, HOST_CFG_NAME);
    snprintf(g_savedir, sizeof g_savedir, "%s/%s", dir.fileSystemRepresentation, HOST_SAVES_NAME);
}

/* ------------------------------------------------------------------------------------------------ sound */

#define AQ_BUFS 3
#define AQ_FRAMES 512               /* per buffer: 10.7 ms */
#define AUDIO_TARGET 3072           /* frames kept ahead in the ring: 64 ms, as on Windows */
#define RING_FRAMES 16384

static AudioQueueRef g_aq;
static int16_t g_ring[RING_FRAMES * 2];
static size_t g_rhead, g_rcount;    /* the oldest frame, the frames held (g_rlock) */
static os_unfair_lock g_rlock = OS_UNFAIR_LOCK_INIT;

static size_t ring_count(void)
{
    size_t n;
    os_unfair_lock_lock(&g_rlock);
    n = g_rcount;
    os_unfair_lock_unlock(&g_rlock);
    return n;
}

static void ring_write(const int16_t *src, size_t frames)
{
    size_t k;
    os_unfair_lock_lock(&g_rlock);
    for (k = 0; k < frames; k++) {
        size_t at;
        if (g_rcount == RING_FRAMES) {          /* full: drop the oldest sound rather than fall behind */
            g_rhead = (g_rhead + 1) % RING_FRAMES;
            g_rcount--;
        }
        at = (g_rhead + g_rcount) % RING_FRAMES;
        g_ring[at * 2] = src[k * 2];
        g_ring[at * 2 + 1] = src[k * 2 + 1];
        g_rcount++;
    }
    os_unfair_lock_unlock(&g_rlock);
}

/* the sound thread: fills a buffer from the ring, silence when it runs dry */
static void aq_callback(void *ud, AudioQueueRef q, AudioQueueBufferRef b)
{
    int16_t *out = (int16_t *)b->mAudioData;
    size_t k = 0;
    (void)ud;
    os_unfair_lock_lock(&g_rlock);
    for (; k < AQ_FRAMES && g_rcount > 0; k++) {
        out[k * 2] = g_ring[g_rhead * 2];
        out[k * 2 + 1] = g_ring[g_rhead * 2 + 1];
        g_rhead = (g_rhead + 1) % RING_FRAMES;
        g_rcount--;
    }
    os_unfair_lock_unlock(&g_rlock);
    if (k < AQ_FRAMES) {
        memset(out + k * 2, 0, (AQ_FRAMES - k) * 4);
        if (g_booted && !g_pause && !g_minimized) g_underruns++;
    }
    b->mAudioDataByteSize = AQ_FRAMES * 4;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

static void audio_open(void)
{
    AudioStreamBasicDescription fmt;
    int k;
    memset(&fmt, 0, sizeof fmt);
    fmt.mSampleRate = MAYA_AUDIO_RATE;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mBytesPerPacket = 4;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = 4;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 16;
    if (AudioQueueNewOutput(&fmt, aq_callback, NULL, NULL, NULL, 0, &g_aq) != noErr) {
        g_aq = NULL;
        return;
    }
    for (k = 0; k < AQ_BUFS; k++) {
        AudioQueueBufferRef b;
        if (AudioQueueAllocateBuffer(g_aq, AQ_FRAMES * 4, &b) != noErr) break;
        aq_callback(NULL, g_aq, b);
    }
    if (k < AQ_BUFS || AudioQueueStart(g_aq, NULL) != noErr) {
        AudioQueueDispose(g_aq, true);
        g_aq = NULL;
    }
}

static void audio_close(void)
{
    if (g_aq == NULL) return;
    AudioQueueStop(g_aq, true);
    AudioQueueDispose(g_aq, true);
    g_aq = NULL;
}

static void audio_pump(void)
{
    static int16_t buf[4096 * 2];
    size_t got;
    while ((got = maya_audio(buf, 4096)) > 0)
        if (g_aq != NULL) ring_write(buf, got);
}

/* ------------------------------------------------------------------------------------------------ game controller */

static void pad_poll(void)
{
    GCController *c = nil;
    GCExtendedGamepad *g;
    int bits = 0;
    if (@available(macOS 11.0, *)) c = GCController.current;
    if (c == nil || c.extendedGamepad == nil) {
        for (GCController *k in GCController.controllers)
            if (k.extendedGamepad != nil) {
                c = k;
                break;
            }
    }
    g = c.extendedGamepad;
    if (g == nil) {
        g_pjoy = 0;
        return;
    }
    if (g.dpad.up.pressed || g.leftThumbstick.yAxis.value > 0.37f) bits |= 1;
    if (g.dpad.down.pressed || g.leftThumbstick.yAxis.value < -0.37f) bits |= 2;
    if (g.dpad.left.pressed || g.leftThumbstick.xAxis.value < -0.37f) bits |= 4;
    if (g.dpad.right.pressed || g.leftThumbstick.xAxis.value > 0.37f) bits |= 8;
    if (g.buttonA.pressed || g.buttonB.pressed) bits |= 16;
    g_pjoy = bits;
}

/* ------------------------------------------------------------------------------------------------ picture */

static CGFloat backing_scale(void)
{
    CGFloat s = g_window != nil ? g_window.backingScaleFactor : 1.0;
    return s > 0 ? s : 1.0;
}

/* the image height of the frame full screen on the window's screen: the scanlines' period (as on Windows) */
static int32_t full_screen_rows(int fw, int fh)
{
    NSScreen *sc = g_window.screen ?: NSScreen.mainScreen;
    PresentLayout L;
    CGFloat s, w, h;
    if (sc == nil) return 0;
    s = sc.backingScaleFactor;
    w = sc.frame.size.width;
    h = sc.frame.size.height;
    if (@available(macOS 12.0, *)) h -= sc.safeAreaInsets.top;     /* the camera housing: full screen sits below it */
    present_layout(&L, (int32_t)lround(w * s), (int32_t)lround(h * s), fw, fh,
                   g_cfg.fit ? PRESENT_FIT : PRESENT_INTEGER, g_cfg.aspect43);
    return L.sh;
}

static CGImageRef make_image(const uint32_t *px, int w, int h)
{
    CFDataRef d = CFDataCreate(NULL, (const UInt8 *)px, (CFIndex)w * h * 4);
    CGDataProviderRef p = CGDataProviderCreateWithCFData(d);
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, g_srgb,
                                   kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst, p, NULL,
                                   false, kCGRenderingIntentDefault);
    CGDataProviderRelease(p);
    CFRelease(d);
    return img;
}

/* The picture into the window: without the CRT filter the 320 x 200 frame itself, magnified by the GPU with
 * nearest-neighbour sampling into the layout's image rectangle (the same pixels present_scale gives); with it, the
 * image scaled and filtered by present_scale and present_crt_apply at the backing store's pixel size. */
static void present_now(void)
{
    int fw, fh;
    const uint32_t *frame;
    NSSize b;
    CGFloat s;
    int32_t cw, ch;
    CGImageRef img;
#ifdef MAYA_DEVTEST
    double t0 = now_s();
#endif
    if (g_view == nil || !g_booted) return;
    frame = maya_picture(&fw, &fh);
    b = g_view.bounds.size;
    s = backing_scale();
    cw = (int32_t)lround(b.width * s);
    ch = (int32_t)lround(b.height * s);
    if (cw < 1 || ch < 1 || fw < 1 || fh < 1 || frame == NULL) return;
    present_layout(&g_layout, cw, ch, fw, fh, g_cfg.fit ? PRESENT_FIT : PRESENT_INTEGER, g_cfg.aspect43);
    if (g_layout.sw < 1 || g_layout.sh < 1) return;
    if (g_cfg.crt) {
        PresentLayout L = g_layout;              /* the image rectangle alone: the black border is the view's */
        size_t need = (size_t)L.sw * (size_t)L.sh;
        int32_t ph;
        L.cw = L.sw;
        L.ch = L.sh;
        L.ox = L.oy = 0;
        if (need > g_client_px) {
            free(g_client);
            g_client = (uint32_t *)malloc(need * 4);
            g_client_px = g_client != NULL ? need : 0;
            if (g_client == NULL) return;
        }
        present_scale(&L, frame, g_client);
        ph = full_screen_rows(fw, fh);
        if (!present_crt_matches(&g_crt, &L, ph)) present_crt_set(&g_crt, &L, fw, fh, ph);
        present_crt_apply(&g_crt, g_client);
        img = make_image(g_client, L.sw, L.sh);
    } else {
        img = make_image(frame, fw, fh);
    }
    if (img == NULL) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    g_image.frame = CGRectMake(g_layout.ox / s, (ch - g_layout.oy - g_layout.sh) / s, g_layout.sw / s, g_layout.sh / s);
    g_image.contents = (__bridge id)img;
    [CATransaction commit];
    CGImageRelease(img);
#ifdef MAYA_DEVTEST
    devtest_present((now_s() - t0) * 1000.0);
#endif
}

/* ------------------------------------------------------------------------------------------------ the game loop */

static uint64_t g_next;             /* the PAL clock's next frame (mach time) when there is no sound */
static mach_timebase_info_data_t g_tb;

static uint64_t mach_from_s(double s) { return (uint64_t)(s * 1e9 * g_tb.denom / g_tb.numer); }

static void game_over(void)
{
    const char *e = maya_error();
    g_ended = 1;
    if (e != NULL && e[0]) message([NSString stringWithFormat:@"The game stopped:\n\n%@", ns(e)], NSAlertStyleCritical);
    [NSApp terminate:nil];
}

static int run_one(void)
{
#ifdef MAYA_DEVTEST
    devtest_frame(g_frames);
    if (g_ended) return 0;
#endif
    maya_joystick(g_kjoy | g_pjoy);
    if (!maya_run_frame()) {
        game_over();
        return 0;
    }
    g_frames++;
    audio_pump();
    return 1;
}

/* runs the frames the sound (or the clock) needs; presents the last one */
static void tick(void)
{
    int ran = 0;
    if (g_pause > 0 || g_minimized || g_ended || !g_booted) return;
    pad_poll();
    if (g_aq != NULL) {
        while (!g_ended && ring_count() < AUDIO_TARGET && ran < 4) {
            if (!run_one()) return;
            ran++;
        }
    } else {
        uint64_t now = mach_absolute_time();
        uint64_t period = mach_from_s((double)MAYA_FRAME_HZ_DEN / MAYA_FRAME_HZ_NUM);
        if (now > g_next + mach_from_s(0.2)) g_next = now;
        while (!g_ended && now >= g_next && ran < 4) {
            if (!run_one()) return;
            g_next += period;
            ran++;
        }
    }
    if (ran && !g_ended) present_now();
}

/* ------------------------------------------------------------------------------------------------ input */

static void keymap_init(void)
{
    /* macOS virtual key codes (HIToolbox/Events.h, kVK_...) to Amiga raw keys */
    static const struct { int vk; int raw; } map[] = {
        { 0x32, 0x00 }, { 0x12, 0x01 }, { 0x13, 0x02 }, { 0x14, 0x03 }, { 0x15, 0x04 }, { 0x17, 0x05 },
        { 0x16, 0x06 }, { 0x1a, 0x07 }, { 0x1c, 0x08 }, { 0x19, 0x09 }, { 0x1d, 0x0a }, { 0x1b, 0x0b },
        { 0x18, 0x0c }, { 0x2a, 0x0d }, { 0x33, 0x41 }, { 0x30, 0x42 },
        { 0x0c, 0x10 }, { 0x0d, 0x11 }, { 0x0e, 0x12 }, { 0x0f, 0x13 }, { 0x11, 0x14 }, { 0x10, 0x15 },
        { 0x20, 0x16 }, { 0x22, 0x17 }, { 0x1f, 0x18 }, { 0x23, 0x19 }, { 0x21, 0x1a }, { 0x1e, 0x1b },
        { 0x24, 0x44 },
        { 0x00, 0x20 }, { 0x01, 0x21 }, { 0x02, 0x22 }, { 0x03, 0x23 }, { 0x05, 0x24 }, { 0x04, 0x25 },
        { 0x26, 0x26 }, { 0x28, 0x27 }, { 0x25, 0x28 }, { 0x29, 0x29 }, { 0x27, 0x2a },
        { 0x0a, 0x30 },                                                              /* ISO: the extra key */
        { 0x06, 0x31 }, { 0x07, 0x32 }, { 0x08, 0x33 }, { 0x09, 0x34 }, { 0x0b, 0x35 }, { 0x2d, 0x36 },
        { 0x2e, 0x37 }, { 0x2b, 0x38 }, { 0x2f, 0x39 }, { 0x2c, 0x3a },
        { 0x31, 0x40 }, { 0x35, 0x45 }, { 0x75, 0x46 },
        { 0x7a, 0x50 }, { 0x78, 0x51 }, { 0x63, 0x52 }, { 0x76, 0x53 }, { 0x60, 0x54 }, { 0x61, 0x55 },
        { 0x62, 0x56 }, { 0x64, 0x57 }, { 0x65, 0x58 }, { 0x6d, 0x59 },
        { 0x67, 0x5f }, { 0x72, 0x5f }, { 0x73, 0x5f },                              /* F11, Help, Home: Help */
        { 0x7e, 0x4c }, { 0x7d, 0x4d }, { 0x7c, 0x4e }, { 0x7b, 0x4f },
        /* the numeric keypad */
        { 0x52, 0x0f }, { 0x53, 0x1d }, { 0x54, 0x1e }, { 0x55, 0x1f }, { 0x56, 0x2d }, { 0x57, 0x2e },
        { 0x58, 0x2f }, { 0x59, 0x3d }, { 0x5b, 0x3e }, { 0x5c, 0x3f }, { 0x41, 0x3c }, { 0x4c, 0x43 },
        { 0x4e, 0x4a }, { 0x45, 0x5e }, { 0x43, 0x5d }, { 0x4b, 0x5c },
        /* the modifiers (Command stays the Mac's) */
        { 0x38, 0x60 }, { 0x3c, 0x61 }, { 0x39, 0x62 }, { 0x3b, 0x63 }, { 0x3e, 0x63 }, { 0x3a, 0x64 },
        { 0x3d, 0x65 },
    };
    size_t k;
    memset(g_amiga, 0xff, sizeof g_amiga);
    for (k = 0; k < sizeof map / sizeof map[0]; k++) g_amiga[map[k].vk] = (uint8_t)map[k].raw;
}

static int is_keypad(int vk)
{
    return vk == 0x41 || vk == 0x43 || vk == 0x45 || vk == 0x47 || vk == 0x4b || vk == 0x4c || vk == 0x4e ||
           (vk >= 0x51 && vk <= 0x5c);
}

/* the joystick bits of a key (D15): arrows, the keypad's 1-9 and its fire keys */
static int key_joy(int vk)
{
    switch (vk) {
    case 0x7e: case 0x5b: return 1;
    case 0x7d: case 0x54: return 2;
    case 0x7b: case 0x56: return 4;
    case 0x7c: case 0x58: return 8;
    case 0x59: return 1 | 4;
    case 0x5c: return 1 | 8;
    case 0x53: return 2 | 4;
    case 0x55: return 2 | 8;
    case 0x3b: case 0x3e: case 0x52: case 0x4c: return 16;
    default: return 0;
    }
}

/* letters, "?" and "/" by the character the key types in the current layout (D17), on the Amiga's US keyboard */
static int char_raw(NSEvent *e)
{
    static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
    NSString *s = e.charactersIgnoringModifiers;
    unichar c;
    const char *p;
    if (s.length != 1) return -1;
    c = [s characterAtIndex:0];
    if (c >= 'A' && c <= 'Z') c = (unichar)(c - 'A' + 'a');
    if (c == '?' || c == '/') return 0x3a;
    if (c < 'a' || c > 'z') return -1;
    if ((p = strchr(row1, (char)c)) != NULL) return 0x10 + (int)(p - row1);
    if ((p = strchr(row2, (char)c)) != NULL) return 0x20 + (int)(p - row2);
    if ((p = strchr(row3, (char)c)) != NULL) return 0x31 + (int)(p - row3);
    return -1;
}

static void joy_update(void)
{
    int k, b = 0;
    for (k = 0; k < 128; k++) b |= g_jkey[k];
    g_kjoy = b;
}

static void key_down(int vk, int raw)
{
    if (vk < 0 || vk >= 128 || g_sent[vk]) return;
    g_jkey[vk] = (uint8_t)key_joy(vk);
    joy_update();
    if (raw != 0xff) {
        g_sent[vk] = (uint8_t)(raw + 1);
        maya_key(raw, 1);
    }
}

static void key_up(int vk)
{
    if (vk < 0 || vk >= 128) return;
    if (g_sent[vk]) maya_key(g_sent[vk] - 1, 0);
    g_sent[vk] = 0;
    g_jkey[vk] = 0;
    joy_update();
}

static void keys_release(void)
{
    memset(g_sent, 0, sizeof g_sent);
    memset(g_jkey, 0, sizeof g_jkey);
    g_kjoy = 0;
    g_mbuttons = 0;
    if (!g_booted) return;
    maya_keys_release();
    maya_joystick(0);
}

/* ------------------------------------------------------------------------------------------------ About */

static const char g_license[] =
    "MIT License\n\n"
    "Copyright (c) 2026 Onur BUYUKCAGLAR\n\n"
    "Permission is hereby granted, free of charge, to any person obtaining a copy of this "
    "software and associated documentation files (the \"Software\"), to deal in the Software "
    "without restriction, including without limitation the rights to use, copy, modify, merge, "
    "publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons "
    "to whom the Software is furnished to do so, subject to the following conditions:\n\n"
    "The above copyright notice and this permission notice shall be included in all copies or "
    "substantial portions of the Software.\n\n"
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, "
    "INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR "
    "PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE "
    "FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR "
    "OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER "
    "DEALINGS IN THE SOFTWARE.\n\n"
    "The licence covers this port's own code (" HOST_CAPTION "). Le F\xC3\xA9tiche Maya, its "
    "program and its data belong to their owners; they are not part of this software, which "
    "needs a copy of the original Amiga game to run.\n\n"
    "Acknowledgements\n\n"
    "The game's fixes for modern machines (PAL display, the interrupt-driven sound timer and two data "
    "fixes) follow the WHDLoad installer of the game by Mr.Larmer and CFou!; no code of it is included.\n\n"
    "The CRT scanlines filter follows the math of the CRT filter of DOSBox Pure Unleashed by Bernhard "
    "Schelling; it is a reimplementation, and no code of it is included.";

@interface MayaAbout : NSObject
@end

@implementation MayaAbout
- (void)ok:(id)sender
{
    (void)sender;
    [NSApp stopModal];
}
- (void)link:(id)sender
{
    (void)sender;
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@HOST_ABOUT_URL]];
}
@end

static MayaAbout *g_about_target;

static NSTextField *label(NSString *text, NSRect r, NSFont *font)
{
    NSTextField *t = [NSTextField labelWithString:text];
    t.frame = r;
    t.font = font;
    return t;
}

static void about_box(void)
{
    const CGFloat W = 500, H = 420;
    NSView *v;
    NSImageView *icon;
    NSButton *link, *ok;
    NSScrollView *sv;
    NSTextView *tv;
    if (g_about == nil) {
        g_about_target = [[MayaAbout alloc] init];
        g_about = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                               backing:NSBackingStoreBuffered defer:NO];
        g_about.title = [NSString stringWithFormat:@"About %@ v%s", ns(HOST_CAPTION), HOST_VERSION_STR];
        g_about.releasedWhenClosed = NO;
        v = g_about.contentView;
        icon = [NSImageView imageViewWithImage:NSApp.applicationIconImage];
        icon.frame = NSMakeRect(20, H - 20 - 64, 64, 64);
        [v addSubview:icon];
        [v addSubview:label([NSString stringWithFormat:@"%@ v%s", ns(HOST_CAPTION), HOST_VERSION_STR],
                            NSMakeRect(100, H - 40, W - 120, 20), [NSFont boldSystemFontOfSize:14])];
        [v addSubview:label(@"64bit version by " HOST_AUTHOR, NSMakeRect(100, H - 60, W - 120, 17),
                            [NSFont systemFontOfSize:12])];
        [v addSubview:label(@"with Anthropic Claude AI assistance", NSMakeRect(100, H - 77, W - 120, 17),
                            [NSFont systemFontOfSize:12])];
        link = [NSButton buttonWithTitle:@HOST_ABOUT_URL target:g_about_target action:@selector(link:)];
        link.bordered = NO;
        link.attributedTitle = [[NSAttributedString alloc] initWithString:@HOST_ABOUT_URL attributes:@{
            NSForegroundColorAttributeName: NSColor.linkColor,
            NSFontAttributeName: [NSFont systemFontOfSize:12],
            NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle)
        }];
        [link sizeToFit];
        [link setFrameOrigin:NSMakePoint(100, H - 98)];
        [v addSubview:link];
        [v addSubview:label(@"Licenses:", NSMakeRect(20, H - 128, W - 40, 17), [NSFont systemFontOfSize:12])];
        sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 56, W - 40, H - 128 - 62)];
        sv.hasVerticalScroller = YES;
        sv.borderType = NSBezelBorder;
        tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, sv.contentSize.width, sv.contentSize.height)];
        tv.editable = NO;
        tv.selectable = YES;
        tv.verticallyResizable = YES;
        tv.textContainer.widthTracksTextView = YES;
        tv.textContainerInset = NSMakeSize(4, 4);
        tv.font = [NSFont systemFontOfSize:11];
        tv.string = ns(g_license);
        sv.documentView = tv;
        [v addSubview:sv];
        ok = [NSButton buttonWithTitle:@"OK" target:g_about_target action:@selector(ok:)];
        ok.keyEquivalent = @"\r";
        ok.frame = NSMakeRect(W - 20 - 90, 14, 90, 32);
        [v addSubview:ok];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:g_about
                                                           queue:nil usingBlock:^(NSNotification *n) {
            (void)n;
            [NSApp stopModal];
        }];
    }
    g_pause++;                                  /* the About box pauses the game */
    keys_release();
    [g_about center];
    [NSApp runModalForWindow:g_about];
    [g_about orderOut:nil];
    g_pause--;
}

/* ------------------------------------------------------------------------------------------------ the view */

@interface MayaView : NSView
@end

@implementation MayaView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (BOOL)wantsUpdateLayer { return YES; }

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    present_now();
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    self.layer.contentsScale = backing_scale();
    g_image.contentsScale = backing_scale();
    present_now();
}

- (void)keyDown:(NSEvent *)e
{
    int vk = e.keyCode, raw;
    if (e.modifierFlags & NSEventModifierFlagCommand) return;     /* the Mac's: menu shortcuts */
    if (e.isARepeat || vk >= 128) return;
    raw = g_amiga[vk];
    if (!is_keypad(vk)) {
        int c = char_raw(e);
        if (c >= 0) raw = c;
    }
    key_down(vk, raw);
}

- (void)keyUp:(NSEvent *)e
{
    key_up(e.keyCode);
}

/* the modifier keys: down or up by their own (device-dependent) flag bits */
- (void)flagsChanged:(NSEvent *)e
{
    NSUInteger f = e.modifierFlags;
    int vk = e.keyCode, down;
    switch (vk) {
    case 0x38: down = (f & 0x02) != 0; break;          /* left Shift */
    case 0x3c: down = (f & 0x04) != 0; break;          /* right Shift */
    case 0x3b: down = (f & 0x01) != 0; break;          /* left Control */
    case 0x3e: down = (f & 0x2000) != 0; break;        /* right Control */
    case 0x3a: down = (f & 0x20) != 0; break;          /* left Option */
    case 0x3d: down = (f & 0x40) != 0; break;          /* right Option */
    case 0x39: down = (f & NSEventModifierFlagCapsLock) != 0; break;
    default: return;
    }
    if (down) key_down(vk, g_amiga[vk]);
    else key_up(vk);
}

- (void)mouseAt:(NSEvent *)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    CGFloat s = backing_scale();
    double px = p.x * s, py = (self.bounds.size.height - p.y) * s;
    int gx, gy;
    g_last_mouse = now_s();
    if (g_layout.sw < 1 || g_layout.sh < 1 || !g_booted) return;
    gx = (int)floor((px - g_layout.ox) * g_layout.fw / g_layout.sw);
    gy = (int)floor((py - g_layout.oy) * g_layout.fh / g_layout.sh);
    maya_mouse(gx, gy, g_mbuttons);
}

- (void)mouseMoved:(NSEvent *)e { [self mouseAt:e]; }
- (void)mouseDragged:(NSEvent *)e { [self mouseAt:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self mouseAt:e]; }
- (void)mouseDown:(NSEvent *)e { g_mbuttons |= 1; [self mouseAt:e]; }
- (void)mouseUp:(NSEvent *)e { g_mbuttons &= ~1; [self mouseAt:e]; }
- (void)rightMouseDown:(NSEvent *)e { g_mbuttons |= 2; [self mouseAt:e]; }
- (void)rightMouseUp:(NSEvent *)e { g_mbuttons &= ~2; [self mouseAt:e]; }
@end

/* ------------------------------------------------------------------------------------------------ the application */

@interface MayaApp : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuItemValidation>
@end

@implementation MayaApp

- (void)tick:(NSTimer *)t
{
    (void)t;
    tick();
}

/* full screen: the pointer hides after 2 s without movement (D16) */
- (void)cursorTick:(NSTimer *)t
{
    (void)t;
    if (g_full && !g_pause && now_s() - g_last_mouse > 2.0 && NSApp.isActive) [NSCursor setHiddenUntilMouseMoves:YES];
}

- (void)toggleIntegerScaling:(id)sender { (void)sender; g_cfg.fit = 0; present_now(); }
- (void)toggleFit:(id)sender { (void)sender; g_cfg.fit = 1; present_now(); }

- (void)toggleAspect:(id)sender
{
    (void)sender;
    g_cfg.aspect43 = !g_cfg.aspect43;
    g_aspect_file = -1;                        /* the menu's choice is saved */
    present_now();
}

- (void)toggleCrt:(id)sender
{
    (void)sender;
    g_cfg.crt = !g_cfg.crt;
    g_crt_file = -1;                           /* the menu's choice is saved */
    present_now();
}

- (void)about:(id)sender
{
    (void)sender;
    about_box();
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    SEL a = item.action;
    if (a == @selector(toggleIntegerScaling:)) item.state = g_cfg.fit ? NSControlStateValueOff : NSControlStateValueOn;
    else if (a == @selector(toggleFit:)) item.state = g_cfg.fit ? NSControlStateValueOn : NSControlStateValueOff;
    else if (a == @selector(toggleAspect:)) item.state = g_cfg.aspect43 ? NSControlStateValueOn : NSControlStateValueOff;
    else if (a == @selector(toggleCrt:)) item.state = g_cfg.crt ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}

- (void)windowDidMiniaturize:(NSNotification *)n { (void)n; g_minimized = 1; }
- (void)windowDidDeminiaturize:(NSNotification *)n { (void)n; g_minimized = 0; present_now(); }
- (void)windowDidResignKey:(NSNotification *)n { (void)n; keys_release(); }
- (void)windowDidEnterFullScreen:(NSNotification *)n { (void)n; g_full = 1; g_last_mouse = now_s(); present_now(); }
- (void)windowDidExitFullScreen:(NSNotification *)n { (void)n; g_full = 0; present_now(); }
- (void)windowDidChangeScreen:(NSNotification *)n { (void)n; present_now(); }
- (void)windowWillClose:(NSNotification *)n { (void)n; [NSApp terminate:nil]; }

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app { (void)app; return YES; }

- (void)applicationWillTerminate:(NSNotification *)n
{
    (void)n;
    if (g_booted) cfg_save();
    audio_close();
    present_crt_free(&g_crt);
}

/* the game's folder: --data, the settings, next to the application, or chosen by the player (remembered) */
- (NSString *)resolveData:(NSString *)arg
{
    NSString *d;
    if (arg != nil) {
        if ((d = data_find(arg)) != nil) return d;
        message([NSString stringWithFormat:@"The folder given with --data does not hold the game:\n\n%@", arg],
                NSAlertStyleWarning);
    }
    if (g_cfg.data[0] && (d = data_find(ns(g_cfg.data))) != nil) return d;
    if ((d = data_find(NSBundle.mainBundle.bundlePath.stringByDeletingLastPathComponent)) != nil) return d;
    message(@"Le Fétiche Maya 64bit needs the files of the original Amiga game (the folder with MAIN.CO and "
            @"the other data files).\n\nChoose the game's folder in the next window. It is remembered for the next "
            @"start.", NSAlertStyleInformational);
    for (;;) {
        NSOpenPanel *p = [NSOpenPanel openPanel];
        NSAlert *a;
        p.canChooseDirectories = YES;
        p.canChooseFiles = NO;
        p.allowsMultipleSelection = NO;
        p.message = @"Choose the folder of the Amiga game (it holds MAIN.CO)";
        p.prompt = @"Choose";
        if ([p runModal] != NSModalResponseOK || p.URL == nil) return nil;
        if ((d = data_find(p.URL.path)) != nil) return d;
        a = [[NSAlert alloc] init];
        a.alertStyle = NSAlertStyleWarning;
        a.messageText = ns(HOST_CAPTION);
        a.informativeText = @"That folder does not hold the game's files (MAIN.CO was not found). Choose another?";
        [a addButtonWithTitle:@"Choose Another"];
        [a addButtonWithTitle:@"Quit"];
        if ([a runModal] != NSAlertFirstButtonReturn) return nil;
    }
}

- (void)buildMenus
{
    NSMenu *bar = [[NSMenu alloc] init], *m;
    NSMenuItem *it;
    NSString *name = ns(HOST_CAPTION);

    m = [[NSMenu alloc] initWithTitle:name];
    [m addItemWithTitle:[@"About " stringByAppendingString:name] action:@selector(about:) keyEquivalent:@""].target = self;
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
    it = [m addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    it.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [m addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(terminate:) keyEquivalent:@"q"];
    it = [[NSMenuItem alloc] init];
    it.submenu = m;
    [bar addItem:it];

    m = [[NSMenu alloc] initWithTitle:@"View"];
    it = [m addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    it.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:@"Integer Scaling" action:@selector(toggleIntegerScaling:) keyEquivalent:@""].target = self;
    [m addItemWithTitle:@"Fit to Window" action:@selector(toggleFit:) keyEquivalent:@""].target = self;
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:@"4:3 Monitor Aspect" action:@selector(toggleAspect:) keyEquivalent:@""].target = self;
    [m addItemWithTitle:@"CRT Scanlines" action:@selector(toggleCrt:) keyEquivalent:@""].target = self;
    it = [[NSMenuItem alloc] init];
    it.submenu = m;
    [bar addItem:it];

    m = [[NSMenu alloc] initWithTitle:@"Window"];
    [m addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [m addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    it = [[NSMenuItem alloc] init];
    it.submenu = m;
    [bar addItem:it];
    NSApp.windowsMenu = m;

    NSApp.mainMenu = bar;
}

/* the window's first content size: the largest whole multiple of 320 x 200 (or 320 x 240) points within 85% of
 * the screen's visible area, at least 1x */
static NSSize first_size(void)
{
    NSRect wa = (NSScreen.mainScreen ?: NSScreen.screens.firstObject).visibleFrame;
    int k, fh = g_cfg.aspect43 ? 240 : 200;
    for (k = 6; k > 1; k--)
        if (320 * k <= wa.size.width * 0.85 && fh * k <= wa.size.height * 0.85) break;
    return NSMakeSize(320 * k, fh * k);
}

- (void)makeWindow
{
    NSRect r = { NSZeroPoint, first_size() };
    g_window = [[NSWindow alloc] initWithContentRect:r
                                           styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                     NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                             backing:NSBackingStoreBuffered defer:NO];
    g_window.title = ns(HOST_CAPTION);
    g_window.delegate = self;
    g_window.releasedWhenClosed = NO;
    g_window.contentMinSize = NSMakeSize(320, 200);
    g_window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    g_window.acceptsMouseMovedEvents = YES;
    g_window.backgroundColor = NSColor.blackColor;
    g_view = [[MayaView alloc] initWithFrame:r];
    g_view.wantsLayer = YES;
    g_view.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    g_view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    g_image = [CALayer layer];
    g_image.magnificationFilter = kCAFilterNearest;
    g_image.minificationFilter = kCAFilterNearest;
    g_image.contentsGravity = kCAGravityResize;
    g_image.actions = @{ @"contents": [NSNull null], @"bounds": [NSNull null], @"position": [NSNull null] };
    [g_view.layer addSublayer:g_image];
    g_window.contentView = g_view;
    if (![g_window setFrameUsingName:@"MayaWindow"]) [g_window center];
    g_window.frameAutosaveName = @"MayaWindow";
    [g_window makeFirstResponder:g_view];
}

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    NSArray<NSString *> *args = NSProcessInfo.processInfo.arguments;
    NSString *data_arg = nil, *data;
    char err[600];
    int crt = -1, aspect = -1;
    NSUInteger k;
    (void)n;
    for (k = 1; k < args.count; k++) {
        if ([args[k] isEqualToString:@"--data"] && k + 1 < args.count) data_arg = args[++k];
        else if ([args[k] isEqualToString:@"--crt"] && k + 1 < args.count) crt = args[++k].intValue != 0;
        else if ([args[k] isEqualToString:@"--aspect"] && k + 1 < args.count) aspect = [args[++k] isEqualToString:@"4:3"];
    }
    [NSUserDefaults.standardUserDefaults registerDefaults:@{ @"NSFullScreenMenuItemEverywhere": @NO }];
    g_srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    mach_timebase_info(&g_tb);
    paths_init();
#ifdef MAYA_DEVTEST
    devtest_init();
#endif
    cfg_load();
    if (aspect >= 0) {
        g_aspect_file = g_cfg.aspect43;        /* --aspect: this run only */
        g_cfg.aspect43 = aspect;
    }
    if (crt >= 0) {
        g_crt_file = g_cfg.crt;                /* --crt: this run only */
        g_cfg.crt = crt;
    }
    [self buildMenus];
    [NSApp activateIgnoringOtherApps:YES];
    data = [self resolveData:data_arg];
    if (data == nil) {
        [NSApp terminate:nil];
        return;
    }
    snprintf(g_cfg.data, sizeof g_cfg.data, "%s", data.fileSystemRepresentation);
    if (maya_boot(data.fileSystemRepresentation, g_savedir, err, sizeof err) != 0) {
        message(ns(err), NSAlertStyleCritical);
        [NSApp terminate:nil];
        return;
    }
    g_booted = 1;
    keymap_init();
    audio_open();
    g_next = mach_absolute_time();
    [self makeWindow];
    [g_window makeKeyAndOrderFront:nil];
    present_now();
    if (g_cfg.fullscreen) [g_window toggleFullScreen:nil];
    g_activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep |
                                                                     NSActivityLatencyCritical
                                                              reason:@"Playing the game"];
    g_timer = [NSTimer timerWithTimeInterval:0.004 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    g_timer.tolerance = 0;
    [NSRunLoop.currentRunLoop addTimer:g_timer forMode:NSRunLoopCommonModes];
    [NSRunLoop.currentRunLoop addTimer:[NSTimer timerWithTimeInterval:0.5 target:self selector:@selector(cursorTick:)
                                                             userInfo:nil repeats:YES]
                               forMode:NSRunLoopCommonModes];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { (void)app; return YES; }
@end

#ifdef MAYA_DEVTEST
#include "maya_devtest.inc"
#endif

/* ------------------------------------------------------------------------------------------------ entry */

int main(int argc, const char **argv)
{
    @autoreleasepool {
        MayaApp *app;
        (void)argc;
        (void)argv;
        [NSApplication sharedApplication];
        NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
        app = [[MayaApp alloc] init];
        NSApp.delegate = app;
        [NSApp run];
    }
    return 0;
}
