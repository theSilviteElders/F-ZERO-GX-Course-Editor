#!/usr/bin/env bash
# Starts F-Zero GX from the editor's mod copy (an extracted disc folder).
#
#   run-test.sh glinux  <game-dir> <GLINUX.AppImage> <glinux-user-dir> <runtime-dir> [course-id]
#   run-test.sh dolphin <game-dir>
#
# With a course id the race starts directly (Practice, Free Run, no CPUs):
#   1. Once per GLINUX version the menus are navigated automatically
#      (Practice > Ruby Cup > Twist Road > Blue Falcon > settings dialog) and a
#      savestate is taken right before the course is loaded.
#   2. Every test loads that savestate, writes the course id to the race setup
#      (byte 0x803FFB71 with the original main.dol: the COLI_COURSExx number the
#      game loads next) and presses OK. The course itself is then read from the
#      edited disc folder. The address is taken relative to the main module
#      (po = [0x800030C8]) because the custom-cup patch moves it by a few KiB.
set -euo pipefail
mode="$1"; game="$2"; appimage="${3:-}"; src_user_dir="${4:-}"; runtime="${5:-}"; course="${6:-}"
echo "[run-test] mode=$mode game=$game course=${course:-menu}"

if [[ "$mode" == dolphin ]]; then
    exec flatpak run --filesystem="$game" org.DolphinEmu.dolphin-emu -e "$game/sys/main.dol"
fi

[[ -f "$appimage" ]] || { echo "GLINUX AppImage not found: $appimage" >&2; exit 1; }
src_user_dir="${src_user_dir:-${XDG_DATA_HOME:-$HOME/.local/share}/fzero-glinux}"

# Unpack the AppImage once (again when it changes) so the runner can be started
# directly with an arbitrary game folder. The quick-start savestate lives in the
# same folder, so it is rebuilt automatically for a new GLINUX version.
stamp="$(stat -c '%s-%Y' "$appimage")"
if [[ ! -x "$runtime/squashfs-root/usr/bin/moderngekko-run" || "$(cat "$runtime/stamp" 2>/dev/null)" != "$stamp" ]]; then
    echo "[run-test] unpacking $appimage"
    rm -rf "$runtime"; mkdir -p "$runtime"
    (cd "$runtime" && "$appimage" --appimage-extract >/dev/null)
    echo "$stamp" > "$runtime/stamp"
fi
appdir="$runtime/squashfs-root"

# Tests run with their own copy of the GLINUX user folder (settings, controller,
# memory card), so a savestate can never overwrite your real save data.
user_dir="$runtime/../test-userdir"
if [[ ! -d "$user_dir" ]]; then
    echo "[run-test] creating test user dir from $src_user_dir"
    mkdir -p "$user_dir"
    if [[ -d "$src_user_dir" ]]; then
        cp -a --reflink=auto "$src_user_dir/." "$user_dir/"
        rm -rf "$user_dir/games"  # extracted game copies are not needed
    fi
fi
user_dir="$(cd "$user_dir" && pwd)"

# Same environment as the AppImage's AppRun.
export MODERNGEKKO_STATICRECOMP=0
unset STATICRECOMP_MODULE
export LD_LIBRARY_PATH="$appdir/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=1
export SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1
runner=("$appdir/usr/bin/moderngekko-run" --game "$game" --user-dir "$user_dir")

if [[ -z "$course" ]]; then
    exec "${runner[@]}"
fi

# --- Direct start via the runner's automation directory ----------------------
auto="$runtime/automation"
state="$runtime/quickstart-practice.sav"
rm -rf "$auto"; mkdir -p "$auto/commands"
seq=0
# cmd "key=value ..." : queue one command and wait until the runner took it.
cmd() {
    seq=$((seq + 1))
    local name; name="$(printf '%05d.txt' "$seq")"
    tr ' ' '\n' <<<"$1" > "$auto/$name.tmp"
    mv "$auto/$name.tmp" "$auto/commands/$name"
    for _ in $(seq 1 1200); do
        [[ -e "$auto/commands/$name" ]] || break
        kill -0 "$pid" 2>/dev/null || { echo "[run-test] game exited" >&2; exit 1; }
        sleep 0.05
    done
    if [[ -e "$auto/failed/$name" ]]; then
        echo "[run-test] command failed: $1 ($(grep last_error "$auto/status.txt" 2>/dev/null))" >&2
        return 1
    fi
}
press() { cmd "command=pad_frames port=0 frames=4 $1=1"; sleep "${2:-1}"; }
# Race setup struct (module 1 BSS): po + 0x2453E0 (= 0x803FFAE0 with po 0x801BA700).
read_setup() {
    cmd "command=read_memory address=0x800030C8 size=4 path=po.bin"
    setup=$(( 16#$(od -An -tx1 -N4 "$auto/po.bin" | tr -d ' \n') + 0x2453E0 ))
}
frames() { sed -n 's/^frame_count=//p' "$auto/status.txt" 2>/dev/null || echo 0; }

"${runner[@]}" --automation-dir "$auto" &
pid=$!
# Wait until the game is running.
for _ in $(seq 1 600); do
    [[ "$(sed -n 's/^booted=//p' "$auto/status.txt" 2>/dev/null)" == 1 ]] && break
    kill -0 "$pid" 2>/dev/null || { echo "[run-test] game exited during boot" >&2; exit 1; }
    sleep 0.1
done

if [[ -f "$state" ]]; then
    cmd "command=load_state path=$state"
    sleep 1.5
else
    echo "[run-test] building quick-start savestate (one time, about 30 s)"
    # Title screen: wait for the logo, then Start.
    while (( $(frames) < 1500 )); do sleep 0.2; done
    press start 2.5          # -> Select Mode (cursor on Grand Prix)
    press dpad_down 0.8      # -> Practice
    press a 3                # -> Select Cup (Ruby)
    press a 3                # -> Select Course (Twist Road)
    press a 3                # -> Select Machine (Blue Falcon)
    press a 2.5              # -> Machine Settings
    press a 2.5              # -> Practice dialog, cursor on OK
    read_setup
    cmd "command=read_memory address=$(printf '0x%X' $((setup + 0x90))) size=4 path=check.bin"
    if [[ "$(od -An -tx1 -j1 -N1 "$auto/check.bin" | tr -d ' ')" != 01 ]]; then
        echo "[run-test] menu navigation did not reach the course selection; staying in the menu" >&2
        wait "$pid"; exit 0
    fi
    cmd "command=save_state path=$state"
    sleep 1.5                # saving pauses the core; inputs sent now would be lost
    echo "[run-test] savestate written: $state"
fi

read_setup
cmd "command=write_memory address=$(printf '0x%X' $((setup + 0x91))) data=$(printf '%02X' "$course")"
press a 1.5                  # OK -> loads COLI_COURSE$course from the edited disc
press a 0.5                  # safety repeat (only skips the course intro if the first one worked)
echo "[run-test] racing on course $course"
wait "$pid"
