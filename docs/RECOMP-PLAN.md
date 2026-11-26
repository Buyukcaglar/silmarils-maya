# Static recompilation of T.X (Amiga, 68000): plan and status

## Target binary

`data/T.X` is an AmigaDOS hunk executable. Its hunks are loaded at fixed addresses by `tools/m68k/image.py`
(`LOAD_BASE = $40000`, an 8-byte segment header before each hunk, as LoadSeg does):

| Hunk | Kind | Base | Size | Contents |
|---|---|---|---|---|
| 0 | CODE | $040008 | $784c | ALIS interpreter, startup, OS glue, level-3 interrupt handler |
| 1 | DATA | $047860 | $3d8 | variables, library bases ($47930 dos, $47934 graphics, $47938 intuition) |
| 2 | CODE | $047c40 | $26c | **data** (variables; no instructions) |
| 3 | CODE | $047eb8 | $1014 | **data** (buffers: $483f8, $485f8, $487c8, ...) |
| 4 | CODE | $048ed8 | $b88 | sound and music player (Paula + CIA-B timer, level-4 interrupt) |
| 5 | CODE | $049a68 | $1298 | ALIS graphics routines (blitter) |

## Status (2026-11-26): Windows and macOS builds complete

The game runs from boot to play on Windows x64: the Silmarils logo, the intro, the "HIT THE ? KEY / L KEY"
prompt, the game's scenes, walking with the joystick, the F-key icons, music and sound effects, saving (F10) and
loading (L at the prompt). Clang (clang-cl) and MSVC (cl) builds produce identical frames. The emulated machine runs
about 70 times faster than real time.

The macOS host is built too (D24 to D30 in DECISIONS.md, [MACOS-PORT.md](MACOS-PORT.md)): one universal app
(arm64 + x86_64) whose frames are byte-identical to `maya_cli`'s on both architectures. Released: v1.0.0 for Windows
and for macOS (signed and notarized) ([RELEASE-NOTES.md](RELEASE-NOTES.md)).

### Repository layout

