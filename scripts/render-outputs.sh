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
"$BUILD_DIR/synthc" build examples/advanced
"$BUILD_DIR/synthc" build examples/darksynth
"$BUILD_DIR/synthc" build examples/song

# All outputs land under the examples root's _build/, mirroring the tree.
mkdir -p outputs/primitives outputs/basic outputs/advanced outputs/song outputs/darksynth
cp examples/_build/primitives/artifacts/*.wav outputs/primitives/ 2>/dev/null || true
cp examples/_build/primitives/artifacts/*.svg outputs/primitives/ 2>/dev/null || true
cp examples/_build/basic/artifacts/*.wav outputs/basic/ 2>/dev/null || true
cp examples/_build/basic/artifacts/*.svg outputs/basic/ 2>/dev/null || true
cp examples/_build/advanced/artifacts/*.wav outputs/advanced/ 2>/dev/null || true
cp examples/_build/advanced/artifacts/*.svg outputs/advanced/ 2>/dev/null || true
# Only the master + waveform are committed; the per-instrument stems
# (song-drums/pad/piano/guitar.wav, ~3 MB each) build locally.
cp examples/_build/song/artifacts/song.wav outputs/song/ 2>/dev/null || true
cp examples/_build/song/artifacts/song-wave.svg outputs/song/ 2>/dev/null || true
cp examples/_build/song/artifacts/song-stems-wave.svg outputs/song/ 2>/dev/null || true
# darksynth: master + overview only; the five stems (~15 MB each) build locally
cp examples/_build/darksynth/artifacts/darksynth.wav outputs/darksynth/ 2>/dev/null || true
cp examples/_build/darksynth/artifacts/darksynth-stems-wave.svg outputs/darksynth/ 2>/dev/null || true
echo "outputs/ refreshed"
