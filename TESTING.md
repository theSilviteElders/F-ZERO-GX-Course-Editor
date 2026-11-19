# Testing the editor

A short guide to try the F-Zero GX Course Editor once from start to finish:
change an object, save it, check it in the game.

## Requirements

- The editor is built (`build/fzgx-editor`, see the [README](README.md)).
- The disc is extracted to `work/disc`.
- The F-ZERO GLINUX AppImage is installed. The editor finds it automatically
  in `~/.var/app/io.github.mabecgames.FZeroGLinux/`, `~/Applications` or
  `~/Downloads`. You can change the path under *File → Settings*.

## 1. Start the editor

In the repository folder (`F-ZERO-GX-Course-Editor`):

```bash
./build/fzgx-editor
```

Course 01 (Mute City) is loaded automatically.

## 2. Change a conspicuous object

1. Type `DASH01_BOARD` into the object filter at the top left. These are the
   dash plates on the track.
2. Double-click `1 DASH01_BOARD`: the camera flies there.
3. Set **Scale** in the inspector on the right from `1.000` to `2.000`.
   Alternatively move it sideways with the gizmo (key `W`).

Scaling is the best first test because a larger dash plate is easy to spot in
the game.

**Linked objects:** in the game a dash plate consists of several objects at
the same spot: the plate (`DASH01_BOARD`), the arrow marker
(`DASH01_SIGN_A_LOD`) and the ring (`DASH01_SIGN_CIRCLE_A_LOD`). With
*Move linked objects along* (on by default) they are moved, rotated and scaled
together. They are highlighted as well and listed on the right. Objects are
linked when they share the name prefix (e.g. `DASH01`) and lie within the
radius.

**Boost zone:** the actual boost effect is a separate collision polygon in
the course file (surface type 3). With *Move collision/boost areas along* (on
by default) it is transformed too. It is outlined in yellow in the 3D view, and
the inspector shows e.g. "2 collision polygons attached (2 boost)". If the red
warning "polygon extends outside the collision grid" appears, that part
probably has no effect. On Twist Road this affects dash plate #0, which lies at
the edge of the grid.

## 2b. Place a new structure

1. Switch the left panel from **Objects** to **Structures**.
2. Under *Boost & special zones* click `DASH01_BOARD + 3` (the dash plate set:
   plate, arrow marker, two rings and its boost polygons). A green preview
   follows the mouse in the 3D view.
3. Click on the road where the new dash plate should go. Shift+click places
   several, Esc cancels.
4. The copy is selected: move/rotate it with the gizmo like any other object.
   `Del` deletes it again, Ctrl+Z undoes the placing.

Alternatively select an existing object and press **Ctrl+D** (duplicate,
including linked objects and boost zone; the copy appears to the side).

New objects are appended to the course file (the object table and the
collision polygon arrays are copied to the end with the new entries). The
format check in `fzgx-cli selftest` passes for all courses, but whether the
game accepts it is not tested yet. That is the main question here.

## 2c. Move the track spline

1. Press **T** (or tick *Edit track spline* on the right). Orange points
   appear on the track line, the cyan lines are the track edges.
2. Click a point on a straight and drag the green (up) arrow of the gizmo
   about 100 units up. The track line forms a hill, the road model stays.
3. Ctrl+Z undoes it, *Reset whole track* restores the original spline.

In the game: does the vehicle drive over the invisible hill (follows the
spline), or along the road model? Does the course load, do lap counting and
the position display still work?

## 3. Save and start the game

- **F5** saves the course and starts GLINUX **directly in a race** on the open
  course (Practice, Free Run, no CPU opponents, Blue Falcon).
- **On the very first start** (and after a GLINUX update) the game navigates
  the menus automatically for about 30 seconds and creates a savestate.
  **Do not touch the controller meanwhile.** After that every start takes
  about 5 seconds.
- **Ctrl+S** only saves.
- The direct start can be switched off under *File → Settings*. The game then
  starts normally at the title screen.

## 4. What to check

| Check | Expectation / question |
|---|---|
| Does the course load? | No crash and no hang while loading |
| Visuals | Dash plate, marker and ring are larger or at the new spot |
| Boost | Does the boost work on the **whole** new area or at the new spot? |
| Rest of the course | Other objects and the track are unchanged |
| Placed structure | Does the new dash plate appear, and does its boost work? |
| Moved track point | Does the vehicle follow the new spline? Lap counting OK? |

## 5. If something goes wrong

- The game's launch output is in `work/launch.log`.
- *File → Revert course to original* restores the original course in the mod
  folder.
- The regular GLINUX installation is not touched, since all changes only go
  into `work/mod/GFZE01`.
- Tests run with a **separate copy** of the GLINUX user folder
  (`work/test-userdir`, copied from `~/.local/share/fzero-glinux` on the first
  test). Your real saves are never changed. To pick up new controller settings
  from GLINUX, delete `work/test-userdir`; it is copied again on the next test.
- If the direct start gets stuck (e.g. the game stays in a menu), delete
  `work/glinux-runtime/quickstart-practice.sav`; it is rebuilt on the next
  start.

## Editor controls

| Action | Input |
|---|---|
| Look around and fly | hold right mouse button + WASD, Q/E down/up |
| Flying speed | mouse wheel while holding the right mouse button, Shift faster |
| Select | left click in the 3D view or in the list |
| Focus selection | `F` or double-click in the list |
| Move / rotate / scale | `W` / `E` / `R` |
| Undo / redo | Ctrl+Z / Ctrl+Y |
| Place a structure | *Structures* tab, click an entry, click in the 3D view |
| Edit track spline | `T`, click a point, move it with the gizmo |
| Duplicate / delete placed object | Ctrl+D / Del |
| Save / test in game | Ctrl+S / F5 |
| Textures on/off | *View → Textures* |

## Checking without the GUI (optional)

```bash
./build/fzgx-cli selftest work      # all 37 courses: parsing, round trip, compression, collision, adding objects, track spline
./build/fzgx-cli dump work 1 | head # list the objects of course 01
```

Expected: 37 lines with `OK`, none with `FAIL`.

## Reporting results

Please note:

- which course and which object was changed
- what was changed (position, rotation or scale)
- what you saw in the game, especially where the boost worked
- if there were problems, the contents of `work/launch.log`
