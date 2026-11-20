# Cups and custom cups

Notes from reverse engineering the Grand Prix cup selection of F-Zero GX
(NTSC-U `GFZE01`) and how the editor adds custom cups.

## Where things live

- **Main module (module 1).** It contains the cup tables, the race setup and
  most game code. It is linked at runtime from data that is stored compressed
  (not as a plain file on the disc), so it can only be changed in memory.
  `po = [0x800030C8]` points to it. With the original `main.dol` it was always
  at `0x801ba700` in GLINUX.
- **`fze.sel.rel` (module 10).** The menus, including the cup selection.
- **`fze.winning.rel` (module 15).** The results screen.
- **Cup icons.** Sprites `0xa102`–`0xa106` = `bmp_e/bmp_selgf.tpl` textures
  2–6 (92×116 RGB5A3). A sprite id is `bank << 8 | texture index`. The banks
  follow the order of the `bmp_e/bmp_*.tpl` list in module 1: `0x9a`
  `bmp_sel`, …, `0xa0` `bmp_selfa`, `0xa1` `bmp_selgf`. `0x9a00` is the blue
  “>” arrow, `0xa109` the pipe frame around a cup icon.

Addresses below are runtime addresses with the original `main.dol`
(module 1 at `0x801ba700`, module 10 at `0x805960a0`).

## Race setup (module 1 BSS, `0x803FFAE0` = po + `0x2453E0`)

| Offset | Type | Meaning |
|---|---|---|
| `0x00` | s16 | game mode (1 = Grand Prix) |
| `0x8c` | s16 | cup index |
| `0x8e` | s16 | course slot within the cup (0–5) |
| `0x90` | s16 | course id (`COLI_COURSExx`); the direct start writes byte `+0x91` |
| `0x94` | u32 | mode flags (`0x200`: cup 0 selectable, `0x40000000`: cups 6–8) |

## Cup → course tables

There are three identical tables in module 1, at `0x80322040`, `0x803220c4`
and `0x80322148`. Each holds 11 cups × 6 `s16` course ids (`-1` = empty).
Names come from the `fze.sel.rel` string table.

| Cup | Name | Courses |
|---|---|---|
| 0 | ALL Cup | – |
| 1 | Ruby Cup | 1 16 26 8 5 |
| 2 | Sapphire Cup | 14 7 11 13 3 |
| 3 | Emerald Cup | 15 10 29 9 27 |
| 4 | Diamond Cup | 24 25 17 21 28 |
| 5 | AX Cup | 31 32 33 34 35 36 |
| 6, 7 | Ac Cup | 36 31 32 33 34 35 / 82 54–58 |
| 8 | Ac | 81 85–89 |
| 9 | E3-0 | 1 8 7 5 |
| 10 | E3-1 | 16 |

Cups 6–10 are arcade (AX card) and E3 demo leftovers. Cups 9 and 10 are never
selectable in the retail game.

Accessors in module 1:

- `0x802e9700 GetCourse(cup, slot)`: returns `table1[cup*6+slot]`.
- `0x802e975c FindCup(course, &cup, &slot)`: searches table 2 with fixed
  bounds of 11 × 6 and returns the first cup that contains the course.
- `0x802e9804`: uses table 3; cup 5 (AX) is a special case.

Cup → icon sprite: two tables of 11 `u32`, one at `0x80322fd0` (module 1, used
by the intro and course screens) and one at `0x805bdff0` (`fze.sel.rel`).

Course id → track name: a pointer table in module 1, indexed by id − 1. The
editor keeps a copy in `CourseName()`.

## Cup selection (`fze.sel.rel`)

- `0x805a0958 CupSelectable(cup)`:
  - cups 1–3 are always selectable;
  - cup 4 needs `0x802b484c()` (Diamond unlocked);
  - cup 5 needs `0x802b4898(mode == 1)` (AX unlocked);
  - cup 0 needs the `0x200` flag.
- Cursor step (`0x805a0bcc`): the cursor *is* the cup index. It wraps over
  0–10 and skips rejected cups with an inline copy of the same checks
  (reject path `0x805a0ce0`).
