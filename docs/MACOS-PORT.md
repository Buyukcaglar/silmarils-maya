# macOS port: as built

The macOS host was written and released on 2026-11-26 (decisions D24 to D30 in [DECISIONS.md](DECISIONS.md)). It is
one universal application, `Le Fetiche Maya 64bit.app` (arm64 + x86_64, macOS 11 Big Sur or later), signed with a
Developer ID and notarized. The game code (`runtime/`, `present/`, the code generated from `T.X`) is the same as
in the Windows build; only the host is new.

## Building

You need the Xcode command line tools, CMake and Python 3 (Ninja is used when present), and the Amiga game's `T.X`.

```
./build.sh [the Amiga game's folder]       (default: _extracted/amiga/Maya/data)
```

This builds `build/mac/Le Fetiche Maya 64bit.app` (signed ad hoc) and `build/mac/maya_cli`. Extra arguments go to
CMake, for example `-DMAYA_CODESIGN_IDENTITY="Developer ID Application: <name> (<team>)"` for a Developer ID
signature with the hardened runtime, or `-DMAYA_DEVTEST=ON` for the development checks below.

The release (signed, notarized, stapled; a zip and a disk image) is made by:

```
tools/package_mac.sh [the Amiga game's folder]
```

It uses the identity "Developer ID Application: Onur Buyukcaglar (SZ4F4WP934)" and the notarytool keychain profile
`civ64-notary` unless `MAYA_SIGN_ID` and `MAYA_NOTARY` say otherwise; `MAYA_NOTARIZE=0` signs without notarizing.
The outputs go to `build/release/`. The script also puts `recomplauncher.json` next to the app in what it packs:
every release archive carries it for RecompLauncher (D33).

## What the host does

`host/macos/maya_macos.m` is the counterpart of `host/win32/maya_win32.c`:

