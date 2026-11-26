# Release notes

## v1.0.0 (2026-11-26): first release, Windows x64

The first release of Le Fétiche Maya 64bit: the Amiga game, statically recompiled, as a native Windows program.

**You need the original Amiga game's files** (the folder with `MAIN.CO`). They are not included. On the first
start, `maya64.exe` asks for that folder and remembers it.

### What works

- The whole game from the Silmarils logo and the intro to play: scenes, walking, the F1 to F10 icons.
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

- Windows 10 or 11, 64-bit only. A macOS universal build is in progress.
- Only the Amiga version's data works. The DOS version's files are not supported.
- The recompiler was written for the `T.X` of the WHDLoad v1.4 package. Building from another version's `T.X`
  warns and may not work.
- The executable is not code-signed, so Windows SmartScreen may ask for confirmation on the first start.

### Files

- `maya64-1.0.0-win64.zip`: `maya64.exe`, `README.txt`, `LICENSE.txt`.
