#!/usr/bin/env bash
# Scaffold a new SynthGraph unit: a standalone project, a library, or a
# workspace root with a first project inside it. Every kind it writes is
# immediately buildable with `synthc build <dir>`.
#
# Usage: scripts/new-project.sh [options] <dir>
#
#   --kind project|library|workspace   what to create (default: project)
#   --name NAME                        manifest name (default: from <dir>)
#   --deps A,B                         library dependencies, comma separated
#   --description TEXT                 manifest "description"
#   --force                            write into a non-empty directory
#
# Names default to the directory's basename: a project keeps it as a slug
# ("drum-kit"), a library capitalizes it into an identifier ("DrumKit"),
# because the compiler requires capitalized library names.
set -euo pipefail

kind=project
name=
deps=
description=
force=0
dir=

die() { printf 'new-project: %s\n' "$1" >&2; exit 1; }

# The header comment block above is the help text.
usage() {
  awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --kind) [ $# -ge 2 ] || die "--kind needs a value"; kind=$2; shift 2 ;;
    --name) [ $# -ge 2 ] || die "--name needs a value"; name=$2; shift 2 ;;
    --deps) [ $# -ge 2 ] || die "--deps needs a value"; deps=$2; shift 2 ;;
    --description)
      [ $# -ge 2 ] || die "--description needs a value"
      description=$2; shift 2 ;;
    --force) force=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --*) die "unknown option '$1' (try --help)" ;;
    *)
      [ -z "$dir" ] || die "expected one directory, got '$dir' and '$1'"
      dir=$1; shift ;;
  esac
done

[ -n "$dir" ] || { usage >&2; exit 2; }
case "$kind" in
  project|library|workspace) ;;
  *) die "--kind must be project, library, or workspace (got '$kind')" ;;
esac

base=$(basename -- "$dir")
[ -n "$base" ] && [ "$base" != "/" ] && [ "$base" != "." ] && [ "$base" != ".." ] \
  || die "'$dir' has no usable name; pass --name"

# A source-file stem the compiler accepts: lowercase, and only characters
# that survive into a module name (capitalized stem, so no hyphens).
slug=$(printf '%s' "$base" | tr '[:upper:]' '[:lower:]' | tr -cs 'a-z0-9_' '-')
slug=${slug#-}; slug=${slug%-}
[ -n "$slug" ] || die "cannot derive a name from '$base'; pass --name"
stem=$(printf '%s' "$slug" | tr '-' '_')
case "$stem" in [0-9]*) stem=s$stem ;; esac

# Library names and module names are capitalized identifiers: fold the
# separators away and upper-case each part ("drum-kit" -> "DrumKit").
camel=$(printf '%s' "$slug" | awk -F'[-_]' '{
  out = ""
  for (i = 1; i <= NF; i++)
    if ($i != "") out = out toupper(substr($i, 1, 1)) substr($i, 2)
  print out
}')
case "$camel" in [0-9]*) camel=S$camel ;; esac
module=$(printf '%s' "$stem" | awk '{ print toupper(substr($0,1,1)) substr($0,2) }')

if [ -z "$name" ]; then
  case "$kind" in
    library) name=$camel ;;
    *) name=$slug ;;
  esac
fi
if [ "$kind" = library ]; then
  case "$name" in
    [A-Z]*) ;;
    *) die "library names are capitalized identifiers ('$name' is not)" ;;
  esac
  case "$name" in
    *[!A-Za-z0-9_]*) die "library name '$name' is not an identifier" ;;
  esac
fi

if [ -e "$dir" ]; then
  [ -d "$dir" ] || die "'$dir' exists and is not a directory"
  if [ "$force" -ne 1 ] && [ -n "$(ls -A -- "$dir" 2>/dev/null)" ]; then
    die "'$dir' is not empty (pass --force to write into it anyway)"
  fi
fi

# "dependencies": ["A", "B"] from a comma-separated list.
dep_json=
if [ -n "$deps" ]; then
  IFS=','
  for d in $deps; do
    d=$(printf '%s' "$d" | tr -d '[:space:]')
    [ -n "$d" ] || continue
    case "$d" in
      [A-Z]*) ;;
      *) die "dependency names are capitalized ('$d' is not)" ;;
    esac
    dep_json="${dep_json:+$dep_json, }\"$d\""
  done
  unset IFS
fi

json_escape() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }

created=()
write() { # write <path> <<'EOF' ... — refuses to clobber
  if [ -e "$1" ] && [ "$force" -ne 1 ]; then
    die "'$1' already exists (pass --force to overwrite)"
  fi
  mkdir -p -- "$(dirname -- "$1")"
  cat > "$1"
  created+=("$1")
}

