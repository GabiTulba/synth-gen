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

## Incremental builds & caching

Caching is fully automatic, with no user-facing controls.

- **Target keying.** Each render target is keyed by a Merkle-style
  content hash of its declaring definition's dependency closure (across
  modules), salted with the stamps of all audio inputs and an
  engine-version constant (`src/incremental.*`). An edit re-renders only
  the targets whose closure actually changed.
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
`build.json` manifest, imported audio files, and user-external `.cpp`
files, rebuilding on change (polling-based, whole-project rebuild in
v1). Partial-failure error surfacing goes through the same metadata file
as one-shot builds, so the dev app shows diagnostics live. The daemon
keeps both the target cache and the cross-build sample cache across
rebuilds; the verbose log reports reuse (`samples: 92 window(s) cached
(36091 KiB), 0 rendered this build, 76 reused from previous builds`).

Pair it with the dev app: `synthc watch` in one terminal, `synth-dev` in
another — a save rebuilds, and the app live-refreshes when the metadata
file is rewritten.
