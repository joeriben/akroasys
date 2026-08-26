#!/usr/bin/env bash
# Build one of the offline tools in tools/*.cpp against the plugin's own static
# library, with the same flags CMake compiled that library with.
#
#   tools/build_offline_tool.sh test_mpe_parity [outdir]
#
# It resolves the library names rather than naming them, because they follow the
# PRODUCT name: the artefact is libakroasys_SharedCode.a since the rename, and a
# stale libacroasys_SharedCode.a from before it can still be sitting beside it.
# The recipes in the tool headers named the pre-rename path and silently failed
# to link, which reads exactly like the tool not building.
#
# Csound is linked too. Only some tools pull CsoundEngine in, but linking it
# unconditionally costs nothing and the ones that need it fail with an
# "Undefined symbols" wall that names no cause.
set -euo pipefail
cd "$(dirname "$0")/.."
TOOL="${1:?usage: build_offline_tool.sh <tool-name> [outdir]}"
OUT="${2:-/tmp}"
BUILD=build_clean
[ -f "tools/$TOOL.cpp" ] || { echo "no such tool: tools/$TOOL.cpp" >&2; exit 2; }

FLAGS="$BUILD/CMakeFiles/T5ynth.dir/flags.make"
[ -f "$FLAGS" ] || { echo "configure/build $BUILD first" >&2; exit 2; }
RSP="$(mktemp)"
{ grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
    | sed 's/^CXX_[A-Z]* = //' > "$RSP"
echo "-I$PWD/$BUILD/_deps/signalsmith_stretch-src" >> "$RSP"

# Newest SharedCode archive wins; the rename left an older one behind.
SHARED="$(ls -t "$BUILD"/T5ynth_artefacts/Release/lib*_SharedCode.a 2>/dev/null | head -1)"
[ -n "$SHARED" ] || { echo "no SharedCode archive in $BUILD" >&2; exit 2; }
LIBS=("$SHARED")
[ -f "$BUILD/libT5ynthData.a" ] && LIBS+=("$BUILD/libT5ynthData.a")
CS="third_party/csound/macos-arm64/lib/CsoundLib64"
[ -f "$CS" ] && LIBS+=("$CS")

clang++ -std=c++17 -O2 "@$RSP" "tools/$TOOL.cpp" "${LIBS[@]}" \
    -framework Accelerate -framework AudioToolbox -framework Cocoa \
    -framework CoreAudio -framework CoreAudioKit -framework CoreMIDI \
    -framework DiscRecording -framework Foundation -framework IOKit \
    -framework QuartzCore -framework Security -framework WebKit \
    -weak_framework Metal -weak_framework MetalKit \
    -o "$OUT/$TOOL"
rm -f "$RSP"
echo "$OUT/$TOOL"
