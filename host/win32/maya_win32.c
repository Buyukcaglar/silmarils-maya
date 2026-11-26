/*
 * maya_win32.c - the Windows host of the recompiled "Le Fetiche Maya" (docs/HOST-DESIGN.md, docs/DECISIONS.md).
 *
 * One native window.  Its system menu (also Alt+Space, in full screen too) holds the host's commands: Full screen
 * (Alt+Enter), Integer scaling / Fit to window, 4:3 aspect, CRT scanlines, and About... above Close.  The title bar,
 * the menus and the About box follow the system's light or dark app mode, as in the user's Civ64 port.  The game
 * runs on this thread: a frame whenever the sound output needs more (waveOut, about 60 ms queued), or on the PAL
 * frame clock when there is no sound device.  A timer keeps it running while Windows runs a modal loop (moving or
 * sizing the window, an open menu).
 *
 * Input: every key goes to the game as the Amiga key in its place (letters, "?" and "/" by the character they type,
 * D17); the arrow keys and the numeric keypad also drive the joystick in port 2, the Ctrl keys, Numpad 0 and
 * keypad Enter are its fire button (D15); an XInput game controller too.  The mouse is the Amiga mouse in port 1.
 *
 * Settings (maya64.cfg) and saves (saves\) sit next to maya64.exe, or in %APPDATA%\Le Fetiche Maya 64bit when that
 * folder cannot be written (D7, D18).  Command line: --data <folder>, --crt 0|1, --aspect 4:3|square (the last two
 * for this run only; the menu's choices are saved).
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define COBJMACROS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <xinput.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../../present/present.h"
#include "../../runtime/maya.h"
#include "host_config.h"

/* ------------------------------------------------------------------------------------------------ state */

#define IDM_FULL    0x0010
#define IDM_INTEGER 0x0020
#define IDM_FIT     0x0030
#define IDM_ASPECT  0x0040
#define IDM_CRT     0x0050
#define IDM_ABOUT   0x0060

#define TIMER_MODAL 1
#define TIMER_CURSOR 2

typedef struct Cfg {
    char data[1024];          /* UTF-8 */
    int x, y, w, h;           /* the window's position and its client size; w 0: none saved */
    int maximized, fullscreen;
    int fit, aspect43, crt;
} Cfg;

static HINSTANCE g_inst;
static HWND g_hwnd;
static Cfg g_cfg;
static wchar_t g_exedir[MAX_PATH];
static wchar_t g_cfgpath[MAX_PATH + 32];
static wchar_t g_savedir[MAX_PATH + 32];
static int g_aspect_file = -1;    /* the file's aspect while --aspect overrides it for this run */
static int g_crt_file = -1;   /* the file's CRT setting while --crt overrides it for this run */
static int g_pause;           /* > 0: the game waits (the About box, a message) */
static int g_minimized;
static int g_ended;
static int g_full;
static WINDOWPLACEMENT g_wp = { sizeof(WINDOWPLACEMENT) };

/* the picture */
static uint32_t *g_client;
static int g_cw, g_ch;
static PresentLayout g_layout;
static PresentCrt g_crt;

/* input */
static uint8_t g_sent[512];   /* per scancode (+256 extended): the Amiga raw key sent + 1, 0 none */
static uint8_t g_jkey[512];   /* per scancode: the joystick bits it holds */
static int g_kjoy, g_pjoy;    /* the joystick from the keys, from a game controller */
static int g_mbuttons;
static DWORD g_last_mouse;

/* ------------------------------------------------------------------------------------------------ utilities */

static void to_utf8(const wchar_t *w, char *s, int n)
{
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) s[0] = 0;
}

static void to_wide(const char *s, wchar_t *w, int n)
{
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) w[0] = 0;
}

static void message(const wchar_t *text, UINT icon)
{
    g_pause++;
    MessageBoxW(g_hwnd, text, HOST_CAPTIONW, MB_OK | icon);
    g_pause--;
}

static int data_ok(const wchar_t *dir)
{
    char s[1024];
    to_utf8(dir, s, sizeof s);
    return s[0] && maya_is_data_dir(s);
}

