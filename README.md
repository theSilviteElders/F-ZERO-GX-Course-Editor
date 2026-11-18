# F-Zero GX Course Editor

https://github.com/theSilviteElders/F-ZERO-GX-Course-Editor.git

A standalone course editor for F-Zero GX (USA, `GFZE01`) with a test button for
[F-ZERO GLINUX](https://github.com/Mabec-games/F-ZERO-GLINUX). First
milestone: view courses and move, rotate and scale placed objects (dash plates,
signs, buildings, road models …) together with their boost areas.

No game data is included: you need your own F-Zero GX image (USA, RVZ or ISO).
Put it in `rom/` (inside the repository, git-ignored), in a `rom/` folder next
to the repository, or pick it in the editor under *Settings*.

## Building

Requires CMake ≥ 3.20, a C++20 compiler, SDL3, libepoxy and glm (Arch/CachyOS:
`sdl3 libepoxy glm`). CMake downloads Dear ImGui 1.91.9 and ImGuizmo itself.

```bash
git clone https://github.com/theSilviteElders/F-ZERO-GX-Course-Editor.git
cd F-ZERO-GX-Course-Editor
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/fzgx-editor
```

A step-by-step test guide is in [TESTING.md](TESTING.md).

## Workspace (`work/`, git-ignored)

| Path | Contents |
|---|---|
| `work/disc/` | original disc, extracted once from the RVZ (never modified) |
| `work/mod/GFZE01/` | test copy (btrfs reflink, takes almost no space) with the edited files |
| `work/glinux-runtime/` | unpacked GLINUX AppImage + direct-start savestate (`quickstart-practice.sav`) |
| `work/test-userdir/` | separate copy of the GLINUX user folder for tests (protects your saves) |
| `work/editor.ini` | settings (ROM, AppImage, GLINUX user folder, launcher) |
| `work/launch.log` | output of the last game launch |

The disc is extracted with `dolphin-tool` (Dolphin Flatpak), either in the
editor under *File → Settings* or with
`scripts/extract-disc.sh <rom.rvz> work/disc`.

## Usage

- **Camera:** hold the right mouse button + WASD/QE to fly (Shift faster,
  Ctrl slower, mouse wheel changes the speed). Without the right mouse button:
  mouse wheel moves forward/back, middle mouse button pans.
- **Selection:** left click in the 3D view or in the object list
  (double click focuses), `F` focuses, `Esc` clears the selection.
- **Gizmo:** `W` move, `E` rotate, `R` scale; optional snapping.
  Values can also be typed into the inspector on the right.
- **Ctrl+Z / Ctrl+Y:** undo / redo.
- **Ctrl+S:** saves the compressed course into `work/mod/GFZE01`.
- **F5:** saves and starts the game from the mod folder directly in a race on
  the open course (see *Direct start*).
- **Linked objects** (e.g. dash plate + arrow marker + ring) are edited
  together: same name prefix, within the radius.

## Direct start

`scripts/run-test.sh` uses the GLINUX runner's automation directory
(`--automation-dir`, command files for controller input, memory and
savestates):

1. Once per GLINUX version: navigate the menus automatically (Practice →
   Ruby Cup → Twist Road → Blue Falcon → settings dialog) and save a savestate
   there, right before the course is loaded.
2. On every test: load that savestate, write the course number to
   **`0x803FFB71`** (u8, the number of the `COLI_COURSExx.lz` the game loads
   next; found by comparing RAM for the five Ruby Cup courses and cross-checked
   with course 14 → Big Blue) and press OK. The course is then loaded from the
   mod folder.

NTSC-U (`GFZE01`) only. There is no direct start with Dolphin as launcher.

## What is known (formats)

- `.lz`: 8-byte header (LE: packed size without header, unpacked size) +
  LZSS with a 4 KiB ring starting at `0xFEE`. The editor's compressor is
  verified lossless and produces files about the same size as the originals.
- `COLI_COURSExx`: header `0x48`/`0x54` = count/pointer of the placed objects
  (`0x40` bytes each: position, rotation as s16 with 65536 = 360°, scale,
  pointer to a 3×4 matrix). `M = Rz·Ry·Rx·S` holds for all ~18,000 objects.
  The editor writes both (TRS + matrix).
  Track checkpoints (`0x08`) provide the centre line and width, the static
  collision (`0x1C`) triangles/quads in 14 surface grids.
- Static collision: triangle `0x58` / quad `0x70` bytes:
  `d = −n·v0`, `n = normalize((v1−v0)×(v2−v0))`, vertices, then per edge
  `e_k = (n × (v[k+1]−v[k])) · |(v[k+1]−v[k]) × (v[k−1]−v[k])|`. Verified
  against every polygon of all 37 courses. Polygons are found through one XZ
  grid per surface type (u16 index lists terminated by `0xFFFF`).
  **Type 3 = dash plate (boost).** The boost zone is a separate collision
  polygon in world coordinates and is not attached to the dash plate object.
  The editor moves collision polygons lying entirely under an edited object
  along with it, recomputes plane and edges, and registers them in newly
  covered grid cells by appending new index lists to the file. Outside the
  grid a polygon probably has no effect (the editor warns).
- `.gma`: GCMF models with GX display lists. Geometry is rendered with the
  material colour, no textures yet. Course models are in `stage/stNN.gma`,
  shared objects in `bg/bg_*.gma`.

`./build/fzgx-cli selftest work` checks all 37 courses: parsing, unchanged
round trip (byte-exact), LZ round trip, transform formula, collision formulas
and model coverage. More commands: `dump <work> <course>`,
`move <work> <course> <index> dx dy dz`,
`dashscale <work> <course> <factor> <out.lz>` (test: scale all boost polygons
and write to a separate file), `unlz`, `lz`.

## Limitations / next steps

- Road models are visuals only: driving surface, physics and AI come from the
  track spline (yellow line), which cannot be edited yet.
- Objects can be changed but not added or deleted yet (that requires
  re-laying out the file, including all pointers).
- Some models (skinned/animated, 16-bit vertices) appear as a blue box,
  models that are not found as a red box.
- Whether every object really appears in the game where it was moved
  (e.g. visibility/collision grids) has to be tested in the game.
- Textures (`.tpl`), track spline editing, duplicating objects.

## License

MIT, see [LICENSE](LICENSE). F-Zero GX is © Nintendo / Sega / Amusement
Vision; this project contains no game data.
