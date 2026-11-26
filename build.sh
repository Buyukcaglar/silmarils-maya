#!/bin/sh
# build.sh [MAYA_DATA folder] [CMake options...] - the macOS counterpart of build.cmd: a universal (arm64 + x86_64)
# Release build into build/mac: "Le Fetiche Maya 64bit.app" (the game) and maya_cli (the headless test runner).
# Needs the Xcode command line tools, CMake and Python 3 (Ninja is used when present).  For a Developer ID signature:
#   ./build.sh ~/Games/Maya/data -DMAYA_CODESIGN_IDENTITY="Developer ID Application: <name> (<team>)"
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
DATA="$ROOT/_extracted/amiga/Maya/data"
case "$1" in
    ""|-*) ;;
    *) DATA="$1"; shift ;;
esac
GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"
cmake -S "$ROOT" -B "$ROOT/build/mac" $GEN -DCMAKE_BUILD_TYPE=Release "-DMAYA_DATA=$DATA" "$@"
cmake --build "$ROOT/build/mac" --parallel
echo
echo "Built: $ROOT/build/mac/Le Fetiche Maya 64bit.app"
