# Le Fétiche Maya: graphics fidelity check (Amiga vs DOS)

**Verdict: the Amiga port wins.** It is a strict superset of the DOS graphics: every DOS asset exists
on the Amiga with identical pixels, and the Amiga adds 32-colour art, finer palettes and screens the DOS
release does not have. The Amiga build (`T.X`, 68000) is the one to statically recompile.

## Sources compared

| | Amiga | DOS |
|---|---|---|
| Archive | `Maya_v1.4_1742.lha` (WHDLoad install v1.4, CFou!) | `Le-Fetiche-Maya_DOS_EN.zip` |
| Interpreter | `data/T.X`: AmigaDOS hunk exe, 68000, 6 hunks | `START.EXE`: MZ, 16-bit x86 |
| Scripts/resources | 55 × `.CO` (big-endian) | 50 × `.IO` (little-endian) |
| Video | OCS lores 320×200, up to 32 colours from 4096 | CGA / Hercules / Tandy / Amstrad / EGA / VGA, 16 colours |

Both builds run the same Silmarils **ALIS** v1.3 bytecode VM. Graphics are embedded in the scripts' resource
tables. All assets were decoded with the project tool `tools/alis_res.py`, not taken from screenshots.

## Method

1. Unpack every script (old Silmarils RLE, plus the column-interleaved variant, typepack `0xC0`).
2. Walk each resource table (`u32 @ data+0x0E`) and pair resources by script and index across the ports.
3. Decode each bitmap to colour indices and compare them pixel by pixel. Decode each palette (`0xFE`) to RGB.
4. Render side-by-side PNGs, Amiga on the **left** and DOS on the **right**, at 2× (in this folder).

Bitmap formats found:

| Type | Where | Layout |
|---|---|---|
| `0`/`2` | DOS, and Amiga scripts with typepack `0x81` | 4bpp chunky, high nibble first |
| `0`/`2` | Amiga scripts with typepack `0x80`/`0xC0` | 4 whole bitplanes |
| `0x10`/`0x12` | **Amiga only** | **5 whole bitplanes = 32 colours** |
| `0xFE` palette | Amiga: 12-bit `0RGB`; DOS: 9-bit ST-style `0RGB` (3 bits/gun) |

## Results

**Shared bitmaps: 790 pairs, 783 index-identical.** The two ports share the same artwork at the same sizes.
The 7 that differ:

| Image | Size | Difference |
|---|---|---|
| PRESENTE #1 (title screen) | 320×200 | Amiga 32 colours; DOS re-dithered to 16 and **loses the "CHARTER +" credit** |
| RANGE #1 (jeep dashboard) | 80×200 | Amiga 32 colours with a green panel; DOS 16 colours, olive |
| DECOR #14 (mountain horizon) | 208×56 | Amiga smooth 5-plane sky; DOS checkerboard dither |
| COPAN #12 | 80×66 | Amiga 32 colours (DOS render may use a different runtime palette, not verified) |
| DEBUVISI #39 (sky band) | 128×51 | Amiga soft 5-plane clouds; DOS harsh 2-tone dither |
| DJEEP #0 / #2 | 128×62 / 80×30 | 5-pixel and 1-pixel touch-ups only |

**Palettes.** In the 16-colour palettes, each DOS value is exactly the Amiga value halved (`a64` → `532`), so
the two ports show the same colours there. The Amiga also has **8 × 32-colour palettes** (CODETROC, COMMERCE,
COPAN, PRESENTE, PYRAMIDE, DECOR #17, PERDU, RANGE) with true 4-bit values. These also re-tune the *shared*
first 16 colours, so even the shared sprites look better in those scenes. Example: COMMERCE's floor is
warm red-brown on the Amiga and flat ochre on DOS.

**Content found only on the Amiga:**
- `PERDU`: a 32-colour 320×200 game-over screen (skeleton tomb). DOS has no equivalent.
- `GAGNE`: the full 320×200 win screen with the "JC-CHARTER 89" signature. DOS has only `IGAGNE`, a
  320×180 crop without the signature.
- `ABOMAYA`, `SJUNGLE` (jungle ambience samples).

**Content found only on DOS:** `BLANCPC.IO` (x86 PC-speaker driver code) and `LAMPE.IO`. `LAMPE.IO` is the
DOS copy protection: a manual word lookup ("page, line, word") whose encoded answers decode to words such as
DEL, HALIFAX and MAYA. The Amiga used disk-based protection instead, which the WHDLoad patch removes. Neither
file adds graphics. Both versions' `MAIN` scripts reference the same set of script files, including files the
other version doesn't ship, so the two disk sets were trimmed per platform from one shared game script.
Both versions are English with identical game text.

## Comparison images (Amiga left, DOS right)

`presente_1.png`, `range_1.png`, `decor_14.png`, `debuvisi_39.png`, `commerce_0.png`, `copan_12.png`
show the differences. `chambre_0.png`, `logo_0.png`, `pyramide_5.png`, `village_3.png`, `igagne_1.png` show
identical assets for control. `amiga_only_perdu.png` and `amiga_only_gagne.png` show the Amiga-only screens.

## Audio scope

- **In scope: Amiga only.** Paula sample playback: `blanc.snd` (2,266-byte raw 8-bit sample) plus the sample
  scripts `SDIVERS`, `SBIRD` and `SJUNGLE` (`SJUNGLE` is Amiga-only). Music is driven from the interpreter.
- **Dropped (decision of 2026-11-25):** DOS PC-speaker audio (the `BLANCPC.IO` driver and its data).

A full sound and music catalogue is part of the recompilation phase.

## Role of the DOS version

**Reference only.** The recompiled game loads nothing from the DOS files. They remain in `_references` as a
second build of the same game script, for cross-checking behaviour while debugging.

## References

- ALIS reimplementation, MIT, used only as a format reference: https://github.com/maestun/alis
  (cloned read-only to `_research/alis-ref`).