| Path | Contents |
|---|---|
| `tools/m68k/` | `image.py` (hunk loader), `decoder.py`, `analyze.py` (recursive descent, `hints_maya.py`), `patches_maya.py` (the WHDLoad slave's patches, D8), `gen_c.py` (the C generator) |
| `runtime/` | `machine.c` (CPU state, dispatcher, exceptions, the `maya_*` host API, UTF-8 file wrappers), `custom.c` (Agnus/Denise: copper, bitplanes, beam, interrupts), `blitter.c`, `paula.c`, `cia.c`, `hle.c` (exec, dos, graphics, intuition, keyboard.device), `hooks.c` |
| `present/` | `layout.c` (integer/fit, square/4:3), `crt.c` (CRT scanlines, from Civ64) |
| `host/win32/` | `maya_win32.c` (window, menu, theme, About, input, waveOut), `maya64.rc`, manifest, `host_config.h` |
| `host/macos/` | `maya_macos.m` (Cocoa window, menus, About, input, GameController, AudioQueue), `Info.plist.in`, `host_config.h`, `maya_devtest.inc` (development checks, D30) |
| `res/` | the application icon: the artwork `maya64-icon.png`, and `maya64.ico` and `maya64-icon-1024.png` made from it by `tools/make_icon.py`; `macos/maya64-icon-macos.png` (`tools/make_mac_icon.m`, D28) and `macos/README.txt` (the Mac release's read-me) |
| `tests/maya_cli.c` | headless runner: scripted keys, joystick and clicks; BMP screenshots; WAV output |

### Build

```
build.cmd [the Amiga game's folder]      (default: _extracted\amiga\Maya\data)
```

This runs CMake and Ninja with clang-cl, or with cl when LLVM is missing. CMake runs `gen_c.py` on the folder's
`T.X` and builds `build\win\maya64.exe` and `build\win\maya_cli.exe`. The generator warns when T.X's SHA-1 differs from
`patches_maya.T_X_SHA1`; every patch checks its original bytes and refuses a mismatch.

On macOS, `./build.sh [folder]` does the same into `build/mac` (the universal app and `maya_cli`), and
`tools/package_mac.sh` makes the signed and notarized release ([MACOS-PORT.md](MACOS-PORT.md)).

### Testing and debugging

- `maya_cli <data> <frames> [shot.bmp] [sound.wav] [--key F RAW] [--joy F BITS N] [--click F X Y] [--shot F file]`.
  Example run to the first scene: `--key 1000 0x40 --key 1600 0x3a --joy 2000 8 100`.
- Build flags (`build/cli/b.sh "-DFLAG"`, a direct clang build): `MAYA_LOG` logs OS calls (files, memory);
  `MAYA_PROFILE` with `MAYA_PROF0=<frame>` lists the hottest regions; `MAYA_ALISTRACE` with `MAYA_ALIS=<frame>`
  traces ALIS opcodes (the interpreter loop at $4200a, script pointer in A3). `MAYA_MUSTRACE` logs the sound player's calls, and with `MAYA_MUS=<frame>` each music timer tick from that frame on (stream pointer, duration, command).
  `MAYA_TRACE=1` (environment)
  prints the CPU and interrupt state every 100 frames.
- The ALIS opcode and operator names are in the reference interpreter (`_research/alis-ref/src/opcodes.c`,
  `opernames.c`).

### Findings during bring-up

- The logo waits for a key (`oinkey`); the game's key table at $47ac8 maps Amiga raw codes to characters. It is a
  QWERTY letter layout with an AZERTY-style digit row.
- The hero and the menu hand move with the joystick in port 2 (`ojoy`: JOY1DAT and CIA-A PRA bit 7). The mouse is
  not used in play.
- The sound player in hunk 4 dispatches through three absolute tables ($49620, $49704, $497d2), now in
  `hints_maya.py`. Without them, the first sound effect after the logo jumped to uncompiled code.
- The game saves by rewriting `p.dat` in place (mode 1004). See D14 for the copy-on-write rule.

## Code generation design (as built)
- **Faithful stack and dispatcher model.** The generated code keeps the 68000 state (D0–D7, A0–A7, SR/CCR, PC) in
  a struct and the stack in emulated memory. `jsr`/`bsr` push a real return address and `rts` pops one. Direct
  branches inside a region are C `goto`s. Every other transfer (calls across regions, returns, indirect jumps,
  table dispatch) sets `pc` and returns to an outer loop, which maps the address to its region function
  (generated table, binary search). This handles the ALIS `pea`/`rts` loop, the sound player's `jmp (a5)`
  chain and stack tricks exactly. An unknown target stops with a diagnostic.
- **Regions** are subroutine-sized C functions, so MSVC and clang compile each quickly. Each opens with a
  `switch (pc)` over its entry, label and return-address points.
- **Time and interrupts.** Each instruction adds an estimated 68000 cycle count. At control transfers, the
  generated code checks a "next hardware event" cycle and calls the runtime. The runtime advances the beam
  position, CIA timers and Paula DMA, and delivers interrupts as the CPU does (push PC and SR, enter the vector
  at $64 + 4·level). Reads of the beam counters (VPOSR/VHPOSR) sync to the cycle count first. Instructions that touch a CIA or custom chip register
  check for a due event first, so an interrupt is taken before the access as on the 68000 (D32).
- **Memory.** 24-bit addresses: 2 MB chip RAM at 0, plus memory for the OS structures the game reads (ExecBase,
  library bases). Custom chip ($dff000) and CIA ($bfd000/$bfe001) accesses go to the hardware model.
- **OS calls (HLE).** Library bases point into a reserved area. A call such as `jsr -30(a6)` lands at an
  address in that area, and the dispatcher runs the C implementation, then returns as `rts` would. dos.library
  maps to the game's data folder. The WHDLoad v1.4 patches were decoded from `Maya.Slave` and are applied at
  generation time (D8).
- The generated C is produced at build time from the user's own T.X (CMake runs the Python generator). Nothing
  lifted from the game is committed.

## Host

See [HOST-DESIGN.md](HOST-DESIGN.md) and the decisions D3-D7, D10, D15-D20 in [DECISIONS.md](DECISIONS.md).
