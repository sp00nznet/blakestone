#!/bin/bash
# Linux and macOS setup: the same steps as Setup.cmd (scripts/setup.ps1), for a
# native program -- no Wine, no DOSBox.
#
#   1. check the tools (a C compiler, CMake, Python 3, git, pkg-config, SDL2);
#      on macOS Homebrew installs what is missing, on Linux it says which
#      packages to install
#   2. copy your copy of the games into original/aog and original/ps: from
#      Steam, GOG, or the folder you name (any folder holding BS_AOG.EXE or
#      BS_FIRE.EXE, in any case)
#   3. fetch the pcrecomp toolkit beside this folder, as ../pcrecomp
#   4. lift each game to C (tools/lift.py), then build build-linux/ (or
#      build-macos/)
#   5. leave a launcher here for each game: "Blake Stone - Aliens of Gold.sh"
#
#   ./setup.sh                       # look for the games
#   ./setup.sh ~/GOG\ Games/Blake\ Stone
#   ./setup.sh --yes --game-dir DIR  # as the recomp launcher runs it (it asks nothing either way)
#
# Every step is skipped when its result is already there, so running it
# again carries on from where it stopped.
set -e
cd "$(dirname "$0")"
ROOT=$PWD
GAME_ARG=
while [ $# -gt 0 ]; do
  case $1 in
    --yes) ;;                                  # nothing here asks
    --game-dir) GAME_ARG=$2; shift ;;
    *) GAME_ARG=$1 ;;
  esac
  shift
done
# The toolkit revision this game needs (README, "Building from source"); a
# commit, not a branch: main moves, this build should not.
PCRECOMP_URL=https://github.com/sp00nznet/pcrecomp
PCRECOMP_REF=fc852e31765e8661cdde4326f2e2335090a05c30
if [ "$(uname)" = Darwin ]; then MAC=1; BUILD=build-macos; else MAC=; BUILD=build-linux; fi

