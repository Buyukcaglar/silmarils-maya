/*
 * maya_devtest.inc - development checks of the macOS app, built only with -DMAYA_DEVTEST=ON (never in a release).
 * Included by maya_macos.m, so it sees the host's state.
 *
 * With the environment variable MAYA_DEVTEST=<folder>, the app keeps its settings and saves in <folder> and runs a
 * script at fixed game frames, in its own process: key events go through the window (NSWindow sendEvent:, so the
 * key map, the character mapping and the joystick keys are the real ones), the View menu's commands are called,
 * full screen is entered and left, and the About box is opened.  It writes the game's picture as BMP (to compare
 * with maya_cli's frames of the same input: docs/MACOS-PORT.md), the window's contents as PNG, and log.txt (frame
 * rate, sound underruns, presentation time), then quits.  The About box's capture leaves out the window's
 * background, so in dark mode its labels are white on white: MAYA_DEVTEST_APPEARANCE=NSAppearanceNameAqua gives a
 * readable one.
 */

static NSString *g_dt_dir;
static double g_dt_t0, g_dt_present_ms, g_dt_present_max;
static long g_dt_presents;
static long g_dt_t0_frame;
static FILE *g_dt_log;

static void dt_log(const char *fmt, ...)
{
    va_list ap;
    if (g_dt_log == NULL) return;
    va_start(ap, fmt);
    vfprintf(g_dt_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_dt_log);
    fflush(g_dt_log);
}

static const char *dt_path(const char *name)
{
    return [g_dt_dir stringByAppendingPathComponent:ns(name)].fileSystemRepresentation;
}

static void dt_bmp(const char *name)
{
    int w, h, x, y;
    const uint32_t *px = maya_picture(&w, &h);
    FILE *f = fopen(dt_path(name), "wb");
    uint32_t hdr[13];
    if (f == NULL) return;
    fputc('B', f);
    fputc('M', f);
    hdr[0] = 54u + (uint32_t)(w * h * 4); hdr[1] = 0; hdr[2] = 54; hdr[3] = 40; hdr[4] = (uint32_t)w; hdr[5] = (uint32_t)h;
    hdr[6] = 1u | (32u << 16); hdr[7] = 0; hdr[8] = (uint32_t)(w * h * 4); hdr[9] = 2835; hdr[10] = 2835; hdr[11] = 0;
    hdr[12] = 0;
    fwrite(hdr, 4, 13, f);                        /* little-endian host: the BMP header as maya_cli writes it */
    for (y = h - 1; y >= 0; y--)
        for (x = 0; x < w; x++) fwrite(&px[y * w + x], 4, 1, f);
    fclose(f);
    dt_log("frame %ld: picture %s", g_frames, name);
}

static void dt_png_rep(NSBitmapImageRep *rep, const char *name)
{
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:ns(dt_path(name)) atomically:YES];
}

/* the window's contents as the compositor gets them (the layer tree rendered) */
static void dt_snap(const char *name)
{
    NSView *v = g_window.contentView;
    CGFloat s = backing_scale();
    NSSize b = v.bounds.size;
    size_t w = (size_t)lround(b.width * s), h = (size_t)lround(b.height * s);
    CGContextRef c = CGBitmapContextCreate(NULL, w, h, 8, 0, g_srgb,
                                           kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaPremultipliedFirst);
    CGImageRef img;
    if (c == NULL) return;
    CGContextSetInterpolationQuality(c, kCGInterpolationNone);
    CGContextScaleCTM(c, s, s);
    [v.layer renderInContext:c];
    img = CGBitmapContextCreateImage(c);
    dt_png_rep([[NSBitmapImageRep alloc] initWithCGImage:img], name);
    CGImageRelease(img);
    CGContextRelease(c);
    dt_log("frame %ld: window %s, %zu x %zu px (scale %.1f), image at %d,%d size %d x %d, full screen %d, "
           "sound buffers dry so far %ld", g_frames, name, w, h, s, g_layout.ox, g_layout.oy, g_layout.sw, g_layout.sh,
           g_full, (long)g_underruns);
}

static void dt_key(int vk, NSString *chars, int down, NSEventModifierFlags mods)
{
    NSEvent *e = [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp location:NSZeroPoint
                             modifierFlags:mods timestamp:NSProcessInfo.processInfo.systemUptime
                              windowNumber:g_window.windowNumber context:nil characters:chars
               charactersIgnoringModifiers:chars isARepeat:NO keyCode:(unsigned short)vk];
    [g_window sendEvent:e];
}

