# Host application design (Windows x64 and macOS universal)

User requirement (2026-11-26): the host application **follows the OS theme**, has an **About dialog**, and has a
**scanlines filter**, all "like the one in Civ64". The reference implementation is the user's own Civ64 port in
`C:\Projects\Civ1Re\claude\port` (MIT, Onur BUYUKCAGLAR). The sections below record what was analysed there and how
Maya adopts it.

## Architecture: native hosts, no SDL

Civ64 has one native host per OS (`platform/win32/host_win32.c`, `platform/macos/host_macos.m`). Both share
portable C presentation code (`present/`: layout, scaling, filters). Native windows, menus and dialogs are what make
the title bar, menus and dialogs follow the system theme. Maya follows the same split:

| Layer | Maya location | Contents |
|---|---|---|
| Recompiled game | `gen/` (generated at build time from the user's `T.X`) | 68000 code lifted to C |
| Amiga runtime | `runtime/` | memory map, CPU state, dispatcher, custom chips (Denise/Agnus/Paula), CIAs, OS HLE |
| Presentation | `present/` (portable C) | 320×200 (or overscan) frame to an XRGB client image: integer/fit scaling, 4:3 aspect, **CRT scanlines** |
| Windows host | `host/win32/` | window, window menu, theme, About box, input, audio output, settings file |
| macOS host | `host/macos/` | Cocoa equivalents; universal app (arm64 + x86_64); see [MACOS-PORT.md](MACOS-PORT.md) |

Toolchain on this machine: Visual Studio 18 (MSVC), LLVM clang 23, CMake, Ninja.

## Scanlines filter (from Civ64 `present/crt.c`, `crt.h`)

One fixed filter, not a menu of shaders. It is a reimplementation in plain C of DOSBox Pure Unleashed's CRT shader
math with the settings "Only Scanlines", "Normal gaps", "Sharpest", no curvature, no mask, no border. No shader code
is copied; the attribution stays in the source comments.

- Scanline thinness T = 0.65. A row at distance a (0..0.5 scanlines) from a scanline's centre has weight
  w = s(a·T) + s((1−a)·T) with s(x) = cos(min(0.5, x)·2π)/2 + 1/2, clamped to 1 and quantised to 256 levels.
- Tonal curve in linear light: midOut = 0.18/(1.5−T), ty, tz as in `crt.h`. For a pixel whose brightest channel
  is p, every channel is multiplied by w·f(p)/p with f(p) = p/(p·ty + tz). This uses three lookup tables:
  sRGB→linear (256 entries), linear→sRGB (65536 entries), and gain (256 levels × 256 values).
- **No resampling** (Civ64 CRT2). Each scaled pixel keeps its clean value and is multiplied by its output row's
  weight. Runs of equal pixels reuse the last result. The border outside the image is copied unchanged.
- **Double scan** (CRT4): 2 scanlines per source row (a 200-line Amiga frame gives 400 scanlines).
- **Grid rule** (CRT5): if P = output rows per scanline falls where the gaps disappear (not P ≥ 2.25, and not
  1.4 ≤ P < 1.75), the scanlines go on a fixed two-output-row grid instead: centre on even rows, gap on odd rows.
- **Same look in a window** (CRT5b): scanlines are spaced as the full-screen image on the window's monitor would
  space them, so a window shows the same period and darkness as full screen.
- Civ64's CRT6 (a separate reference frame per screen type) is not needed: Maya's frame size never changes.
- Applied after scaling, to the XRGB client image. Menu item "CRT scanlines" (a checkbox), saved in the settings
  file, with the command-line override `--crt 0|1`.

## OS theme (from Civ64 `host_win32.c`, "F109")

Windows:
- Dark when `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme` is 0, light
  otherwise or when the value is missing. A change takes effect immediately on `WM_SETTINGCHANGE` with
  `"ImmersiveColorSet"`.
- Title bar: `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)`, attribute 20 (19 before build 18985), only
  from build 17763. The build number comes from `RtlGetVersion`.
- Menus: uxtheme ordinals 135 SetPreferredAppMode (2 = ForceDark, 3 = ForceLight), 133 AllowDarkModeForWindow,
  104 RefreshImmersiveColorPolicyState, 136 FlushMenuThemes. Used only from build 18362 and only when all four are
  exported; otherwise the menus stay light.
- Dialogs in dark mode: background RGB(32,32,32), text fields RGB(43,43,43), text white, links RGB(96,205,255),
  scroll bars `SetWindowTheme(L"DarkMode_Explorer")`. Buttons are owner-drawn, because no Windows theme draws a dark
  push button. The game picture itself is never recoloured.

macOS (as built): native `NSMenu`, `NSAlert`, `NSPanel` and `NSTextView` follow the system appearance on their
own; the host does nothing for the theme.

## About box (from Civ64 "F108")

- Opened from the **window (system) menu**: "&About…" in its own group above "&Close\tAlt+F4". On macOS it is
  the application menu's "About …". The box is modal and **pauses the game** while open.
- Title: `About <product name> v<version>`. The version comes from the `.rc` VERSIONINFO on Windows and from
  Info.plist `CFBundleShortVersionString` on macOS.
- Contents: application icon (resource 1); "64bit version by Onur BUYUKCAGLAR"; "with Anthropic Claude AI
  assistance"; the project URL as a clickable link (`ShellExecuteW` / `NSWorkspace openURL`); "Licenses:" followed
  by a read-only scrolling text box. That box holds the MIT licence of the port's own code, a note that the
  original game and its data belong to their owners and are not included, and any third-party notices.
- Windows: a dialog built from an in-memory template, "Segoe UI" 9 pt, 260×196 dialog units, owner-drawn OK button,
  themed as above.
- macOS: an `NSPanel` (500×420 points) run modally from the application menu's "About Le Fétiche Maya 64bit"; the
  link is an `NSButton` that opens the URL; the licence text is in `maya_macos.m` (`g_license`, the same text as
  Windows) and the names in `host/macos/host_config.h`.

The About link is `https://github.com/Buyukcaglar/silmarils-maya` (decision D4, `HOST_ABOUT_URL` in
`host/win32/host_config.h`).

## Window menu (initial set, after the Civ64 pattern)

As built: Full screen (Alt+Enter) / separator / Integer scaling, Fit to window (radio), 4:3 monitor aspect,
CRT scanlines / separator / About… / separator / Close (Alt+F4). The window position and size, full-screen state and CRT setting are saved in
a settings file next to the executable, as `civ64.cfg` does.

On macOS the same commands are in the menu bar: the application menu (About, Hide, Hide Others, Show All, Quit) and
a View menu (Enter Full Screen Ctrl+Cmd+F / Integer Scaling, Fit to Window / 4:3 Monitor Aspect, CRT Scanlines),
plus the standard Window menu. Settings go to `~/Library/Application Support/Le Fetiche Maya 64bit/` (D24).