# Each starter opens exactly what it uses, so `synthc lint` is quiet on a
# fresh scaffold; open more of Core (Sig, Math, Pitch, Io, ...) as you go.
project_opens='open Core
open Core.Osc
open Core.Fx
open Core.Arrange
open Core.Render
open Core.Time'
lib_opens='open Core
open Core.Osc
open Core.Fx'

project_manifest() { # <name> <description> <source-stem>
  {
    printf '{\n  "project": "%s",\n' "$(json_escape "$1")"
    printf '  "description": "%s",\n' "$(json_escape "$2")"
    [ -n "$dep_json" ] && printf '  "dependencies": [%s],\n' "$dep_json"
    printf '  "sources": ["%s.synth"]\n}\n' "$3"
  }
}

project_source() { # <slug> <stem>
  cat <<EOF
(* $1 - starter project. One decaying 440 Hz tone, placed four times,
   rendered as "$2". Replace the body; keep the trailing \`render\`
   (a project builds the artifacts its top-level \`render\` calls name). *)

$project_opens

let tone : Scalar Signal =
  sine 440.0 *. exp_decay 6.0
;;

let song : Scalar Signal =
  let hit : Scalar Sample = tone |> sample ~from:0s ~to:800ms in
  place_multi hit (time_steps ~start:0s ~step:500ms ~count:4)
;;

let _ =
  song
  |> sample ~from:0s ~to:2s
  |> render ~name:"$2" ~rate:48000.0
;;
EOF
}

case "$kind" in
  project)
    desc=${description:-"A SynthGraph project."}
    write "$dir/build.json" <<<"$(project_manifest "$name" "$desc" "$stem")"
    write "$dir/$stem.synth" <<<"$(project_source "$name" "$slug")"
    ;;

  library)
    desc=${description:-"The $name library."}
    write "$dir/build.json" <<<"$(
      printf '{\n  "library": "%s",\n' "$(json_escape "$name")"
      printf '  "description": "%s"' "$(json_escape "$desc")"
      [ -n "$dep_json" ] && printf ',\n  "dependencies": [%s]' "$dep_json"
      printf '\n}'
    )"

    libstem=voice
    libmodule=Voice
    write "$dir/lib.synth" <<EOF
(* \`$name\` library interface: every module a consumer can reach is
   listed here. A library lists no "sources" - every .synth file in this
   directory is a member, and this file decides what is public. *)

module $libmodule = $libmodule ;;
EOF

    write "$dir/$libstem.synth" <<EOF
(* $libmodule - the $name library's first module. Definitions here are
   reachable as \`$name.$libmodule.<name>\` once a consumer declares
   "$name" in its manifest "dependencies" and writes \`import $name\`. *)

$lib_opens

let tone ~freq:Scalar ~decay:Scalar : Scalar Signal =
  sine freq *. exp_decay decay
;;
EOF
    ;;

  workspace)
    desc=${description:-"Workspace root: builds every rule listed below."}
    [ -z "$dep_json" ] || die "a workspace root declares no dependencies"
    write "$dir/build.json" <<<"$(
      printf '{\n  "project": "%s",\n' "$(json_escape "$name")"
      printf '  "description": "%s",\n' "$(json_escape "$desc")"
      printf '  "build": ["main"]\n}'
    )"

    write "$dir/main/build.json" <<<"$(
      project_manifest "$slug-main" "First project in the $name workspace." main)"
    write "$dir/main/main.synth" <<<"$(project_source "$name" main)"
    ;;
esac

printf 'created %s "%s" in %s\n' "$kind" "$name" "$dir"
for f in "${created[@]}"; do printf '  %s\n' "$f"; done

# A unit inside an existing workspace only builds once its root lists it.
root= ; probe=$(cd -- "$(dirname -- "$dir")" && pwd)
while [ -n "$probe" ] && [ "$probe" != "/" ]; do
  if [ -f "$probe/build.json" ] && grep -q '"build"' "$probe/build.json"; then
    root=$probe; break
  fi
  probe=$(dirname -- "$probe")
done

printf '\nnext:\n'
if [ -n "$root" ]; then
  rel=$(cd -- "$dir" && pwd); rel=${rel#"$root"/}
  printf '  add "%s" to the "build" list in %s/build.json\n' "$rel" "$root"
fi
printf '  synthc build %s\n' "$dir"
printf '  synthc watch %s   # rebuild on every save\n' "$dir"