/* a folder that holds the game, or one with a "data" folder that does: the game's folder in out */
static int data_find(const wchar_t *dir, wchar_t *out, size_t n)
{
    wchar_t t[MAX_PATH + 16];
    if (dir == NULL || dir[0] == 0) return 0;
    if (data_ok(dir)) {
        _snwprintf(out, n, L"%ls", dir);
        return 1;
    }
    _snwprintf(t, MAX_PATH + 16, L"%ls\\data", dir);
    if (data_ok(t)) {
        _snwprintf(out, n, L"%ls", t);
        return 1;
    }
    _snwprintf(t, MAX_PATH + 16, L"%ls\\Maya\\data", dir);
    if (data_ok(t)) {
        _snwprintf(out, n, L"%ls", t);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ settings */

static void cfg_defaults(Cfg *c)
{
    memset(c, 0, sizeof *c);
    c->fit = 0;
    c->aspect43 = 0;
    c->crt = 0;
}

static void cfg_load(void)
{
    FILE *f = _wfopen(g_cfgpath, L"rb");
    char line[1200];
    cfg_defaults(&g_cfg);
    if (f == NULL) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *v, *e;
        if (eq == NULL) continue;
        *eq = 0;
        v = eq + 1;
        e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        if (strcmp(line, "data") == 0) snprintf(g_cfg.data, sizeof g_cfg.data, "%s", v);
        else if (strcmp(line, "window") == 0) sscanf(v, "%d,%d,%d,%d", &g_cfg.x, &g_cfg.y, &g_cfg.w, &g_cfg.h);
        else if (strcmp(line, "maximized") == 0) g_cfg.maximized = atoi(v) != 0;
        else if (strcmp(line, "fullscreen") == 0) g_cfg.fullscreen = atoi(v) != 0;
        else if (strcmp(line, "scale") == 0) g_cfg.fit = strcmp(v, "fit") == 0;
        else if (strcmp(line, "aspect") == 0) g_cfg.aspect43 = strcmp(v, "4:3") == 0;
        else if (strcmp(line, "crt") == 0) g_cfg.crt = atoi(v) != 0;
    }
    fclose(f);
}

static void cfg_save(void)
{
    FILE *f = _wfopen(g_cfgpath, L"wb");
    if (f == NULL) return;
    fprintf(f, "# " HOST_PRODUCT " " HOST_VERSION_STR " settings\r\n");
    fprintf(f, "data=%s\r\n", g_cfg.data);
    if (g_cfg.w > 0) fprintf(f, "window=%d,%d,%d,%d\r\n", g_cfg.x, g_cfg.y, g_cfg.w, g_cfg.h);
    fprintf(f, "maximized=%d\r\nfullscreen=%d\r\n", g_cfg.maximized, g_cfg.fullscreen);
    fprintf(f, "scale=%s\r\naspect=%s\r\ncrt=%d\r\n", g_cfg.fit ? "fit" : "integer", (g_aspect_file >= 0 ? g_aspect_file : g_cfg.aspect43) ? "4:3" : "square",
            g_crt_file >= 0 ? g_crt_file : g_cfg.crt);
    fclose(f);
}

/* the window's normal placement into g_cfg */
static void cfg_take_window(void)
{
    WINDOWPLACEMENT wp = { sizeof wp };
    RECT r;
    if (g_hwnd == NULL) return;
    if (g_full) wp = g_wp;
    else if (!GetWindowPlacement(g_hwnd, &wp)) return;
    r = wp.rcNormalPosition;
    g_cfg.x = r.left;
    g_cfg.y = r.top;
    {
        /* the client size of that normal rectangle */
        RECT a = { 0, 0, 0, 0 };
        UINT dpi = GetDpiForWindow(g_hwnd);
        AdjustWindowRectExForDpi(&a, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        g_cfg.w = (r.right - r.left) - (a.right - a.left);
        g_cfg.h = (r.bottom - r.top) - (a.bottom - a.top);
    }
    g_cfg.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    g_cfg.fullscreen = g_full;
}

/* the settings' and saves' folder: next to the exe when it can be written there, else %APPDATA%\... (D18) */
static void paths_init(void)
{
    wchar_t *slash;
    HANDLE h;
    GetModuleFileNameW(NULL, g_exedir, MAX_PATH);
    slash = wcsrchr(g_exedir, L'\\');
    if (slash) *slash = 0;
    _snwprintf(g_cfgpath, MAX_PATH + 32, L"%ls\\%ls", g_exedir, HOST_CFG_NAME);
    _snwprintf(g_savedir, MAX_PATH + 32, L"%ls\\%ls", g_exedir, HOST_SAVES_NAME);
    h = CreateFileW(g_cfgpath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz;
        int fresh = GetFileSizeEx(h, &sz) && sz.QuadPart == 0;
        CloseHandle(h);
        if (fresh) DeleteFileW(g_cfgpath);     /* written at exit */
        return;
    }
    {
        PWSTR app = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, KF_FLAG_CREATE, NULL, &app))) {
            wchar_t dir[MAX_PATH];
            _snwprintf(dir, MAX_PATH, L"%ls\\%ls", app, HOST_APPDATA_NAME);
            CreateDirectoryW(dir, NULL);
            _snwprintf(g_cfgpath, MAX_PATH + 32, L"%ls\\%ls", dir, HOST_CFG_NAME);
            _snwprintf(g_savedir, MAX_PATH + 32, L"%ls\\%ls", dir, HOST_SAVES_NAME);
            CoTaskMemFree(app);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ theme (F109) */

#define THEME_DARK_BG RGB(32, 32, 32)
#define THEME_DARK_FIELD RGB(43, 43, 43)
#define THEME_DARK_TEXT RGB(255, 255, 255)
#define THEME_DARK_LINK RGB(96, 205, 255)

typedef int(WINAPI *ThemeModeFn)(int);
typedef BOOL(WINAPI *ThemeWindowFn)(HWND, BOOL);
typedef void(WINAPI *ThemeVoidFn)(void);
static ThemeModeFn g_ux_mode;
static ThemeWindowFn g_ux_window;
static ThemeVoidFn g_ux_refresh;
static ThemeVoidFn g_ux_flush;
static int g_ux_tried;
static DWORD g_os_build;
static int g_dark;

static DWORD os_build(void)
{
    typedef LONG(WINAPI * VersionFn)(OSVERSIONINFOW *);
    OSVERSIONINFOW v;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    VersionFn fn = nt != NULL ? (VersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (fn == NULL || fn(&v) != 0 || v.dwMajorVersion < 10u) return 0u;
    return v.dwBuildNumber;
}

static void theme_load(void)
{
    HMODULE ux;
    if (g_ux_tried) return;
    g_ux_tried = 1;
    g_os_build = os_build();
    if (g_os_build < 18362u || (ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)) == NULL) return;
    g_ux_mode = (ThemeModeFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    g_ux_window = (ThemeWindowFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    g_ux_refresh = (ThemeVoidFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(104));
    g_ux_flush = (ThemeVoidFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (g_ux_mode == NULL || g_ux_window == NULL || g_ux_refresh == NULL || g_ux_flush == NULL) {
        g_ux_mode = NULL;
        g_ux_window = NULL;
        g_ux_refresh = NULL;
        g_ux_flush = NULL;
    }
}

static int theme_wanted(void)
{
    DWORD v = 1u, n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &n) != ERROR_SUCCESS) {
        return 0;
    }
    return v == 0u;
}

static int theme_setting(LPARAM l)
{
    return l != 0 && CompareStringOrdinal((LPCWCH)l, -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL;
}

static void theme_window(HWND h, int dark)
{
    BOOL on = dark ? TRUE : FALSE;
    theme_load();
    if (g_ux_window != NULL) (void)g_ux_window(h, on);
    if (g_os_build >= 17763u) (void)DwmSetWindowAttribute(h, g_os_build >= 18985u ? 20u : 19u, &on, sizeof on);
}

static void theme_apply(int redraw)
{
    theme_load();
    g_dark = theme_wanted();
    if (g_ux_mode != NULL) {
        (void)g_ux_mode(g_dark ? 2 : 3);       /* ForceDark, ForceLight */
        g_ux_refresh();
        g_ux_flush();
    }
    if (g_hwnd != NULL) {
        theme_window(g_hwnd, g_dark);
        if (redraw) {
            SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ sound */

#define AUDIO_BUFS 12
#define AUDIO_FRAMES 512            /* per buffer: 10.7 ms */
#define AUDIO_TARGET 6              /* buffers kept queued: 64 ms */

static HWAVEOUT g_wave;
static HANDLE g_wave_event;
static WAVEHDR g_hdr[AUDIO_BUFS];
static int16_t g_wbuf[AUDIO_BUFS][AUDIO_FRAMES * 2];
static int16_t g_fifo[16384 * 2];
static size_t g_fifo_n;
static int g_wave_paused;

static void audio_open(void)
{
    WAVEFORMATEX fmt;
    int k;
    memset(&fmt, 0, sizeof fmt);
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = MAYA_AUDIO_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = MAYA_AUDIO_RATE * 4;
    g_wave_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &fmt, (DWORD_PTR)g_wave_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        g_wave = NULL;
        return;
    }
    for (k = 0; k < AUDIO_BUFS; k++) {
        memset(&g_hdr[k], 0, sizeof g_hdr[k]);
        g_hdr[k].lpData = (LPSTR)g_wbuf[k];
        g_hdr[k].dwBufferLength = AUDIO_FRAMES * 4;
        waveOutPrepareHeader(g_wave, &g_hdr[k], sizeof g_hdr[k]);
        g_hdr[k].dwFlags |= WHDR_DONE;
    }
}

static void audio_close(void)
{
    int k;
    if (g_wave == NULL) return;
    waveOutReset(g_wave);
    for (k = 0; k < AUDIO_BUFS; k++) waveOutUnprepareHeader(g_wave, &g_hdr[k], sizeof g_hdr[k]);
    waveOutClose(g_wave);
    g_wave = NULL;
}

static int audio_queued(void)
{
    int k, n = 0;
    for (k = 0; k < AUDIO_BUFS; k++)
        if (!(g_hdr[k].dwFlags & WHDR_DONE)) n++;
    return n;
}

static void audio_pump(void)
{
    size_t got;
    int k;
    while ((got = maya_audio(g_fifo + g_fifo_n * 2, (sizeof g_fifo / 4) - g_fifo_n)) > 0) g_fifo_n += got;
    if (g_wave == NULL) {
        g_fifo_n = 0;
        return;
    }
    while (g_fifo_n >= AUDIO_FRAMES) {
        for (k = 0; k < AUDIO_BUFS; k++)
            if (g_hdr[k].dwFlags & WHDR_DONE) break;
        if (k == AUDIO_BUFS) {
            /* every buffer queued: drop the oldest sound rather than fall behind */
            memmove(g_fifo, g_fifo + AUDIO_FRAMES * 2, (g_fifo_n - AUDIO_FRAMES) * 4);
            g_fifo_n -= AUDIO_FRAMES;
            continue;
        }
        memcpy(g_wbuf[k], g_fifo, AUDIO_FRAMES * 4);
        memmove(g_fifo, g_fifo + AUDIO_FRAMES * 2, (g_fifo_n - AUDIO_FRAMES) * 4);
        g_fifo_n -= AUDIO_FRAMES;
        g_hdr[k].dwFlags &= ~(DWORD)WHDR_DONE;
        waveOutWrite(g_wave, &g_hdr[k], sizeof g_hdr[k]);
    }
}

static void audio_pause(int on)
{
    if (g_wave == NULL || on == g_wave_paused) return;
    g_wave_paused = on;
    if (on) waveOutPause(g_wave);
    else waveOutRestart(g_wave);
}

/* ------------------------------------------------------------------------------------------------ game controller */

typedef DWORD(WINAPI *XGetStateFn)(DWORD, XINPUT_STATE *);
static XGetStateFn g_xget;
static int g_xpad = -1;
static DWORD g_xscan;

static void pad_init(void)
{
    HMODULE m = LoadLibraryExW(L"xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m == NULL) m = LoadLibraryExW(L"xinput9_1_0.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m != NULL) g_xget = (XGetStateFn)(void *)GetProcAddress(m, "XInputGetState");
}

static void pad_poll(void)
{
    XINPUT_STATE s;
    int bits = 0;
    if (g_xget == NULL) return;
    if (g_xpad < 0) {
        DWORD now = GetTickCount();
        int k;
        if (now - g_xscan < 2000u) return;
        g_xscan = now;
        for (k = 0; k < 4; k++) {
            if (g_xget((DWORD)k, &s) == ERROR_SUCCESS) {
                g_xpad = k;
                break;
            }
        }
        if (g_xpad < 0) return;
    }
    if (g_xget((DWORD)g_xpad, &s) != ERROR_SUCCESS) {
        g_xpad = -1;
        g_pjoy = 0;
        return;
    }
    {
        WORD b = s.Gamepad.wButtons;
        SHORT lx = s.Gamepad.sThumbLX, ly = s.Gamepad.sThumbLY;
        if ((b & XINPUT_GAMEPAD_DPAD_UP) || ly > 12000) bits |= 1;
        if ((b & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -12000) bits |= 2;
        if ((b & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -12000) bits |= 4;
        if ((b & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > 12000) bits |= 8;
        if (b & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B)) bits |= 16;
    }
    g_pjoy = bits;
}

/* ------------------------------------------------------------------------------------------------ picture */

/* the image height of the frame full screen on the window's monitor: the scanlines' period (CRT5b) */
static int32_t full_screen_rows(int fw, int fh)
{
    MONITORINFO mi = { sizeof mi };
    PresentLayout L;
    if (!GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return 0;
    present_layout(&L, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, fw, fh,
                   g_cfg.fit ? PRESENT_FIT : PRESENT_INTEGER, g_cfg.aspect43);
    return L.sh;
}

static void compose(void)
{
    RECT rc;
    int fw, fh;
    const uint32_t *frame = maya_picture(&fw, &fh);
    GetClientRect(g_hwnd, &rc);
    if (rc.right < 1 || rc.bottom < 1 || fw < 1 || fh < 1) return;
    if (rc.right != g_cw || rc.bottom != g_ch || g_client == NULL) {
        free(g_client);
        g_cw = rc.right;
        g_ch = rc.bottom;
        g_client = (uint32_t *)malloc((size_t)g_cw * (size_t)g_ch * 4);
        if (g_client == NULL) {
            g_cw = g_ch = 0;
            return;
        }
    }
    present_layout(&g_layout, g_cw, g_ch, fw, fh, g_cfg.fit ? PRESENT_FIT : PRESENT_INTEGER, g_cfg.aspect43);
    present_scale(&g_layout, frame, g_client);
    if (g_cfg.crt) {
        int32_t ph = full_screen_rows(fw, fh);
        if (!present_crt_matches(&g_crt, &g_layout, ph)) present_crt_set(&g_crt, &g_layout, fw, fh, ph);
        present_crt_apply(&g_crt, g_client);
    }
}

static void blit(HDC dc)
{
    BITMAPINFO bi;
    if (g_client == NULL) return;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = g_cw;
    bi.bmiHeader.biHeight = -g_ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, 0, 0, (DWORD)g_cw, (DWORD)g_ch, 0, 0, 0, (UINT)g_ch, g_client, &bi, DIB_RGB_COLORS);
}

static void present_now(void)
{
    HDC dc;
    compose();
    dc = GetDC(g_hwnd);
    blit(dc);
    ReleaseDC(g_hwnd, dc);
}

/* ------------------------------------------------------------------------------------------------ the game loop */

static LARGE_INTEGER g_qpf, g_next;

static void game_over(void)
{
    const char *e = maya_error();
    g_ended = 1;
    audio_pause(1);
    if (e != NULL && e[0]) {
        wchar_t w[1200], t[1400];
        to_wide(e, w, 1200);
        _snwprintf(t, 1400, L"The game stopped:\n\n%ls", w);
        message(t, MB_ICONERROR);
    }
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

static int run_one(void)
{
    maya_joystick(g_kjoy | g_pjoy);
    if (!maya_run_frame()) {
        game_over();
        return 0;
    }
    audio_pump();
    return 1;
}

/* runs the frames the sound (or the clock) needs; presents the last one */
static void tick(void)
{
    int ran = 0;
    if (g_pause > 0 || g_minimized || g_ended) return;
    pad_poll();
    if (g_wave != NULL) {
        audio_pause(0);
        while (!g_ended && audio_queued() < AUDIO_TARGET && ran < 4) {
            if (!run_one()) break;
            ran++;
        }
    } else {
        LARGE_INTEGER now;
        LONGLONG period = g_qpf.QuadPart * MAYA_FRAME_HZ_DEN / MAYA_FRAME_HZ_NUM;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - g_next.QuadPart > g_qpf.QuadPart / 5) g_next = now;
        while (!g_ended && now.QuadPart >= g_next.QuadPart && ran < 4) {
            if (!run_one()) break;
            g_next.QuadPart += period;
            ran++;
        }
    }
    if (ran && !g_ended) present_now();
}

/* ------------------------------------------------------------------------------------------------ input */

/* Amiga raw keys by PC set-1 scancode (index: scancode, +256 for the extended ones); 0xff: none */
static uint8_t g_amiga[512];

static void keymap_init(void)
{
    static const struct { int sc; int raw; } map[] = {
        { 0x29, 0x00 }, { 0x02, 0x01 }, { 0x03, 0x02 }, { 0x04, 0x03 }, { 0x05, 0x04 }, { 0x06, 0x05 },
        { 0x07, 0x06 }, { 0x08, 0x07 }, { 0x09, 0x08 }, { 0x0a, 0x09 }, { 0x0b, 0x0a }, { 0x0c, 0x0b },
        { 0x0d, 0x0c }, { 0x2b, 0x0d }, { 0x0e, 0x41 }, { 0x0f, 0x42 },
        { 0x10, 0x10 }, { 0x11, 0x11 }, { 0x12, 0x12 }, { 0x13, 0x13 }, { 0x14, 0x14 }, { 0x15, 0x15 },
        { 0x16, 0x16 }, { 0x17, 0x17 }, { 0x18, 0x18 }, { 0x19, 0x19 }, { 0x1a, 0x1a }, { 0x1b, 0x1b },
        { 0x1c, 0x44 }, { 0x1d, 0x63 }, { 0x3a, 0x62 },
        { 0x1e, 0x20 }, { 0x1f, 0x21 }, { 0x20, 0x22 }, { 0x21, 0x23 }, { 0x22, 0x24 }, { 0x23, 0x25 },
        { 0x24, 0x26 }, { 0x25, 0x27 }, { 0x26, 0x28 }, { 0x27, 0x29 }, { 0x28, 0x2a },
        { 0x2a, 0x60 }, { 0x56, 0x30 },
        { 0x2c, 0x31 }, { 0x2d, 0x32 }, { 0x2e, 0x33 }, { 0x2f, 0x34 }, { 0x30, 0x35 }, { 0x31, 0x36 },
        { 0x32, 0x37 }, { 0x33, 0x38 }, { 0x34, 0x39 }, { 0x35, 0x3a }, { 0x36, 0x61 },
        { 0x38, 0x64 }, { 0x39, 0x40 }, { 0x01, 0x45 },
        { 0x3b, 0x50 }, { 0x3c, 0x51 }, { 0x3d, 0x52 }, { 0x3e, 0x53 }, { 0x3f, 0x54 }, { 0x40, 0x55 },
        { 0x41, 0x56 }, { 0x42, 0x57 }, { 0x43, 0x58 }, { 0x44, 0x59 }, { 0x57, 0x5f },   /* F11: Help */
        /* the numeric keypad */
        { 0x47, 0x3d }, { 0x48, 0x3e }, { 0x49, 0x3f }, { 0x4a, 0x4a }, { 0x4b, 0x2d }, { 0x4c, 0x2e },
        { 0x4d, 0x2f }, { 0x4e, 0x5e }, { 0x4f, 0x1d }, { 0x50, 0x1e }, { 0x51, 0x1f }, { 0x52, 0x0f },
        { 0x53, 0x3c }, { 0x37, 0x5d }, { 0x100 + 0x35, 0x5c }, { 0x100 + 0x1c, 0x43 },
        /* the extended keys */
        { 0x100 + 0x48, 0x4c }, { 0x100 + 0x50, 0x4d }, { 0x100 + 0x4d, 0x4e }, { 0x100 + 0x4b, 0x4f },
        { 0x100 + 0x53, 0x46 }, { 0x100 + 0x1d, 0x63 }, { 0x100 + 0x38, 0x65 },
        { 0x100 + 0x5b, 0x66 }, { 0x100 + 0x5c, 0x67 }, { 0x100 + 0x47, 0x5f },   /* Home: Help */
    };
    size_t k;
    memset(g_amiga, 0xff, sizeof g_amiga);
    for (k = 0; k < sizeof map / sizeof map[0]; k++) g_amiga[map[k].sc] = (uint8_t)map[k].raw;
}

/* the joystick bits of a key (D15): arrows, the keypad's 1-9 and its fire keys */
static int key_joy(int idx)
{
    switch (idx) {
    case 0x100 + 0x48: case 0x48: return 1;
    case 0x100 + 0x50: case 0x50: return 2;
    case 0x100 + 0x4b: case 0x4b: return 4;
    case 0x100 + 0x4d: case 0x4d: return 8;
    case 0x47: return 1 | 4;
    case 0x49: return 1 | 8;
    case 0x4f: return 2 | 4;
    case 0x51: return 2 | 8;
    case 0x1d: case 0x100 + 0x1d: case 0x52: case 0x100 + 0x1c: return 16;
    default: return 0;
    }
}

/* letters, "?" and "/" by the character the key types in the current layout (D17), on the Amiga's US keyboard */
static int char_raw(unsigned vk, unsigned sc)
{
    static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
    BYTE ks[256];
    wchar_t buf[4];
    int n;
    const char *p;
    if (!GetKeyboardState(ks)) return -1;
    ks[VK_CONTROL] = ks[VK_LCONTROL] = ks[VK_RCONTROL] = 0;
    ks[VK_MENU] = ks[VK_LMENU] = ks[VK_RMENU] = 0;
    n = ToUnicodeEx(vk, sc, ks, buf, 4, 4u /* do not change the keyboard state */, GetKeyboardLayout(0));
    if (n != 1) return -1;
    if (buf[0] >= L'A' && buf[0] <= L'Z') buf[0] = (wchar_t)(buf[0] - L'A' + L'a');
    if (buf[0] == L'?' || buf[0] == L'/') return 0x3a;
    if (buf[0] < L'a' || buf[0] > L'z') return -1;
    if ((p = strchr(row1, (char)buf[0])) != NULL) return 0x10 + (int)(p - row1);
    if ((p = strchr(row2, (char)buf[0])) != NULL) return 0x20 + (int)(p - row2);
    if ((p = strchr(row3, (char)buf[0])) != NULL) return 0x31 + (int)(p - row3);
    return -1;
}

static void joy_update(void)
{
    int k, b = 0;
    for (k = 0; k < 512; k++) b |= g_jkey[k];
    g_kjoy = b;
}

static void key_event(WPARAM vk, LPARAM l, int down)
{
    int sc = (int)((l >> 16) & 0xff);
    int idx = sc + (((l >> 24) & 1) ? 256 : 0);
    if (vk == VK_PAUSE || sc == 0) return;
    if (down) {
        int raw;
        if (g_sent[idx]) return;               /* auto-repeat */
        raw = g_amiga[idx];
        if (idx < 256 && !(sc >= 0x47 && sc <= 0x53) && sc != 0x37) {
            int c = char_raw((unsigned)vk, (unsigned)sc);
            if (c >= 0) raw = c;
        }
        g_jkey[idx] = (uint8_t)key_joy(idx);
        joy_update();
        if (raw != 0xff) {
            g_sent[idx] = (uint8_t)(raw + 1);
            maya_key(raw, 1);
        }
    } else {
        if (g_sent[idx]) maya_key(g_sent[idx] - 1, 0);
        g_sent[idx] = 0;
        g_jkey[idx] = 0;
        joy_update();
    }
}

static void keys_release(void)
{
    memset(g_sent, 0, sizeof g_sent);
    memset(g_jkey, 0, sizeof g_jkey);
    g_kjoy = 0;
    g_mbuttons = 0;
    maya_keys_release();
    maya_joystick(0);
}

static void game_mouse(LPARAM l)
{
    int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
    int gx, gy;
    if (g_layout.sw < 1 || g_layout.sh < 1) return;
    gx = (int)(((int64_t)(x - g_layout.ox) * g_layout.fw) / g_layout.sw);
    gy = (int)(((int64_t)(y - g_layout.oy) * g_layout.fh) / g_layout.sh);
    maya_mouse(gx, gy, g_mbuttons);
}

/* ------------------------------------------------------------------------------------------------ window commands */

static void menu_update(void)
{
    HMENU m = GetSystemMenu(g_hwnd, FALSE);
    if (m == NULL) return;
    CheckMenuItem(m, IDM_FULL, MF_BYCOMMAND | (g_full ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(m, IDM_INTEGER, IDM_FIT, g_cfg.fit ? IDM_FIT : IDM_INTEGER, MF_BYCOMMAND);
    CheckMenuItem(m, IDM_ASPECT, MF_BYCOMMAND | (g_cfg.aspect43 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_CRT, MF_BYCOMMAND | (g_cfg.crt ? MF_CHECKED : MF_UNCHECKED));
}

static void menu_init(void)
{
    HMENU m = GetSystemMenu(g_hwnd, FALSE);
    int n, pos = -1, k;
    if (m == NULL) return;
    n = GetMenuItemCount(m);
    for (k = 0; k < n; k++)
        if (GetMenuItemID(m, k) == SC_CLOSE) pos = k;
    if (pos < 0) pos = n;
    InsertMenuW(m, pos++, MF_BYPOSITION | MF_STRING, IDM_FULL, L"&Full screen\tAlt+Enter");
    InsertMenuW(m, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(m, pos++, MF_BYPOSITION | MF_STRING, IDM_INTEGER, L"&Integer scaling");
    InsertMenuW(m, pos++, MF_BYPOSITION | MF_STRING, IDM_FIT, L"Fit to &window");
    InsertMenuW(m, pos++, MF_BYPOSITION | MF_STRING, IDM_ASPECT, L"&4:3 monitor aspect");
    InsertMenuW(m, pos++, MF_BYPOSITION | MF_STRING, IDM_CRT, L"&CRT scanlines");
    InsertMenuW(m, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(m, pos++, MF_BYPOSITION | MF_STRING, IDM_ABOUT, L"&About…");
    InsertMenuW(m, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    menu_update();
}

static void full_screen(int on)
{
    if (on == g_full) return;
    if (on) {
        MONITORINFO mi = { sizeof mi };
        g_wp.length = sizeof g_wp;
        GetWindowPlacement(g_hwnd, &g_wp);
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        g_full = 1;
        g_last_mouse = GetTickCount();
    } else {
        g_full = 0;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(g_hwnd, &g_wp);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        theme_window(g_hwnd, g_dark);
    }
    menu_update();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void about_box(void);

static void command(int id)
{
    switch (id) {
    case IDM_FULL: full_screen(!g_full); break;
    case IDM_INTEGER: g_cfg.fit = 0; break;
    case IDM_FIT: g_cfg.fit = 1; break;
    case IDM_ASPECT:
        g_cfg.aspect43 = !g_cfg.aspect43;
        g_aspect_file = -1;                    /* the menu's choice is saved */
        break;
    case IDM_CRT:
        g_cfg.crt = !g_cfg.crt;
        g_crt_file = -1;                       /* the menu's choice is saved */
        break;
    case IDM_ABOUT: about_box(); break;
    default: return;
    }
    menu_update();
    present_now();
}

/* ------------------------------------------------------------------------------------------------ About (F108) */

#define IDC_ABOUT_ICON 201
#define IDC_ABOUT_LINK 202
#define IDC_ABOUT_LICENSE 203

static const wchar_t g_license[] =
    L"MIT License\r\n\r\n"
    L"Copyright (c) 2026 Onur BUYUKCAGLAR\r\n\r\n"
    L"Permission is hereby granted, free of charge, to any person obtaining a copy of this "
    L"software and associated documentation files (the \"Software\"), to deal in the Software "
    L"without restriction, including without limitation the rights to use, copy, modify, merge, "
    L"publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons "
    L"to whom the Software is furnished to do so, subject to the following conditions:\r\n\r\n"
    L"The above copyright notice and this permission notice shall be included in all copies or "
    L"substantial portions of the Software.\r\n\r\n"
    L"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, "
    L"INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR "
    L"PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE "
    L"FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR "
    L"OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER "
    L"DEALINGS IN THE SOFTWARE.\r\n\r\n"
    L"The licence covers this port's own code (" HOST_CAPTIONW L"). Le Fétiche Maya, its "
    L"program and its data belong to their owners; they are not part of this software, which "
    L"needs a copy of the original Amiga game to run.\r\n\r\n"
    L"Acknowledgements\r\n\r\n"
    L"The game's fixes for modern machines (PAL display, the interrupt-driven sound timer and two data "
    L"fixes) follow the WHDLoad installer of the game by Mr.Larmer and CFou!; no code of it is included.\r\n\r\n"
    L"The CRT scanlines filter follows the math of the CRT filter of DOSBox Pure Unleashed by Bernhard "
    L"Schelling; it is a reimplementation, and no code of it is included.";

static int g_about_dark;
static HBRUSH g_about_bg;
static HBRUSH g_about_field;

static void about_theme(HWND dlg)
{
    HWND c = GetDlgItem(dlg, IDC_ABOUT_LICENSE);
    g_about_dark = theme_wanted();
    theme_window(dlg, g_about_dark);
    if (c != NULL) {
        if (g_ux_window != NULL) (void)g_ux_window(c, g_about_dark ? TRUE : FALSE);
        SetWindowTheme(c, g_about_dark ? L"DarkMode_Explorer" : NULL, NULL);
        SendMessageW(c, WM_THEMECHANGED, 0u, 0);
    }
    if ((c = GetDlgItem(dlg, IDOK)) != NULL) InvalidateRect(c, NULL, TRUE);
}

/* the OK button, owner-drawn: no Windows theme draws a dark push button */
static void about_button(const DRAWITEMSTRUCT *di)
{
    RECT r = di->rcItem;
    wchar_t text[32];
    int pressed = (di->itemState & ODS_SELECTED) != 0u;
    int focus = (di->itemState & ODS_FOCUS) != 0u;
    HFONT font = (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0u, 0);
    HGDIOBJ old = font != NULL ? SelectObject(di->hDC, font) : NULL;
    text[0] = 0;
    GetWindowTextW(di->hwndItem, text, 32);
    if (g_about_dark) {
        HBRUSH face = CreateSolidBrush(pressed ? RGB(45, 45, 45) : RGB(60, 60, 60));
        HBRUSH edge = CreateSolidBrush(focus ? THEME_DARK_LINK : RGB(100, 100, 100));
        if (g_about_bg != NULL) FillRect(di->hDC, &r, g_about_bg);
        if (face != NULL) {
            FillRect(di->hDC, &r, face);
            DeleteObject(face);
        }
        if (edge != NULL) {
            FrameRect(di->hDC, &r, edge);
            DeleteObject(edge);
        }
        SetTextColor(di->hDC, THEME_DARK_TEXT);
    } else {
        HTHEME th = OpenThemeData(di->hwndItem, L"BUTTON");
        if (th != NULL) {
            FillRect(di->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
            DrawThemeBackground(th, di->hDC, BP_PUSHBUTTON, pressed ? PBS_PRESSED : PBS_DEFAULTED, &r, NULL);
            CloseThemeData(th);
        } else {
            DrawFrameControl(di->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pressed ? DFCS_PUSHED : 0u));
        }
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
        if (focus && (di->itemState & ODS_NOFOCUSRECT) == 0u) {
            RECT f = r;
            InflateRect(&f, -3, -3);
            DrawFocusRect(di->hDC, &f);
        }
    }
    SetBkMode(di->hDC, TRANSPARENT);
    if (pressed) OffsetRect(&r, 1, 1);
    DrawTextW(di->hDC, text, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | ((di->itemState & ODS_NOACCEL) != 0u ? DT_HIDEPREFIX : 0u));
    if (old != NULL) SelectObject(di->hDC, old);
}

static INT_PTR CALLBACK about_proc(HWND dlg, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HICON icon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0);
        if (icon != NULL) SendDlgItemMessageW(dlg, IDC_ABOUT_ICON, STM_SETICON, (WPARAM)icon, 0);
        SetDlgItemTextW(dlg, IDC_ABOUT_LICENSE, g_license);
        g_about_bg = CreateSolidBrush(THEME_DARK_BG);
        g_about_field = CreateSolidBrush(THEME_DARK_FIELD);
        about_theme(dlg);
        SetFocus(GetDlgItem(dlg, IDOK));
        return FALSE;
    }
    case WM_DESTROY:
        if (g_about_bg != NULL) DeleteObject(g_about_bg);
        if (g_about_field != NULL) DeleteObject(g_about_field);
        g_about_bg = g_about_field = NULL;
        break;
    case WM_SETTINGCHANGE:
        if (theme_setting(l)) {
            about_theme(dlg);
            RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        break;
    case WM_DRAWITEM:
        if (w == IDOK) {
            about_button((const DRAWITEMSTRUCT *)l);
            return TRUE;
        }
        break;
    case WM_CTLCOLORDLG:
        if (g_about_dark && g_about_bg != NULL) return (INT_PTR)g_about_bg;
        break;
    case WM_CTLCOLORSTATIC: {
        int dark = g_about_dark && g_about_bg != NULL && g_about_field != NULL;
        if ((HWND)l == GetDlgItem(dlg, IDC_ABOUT_LINK)) {
            SetTextColor((HDC)w, dark ? THEME_DARK_LINK : RGB(0, 0, 204));
            SetBkMode((HDC)w, TRANSPARENT);
            return (INT_PTR)(dark ? g_about_bg : GetSysColorBrush(COLOR_BTNFACE));
        }
        if (dark) {
            int field = (HWND)l == GetDlgItem(dlg, IDC_ABOUT_LICENSE);
            SetTextColor((HDC)w, THEME_DARK_TEXT);
            SetBkColor((HDC)w, field ? THEME_DARK_FIELD : THEME_DARK_BG);
            return (INT_PTR)(field ? g_about_field : g_about_bg);
        }
        break;
    }
    case WM_SETCURSOR:
        if ((HWND)w == GetDlgItem(dlg, IDC_ABOUT_LINK)) {
            SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32649)));   /* IDC_HAND */
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(w) == IDC_ABOUT_LINK && HIWORD(w) == STN_CLICKED) {
            ShellExecuteW(dlg, L"open", HOST_ABOUT_URL, NULL, NULL, SW_SHOWNORMAL);
            return TRUE;
        }
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            EndDialog(dlg, 1);
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

/* the in-memory dialog template: WORD-aligned header, DWORD-aligned items */
typedef struct Tmpl {
    WORD buf[1024];
    size_t n;
} Tmpl;

static void t_word(Tmpl *t, WORD w)
{
    if (t->n < 1024u) t->buf[t->n++] = w;
}

static void t_str(Tmpl *t, const wchar_t *s)
{
    do {
        t_word(t, (WORD)*s);
    } while (*s++ != L'\0');
}

static void t_item(Tmpl *t, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t *text)
{
    if (t->n & 1u) t_word(t, 0);
    t_word(t, LOWORD(style));
    t_word(t, HIWORD(style));
    t_word(t, 0);
    t_word(t, 0);
    t_word(t, (WORD)x);
    t_word(t, (WORD)y);
    t_word(t, (WORD)cx);
    t_word(t, (WORD)cy);
    t_word(t, id);
    t_word(t, 0xFFFF);
    t_word(t, cls);
    t_str(t, text);
    t_word(t, 0);
}

static void about_box(void)
{
    static Tmpl t;
    DWORD style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    memset(&t, 0, sizeof t);
    t_word(&t, LOWORD(style));
    t_word(&t, HIWORD(style));
    t_word(&t, 0);
    t_word(&t, 0);
    t_word(&t, 7);
    t_word(&t, 0);
    t_word(&t, 0);
    t_word(&t, 260);
    t_word(&t, 196);
    t_word(&t, 0);
    t_word(&t, 0);
    t_str(&t, L"About " HOST_CAPTIONW L" v" HOST_VERSION_STRW);
    t_word(&t, 9);
    t_str(&t, L"Segoe UI");
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_ICON, 10, 10, 21, 20, IDC_ABOUT_ICON, 0x0082, L"");
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_LEFT, 42, 10, 208, 9, 0xFFFF, 0x0082, L"64bit version by " HOST_AUTHORW);
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_LEFT, 42, 21, 208, 9, 0xFFFF, 0x0082, L"with Anthropic Claude AI assistance");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | SS_LEFT | SS_NOTIFY, 42, 32, 208, 9, IDC_ABOUT_LINK, 0x0082,
           HOST_ABOUT_URL);
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 48, 240, 9, 0xFFFF, 0x0082, L"Licenses:");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 10, 59,
           240, 108, IDC_ABOUT_LICENSE, 0x0081, L"");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 200, 174, 50, 15, IDOK, 0x0080, L"OK");
    g_pause++;                                 /* the About box pauses the game */
    audio_pause(1);
    keys_release();
    DialogBoxIndirectParamW(g_inst, (LPCDLGTEMPLATEW)t.buf, g_hwnd, about_proc, 0);
    g_pause--;
}

/* ------------------------------------------------------------------------------------------------ window */

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = h;
        theme_apply(0);
        menu_init();
        return 0;
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0u) >= IDM_FULL && (w & 0xFFF0u) <= IDM_ABOUT) {
            command((int)(w & 0xFFF0u));
            return 0;
        }
        if ((w & 0xFFF0u) == SC_KEYMENU && l == 0) return 0;     /* a lone Alt or F10: the game's */
        break;
    case WM_SETTINGCHANGE:
        if (theme_setting(l)) theme_apply(1);
        break;
    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP:
        SetTimer(h, TIMER_MODAL, 5, NULL);
        break;
    case WM_EXITSIZEMOVE:
    case WM_EXITMENULOOP:
        KillTimer(h, TIMER_MODAL);
        break;
    case WM_TIMER:
        if (w == TIMER_MODAL) tick();
        if (w == TIMER_CURSOR && g_full && GetTickCount() - g_last_mouse > 2000u) {
            POINT p;
            if (GetCursorPos(&p) && WindowFromPoint(p) == h) SetCursor(NULL);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(g_full && GetTickCount() - g_last_mouse > 2000u ? NULL : LoadCursorW(NULL, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_SIZE:
        g_minimized = w == SIZE_MINIMIZED;
        if (g_minimized) audio_pause(1);
        else present_now();
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)l;
        if (!g_full) SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                                  SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        RECT a = { 0, 0, 320, 200 };
        AdjustWindowRectExForDpi(&a, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(h));
        mm->ptMinTrackSize.x = a.right - a.left;
        mm->ptMinTrackSize.y = a.bottom - a.top;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        compose();
        blit(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) keys_release();
        break;
    case WM_KILLFOCUS:
        keys_release();
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29))) {
            if (!(l & (1 << 30))) command(IDM_FULL);
            return 0;
        }
        if (msg == WM_SYSKEYDOWN && w == VK_F4 && (l & (1 << 29))) break;      /* Alt+F4: close */
        if (msg == WM_SYSKEYDOWN && w == VK_SPACE && (l & (1 << 29))) {
            keys_release();
            SendMessageW(h, WM_SYSCOMMAND, SC_KEYMENU, ' ');
            return 0;
        }
        key_event(w, l, 1);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        key_event(w, l, 0);
        return 0;
    case WM_SYSCHAR:
        return 0;
    case WM_MOUSEMOVE:
        g_last_mouse = GetTickCount();
        game_mouse(l);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        g_mbuttons = ((w & MK_LBUTTON) ? 1 : 0) | ((w & MK_RBUTTON) ? 2 : 0);
        if (g_mbuttons) SetCapture(h);
        else ReleaseCapture();
        game_mouse(l);
        return 0;
    case WM_CLOSE:
        cfg_take_window();
        cfg_save();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* the window's first client size: the largest whole multiple of 320 x 200 (or 320 x 240) within 85% of the work
 * area, at least 1x */
static void first_size(int *cw, int *ch)
{
    RECT wa;
    int k, fh = g_cfg.aspect43 ? 240 : 200;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    for (k = 6; k > 1; k--)
        if (320 * k <= (wa.right - wa.left) * 85 / 100 && fh * k <= (wa.bottom - wa.top) * 85 / 100) break;
    *cw = 320 * k;
    *ch = fh * k;
}

static int window_create(int show)
{
    WNDCLASSEXW wc;
    RECT r;
    int cw, ch, x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    DWORD style = WS_OVERLAPPEDWINDOW;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = g_inst;
    wc.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"Maya64Window";
    if (!RegisterClassExW(&wc)) return 0;
    if (g_cfg.w >= 320 && g_cfg.h >= 200) {
        cw = g_cfg.w;
        ch = g_cfg.h;
    } else {
        first_size(&cw, &ch);
    }
    r.left = 0;
    r.top = 0;
    r.right = cw;
    r.bottom = ch;
    AdjustWindowRectEx(&r, style, FALSE, 0);
    if (g_cfg.w >= 320) {
        RECT t;
        t.left = g_cfg.x;
        t.top = g_cfg.y;
        t.right = g_cfg.x + (r.right - r.left);
        t.bottom = g_cfg.y + (r.bottom - r.top);
        if (MonitorFromRect(&t, MONITOR_DEFAULTTONULL) != NULL) {
            x = g_cfg.x;
            y = g_cfg.y;
        }
    }
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, HOST_CAPTIONW, style, x, y, r.right - r.left, r.bottom - r.top,
                             NULL, NULL, g_inst, NULL);
    if (g_hwnd == NULL) return 0;
    if (x == CW_USEDEFAULT) {
        /* centre it on the primary monitor's work area */
        RECT wa, wr;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        GetWindowRect(g_hwnd, &wr);
        SetWindowPos(g_hwnd, NULL, wa.left + ((wa.right - wa.left) - (wr.right - wr.left)) / 2,
                     wa.top + ((wa.bottom - wa.top) - (wr.bottom - wr.top)) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(g_hwnd, g_cfg.maximized ? SW_SHOWMAXIMIZED : show);
    if (g_cfg.fullscreen) full_screen(1);
    UpdateWindow(g_hwnd);
    SetTimer(g_hwnd, TIMER_CURSOR, 500, NULL);
    return 1;
}

/* ------------------------------------------------------------------------------------------------ data folder */

static int pick_folder(wchar_t *out, size_t n)
{
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    DWORD opts;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog,
                                (void **)&dlg))) {
        return 0;
    }
    if (SUCCEEDED(IFileOpenDialog_GetOptions(dlg, &opts)))
        IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(dlg, L"Choose the folder of the Amiga game (it holds MAIN.CO)");
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, NULL)) && SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
        if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
            _snwprintf(out, n, L"%ls", path);
            CoTaskMemFree(path);
            ok = 1;
        }
        IShellItem_Release(item);
    }
    IFileOpenDialog_Release(dlg);
    return ok;
}

