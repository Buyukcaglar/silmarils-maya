# Release notes

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
