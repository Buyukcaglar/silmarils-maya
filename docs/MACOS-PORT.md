# macOS port: what is left to do

The Windows build is complete (v1.0.0). The macOS universal build (arm64 + x86_64) has not been started, because
it could not be compiled or tested on the Windows machine (decision D20). This page is the hand-over for the Mac.

## Getting the sources and the game on the Mac

Work from the private repository, which holds the game's files as well:

```
git clone https://github.com/Buyukcaglar/silmarils-maya-dev.git
cd silmarils-maya-dev
```

- The Amiga game is in `_extracted/amiga/Maya/data` (and the original archives in `_references/`). `CMakeLists.txt`
  uses that folder by default (`MAYA_DATA`).
- The ALIS reference interpreter is not in the repository. Only when a format question comes up:
  `git clone https://github.com/maestun/alis _research/alis-ref` (the notes refer to commit `31b1811`).
- Repository layout and the rule about what goes where: decision D22 in [DECISIONS.md](DECISIONS.md). Branch `dev`
  here is the public `main` plus the game files; port code that should become public is cherry-picked or merged to
  `main` **without** the game-file commits.

## What already works on any platform

`runtime/`, `present/`, `tools/` and `tests/maya_cli.c` are portable C and Python. `CMakeLists.txt` already builds
`maya_runtime`, `maya_present` and `maya_cli` on non-Windows compilers (the `WIN32` block only adds the host).
`runtime/machine.c` has POSIX paths for its file wrappers (`rt_fopen` and friends).

First check on the Mac, before writing any host code:

```
cmake -G Ninja -B build/mac -DCMAKE_BUILD_TYPE=Release
cmake --build build/mac --target maya_cli
build/mac/maya_cli _extracted/amiga/Maya/data 2400 shot.bmp sound.wav \
    --key 1000 0x40 --key 1600 0x3a --joy 2000 8 100
```

`shot.bmp` should show the first scene (the village, with the jeep), and `sound.wav` the logo's music. This has
**not** been run on macOS or on arm64 yet, so expect small portability fixes (the Windows builds with clang-cl and
cl produce identical frames, so clang itself is known good).

## What to write: `host/macos/`

A Cocoa host that does what `host/win32/maya_win32.c` does, following the user's Civ64 port
(`platform/macos/host_macos.m` there). The design is in [HOST-DESIGN.md](HOST-DESIGN.md). The whole game API is
`runtime/maya.h`; the picture pipeline is `present/present.h`.

| Windows host | macOS equivalent |
|---|---|
| Window, frame from `maya_picture()` through `present_layout` / `present_scale` / `present_crt_apply` | `NSWindow` with a layer-backed `NSView` (or `CALayer` contents from the XRGB buffer) |
| Game clock: one `maya_run_frame()` whenever waveOut needs more sound (about 60 ms queued) | AudioQueue or `AVAudioEngine` pulling `maya_audio()` (48 kHz stereo int16); PAL frame clock `MAYA_FRAME_HZ_NUM / MAYA_FRAME_HZ_DEN` when there is no device |
| System menu: Full screen, Integer scaling / Fit to window, 4:3 monitor aspect, CRT scanlines, About, Close | `NSMenu`: application menu (About, Quit) and a View menu with the same items; Cmd+F or Ctrl+Cmd+F for full screen |
| Theme: dark title bar, menus, dialogs | Nothing to do: AppKit follows the system appearance |
| About box (icon, author line, link, licence text) | A small `NSPanel` or `NSAlert` with an `NSTextView`; the texts are in `maya_win32.c` (`g_license`) and `host_config.h`. Consider moving the shared strings to a common header |
| Keys by position, letters and `?` by character (D17); arrows and keypad as joystick, Ctrl as fire (D15) | `keyDown:` with `keyCode` → Amiga raw key table; `charactersIgnoringModifiers` for letters and `?` |
| XInput controller | `GCController` (GameController framework) |
| Mouse as the Amiga mouse in port 1 (D16) | `mouseMoved:` mapped through the layout to game pixels, `maya_mouse()` |
| `maya64.cfg` and `saves\` next to the exe, `%APPDATA%` fallback (D7, D18) | `~/Library/Application Support/Le Fetiche Maya 64bit/` for both (an app bundle is not writable) |
| First start asks for the game's folder (`maya_is_data_dir`) | `NSOpenPanel`, remembered in the settings |
| `maya64.rc`: icon, version | `Info.plist` (`CFBundleShortVersionString` 1.0.0) and `maya64.icns` made from `res/maya64-icon-1024.png` with `iconutil` |

Build: add an `elseif(APPLE)` block to `CMakeLists.txt` with `enable_language(OBJC)`, a `MACOSX_BUNDLE` target,
`CMAKE_OSX_ARCHITECTURES "arm64;x86_64"` and a deployment target (Civ64's value is a good default), linking Cocoa,
AudioToolbox, GameController and QuartzCore. Add a `build.sh` next to `build.cmd`.

## Done means

- `maya_cli` on the Mac gives the same frames as on Windows for the recipe above (compare the BMPs).
- The app starts from Finder, asks for the game's folder, plays from the logo into the game with sound, saves and
  loads, in a window and full screen, on both Apple silicon and Intel (`lipo -archs` shows both).
- Every decision made on the way is added to [DECISIONS.md](DECISIONS.md), and the status in
  [RECOMP-PLAN.md](RECOMP-PLAN.md) is updated.
