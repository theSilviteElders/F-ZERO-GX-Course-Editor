# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Context

Standalone course editor for F-Zero GX (USA, `GFZE01` only) written in C++20 (SDL3, OpenGL via libepoxy, glm, Dear ImGui + ImGuizmo fetched by CMake). It edits `COLI_COURSExx.lz` files from an extracted disc and launches the result in F-ZERO GLINUX (a Dolphin-derived runner). The sibling folder `../F-ZERO-GLINUX` is a separate upstream project (own git repo), not part of this codebase.

No game data is in the repo. Everything local lives in git-ignored `work/` (see README "Workspace"): `work/disc/` is the pristine extracted disc and must never be written to; all edits go to `work/mod/GFZE01/` (reflink copy).

## Commands

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/fzgx-editor                      # GUI
cmake -S . -B build -DFZGX_BUILD_GUI=OFF # format library + CLI only (no SDL/ImGui needed)
```

There is no unit-test framework. The test is the CLI self-test against the real disc (requires `work/disc`):

```bash
./build/fzgx-cli selftest work           # all 37 courses + cup patch; expect 38 "OK", no "FAIL"
./build/fzgx-cli cups work               # apply work/cups.ini to the mod folder
./build/fzgx-cli dump work 1 | head      # objects of course 01
./build/fzgx-cli move work <course> <index> dx dy dz
./build/fzgx-cli unlz <in.lz> <out.bin> / lz <in.bin> <out.lz>
./build/fzgx-cli tplsheet <file.tpl.lz> <out.ppm>
```

Run `selftest` after any change to parsing/serialization in `src/coli.cpp`, `src/track.cpp` or `src/lz.cpp`. In-game behaviour can only be verified by the maintainer (F5 in the editor → `scripts/run-test.sh`); TESTING.md is the manual test guide. When something is unverified in-game, say so (README/TESTING.md do this consistently).

## Architecture

Two layers, split in `CMakeLists.txt`:

- **`fzgx` static library** (no GUI deps): file formats + project/workspace handling.
  - `binary.hpp` — big-endian read/write helpers; bounds violations throw `FormatError`.
  - `lz.*` — LZSS (8-byte LE header, 4 KiB ring at `0xFEE`); compressor must stay lossless.
  - `coli.*` + `track.cpp` — `Course`: parses the decompressed `COLI_COURSExx` (placed instances, scene objects, static collision polygons + per-surface XZ grids, track spline segments/checkpoints). `track.cpp` holds spline evaluation, handle moving and `UpdateTrack()` which re-derives checkpoints, distances, track length and checkpoint grid.
  - `gma.*` / `tpl.*` — GCMF models (GX display lists) and GX texture decoding.
  - `project.*` — `Settings` (`work/editor.ini`), disc/mod paths, load/save/restore course, `ApplyCups`, launching scripts in `scripts/` (located via compile-time `FZGX_SOURCE_DIR`, so the binary must run against the source tree; a build dir configured under an old checkout path points the scripts there too — reconfigure).
  - `cups.*` — custom Grand Prix cups (`work/cups.ini`): patches `main.dol` with the blob from `src/cup_patch_blob.hpp` and appends icon textures to `bmp_e/bmp_selgf.tpl`.
- **`tools/`** (Python, dev-only, needs `llvm-mc`): `cup_patch.s` is the PowerPC code/data injected into `main.dol` (OSLink hook + patch table applied to modules 1/10/15 after linking); `build_cup_patch.py header` regenerates `src/cup_patch_blob.hpp` — rerun after editing the assembly. Module offsets there were measured at runtime; see `docs/cup-format.md`.
- **`fzgx-editor`** (`main.cpp`, `renderer.*`, `editor.*`): SDL/ImGui app. `Editor` (in `editor.cpp`, the bulk of the code) owns selection, gizmo, linked objects, structures, magnetism, track-spline editing and the undo stack.

Key invariants:

- **Byte-exact patching.** `Course::Parse` interprets only what the editor needs and keeps the raw buffer; `Serialize()` writes edited fields back into it. Unmodified courses must round-trip byte-identical (checked by `selftest`). Don't rebuild files from scratch.
- **Transforms are stored twice** (TRS fields and a 3×4 matrix, `M = Rz·Ry·Rx·S`, rotation s16 with 65536 = 360°); both must be written. Matrices may be shared between instances (`shared_matrix`).
- **Additions are appended, not inserted.** New instances/collision polygons are copies of existing ones; on save the instance table (header `0x48`/`0x54`) and the triangle/quad arrays are relocated to the end of the file with new entries appended, and new grid index lists are appended too. Only session-added objects can be deleted (`removed` flag kept for undo); original objects cannot be removed.
- **Collision polygons** must have plane (`d`, `n`) and edge vectors recomputed whenever vertices move (formulas in `coli.hpp` / README). Surface type 3 = dash plate boost; boost zones are separate world-space polygons carried along with objects lying above them.
- **Undo:** edits go through `BeginEdit` / `ApplyPrimary` / `EndEdit`, producing one `UndoEntry` (instances, polys, alive-state, curves) per user action.
- **Cup patch:** the main game module only exists in RAM (linked from compressed data), so it is patched at link time by code in a new DOL section; addresses must be module-relative (the patch itself moves module 1 by `0xfe0`, so never hard-code `0x80xxxxxx` addresses of module 1/10 in scripts — derive them from `po = [0x800030C8]`). Hook code must not clobber live registers/`cr0` (use `cr7`, `r8`–`r12`). Sprite ids must exist in their texture bank, otherwise the game corrupts its heap.

Format reverse-engineering notes: README "What is known (formats)", `docs/track-format.md` and `docs/cup-format.md` (also describes the GLINUX headless automation used to inspect RAM and take screenshots) — update these when a format detail is discovered or verified. When running GLINUX for experiments, point `--game` at a copy, never at `work/disc`.

## Conventions

- UI text, docs and code comments are in English.
- Commits written with AI carry a `Co-Authored-By: Claude` line (stated in README "AI disclosure").
- `.todos.md` (git-ignored, German) holds the maintainer's feature backlog.
