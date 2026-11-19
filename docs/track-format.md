# Track spline format (work in progress)

Notes from reverse engineering `COLI_COURSExx` (decompressed, big-endian).
Verified by recomputing the checkpoint positions from the curves.

## Where it is

- Header `0x08`/`0x0C`: track nodes (count, pointer), `0x0C` bytes each:
  `u32 checkpoint count, ptr checkpoints, ptr root track segment`.
- Header `0x10`/`0x14`: track-relative surface areas, `0x14` bytes each:
  `f32 from, f32 to` (track distance), `f32 left, f32 right` (lateral, -0.5..0.5),
  `u8 type` (3 on the dash plate areas of Twist Road; meaning not verified).
- Header `0x90`: pointer to `f32` track length (Twist Road: 7958.8).
- Header `0xBC`: checkpoint grid pointer, `0xC0..0xCC` its bounds (4 floats),
  `0xD0`/`0xD4` = 8 × 8 cells (not decoded yet).

## Track segment (`0x50` bytes)

| Offset | Type | Meaning |
|---|---|---|
| `0x00` | u8 | flags: `0x08` root/transform parent, `0x04` branch, `0x02` track surface, `0x00100000` (as u32) unknown |
| `0x04` | ptr | curves (see below) |
| `0x08` | ptr | corner data (0 on most segments) |
| `0x0C` | u32 | child count |
| `0x10` | ptr | children (array of segments, `0x50` stride) |
| `0x14` | 3×f32 | local scale (x of the surface segment = track width) |
| `0x20` | 3×f32 | local rotation, degrees |
| `0x2C` | 3×f32 | local position |
| `0x38` | u32 | unknown |
| `0x3C`/`0x40` | f32 | rail height left/right |
| `0x4C` | u32 | branch index |

Curves: 9 `u32` key counts followed by 9 pointers, channel order
scale x/y/z, rotation x/y/z, position x/y/z. A channel with keys replaces the
static local value. Key (`0x14` bytes): `u32 mode, f32 time, f32 value,
f32 tangent in, f32 tangent out`. Mode 1 = linear, mode 2 = cubic Hermite
(tangents are slopes per time unit).

Transform of a segment: `T · Rz · Ry · Rx · S` (same as placed objects).
World transform = product from the root down to the surface segment.

## Checkpoints (`0x50` bytes)

`f32 t_start, t_end`, `f32 d_start`, `3×f32 normal start`,
`3×f32 position start`, `f32 d_end`, `3×f32 normal end`, `3×f32 position end`,
`f32 distance start, distance end`, `f32 width`, `u32 flags`.
The positions equal the evaluated segment transform at `t_start`/`t_end`.

## Checkpoint grid

`u32` pointers to `nx × nz` index lists at header `0xBC` (u16 track node
indices, ascending, `0xFFFF`-terminated), cell `z * nx + x`. Header `0xC0`:
`min x, min z, step x, step z`, `0xD0`/`0xD4`: `nx`, `nz` (always 8 × 8).
The bounds are exactly the x/z extent of all checkpoint start/end points.
A node is listed in a cell when one of its checkpoints (straight line start →
end) comes within 500 units of the cell (maximum of the x and z distance).
This rule reproduces every original entry of all 37 courses, plus a few
extra entries (a superset is harmless for a spatial lookup).

## Distances

`distance start/end` are cumulative checkpoint lengths
(`|end - start|` of the node's first checkpoint), starting at 0; the track
length (header `0x90`) is the last `distance end`.

## Verification status

- Anchoring: every checkpoint is matched to the segment of its node's tree
  whose transform is closest at `t_start`/`t_end`. Exact (< 0.1) in 32 of 37
  courses, residuals up to 48 units in 16, 24, 27, 43, 44 (branches).
- Editing: when curves change, each checkpoint is moved by the rigid change
  (position + axes, scale removed) of its anchor segment's transform, so
  conventions that are not understood yet stay intact. Distances, track
  length, surface areas (remapped by distance) and the grid are recomputed.
- Branches: a node with several checkpoints has one per path (the centre and
  one per branch); a node with one checkpoint at a branch point sits on the
  parent segment.
- Key mode 3 occurs in a few courses and is evaluated like mode 2 (unverified).
- `fzgx-cli selftest` moves one spline point per course, saves, re-parses and
  compares curves, checkpoints, distances and length.
- Not tested in the game yet.
