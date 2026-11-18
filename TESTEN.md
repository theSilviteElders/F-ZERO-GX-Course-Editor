# Editor testen

Kurzanleitung, um den F-Zero GX Course Editor einmal von Anfang bis Ende
auszuprobieren: Objekt ändern, speichern, im Spiel prüfen.

## Voraussetzungen

- Editor ist gebaut (`build/fzgx-editor`, siehe [README](README.md)).
- Disc ist nach `work/disc` extrahiert.
- F-ZERO GLINUX AppImage ist installiert. Der Editor findet es automatisch
  unter `~/.var/app/io.github.mabecgames.FZeroGLinux/`. Den Pfad kannst du
  unter *Datei → Einstellungen* ändern.

## 1. Editor starten

Im Repo-Ordner (`F-ZERO-GX-Course-Editor`):

```bash
./build/fzgx-editor
```

Kurs 01 (Mute City) wird automatisch geladen.

## 2. Ein auffälliges Objekt verändern

1. Oben links im Objekt-Filter `DASH01_BOARD` eintippen. Das sind die
   Dash-Plates auf der Fahrbahn.
2. Doppelklick auf `0 DASH01_BOARD`: Die Kamera fliegt hin.
3. Rechts im Inspektor die **Skalierung** von `1.000` auf `3.000` setzen.
   Alternativ mit dem Gizmo (Taste `W`) seitlich verschieben.

Die Skalierung eignet sich am besten für den ersten Test, weil man eine
dreimal so große Dash-Plate im Spiel sofort sieht.

**Verknüpfte Objekte:** Eine Dash-Plate besteht im Spiel aus mehreren
Objekten am selben Punkt: Fläche (`DASH01_BOARD`), Pfeil-Marker
(`DASH01_SIGN_A_LOD`) und Ring (`DASH01_SIGN_CIRCLE_A_LOD`). Mit
*Verknüpfte Objekte mitbewegen* (standardmäßig an) werden sie gemeinsam
verschoben, gedreht und skaliert. Sie werden mit hervorgehoben und rechts
aufgelistet. Verknüpft wird, was das gleiche Namenspräfix hat (z. B.
`DASH01`) und innerhalb des Radius liegt.

**Boost-Zone:** Die eigentliche Boost-Wirkung ist eine eigene
Kollisionsfläche in der Kursdatei (Oberflächentyp 3). Mit
*Kollision/Boost-Flächen mitbewegen* (standardmäßig an) wird sie
mittransformiert. Im 3D-Fenster ist sie gelb umrandet, und rechts steht z. B.
„2 Kollisionsflächen hängen dran (2 Boost)“. Erscheint die rote Warnung
„außerhalb des Kollisionsgitters“, wirkt der Teil dort vermutlich nicht.
Auf Twist Road betrifft das Dash-Plate #0, die am Rand des Gitters liegt.

## 3. Speichern und im Spiel starten

- **F5** speichert den Kurs und startet GLINUX **direkt im Rennen** auf dem
  geöffneten Kurs (Practice, Free Run, ohne CPU-Gegner, Blue Falcon).
- **Beim allerersten Start** (und nach einem GLINUX-Update) navigiert das
  Spiel etwa 30 Sekunden lang automatisch durch die Menüs und legt einen
  Savestate an. **Währenddessen nichts am Controller drücken.** Danach
  dauert jeder Start nur noch etwa 5 Sekunden.
- Nur speichern geht mit **Strg+S**.
- Der Direktstart lässt sich unter *Datei → Einstellungen* abschalten. Dann
  startet das Spiel normal im Titelbildschirm.

## 4. Worauf achten

| Prüfpunkt | Erwartung / Frage |
|---|---|
| Lädt der Kurs? | Kein Absturz und kein Hänger beim Laden |
| Optik | Dash-Plate ist größer bzw. an der neuen Stelle |
| Boost | Wirkt der Boost auf der **ganzen** neuen Fläche bzw. an der neuen Stelle? |
| Rest des Kurses | Andere Objekte und Strecke unverändert |

Die Boost-Frage ist die wichtigste: Davon hängt ab, ob die Kollision der
Objekte mitwandert oder separat angepasst werden muss.

## 5. Wenn etwas schiefgeht

- Die Startausgabe des Spiels steht in `work/launch.log`.
- *Datei → Kurs auf Original zurücksetzen* stellt den Originalkurs im
  Mod-Ordner wieder her.
- Die normale GLINUX-Installation bleibt unberührt, weil alle Änderungen
  nur in `work/mod/GFZE01` landen.
- Tests laufen mit einer **eigenen Kopie** des GLINUX-Benutzerordners
  (`work/test-userdir`, beim ersten Test aus `~/.local/share/fzero-glinux`
  kopiert). Deine echten Spielstände werden nie verändert.
  Controller-Einstellungen von GLINUX übernimmst du, indem du
  `work/test-userdir` löschst; beim nächsten Test wird er neu kopiert.
- Klemmt der Direktstart (z. B. bleibt das Spiel im Menü stehen), lösche
  `work/glinux-runtime/quickstart-practice.sav`, dann wird er beim nächsten
  Start neu aufgebaut.

## Bedienung im Editor

| Aktion | Eingabe |
|---|---|
| Umsehen und fliegen | rechte Maustaste halten + WASD, Q/E runter/hoch |
| Flugtempo | Mausrad bei gehaltener rechter Maustaste, Shift schneller |
| Auswählen | Linksklick im 3D-Fenster oder in der Liste |
| Auswahl fokussieren | `F` oder Doppelklick in der Liste |
| Verschieben / Drehen / Skalieren | `W` / `E` / `R` |
| Rückgängig / Wiederholen | Strg+Z / Strg+Y |
| Speichern / im Spiel testen | Strg+S / F5 |

## Ohne GUI prüfen (optional)

```bash
./build/fzgx-cli selftest work      # alle 37 Kurse: Parsen, Roundtrip, Kompression, Kollision
./build/fzgx-cli dump work 1 | head # Objekte von Kurs 01 auflisten
```

Erwartet: 37 Zeilen mit `OK`, keine mit `FAIL`.

## Ergebnis melden

Bitte notieren:

- welcher Kurs und welches Objekt geändert wurde
- was geändert wurde (Position, Rotation oder Skalierung)
- was im Spiel zu sehen war, vor allem wo der Boost gewirkt hat
- bei Problemen den Inhalt von `work/launch.log`
