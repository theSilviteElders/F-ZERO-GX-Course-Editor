# F-Zero GX Course Editor

https://github.com/theSilviteElders/F-ZERO-GX-Course-Editor.git

Eigenständiger Kurs-Editor für F-Zero GX (USA, `GFZE01`) mit Testknopf für
[F-ZERO GLINUX](https://github.com/Mabec-games/F-ZERO-GLINUX). Erster
Meilenstein: Kurse ansehen und platzierte Objekte (Dash-Plates, Schilder,
Gebäude, Straßenmodelle …) samt ihrer Boost-Flächen verschieben, drehen und
skalieren.

Spieldaten sind **nicht** enthalten: Du brauchst dein eigenes F-Zero-GX-Abbild
(USA, RVZ oder ISO). Lege es in `rom/` (im Repo, git-ignoriert) oder in einen
`rom/`-Ordner neben dem Repo, oder wähle es im Editor unter *Einstellungen*.

## Bauen

Benötigt: CMake ≥ 3.20, C++20-Compiler, SDL3, libepoxy, glm (Arch/CachyOS:
`sdl3 libepoxy glm`). Dear ImGui 1.91.9 und ImGuizmo lädt CMake selbst.

```bash
git clone https://github.com/theSilviteElders/F-ZERO-GX-Course-Editor.git
cd F-ZERO-GX-Course-Editor
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/fzgx-editor
```

Eine Schritt-für-Schritt-Testanleitung steht in [TESTEN.md](TESTEN.md).

## Arbeitsordner (`work/`, git-ignoriert)

| Pfad | Inhalt |
|---|---|
| `work/disc/` | Original-Disc, einmal aus der RVZ extrahiert (wird nie verändert) |
| `work/mod/GFZE01/` | Testkopie (btrfs-Reflink, kostet fast keinen Platz) mit bearbeiteten Dateien |
| `work/glinux-runtime/` | entpacktes GLINUX-AppImage + Direktstart-Savestate (`quickstart-practice.sav`) |
| `work/test-userdir/` | eigene Kopie des GLINUX-Benutzerordners für Tests (schützt deine Spielstände) |
| `work/editor.ini` | Einstellungen (ROM, AppImage, GLINUX-Benutzerordner, Starter) |
| `work/launch.log` | Ausgabe des letzten Spielstarts |

Die Disc wird mit `dolphin-tool` (Dolphin-Flatpak) extrahiert, entweder
im Editor unter *Datei → Einstellungen* oder per
`scripts/extract-disc.sh <rom.rvz> work/disc`.

## Bedienung

- **Kamera:** rechte Maustaste halten + WASD/QE fliegen (Shift schneller,
  Strg langsamer, Mausrad ändert das Tempo). Ohne rechte Maustaste: Mausrad
  vor/zurück, mittlere Maustaste schieben.
- **Auswahl:** Linksklick im 3D-Fenster oder in der Objektliste
  (Doppelklick fokussiert), `F` fokussiert, `Esc` hebt die Auswahl auf.
- **Gizmo:** `W` verschieben, `E` drehen, `R` skalieren; optional Raster.
  Werte lassen sich auch rechts im Inspektor eintippen.
- **Strg+Z / Strg+Y:** Rückgängig / Wiederholen.
- **Strg+S:** speichert den Kurs komprimiert in `work/mod/GFZE01`.
- **F5:** speichert und startet das Spiel aus dem Mod-Ordner direkt im
  Rennen auf dem geöffneten Kurs (siehe *Direktstart*).
- **Verknüpfte Objekte** (z. B. Dash-Plate + Pfeil-Marker + Ring) werden
  gemeinsam bearbeitet: gleiches Namenspräfix, innerhalb des Radius.

## Direktstart

`scripts/run-test.sh` nutzt das Automation-Verzeichnis des GLINUX-Runners
(`--automation-dir`, Befehlsdateien für Controller, Speicher, Savestates):

1. Einmal pro GLINUX-Version: Menüs automatisch durchlaufen (Practice →
   Ruby Cup → Twist Road → Blue Falcon → Einstellungsdialog) und dort,
   unmittelbar vor dem Laden des Kurses, einen Savestate speichern.
2. Bei jedem Test: Savestate laden, Kursnummer nach **`0x803FFB71`**
   schreiben (u8, = Nummer der `COLI_COURSExx.lz`, die das Spiel als
   Nächstes lädt; ermittelt per RAM-Vergleich der fünf Ruby-Cup-Kurse und
   gegengetestet mit Kurs 14 → Big Blue) und OK drücken. Der Kurs wird dann
   aus dem Mod-Ordner geladen.

Nur NTSC-U (`GFZE01`). Mit Dolphin als Starter gibt es keinen Direktstart.

## Was bekannt ist (Formate)

- `.lz`: 8-Byte-Header (LE: gepackte Größe ohne Header, entpackte Größe) +
  LZSS mit 4-KiB-Ring ab `0xFEE`. Der eigene Kompressor ist verlustfrei
  geprüft und erzeugt etwa gleich große Dateien wie das Original.
- `COLI_COURSExx`: Header ab `0x48`/`0x54` = Anzahl/Zeiger der platzierten
  Objekte (je `0x40` Bytes: Position, Rotation als s16 mit 65536 = 360°,
  Skalierung, Zeiger auf eine 3×4-Matrix). Für alle ~18.000 Objekte gilt
  `M = Rz·Ry·Rx·S`. Der Editor schreibt beides (TRS + Matrix).
  Track-Checkpoints (`0x08`) liefern Mittellinie und Breite, die statische
  Kollision (`0x1C`) Dreiecke/Vierecke in 14 Oberflächen-Gittern.
- Statische Kollision: Dreieck `0x58` / Viereck `0x70` Bytes:
  `d = −n·v0`, `n = normalize((v1−v0)×(v2−v0))`, Eckpunkte, dann je Kante
  `e_k = (n × (v[k+1]−v[k])) · |(v[k+1]−v[k]) × (v[k−1]−v[k])|`. Gegen alle
  Flächen aller 37 Kurse geprüft. Gefunden werden die Flächen über ein
  XZ-Gitter pro Oberflächentyp (u16-Indexlisten, Ende `0xFFFF`).
  **Typ 3 = Dash-Plate (Boost).** Die Boost-Zone ist eine eigene
  Kollisionsfläche in Weltkoordinaten und hängt nicht am Dash-Plate-Objekt.
  Der Editor bewegt Kollisionsflächen, die vollständig unter einem bearbeiteten
  Objekt liegen, mit, berechnet Ebene und Kanten neu und trägt sie in neu
  überdeckte Gitterzellen ein. Dafür werden neue Indexlisten an die Datei
  angehängt. Außerhalb des Gitters wirkt eine Fläche vermutlich nicht
  (der Editor warnt).
- `.gma`: GCMF-Modelle mit GX-Display-Lists. Gerendert wird Geometrie mit
  Materialfarbe, noch ohne Texturen. Kurs-Modelle stehen in
  `stage/stNN.gma`, gemeinsame Objekte in `bg/bg_*.gma`.

`./build/fzgx-cli selftest work` prüft alle 37 Kurse: Parsen, unveränderter
Roundtrip (bytegleich), LZ-Roundtrip, Matrixformel und Modellabdeckung.
Weitere Befehle: `dump <work> <kurs>`, `move <work> <kurs> <index> dx dy dz`,
`dashscale <work> <kurs> <faktor> <out.lz>` (Test: alle Boost-Flächen
skalieren, in eine separate Datei schreiben), `unlz`, `lz`.

## Grenzen / nächste Schritte

- Straßenmodelle sind nur Optik: Fahrbahn, Physik und KI kommen aus dem
  Track-Spline (gelbe Linie), der noch nicht editierbar ist.
- Objekte können verändert, aber noch nicht hinzugefügt oder gelöscht werden
  (dafür müsste die Datei neu angeordnet werden, inkl. aller Zeiger).
- Einige Modelle (Skin/animiert, 16-Bit-Vertices) erscheinen als blaue Box,
  nicht gefundene als rote Box.
- Ob jedes Objekt im Spiel wirklich dort erscheint, wo es hinbewegt wurde
  (z. B. Sichtbarkeits-/Kollisionsgitter), muss im Spiel getestet werden.
- Texturen (`.tpl`), Track-Spline-Bearbeitung, Objekte duplizieren.
