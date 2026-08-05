#!/usr/bin/env bash
# Renders the showcase projects and refreshes the committed artifacts in
# outputs/. Usage: scripts/render-outputs.sh  (BUILD_DIR overrides `build`)
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD_DIR=${BUILD_DIR:-build}

if [ ! -x "$BUILD_DIR/synthc" ]; then
  cmake -B "$BUILD_DIR"
  cmake --build "$BUILD_DIR" --target synthc
fi

"$BUILD_DIR/synthc" build examples/primitives
"$BUILD_DIR/synthc" build examples/basic
"$BUILD_DIR/synthc" build examples/song

mkdir -p outputs/primitives outputs/basic outputs/song
cp examples/primitives/build/artifacts/*.wav outputs/primitives/ 2>/dev/null || true
cp examples/primitives/build/artifacts/*.svg outputs/primitives/ 2>/dev/null || true
cp examples/basic/build/artifacts/*.wav outputs/basic/ 2>/dev/null || true
cp examples/basic/build/artifacts/*.svg outputs/basic/ 2>/dev/null || true
# Only the master + waveform are committed; the per-instrument stems
# (song-drums/pad/piano/guitar.wav, ~3 MB each) build locally.
cp examples/song/build/artifacts/song.wav outputs/song/ 2>/dev/null || true
cp examples/song/build/artifacts/song-wave.svg outputs/song/ 2>/dev/null || true
echo "outputs/ refreshed"