static void dt_about(void)
{
    NSTimer *t = [NSTimer timerWithTimeInterval:1.0 repeats:NO block:^(NSTimer *tm) {
        NSView *v = g_about.contentView;
        NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
        (void)tm;
        [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
        dt_png_rep(rep, "about.png");
        dt_log("frame %ld: About box shown (game paused: %d), about.png", g_frames, g_pause);
        [NSApp stopModal];
    }];
    [NSRunLoop.currentRunLoop addTimer:t forMode:NSModalPanelRunLoopMode];
    if (getenv("MAYA_DEVTEST_APPEARANCE")) NSApp.appearance = [NSAppearance appearanceNamed:ns(getenv("MAYA_DEVTEST_APPEARANCE"))];
    about_box();
}

static void dt_finish(void)
{
    dt_log("sound: %s, %ld buffers ran dry after the start", g_aq ? "AudioQueue" : "none (PAL clock)", (long)g_underruns);
    dt_log("presents: %ld, average %.2f ms, max %.2f ms", g_dt_presents,
           g_dt_presents ? g_dt_present_ms / g_dt_presents : 0.0, g_dt_present_max);
    dt_log("done");
    fclose(g_dt_log);
    g_dt_log = NULL;
    [NSApp terminate:nil];
}

/* the script, run before game frame f (the same frame numbers as maya_cli's --key/--joy/--shot) */
static void devtest_frame(long f)
{
    MayaApp *app = (MayaApp *)NSApp.delegate;
    if (g_dt_dir == nil) return;
    switch (f) {
    case 100: g_dt_t0 = now_s(); g_dt_t0_frame = f; g_underruns = 0; break;
    case 300: dt_bmp("logo.bmp"); dt_snap("win-logo.png"); break;
    case 1000: dt_key(0x31, @" ", 1, 0); break;                           /* Space: Amiga 0x40 */
    case 1005: dt_key(0x31, @" ", 0, 0); break;
    case 1600: dt_key(0x2c, @"/", 1, 0); break;                           /* "/": Amiga 0x3a, as "?" */
    case 1605: dt_key(0x2c, @"/", 0, 0); break;
    case 2000: dt_key(0x7c, @"", 1, NSEventModifierFlagFunction | NSEventModifierFlagNumericPad); break;
    case 2100: dt_key(0x7c, @"", 0, NSEventModifierFlagFunction | NSEventModifierFlagNumericPad); break;
    case 2400:
        dt_log("frames 100-2400: %.2f frames a second (PAL: %.2f)", (f - g_dt_t0_frame) / (now_s() - g_dt_t0),
               (double)MAYA_FRAME_HZ_NUM / MAYA_FRAME_HZ_DEN);
        dt_bmp("village.bmp");
        dt_snap("win-village.png");
        break;
    case 2410: [app toggleCrt:nil]; break;
    case 2412: dt_snap("win-crt.png"); break;
    case 2420: [app toggleAspect:nil]; break;
    case 2422: dt_snap("win-crt-43.png"); break;
    case 2430: [app toggleFit:nil]; [app toggleCrt:nil]; break;
    case 2432: dt_snap("win-fit-43.png"); break;
    case 2440: [app toggleIntegerScaling:nil]; [app toggleAspect:nil]; [g_window toggleFullScreen:nil]; break;
    case 2600: dt_snap("full.png"); [app toggleCrt:nil]; break;
    case 2602: dt_snap("full-crt.png"); [app toggleCrt:nil]; [g_window toggleFullScreen:nil]; break;
    case 2750: dt_snap("win-after-full.png"); break;
    case 2760: dt_about(); break;
    case 2800: dt_finish(); break;
    default: break;
    }
}

static void devtest_present(double ms)
{
    if (g_dt_dir == nil || g_frames < 100) return;
    g_dt_presents++;
    g_dt_present_ms += ms;
    if (ms > g_dt_present_max) g_dt_present_max = ms;
}

/* MAYA_DEVTEST=<folder>: settings and saves there, the script on */
static void devtest_init(void)
{
    const char *d = getenv("MAYA_DEVTEST");
    if (d == NULL || d[0] == 0) return;
    g_dt_dir = ns(d);
    [NSFileManager.defaultManager createDirectoryAtPath:g_dt_dir withIntermediateDirectories:YES attributes:nil error:nil];
    g_dt_log = fopen(dt_path("log.txt"), "w");
    snprintf(g_cfgpath, sizeof g_cfgpath, "%s", dt_path(HOST_CFG_NAME));
    snprintf(g_savedir, sizeof g_savedir, "%s", dt_path(HOST_SAVES_NAME));
    dt_log("devtest: %s", d);
}
