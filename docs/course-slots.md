# Course slots and new courses

Notes on how F-Zero GX (NTSC-U `GFZE01`) maps a course id (`COLI_COURSExx`)
to its name and venue, and how the editor adds new courses.

Addresses are runtime addresses with the original `main.dol` (module 1 at
`0x801ba700`, section 4 at `0x80319e10`, section 5 at `0x80327d00`). With the
cup patch the module moves (a few KiB), so the patch works with section
offsets.

## Per-course tables in module 1 (indexed by course id)

| Address | Section + offset | Type | Meaning |
|---|---|---|---|
| `0x80352fb8` | 5 + `0x2b2b8` | `char*` | track name ("Twist Road"; unused ids → "---") |
| `0x80353a50` | 5 + `0x2bd50` | 6 × `char*` (24 bytes) | track name per language: JP, EN, DE, FR, ES, IT — used by the race intro and the course selection |
| `0x80352dd4` | 5 + `0x2b0d4` | `u8` | venue index: venue name (`0x803539e4` table: 1 MUTE CITY, 2 PORT TOWN, 4 BIG BLUE, 6 LIGHTNING, 8 SAND OCEAN, 10 GREEN PLANT, 11 FIRE FIELD, 13 CASINO PALACE, 14 OUTER SPACE, 15 AEROPOLIS, 16 COSMO TERMINAL, 19 PHANTOM ROAD) and the background that is loaded |
| `0x8031e18c` | 4 + `0x437c` | `u8` | venue code (Mute City 25, Casino Palace 4, Sand Ocean 41, Lightning 20, Aeropolis 50, Port Town 33, Green Plant 13, Big Blue 27, Fire Field 11, Cosmo Terminal 9, Phantom Road 35, Outer Space 22; `0xff` = none) — meaning not fully known, set to the template's value |

Found by searching RAM dumps (`work/re`) for tables whose entries are equal
for courses of the same venue and different otherwise, and by following
pointers to the track name strings.

## Free slots

Ids with name "---" and a valid venue: **2, 4, 6, 12, 18, 19, 20, 23**
(22 and 30 have venue code `0xff` and are not used). A course placed into such
a slot is loaded and raced like any other: verified with a copy of Twist Road
in slot 2 (direct start) and with a Casino Palace course in slot 2 (direct
start and Practice → custom cup → course selection → race).

A course id needs these files (copied from the template course; missing ones
are skipped):

- `stage/COLI_COURSExx.lz`, `stage/stxx.gma.lz` / `.tpl.lz` (stage models),
  `stage/selstxx.gma.lz` / `.tpl.lz` (course selection preview),
- `enemy_line/linexx.bin` (CPU racing line),
- `cam/livecam_N.bin`, `livecam_ball_N.bin`, `livecam_stage_N.bin`,
  `livecam_stage_demo_N.bin` (cameras, `N` without leading zero),
- `staff_ghost/ghostN.dat` (Time Attack staff ghost).

## How the editor writes it

`PatchDol()` (`src/cups.cpp`) appends four entries per new course to the cup
patch's patch table (kind 1 "copy", module 1): the venue code byte, the venue
index byte, the name pointer and the six language pointers, all pointing to
data (name string, pointers) stored behind the table in the patch section.
They are applied by the OSLink hook right after module 1 is linked, like the
cup tables. The patch is written whenever there are custom cups or new
courses.

## Not covered yet

- The track outline shown in the course selection (another per-course
  resource) and the difficulty stars.
- Records/ghost saving for the new ids in Time Attack and the Grand Prix
  results screen are untested.
