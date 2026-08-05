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

mkdir -p outputs/primitives outputs/basic
cp examples/primitives/build/artifacts/*.wav outputs/primitives/
cp examples/basic/build/artifacts/*.wav outputs/basic/
echo "outputs/ refreshed"
