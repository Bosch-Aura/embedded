#!/bin/bash
# Compila el firmware para la PC (simulador).
#   ./build.sh [firmware_dir] [salida]
# Por defecto: el firmware de este repo -> build/fw_sim
# Compilador: $CXX, si no g++, si no clang++, si no zig (pip install ziglang).
# $SIM_FLAGS: flags extra, p. ej. "-DULTRASONIC_USE_PULSEIN" o "-DSIM_TICK_OFFSET=4294964796u".
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
FW=${1:-$HERE/../..}
OUT=${2:-$HERE/build/fw_sim}
mkdir -p "$(dirname "$OUT")"
if [ -n "$CXX" ]; then CC_CMD="$CXX"
elif command -v g++ >/dev/null; then CC_CMD=g++
elif command -v clang++ >/dev/null; then CC_CMD=clang++
elif python3 -c "import ziglang" 2>/dev/null; then CC_CMD="python3 -m ziglang c++"
else echo "No C++ compiler: install g++ or 'pip install ziglang', or set CXX" >&2; exit 1; fi
$CC_CMD -std=c++17 -O1 -g -w -pthread -DSIM $SIM_FLAGS -I"$HERE/stubs" -I"$FW/include" -I"$FW/src" \
    "$HERE/stubs/sim_core.cpp" "$FW"/src/*.cpp -o "$OUT"
echo "built $OUT"
