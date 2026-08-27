#!/usr/bin/env bash
# Installs the system dependencies for the dev app (design doc Epic 7).
# The core compiler/build system needs nothing beyond a C++20 toolchain
# and CMake; only the dev app (SDL2 + Dear ImGui) has dependencies:
#   - SDL2 comes from the system package manager (run this script)
#   - Dear ImGui is vendored in third_party/imgui (no action needed)
set -euo pipefail

if command -v pkg >/dev/null && [ -d /data/data/com.termux ]; then
  # Termux on Android. SDL2 lives in the x11-repo; xorgproto provides the
  # X11 headers SDL_syswm.h pulls in. See scripts/android-dev.sh for the
  # rest of the on-phone setup (display and audio servers).
  pkg install -y x11-repo
  pkg update || true
  pkg install -y sdl2 xorgproto
elif command -v apt-get >/dev/null; then
  apt-get update || true  # tolerate unrelated failing repos
  apt-get install -y libsdl2-dev
elif command -v dnf >/dev/null; then
  dnf install -y SDL2-devel
elif command -v pacman >/dev/null; then
  pacman -S --noconfirm sdl2
else
  echo "no supported package manager found; install SDL2 dev headers manually" >&2
  exit 1
fi

pkg-config --modversion sdl2 && echo "SDL2 ready"
