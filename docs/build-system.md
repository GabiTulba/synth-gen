# Build system

`synthc` compiles `.synth` projects into audio (and waveform-image)
artifacts. This document covers the CLI, the `build.json` manifest and
its three unit kinds (project, library, root), where outputs land,
incremental caching, and the watch daemon.

Sources: `src/build.*` (manifests, validation, target enumeration,
rendering orchestration, watch loops), `src/library.*` (library
discovery and registry), `src/incremental.*` (dependency hashing),
`src/main.cpp` (CLI).

## The `synthc` CLI

```sh
# One-shot build of a project directory (contains a build.json manifest):
synthc build examples/pluck
# -> examples/_build/pluck/artifacts/demo.wav
# -> examples/_build/pluck/metadata.json

# Build daemon: watch sources, build.json and imported audio files, rebuild
# on change (save file -> rebuild -> dev app reflects new artifacts):
synthc watch examples/pluck

# Front-end checks only (for editor integration):
synthc lint path/to/file.synth

# Language server (JSON-RPC over stdio) for editors:
synthc lsp

# Stream the build log (-v works for watch too): phase timings, one line
# per artifact with its worker thread and discretize/write durations, and
# per-target dependency statistics (direct deps, dependents, closure):
synthc build examples/basic -v
```

`build` and `watch` default to the current directory when no project
directory is given.

## The `build.json` manifest

A single JSON object per directory. Three kinds of unit exist,
distinguished by their keys.

### Standalone project

```json
{ "project": "pluck-demo",
  "description": "optional free text (JSON has no comments)",
  "sources": ["pluck.synth", "other.synth"] }
```

`"sources"` lists the files to compile; an optional `"dependencies"`
array names libraries the project uses.

### Library

A **library** is a reusable, importable unit. It lists no files: every
`.synth` file in the directory is a member, and members import each
other by short name. `"dependencies"` names other libraries it uses:

```json
{ "library": "Basic",
  "dependencies": ["Fx"] }
```

What the library *publishes* is declared in code, in a `lib.synth`
interface file alongside the members. That file **is** the library —
module `Basic` — and only what it binds is visible from outside:

```
import Keys

module Keys = Keys ;;      (* publish a member module *)
module Lead = Pads ;;      (* ...possibly under a different name *)

let strike440 : Scalar Signal = Keys.strike 440.0 ;;   (* or a value *)
```

A member with no binding here (say `internal.synth`) stays internal:
its siblings can import it, consumers cannot. Members must not name
their own library — inside `Basic`, write `import Keys`, not
`import Basic.Keys`. Libraries may declare render targets of their own;
building the library renders them, and a dependency's targets are *not*
re-rendered into a consumer's build.

### Project root

A **root** is the orchestrator. `"build"` rules name the units to build
(directories with a `build.json`, or single `.synth` files); libraries
are discovered dynamically by scanning the tree under the root, so
dependency names resolve wherever the library lives:

```json
{ "project": "my-album",
  "build": ["lib/basic", "tunes", "sketches/idea.synth"] }
```

Discovery skips `_build/` output, legacy `build/` directories, and
hidden directories. Duplicate library names, unknown dependencies, and
library dependency cycles are build errors.

`synthc build`/`synthc watch` at the root builds/watches every rule; in
a subdirectory they build just that unit, resolving dependencies through
the enclosing root (found automatically by searching upward). In code,
`import Basic` + `Basic.Keys.strike`, `open Basic.Keys` + bare
`strike`, and `module K = Basic.Keys` + `K.strike` all reach what a
library's `lib.synth` publishes; `import Basic` + `Basic.strike440`
reaches the interface file's own definitions.

## Render targets

Every `render` (or `render_vis` / `render_stems` / `render_vis_stems`)
call evaluated at build time declares one target — a name, a rate, and a
sample window. Names must be unique per build unit. `render_stems`
declares one ordinary audio target per `(label, sample)` pair, named
`<name>-<label>`, so stems get the same parallel rendering, incremental
caching, metadata and dev-app treatment as any other target. Visual
(`render_vis`) and audio targets share the project-wide name space and
appear in build metadata with a `kind` field.

## Build outputs

All outputs land in a single `_build/` tree at the project root,
mirroring the source layout: rule `song` writes
`<root>/_build/song/artifacts/<name>.wav` (16-bit PCM) and
`<root>/_build/song/metadata.json` — the machine-readable index the dev
app consumes, emitted for failed builds too, with diagnostics included.
A file rule `sketches/idea.synth` writes under `_build/sketches/idea/`.
A project built outside any root uses its own directory as the root
(`<project>/_build/...`); note that any ancestor directory holding a
root manifest determines where a subdirectory build's outputs land.
Stale per-project `build/` directories from older versions can be
removed with `git clean -Xdf examples`.

## Live controls

`Core.Control.slider` / `Core.Control.knob` declare named build-time
Scalar parameters with a range and a default; each declaration
evaluates to the control's current value. Declared controls appear in
the unit's metadata under `"controls"` (name, kind, range, default, and
the value this build used), and the build reads overrides from
`controls.json` next to the metadata:

```json
{ "overrides": { "cutoff": 900.0 } }
```

The other kinds are the same control with a different widget and a
different reading of that one number: `int_slider` (`"kind":
"int_slider"`) is a whole-step slider, `toggle` (and the tickbox
`Control.opt` puts in front of a value) is `0` or `1`, and `choice` is
the selected option's **index**, its entry carrying an extra
`"options"` array of labels for the dev app to write beside each
tickbox. Overrides for those kinds snap to a whole number after
clamping, so a hand-edited `2.7` selects option 3, not a value between
two options.

