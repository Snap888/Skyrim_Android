#!/bin/sh
# Regenerates gen/ (recompiled C) from your own Skyrim files.
# Usage: tools/regen_gen.sh GAME_DIR
# GAME_DIR must contain TESV_unpacked.exe (TESV.exe with SteamStub removed by
# Steamless), d3dx9_42.dll, binkw32.dll and X3DAudio1_7.dll. See the README.
# Needs Python 3.10+: pip install capstone==5.0.9 pefile==2024.8.26
set -e
G=$(cd "${1:?usage: tools/regen_gen.sh GAME_DIR}" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
find_file() { for f in "$G"/*; do
    [ "$(echo "${f##*/}" | tr A-Z a-z)" = "$1" ] && { echo "$f"; return; }; done
    echo "missing $1 in $G" >&2; exit 1; }
EXE=$(find_file tesv_unpacked.exe); D3DX=$(find_file d3dx9_42.dll)
BINK=$(find_file binkw32.dll); X3DA=$(find_file x3daudio1_7.dll)
mkdir -p analysis gen
export LIFT_GAME=skyrim
# discover.py imports peimg.py from tools/, so run it from there.
(cd tools && python3 discover.py "$EXE" ../analysis/functions.json)
(cd tools && python3 discover.py "$D3DX" ../analysis/functions_d3dx.json 08000000)
(cd tools && python3 discover.py "$BINK" ../analysis/functions_bink.json 09000000)
(cd tools && python3 discover.py "$X3DA" ../analysis/functions_x3da.json 0A000000)
python3 tools/lift.py "$EXE" analysis/functions.json gen exe
python3 tools/lift.py "$D3DX" analysis/functions_d3dx.json gen d3dx9 08000000
python3 tools/lift.py "$BINK" analysis/functions_bink.json gen bink 09000000
python3 tools/lift.py "$X3DA" analysis/functions_x3da.json gen x3da 0A000000
echo "gen/ is ready"
