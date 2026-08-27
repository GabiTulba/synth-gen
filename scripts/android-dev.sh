#!/usr/bin/env bash
# One-command SynthGraph session on an Android phone running Termux:
# brings up (or reuses) the termux-x11 display and PulseAudio, launches
# `synthc watch` on the project, and runs the dev app fullscreen on the
# phone screen. Ctrl+C or quitting the app stops the watcher this script
# started; the display and audio servers are shared services and are left
# running for the next session.
#
# Usage: scripts/android-dev.sh [--scale N] [--display N] [--windowed] [PROJECT_DIR]
#   --scale N     UI scale for the dev app (default 2 - phone screens are dense)
#   --display N   X display number (default 0)
#   --windowed    900x600 window instead of fullscreen
#   PROJECT_DIR   the SynthGraph project to watch and browse (default '.')
#
# One-time setup, in Termux:
#   pkg install x11-repo && pkg update
#   pkg install clang cmake ninja sdl2 xorgproto pulseaudio termux-x11-nightly
# plus the Termux:X11 companion app (APK) from
# https://github.com/termux/termux-x11/releases - the phone-screen half of
# the X server. Then build the repo as usual (cmake -B build -G Ninja &&
# cmake --build build).
set -euo pipefail

scale=2
display=0
fullscreen="--fullscreen"
project=.

usage() {
  awk 'NR > 1 && !/^#/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --scale)   [ $# -ge 2 ] || usage; scale=$2; shift 2 ;;
    --display) [ $# -ge 2 ] || usage; display=$2; shift 2 ;;
    --windowed) fullscreen=""; shift ;;
    -h|--help|-*) usage ;;
    *) project=$1; shift ;;
  esac
done

if [ -z "${TERMUX_VERSION:-}" ] && [ ! -d /data/data/com.termux ]; then
  echo "warning: this doesn't look like Termux; continuing anyway" >&2
fi

if [ ! -d "$project" ]; then
  echo "error: project directory '$project' does not exist" >&2
  exit 1
fi

# synthc/synth-dev: prefer PATH, fall back to this repo's build tree.
repo_root=$(cd "$(dirname "$0")/.." && pwd)
find_tool() {
  command -v "$1" 2>/dev/null && return
  [ -x "$repo_root/build/$1" ] && { echo "$repo_root/build/$1"; return; }
  echo "error: '$1' not found on PATH or in $repo_root/build;" \
       "build the repo first (cmake -B build -G Ninja && cmake --build build)" >&2
  return 1
}
synthc=$(find_tool synthc)
synth_dev=$(find_tool synth-dev)

# --- X display: reuse a live termux-x11, else start one and wait for its
# socket. The Termux:X11 *app* renders the screen; this is the server side.
xsocket="${PREFIX:-/data/data/com.termux/files/usr}/tmp/.X11-unix/X$display"
if [ ! -S "$xsocket" ]; then
  command -v termux-x11 >/dev/null ||
    { echo "error: termux-x11 not installed (pkg install termux-x11-nightly)" >&2; exit 1; }
  echo "starting termux-x11 on display :$display ..."
  termux-x11 ":$display" >/dev/null 2>&1 &
  for _ in $(seq 1 50); do
    [ -S "$xsocket" ] && break
    sleep 0.2
  done
  [ -S "$xsocket" ] ||
    { echo "error: X socket never appeared; is the Termux:X11 app installed?" >&2; exit 1; }
fi

# --- PulseAudio: SDL in Termux talks to it over TCP on localhost, so make
# sure the daemon is up and has the TCP module loaded.
if command -v pulseaudio >/dev/null; then
  if ! pulseaudio --check 2>/dev/null; then
    echo "starting pulseaudio ..."
    pulseaudio --start --exit-idle-time=-1 \
      --load="module-native-protocol-tcp auth-ip-acl=127.0.0.1"
  elif command -v pactl >/dev/null &&
       ! pactl list short modules 2>/dev/null | grep -q module-native-protocol-tcp; then
    pactl load-module module-native-protocol-tcp auth-ip-acl=127.0.0.1 >/dev/null
  fi
else
  echo "warning: pulseaudio not installed; playback will not work" >&2
fi

# --- The build watcher: rebuilds on every source or controls.json change,
# which is what makes the app's live controls and auto-refresh do anything.
"$synthc" watch "$project" &
watch_pid=$!
trap 'kill "$watch_pid" 2>/dev/null || true' EXIT

echo "open the Termux:X11 app to see the UI (project: $project)"
DISPLAY=":$display" PULSE_SERVER=127.0.0.1 SDL_RENDER_DRIVER=software \
  "$synth_dev" $fullscreen --scale "$scale" "$project"
