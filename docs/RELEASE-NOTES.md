# Release notes

## v1.0.0 zips replaced (2026-11-26): `recomplauncher.json` added for launchers

`maya64-1.0.0-win64.zip` and `maya64-1.0.0-macos.zip` on the v1.0.0 release were replaced in place. They hold the
same files as before, byte for byte, plus `recomplauncher.json` next to the program. The version number is
unchanged and nothing was rebuilt.

- `recomplauncher.json` is read by [RecompLauncher](https://recomplauncher.com): it names the game folder to pick
  (the one with `MAIN.CO`), how the program is started, the settings shown (picture shape, CRT scanlines) and where
  the saved games are. The program does not read it; without a launcher it can be ignored.
- `maya64-1.0.0-win64.zip`: 713,273 bytes, SHA-256
  `9d045a37772b2537e10ecec92f3a921891c6523a59bfc2769ee8163b469ac8e5` (this replaces the value in the section
  below).
- `maya64-1.0.0-macos.zip`: 3,068,407 bytes, SHA-256
  `9cfd19d244cda8e824fd37d2767cc7c871fac9cbf1ee4269ac84b4fa2d856e55`.
- `maya64-1.0.0-macos.dmg` is unchanged and carries no `recomplauncher.json`, so on a Mac the launcher uses the
  zip.
- The repository also gained the files for sp00nznet's recomp-launcher (`launcher/`). Its installer,
  `Maya64-Installer-windows-x64.exe`, is attached to releases from the next tagged version on; the README says how
  to run the same setup by hand until then.
- Neither launcher contains or downloads the game. The macOS files still have the crash described below.

## v1.0.0 Windows download replaced (2026-11-26)

`maya64-1.0.0-win64.zip` on the v1.0.0 release was replaced with a rebuilt `maya64.exe`. The version number is
unchanged.

- Fixed: a rare crash, "The game stopped: The game jumped to $DFF0A0, which is not recompiled code" (the address
  varies), mostly a moment after music stops, for example when leaving the title or a dialog. A timer interrupt
  could be handled just after the game had stopped the music timer, which restarted it (decision D32 in
  DECISIONS.md).
- SHA-256 of the new zip: `9c7a9baf8407b3e69ffd1736e2ee6c41078c6bf33e7d920e41780c1ee493ea20`.
- The macOS files are still the first build and have this bug until they are rebuilt from the current source.

## v1.0.0 for macOS (2026-11-26): universal, signed and notarized

The same game as the Windows v1.0.0, now as a native Mac application: one universal app for Apple silicon and Intel
Macs, macOS 11 Big Sur or later, signed with a Developer ID and notarized by Apple. The files are attached to the
same v1.0.0 release.

**You need the original Amiga game's files** (the folder with `MAIN.CO`). They are not included. On the first
start, the application asks for that folder and remembers it.

### The Mac application

- Drag it from the disk image to Applications; it opens without Gatekeeper warnings (notarized and stapled).
- Everything in the Windows list below: the game, music and sound, saving and loading, keyboard layouts.
- Follows the macOS light or dark appearance (menus, dialogs, About box).
- Full screen (Control+Command+F or the green window button), integer scaling or fit to window, sharp on Retina
  displays.
- Optional **4:3 monitor aspect** and **CRT scanlines**, in the **View** menu, remembered.
- Keyboard: arrow keys or numeric keypad as the joystick, **Control** as fire; F1 to F10 may need the fn key.
  Command stays the Mac's (Command+Q quits).
- Game controllers through Apple's GameController framework (D-pad or left stick, A or B to fire).
- Settings and saved games in `~/Library/Application Support/Le Fetiche Maya 64bit/`; the game's folder is never
  changed.

### Known limits

- Tested on an Apple M4 Mac with macOS 27, natively and under Rosetta 2 (the Intel code). Not yet tried on an
  Intel Mac or with a game controller; please report anything that does not work.
- As on Windows: not played through to the end yet, Amiga data only, written for the WHDLoad v1.4 package's `T.X`.

### Files

- `maya64-1.0.0-macos.dmg`: the disk image (signed, notarized, stapled): `Le Fetiche Maya 64bit.app`,
  `README.txt`, `LICENSE.txt` and a link to Applications.
- `maya64-1.0.0-macos.zip`: the same application (notarized and stapled), `README.txt`, `LICENSE.txt`.

## v1.0.0 (2026-11-26): first release, Windows x64

The first release of Le Fétiche Maya 64bit: the Amiga game, statically recompiled, as a native Windows program.

**You need the original Amiga game's files** (the folder with `MAIN.CO`). They are not included. On the first
start, `maya64.exe` asks for that folder and remembers it.

### What works

- The game from the Silmarils logo and the intro into play: scenes, walking, the F1 to F10 icons.
- Music and sound effects, from an emulated Paula (stereo, 48 kHz, with the Amiga 500's filters).
- Saving (F10) and loading (`L` at the start prompt). Saved games go to a `saves` folder next to the program; the
  game's own folder is never changed.
- Keyboard (arrow keys or numeric keypad as the joystick, Ctrl as fire) and XInput game controllers.
- Letters and `?` follow the keyboard layout, so AZERTY, QWERTZ and Turkish keyboards press the key that shows
  the character.

### The Windows application

- A single `maya64.exe`: no installer, no runtime libraries to install.
- Follows the Windows light or dark app theme (title bar, menus, About box).
- Full screen (Alt+Enter), integer scaling or fit to window.
- Optional **4:3 monitor aspect** and **CRT scanlines**; the two combine.
- Options are in the window's system menu (the title bar icon, or Alt+Space) and are remembered.

### Fixes carried over from the WHDLoad version

The recompiled game includes the fixes of the game's WHDLoad installer (v1.4): the PAL display, the sound player's
timer on an interrupt server, a display register write fix and a text fix in one script. The original disk's copy
protection check is skipped.

### Known limits

- Tested from the start into the first scenes, with saving and loading. It has not been played through to the
  end yet; please report anything that stops or looks wrong.
- Windows 10 or 11, 64-bit only. (The macOS build followed the same day; see above.)
- Only the Amiga version's data works. The DOS version's files are not supported.
- The recompiler was written for the `T.X` of the WHDLoad v1.4 package. Building from another version's `T.X`
  warns and may not work.
- The executable is not code-signed, so Windows SmartScreen may ask for confirmation on the first start.

### Files

- `maya64-1.0.0-win64.zip`: `maya64.exe`, `README.txt`, `LICENSE.txt`.
