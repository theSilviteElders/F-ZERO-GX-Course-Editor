#!/usr/bin/env bash
# Extracts a GameCube RVZ/ISO into <out>/{files,sys} with dolphin-tool
# (native install or the Dolphin Flatpak).
set -euo pipefail
rom="$1"; out="$2"
mkdir -p "$out"
if command -v dolphin-tool >/dev/null; then
    dolphin-tool extract -i "$rom" -o "$out"
else
    flatpak run --command=dolphin-tool \
        --filesystem="$(dirname "$rom")" --filesystem="$out" \
        org.DolphinEmu.dolphin-emu extract -i "$rom" -o "$out"
fi
echo "[extract-disc] done"