| Windows host | macOS host |
|---|---|
| Window, frame through `present_layout` / `present_scale` / `present_crt_apply` | `NSWindow` with a layer-backed view. Without CRT the 320×200 frame is a `CALayer`'s contents, scaled by the GPU with nearest-neighbour sampling into the layout's rectangle; with CRT the scaled and filtered image (backing pixels) is (D25) |
| Game clock from waveOut (about 64 ms queued), PAL clock without a device | AudioQueue pulling from a ring kept 64 ms ahead; a 4 ms timer in the common run-loop modes; PAL clock without a device (D26) |
| System menu: Full screen, Integer scaling / Fit to window, 4:3, CRT, About | Menu bar: application menu (About, Hide, Quit) and View menu (Enter Full Screen Ctrl+Cmd+F, Integer Scaling / Fit to Window, 4:3 Monitor Aspect, CRT Scanlines) |
| Dark title bar, menus and dialogs by hand | AppKit follows the system appearance |
| About box: icon, author, link, licence text | An `NSPanel` with the same contents; pauses the game |
| Keys by position, letters and `?` by character; arrows and keypad as joystick, Ctrl fire | The same with macOS key codes; Option is Amiga Alt; Command is never sent to the game (D27) |
| XInput controller | GameController framework (D-pad or left stick, A or B fire) |
| Mouse as the Amiga mouse in port 1 | The same; the pointer hides after 2 s without movement in full screen |
| `maya64.cfg` and `saves\` next to the exe, `%APPDATA%` fallback | `~/Library/Application Support/Le Fetiche Maya 64bit/`; the window frame is kept by AppKit (D24) |
| First start asks for the game's folder | `NSOpenPanel`, remembered in the settings; `--data`, a folder next to the app also work |
| `maya64.rc`: icon, version | `host/macos/Info.plist.in` (version 1.0.0) and `maya64.icns` made from `res/macos/maya64-icon-macos.png` (D28) |

`Info.plist` carries usage descriptions for the Documents, Desktop, Downloads, removable and network volume
prompts, because macOS asks before the game may read its files there.

## How it was tested (2026-11-26: Apple M4, macOS 27.2.1)

- `maya_cli` (universal) with the recipe below gives the same frames and sound on arm64 and on x86_64 (Rosetta 2):
  `shot.bmp` SHA-1 `6dfbba4b…`, the logo at frame 300 `b11a62ed…`, `sound.wav` `7e1af132…`.

  ```
  build/mac/maya_cli _extracted/amiga/Maya/data 2400 shot.bmp sound.wav \
      --key 1000 0x40 --key 1600 0x3a --joy 2000 8 100
  ```

- The app itself, with the development checks (`-DMAYA_DEVTEST=ON`, `MAYA_DEVTEST=<folder>`, D30): Space, "/" and
  the right arrow go through the window as key events on the same frames, and the app's pictures at frames 300 and
  2400 are byte-identical to `maya_cli`'s, on arm64 and on x86_64. It runs at 49.91 frames a second (PAL 49.92) with
  the sound from AudioQueue; no sound buffer ran dry in normal play on Apple silicon. The View menu's commands
  (CRT, 4:3, fit, integer), full screen in and out and the About box were exercised and their pictures checked.
- Starting without `--data` uses the folder remembered in the settings.
- Saving (F10, "SAVE THE GAME") writes `saves/p.dat` and loading (L at the prompt) returns to the saved scene; the
  game's folder is not changed (checked with `maya_cli`, which uses the same runtime file code).
- The release (`tools/package_mac.sh`): `codesign --verify --strict --deep` (hardened runtime, secure timestamp,
  "Developer ID Application: Onur Buyukcaglar (SZ4F4WP934)"); the notary accepted the app (submission
  `6d74844a-d2cd-4826-ba98-5690e65840d7`) and the disk image (`9362db5d-0b00-4495-97d3-f7b81aea6270`) with no
  issues; both stapled; `spctl --assess` accepts both as "Notarized Developer ID". A copy of the zip given a
  browser's quarantine flag and unpacked: the app is accepted and its staple validates. The signed release binary
  runs the game with AudioQueue sound.
- SHA-256: `maya64-1.0.0-macos.dmg` `35646fdc0ba49e8b237cd5861ff16431aee6832d795ce640525279c0394f5f04`,
  `maya64-1.0.0-macos.zip` `d2c1356b92d05e8a35a589cbbf80a0082233575c3df34c76b8001554407b1703`. (The zip on the
  release was replaced later that day with the same files plus `recomplauncher.json`; its current SHA-256 is in
  RELEASE-NOTES.md.)

Not tested by the agent: a start from Finder on another Mac, an Intel Mac (the x86_64 slice ran under Rosetta 2
only), a game controller, and what needs a person at the screen: the first-start `NSOpenPanel`, the system's
folder-access prompt and the "downloaded from the Internet" confirmation of a quarantined copy.

## Development checks

```
cmake -S . -B build/mac-devtest -G Ninja -DCMAKE_BUILD_TYPE=Release -DMAYA_DEVTEST=ON
cmake --build build/mac-devtest
MAYA_DEVTEST=/tmp/dt "build/mac-devtest/Le Fetiche Maya 64bit.app/Contents/MacOS/Le Fetiche Maya 64bit" --data <game>
```

The script is in `host/macos/maya_devtest.inc`. It writes `logo.bmp`, `village.bmp` (compare with `maya_cli`),
window snapshots (`win-*.png`, `full*.png`, `about.png`) and `log.txt`, and keeps its settings and saves in the
given folder. Keep the game's folder outside Documents, Desktop, Downloads and external drives for these runs, or
the first file access waits for the system's permission prompt.

## Repositories

Work from the private repository, which also holds the game's files (`_extracted/amiga/Maya/data`, the default
`MAYA_DATA`). Branch `dev` there is the public `main` plus the game-file commits; port code that becomes public goes
to the public repository's `main` **without** those commits (D22).