`Core.Control.multi_slider` declares a whole group at once: each lane
becomes a control named `"<group>.<lane>"` (`env.attack`), so the file
stays flat and per-lane —

```json
{ "overrides": { "cutoff": 900.0, "env.attack": 0.1, "env.decay": 0.2 } }
```

— and the lane's metadata entry carries `group`, `group_index`,
`sum_min` and `sum_max` beside the usual fields, which is what lets the
dev app draw the lanes linked.

A panel's `"controls"` are members, not bare names:
`{"name": "decay curve rate", "depth": 1}`. Depth 0 is a control the
panel names directly; a deeper member is part of a component
(`Nested_controller`) and is drawn indented under the member above it.

Override values clamp to the declared range; a group is then projected
back inside its sum bounds, so a hand-edited file cannot hand the
program an infeasible group. Unknown names and
malformed files are ignored (defaults apply). When a build declares any
control, its `controls.json` is recorded as a build input — so a
running watch daemon rebuilds whenever the dev app (or anything else)
writes it. That file is dev-tool state, not part of the project: it
lives in `_build/` and never fails a build.

`controls.json` is a build input, but it is not where control values
*live*. The dev app records them — along with its window layout — in
`project.json` next to `build.json`, and treats that file as
authoritative: on startup it writes `controls.json` to match, and
thereafter keeps the two in step. So `_build/` stays disposable (a clean
loses nothing you set), while the build keeps reading one small file it
already knows how to hash as an input. Both writes are
temp-then-rename, so a daemon polling mid-write never sees a torn file.

`project.json` records only values that differ from their declared
default, exactly as `controls.json` does — defaults live in the `.synth`
source, and this file records departures from them. It is grouped by
unit (`"."` for a standalone project, otherwise the root's rule path):

```json
{ "version": 1, "controls": { ".": { "cutoff": 2500, "gain": 0.31 } },
  "ui": { "window": { "x": 40, "y": 50, "w": 1024, "h": 700 } } }
```

The build itself neither reads `project.json` nor knows it exists; only
the dev app does. Note the name is deliberately not `metadata.json` —
that name belongs to the build's own output under `_build/`.

## Panels

`Core.Ui.panel` groups part of a project for the dev app, pairing some
of its controls with some of its render targets. A panel is the dev
app's whole unit of UI — one window holding those controls and the
waveforms of those targets together — so this is how you decide what is
shown beside what:

```
let _ = Ui.panel ~name:"Drums" ~controls:["drums.gain"] ~targets:["song-drums"] ;;
```

Anything no panel names collects into one further panel, so a project
that declares none still shows all of itself and nothing is ever
unreachable. Declared panels reach the unit's metadata under `"panels"`,
members recorded as written — a control member naming a whole `multi_slider`
group stays the group name, and the dev app expands it when it draws:

```json
"panels": [
  {"name": "Drums", "controls": ["drums.gain"], "targets": ["song-drums"]}
]
```

Every member name must resolve to a declared control, control group or
target; one that does not fails the build, pointing at the panel's own
line. The check runs after evaluation, so a panel may name something
declared later in the file.

Panels are presentation only. They never enter a target's cache key, so
renaming one re-renders nothing, and the key is emitted only when a
project declares a panel — metadata for a project that groups nothing is
byte-identical to what it was before panels existed. The build otherwise
does nothing with them; only the dev app reads them.

## Incremental builds & caching

Caching is fully automatic, with no user-facing controls.

- **Target keying.** Each render target is keyed by a Merkle-style
  content hash of its declaring definition's dependency closure (across
  modules), salted with the stamps of all audio inputs, the values of
  all live controls, and an engine-version constant
  (`src/incremental.*`). An edit re-renders only the targets whose
  closure actually changed; a moved slider invalidates conservatively
  (every target), with the sample-window cache still skipping the
  windows the control doesn't reach.
- **Bounded by construction.** The cache holds one in-memory entry per
  live target (artifacts live on disk); entries for removed targets are
  pruned each build.
- **Sample windows.** Independently of target caching, the engine caches
  discretized sample windows keyed by structural content hash — shared
  across all targets of a build, and across rebuilds in the daemon. An
  arrangement edit that dirties every target typically re-renders zero
  sample windows. See [`engine.md`](engine.md) for the mechanism.
- **User externals.** Compiled C++ externals are cached under
  `_build/externals/`, keyed by file content — edits recompile, rebuilds
  reuse.

Renders are deterministic, and the cache relies on it: output is
bit-identical across generations and verified against fresh rebuilds.

## Parallel rendering

Cache-miss targets render concurrently across a hardware-sized thread
pool. Signal graphs are immutable and all per-render state is
per-context, so shared subgraphs are safe; when one target's signal is a
shared subtree of another's (stems feeding a master bus), the build
schedules the containing target after its providers and serves their
finished buffers to it block-by-block instead of re-discretizing. The
engine-level mechanics (block rendering, planner decomposition, fusion)
are described in [`engine.md`](engine.md). All parallelism preserves
byte-identical output.

## The watch daemon

`synthc watch` runs the build in a loop, watching sources, the
`build.json` manifest, imported audio files, user-external `.cpp`
files, and — when the build declares live controls — the unit's
`controls.json`, rebuilding on change (polling-based, whole-project
rebuild in v1). Partial-failure error surfacing goes through the same metadata file
as one-shot builds, so the dev app shows diagnostics live. The daemon
keeps both the target cache and the cross-build sample cache across
rebuilds; the verbose log reports reuse (`samples: 92 window(s) cached
(36091 KiB), 0 rendered this build, 76 reused from previous builds`).

Pair it with the dev app: `synthc watch` in one terminal, `synth-dev` in
another — a save rebuilds, and the app live-refreshes when the metadata
file is rewritten.