/* the game's folder: --data, the settings, next to the exe, or chosen by the player (remembered) */
static int data_resolve(const wchar_t *arg, wchar_t *out, size_t n)
{
    wchar_t t[MAX_PATH + 16];
    if (arg != NULL) {
        if (data_find(arg, out, n)) return 1;
        _snwprintf(t, MAX_PATH + 16, L"The folder given with --data does not hold the game:\n\n%ls", arg);
        MessageBoxW(NULL, t, HOST_CAPTIONW, MB_OK | MB_ICONWARNING);
    }
    if (g_cfg.data[0]) {
        to_wide(g_cfg.data, t, MAX_PATH + 16);
        if (data_find(t, out, n)) return 1;
    }
    if (data_find(g_exedir, out, n)) return 1;
    MessageBoxW(NULL,
                L"Le Fétiche Maya 64bit needs the files of the original Amiga game "
                L"(the folder with MAIN.CO and the other data files).\n\n"
                L"Choose the game's folder in the next window. It is remembered for the next start.",
                HOST_CAPTIONW, MB_OK | MB_ICONINFORMATION);
    for (;;) {
        if (!pick_folder(t, MAX_PATH + 16)) return 0;
        if (data_find(t, out, n)) return 1;
        if (MessageBoxW(NULL, L"That folder does not hold the game's files (MAIN.CO was not found). Choose another?",
                        HOST_CAPTIONW, MB_YESNO | MB_ICONWARNING) != IDYES) {
            return 0;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ entry */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    int argc = 0, k, crt = -1, aspect = -1;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *data_arg = NULL;
    wchar_t data[MAX_PATH + 16];
    char data8[1024], save8[1024], err[600];
    MSG m;
    (void)prev;
    (void)cmd;
    g_inst = inst;
    SetDllDirectoryW(L"");
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (k = 1; argv != NULL && k < argc; k++) {
        if (wcscmp(argv[k], L"--data") == 0 && k + 1 < argc) data_arg = argv[++k];
        else if (wcscmp(argv[k], L"--crt") == 0 && k + 1 < argc) crt = _wtoi(argv[++k]) != 0;
        else if (wcscmp(argv[k], L"--aspect") == 0 && k + 1 < argc) aspect = wcscmp(argv[++k], L"4:3") == 0;
    }
    paths_init();
    cfg_load();
    if (aspect >= 0) {
        g_aspect_file = g_cfg.aspect43;        /* --aspect: this run only */
        g_cfg.aspect43 = aspect;
    }
    if (crt >= 0) {
        g_crt_file = g_cfg.crt;                /* --crt: this run only */
        g_cfg.crt = crt;
    }
    theme_apply(0);
    if (!data_resolve(data_arg, data, MAX_PATH + 16)) return 1;
    to_utf8(data, data8, sizeof data8);
    snprintf(g_cfg.data, sizeof g_cfg.data, "%s", data8);
    to_utf8(g_savedir, save8, sizeof save8);
    if (maya_boot(data8, save8, err, sizeof err) != 0) {
        wchar_t w[700];
        to_wide(err, w, 700);
        MessageBoxW(NULL, w, HOST_CAPTIONW, MB_OK | MB_ICONERROR);
        return 1;
    }
    keymap_init();
    pad_init();
    audio_open();
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_next);
    timeBeginPeriod(1);
    if (!window_create(show)) return 1;
    for (;;) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) goto done;
            DispatchMessageW(&m);
        }
        tick();
        if (g_pause > 0 || g_minimized || g_ended) {
            WaitMessage();
        } else if (g_wave != NULL) {
            MsgWaitForMultipleObjects(1, &g_wave_event, FALSE, 20, QS_ALLINPUT);
        } else {
            LARGE_INTEGER now;
            LONGLONG ms;
            QueryPerformanceCounter(&now);
            ms = (g_next.QuadPart - now.QuadPart) * 1000 / g_qpf.QuadPart;
            if (ms > 0) MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)(ms > 20 ? 20 : ms), QS_ALLINPUT);
        }
    }
done:
    timeEndPeriod(1);
    audio_close();
    present_crt_free(&g_crt);
    free(g_client);
    if (argv) LocalFree(argv);
    CoUninitialize();
    return (int)m.wParam;
}