say()  { printf '%s\n' "$*"; }
step() { printf '\n\033[36m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[31mSetup stopped: %s\033[0m\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# ---- 1. tools -------------------------------------------------------------------
step "[1/5] Checking the tools"
if [ -n "$MAC" ]; then
  xcode-select -p >/dev/null 2>&1 || { xcode-select --install 2>/dev/null || true
    fail "Xcode's command line tools are needed: accept the install dialog, then run ./setup.sh again."; }
  if ! have cmake || ! have pkg-config || ! have python3 || ! have git || ! pkg-config --exists sdl2 2>/dev/null; then
    have brew || fail "Homebrew is needed to install CMake and SDL2: see https://brew.sh, then run ./setup.sh again."
    say "  installing with Homebrew: cmake pkgconf sdl2 python git"
    brew install cmake pkgconf sdl2 python git || fail "brew install failed."
  fi
else
  need=()
  have cc || have gcc || have clang || need+=(gcc)
  for t in cmake python3 git pkg-config; do have "$t" || need+=("$t"); done
  have pkg-config && ! pkg-config --exists sdl2 && need+=("SDL2's development files")
  if [ ${#need[@]} -gt 0 ]; then
    say "  Missing: ${need[*]}. From your package manager, for example:"
    say "    Debian, Ubuntu: sudo apt install gcc g++ cmake python3 git pkg-config libsdl2-dev"
    say "    Fedora:         sudo dnf install gcc gcc-c++ cmake python3 git pkgconf sdl2-compat-devel"
    say "    Arch:           sudo pacman -S gcc cmake python git pkgconf sdl2"
    fail "install them, then run ./setup.sh again."
  fi
fi
have ffmpeg || say "  (ffmpeg not found: everything works except --record.)"
say "  ok"

# ---- 2. the games -----------------------------------------------------------------
step "[2/5] Copying your games into original/"
# Where a store may have put them: the folder given, Steam's libraries, GOG.
roots=()
[ -n "$GAME_ARG" ] && roots+=("$GAME_ARG" "$(dirname "$GAME_ARG")")
for s in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" \
         "$HOME/Library/Application Support/Steam"; do
  [ -d "$s/steamapps" ] || continue
  roots+=("$s/steamapps/common")
  [ -f "$s/steamapps/libraryfolders.vdf" ] &&
    while read -r lib; do roots+=("$lib/steamapps/common"); done < <(sed -n 's/.*"path"[[:space:]]*"\(.*\)".*/\1/p' "$s/steamapps/libraryfolders.vdf")
done
roots+=("$HOME/GOG Games" "$HOME/Games" "/Applications")

# the folder under the roots holding $1 (BS_AOG.EXE), any case
find_game() {
  for r in "${roots[@]}"; do
    [ -d "$r" ] || continue
    f=$(find "$r" -maxdepth 5 -type f -iname "$1" 2>/dev/null | head -n 1)
    [ -n "$f" ] && { dirname "$f"; return; }
  done
}

found=()
for g in "aog BS_AOG.EXE Aliens_of_Gold" "ps BS_FIRE.EXE Planet_Strike"; do
  set -- $g
  id=$1 exe=$2 name="Blake Stone - ${3//_/ }"
  dest=original/$id
  if [ -n "$(find "$dest" -maxdepth 1 -iname "$exe" 2>/dev/null)" ]; then
    say "  $name: already copied"; found+=("$id"); continue
  fi
  src=$(find_game "$exe")
  [ -n "$src" ] || { say "  $name: not found, skipping"; continue; }
  say "  $name: copying from $src"
  mkdir -p "$dest"
  # the game's own files only -- not the DOSBox the store version wraps it in
  find "$src" -maxdepth 1 -type f ! -iname '*.conf' ! -iname '*.pdf' ! -iname '*.map' -exec cp {} "$dest/" \;
  # lift.py looks for the upper-case name
  [ -f "$dest/$exe" ] || mv "$(find "$dest" -maxdepth 1 -iname "$exe" | head -n 1)" "$dest/$exe"
  found+=("$id")
done
[ ${#found[@]} -gt 0 ] || fail "no copy of either game was found. Run ./setup.sh again with the folder that holds BS_AOG.EXE or BS_FIRE.EXE."

# ---- 3. the toolkit ----------------------------------------------------------------
step "[3/5] Fetching the pcrecomp toolkit"
if [ -n "$PCRECOMP_HOME" ] && [ -f "$PCRECOMP_HOME/tools/lift/lift16.py" ]; then
  PCR=$PCRECOMP_HOME
else
  PCR=$(cd .. && pwd)/pcrecomp
  if [ ! -f "$PCR/tools/lift/lift16.py" ]; then
    git clone --quiet "$PCRECOMP_URL" "$PCR" || fail "could not fetch pcrecomp."
  fi
  # the recomp launcher unpacks it at the pinned commit as a plain folder
  if [ -d "$PCR/.git" ]; then
    git -C "$PCR" fetch --quiet origin && git -C "$PCR" checkout --quiet "$PCRECOMP_REF" ||
      fail "could not check out pcrecomp $PCRECOMP_REF."
  fi
fi
export PCRECOMP_HOME=$PCR
say "  $PCR"

# ---- 4. lift + build -------------------------------------------------------------
step "[4/5] Lifting and building (several minutes: the lifted C is large)"
mkdir -p work
for id in "${found[@]}"; do
  if [ -f "work/$id/gen/recomp_dispatch.c" ]; then say "  $id: already lifted"; continue; fi
  say "  lifting $id"
  python3 tools/lift.py "$id" > "work/lift_$id.log" 2>&1 || { fail "lifting $id failed; see work/lift_$id.log."; }
done
jobs=$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 4)
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release "-DPCRECOMP_HOME=$PCR" >/dev/null || fail "configuring the build failed."
cmake --build "$BUILD" -j "$jobs" || fail "the build failed."

# ---- 5. launchers -----------------------------------------------------------------
step "[5/5] Making a launcher for each game"
for id in "${found[@]}"; do
  case $id in aog) name="Blake Stone - Aliens of Gold" ;; ps) name="Blake Stone - Planet Strike" ;; esac
  [ -x "$BUILD/bstone_$id" ] || fail "the build finished but $BUILD/bstone_$id is missing."
  printf '#!/bin/sh\ncd "$(dirname "$0")" && exec ./%s/bstone_%s "$@"\n' "$BUILD" "$id" > "$name.sh"
  chmod +x "$name.sh"
  say "  ready: $name.sh"
done
say ""
say "Done. Run a launcher to play. Alt+Enter toggles fullscreen."