- Icon row (`0x805ab158`):
  - The icon count is 3/4/5, depending on the Diamond and AX unlocks
    (`0x805ab3c4`–`0x805ab428`).
  - The per-icon list (`cup − 1`) is at `sp+0x2c`.
  - x positions come from a table at `sp+0x84`, one row of 5 halfwords per
    icon count.
  - Sprite ids are at `sp+0x40`, and sizes at `sp+0x20 + count*2`.
  - The cup name is `table[map[min(cup, 5)]]` at `0x805ab580`.
  - All these tables are copied from the module's data at `0x805bcc80+0x570`.
- Saved cup clears: byte `[0x803f8b60]+3+difficulty`, bit `1 << (cup−1)`.
  For cups 9/10 this is bit 8/9 of a byte, so `stb` drops it and nothing is
  stored.
- `fze.winning.rel` cup names: pointers at section 5 + `0x1c80`, laid out as
  `[language][6]`. Index 9/10 of language 0 falls into language 1's row:
  valid pointers, wrong name, no crash.

## Custom cups (editor)

The editor writes two files into the mod folder:

1. **`sys/main.dol`**, which gets a new text section with the code and data of
   `tools/cup_patch.s`:
   - The section sits at `0x801b7940`, right after the main stack. The
     apploader rejects sections above `0x80700000`. OSInit's `__ArenaLo`
     (`0x8000a4f8`/`0x8000a4fc`) moves behind the section, so heap objects,
     including module 1, move up by `0xfe0`. Gecko codes that use `po` are not
     affected.
   - `bl Link` in OSLink/OSLinkFixed (`0x8000e320`, `0x8000e35c`) calls a hook.
     After a module is linked, the hook runs a patch table for module ids 1,
     10 and 15. The entries copy data, place branches to and from the hooks,
     and store addresses (race setup, FindCup table) for the hooks. All
     targets are section-relative, so they follow the module.
   - Custom cups use cup indices 9 and 10, in every mode that shows the cup
     selection:
     - the rows are written into the three cup tables;
     - the selectability checks accept them;
     - FindCup prefers the active custom cup, so a course that is also in an
       original cup is shown with the custom cup;
     - the names replace the E3 strings;
     - the sprite tables point to the new icons;
     - the results screen gets upper-case names.
   - **Second page:** Page 1 shows the original cups followed by a “>” arrow.
     When the cursor reaches a custom cup, page 2 shows a “<” arrow followed by
     the custom cups. The draw hooks replace the icon count, list, positions,
     sprite ids and sizes.
2. **`files/bmp_e/bmp_selgf.tpl.lz`**, with appended textures: the mirrored
   arrow (index 49, `0xa131`), then one shield per custom cup (`0xa132`,
   `0xa133`). Each shield is the Ruby icon with the gem recoloured.

The config block (cup count, rows, sprites, names) is filled in by
`PatchDol()`. Rebuild `src/cup_patch_blob.hpp` after changing the assembly:
`python3 tools/build_cup_patch.py header` (needs `llvm-mc`).
`build_cup_patch.py dol IN OUT "Name:7,13,21:0xa132"` patches a DOL directly
for experiments.

**Pitfall:** never use a sprite id that does not exist in its bank. The game
then writes past its sprite tables and corrupts the heap; the crash only shows
up a screen later.

## Verified / not verified

Verified in GLINUX (headless automation and screenshots):

- both pages, the cursor, names and icons;
- the intro and course screens with the custom cup's course list;
- Grand Prix: the first race starts with the custom cup's first course;
- Time Attack (mode 2) and Practice (mode 3): second page, the course
  selection lists the custom cup's courses, the chosen course starts;
- the direct start still works with the moved main module.

Not verified: a full Grand Prix to the end (results and ranking screens with
cup 9/10), VS Battle (needs two controllers; the same menu code is used), and
other languages (PAL).

## Tools used for reverse engineering

Headless GLINUX (`moderngekko-run --headless --graphics Null --automation-dir
DIR`) supports these automation commands:

- `read_memory` (at most 16 MiB per command);
- `write_memory`;
- `load_state` and `save_state`;
- `pad_frames` (blocks until the frames are done).

Command values must not contain spaces.

Screenshots only work with a real window (`--graphics Vulkan` without
`--headless`). Headless Vulkan crashes in the ImGui overlay, and the software
renderer does not render headless.

Always point `--game` at a copy, never at `work/disc`: GLINUX rewrites files
in the game folder (`fze.customize.rel`, `.files.sha256-cache`).

Module code was disassembled with capstone from RAM dumps. The section bases
come from the module list (`[0x800030C8]`, the section table at module+`0x10`).
