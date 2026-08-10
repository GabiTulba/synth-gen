# SynthGraph

A functional, text-first language and build system for creating audio
samples and full songs through composable, pure mathematical
transformations on sound.

The full design is in [`docs/synthgraph-design-v2.pdf`](docs/synthgraph-design-v2.pdf).
In short: a sound — from a short sample up to a fully arranged song — is an
ordinary source file (`.synth`), a composition of pure functions that
generate, transform, slice, and arrange signals. A project is compiled by a
build system; the resulting audio artifacts are browsed and played in a
companion dev app. No editor is shipped — the product surface is a
compiler, a linter, and a build daemon.

## Building

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build          # run the test suite
```

Requires a C++20 compiler and CMake ≥ 3.20. The compiler, build system
and daemon have no third-party dependencies. The dev app (`synth-dev`)
additionally needs SDL2 (`scripts/install-deps.sh`); Dear ImGui is
vendored. Without SDL2 the dev app is skipped and everything else still
builds.

## Usage

```sh
# One-shot build of a project directory (contains a build.json manifest):
build/synthc build examples/pluck
# -> examples/_build/pluck/artifacts/demo.wav
# -> examples/_build/pluck/metadata.json

# Build daemon: watch sources, build.json and imported audio files, rebuild
# on change (save file -> rebuild -> dev app reflects new artifacts):
build/synthc watch examples/pluck

# Front-end checks only (for editor integration):
build/synthc lint path/to/file.synth

# Language server (JSON-RPC over stdio): diagnostics, completion,
# go-to-definition and hover for editors; see editor/vscode:
build/synthc lsp

# Stream the build log (-v works for watch too): phase timings, one line
# per artifact with its worker thread and discretize/write durations, and
# per-target dependency statistics (direct deps, dependents, closure):
build/synthc build examples/basic -v

# Dev app: browse and play a project's rendered artifacts; live-updates
# whenever a build rewrites the metadata (pair it with `synthc watch`):
build/synth-dev examples/pluck
```

### Editor support (VS Code)

`editor/vscode/` contains a VS Code extension for `.synth` files: syntax
highlighting, inline diagnostics on every keystroke, completion (names
in scope and `Module.` members), go-to-definition (including into the
bundled Core interface) and type-on-hover. Everything but the grammar is
served by `synthc lsp`, the language server built into the compiler, so
editor analysis and build analysis can never disagree. Setup
instructions are in [`editor/vscode/README.md`](editor/vscode/README.md).

### The `build.json` manifest

A single JSON object per directory. A standalone project:

```json
{ "project": "pluck-demo",
  "description": "optional free text (JSON has no comments)",
  "sources": ["pluck.synth", "other.synth"] }
```

A **library** — a reusable, importable unit. It lists no files: every
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
`import Basic.Keys`.

A **project root** — the orchestrator. `"build"` rules name the units to
build (directories with a `build.json`, or single `.synth` files);
libraries are discovered dynamically by scanning the tree under the
root, so dependency names resolve wherever the library lives:

```json
{ "project": "my-album",
  "build": ["lib/basic", "tunes", "sketches/idea.synth"] }
```

`synthc build`/`synthc watch` at the root builds/watches every rule; in
a subdirectory they build just that unit, resolving dependencies through
the enclosing root. In code,
`import Basic` + `Basic.Keys.strike`, `open Basic.Keys` + bare
`strike`, and `module K = Basic.Keys` + `K.strike` all reach what a
library's `lib.synth` publishes; `import Basic` + `Basic.strike440`
reaches the interface file's own definitions. A dependency's own render
targets are not re-rendered into the consumer's build.

### Build outputs

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

## Language at a glance

```ocaml
(* pluck.synth *)
open Core            (* submodule names: Osc, Fx, Arrange, List, ... *)
open Core.Osc open Core.Fx open Core.Arrange open Core.Render

let pluck freq:Scalar : Scalar Signal =
  (sine freq) * (exp_decay 6.0)
;;

let pluck_sample freq:Scalar : Scalar Sample =
  sample (pluck freq) 0s 800ms
;;

let place_pluck at:Timestamp : Scalar Signal =
  place (pluck_sample 440.0) at
;;

let song : Scalar Signal =
  mix_all (List.map place_pluck [0s; 500ms; 1s; 1500ms])
;;

let _ = render "demo" 48000.0 (sample song 0s 2s)
;;
```

All primitives live in **`Core`** — a real library bundled with the
compiler (`stdlib/core/lib.synth`) whose every definition is an
`external` binding to an engine implementation in `src/core/*.cpp`,
organized into functional submodules: `Osc`, `Fx`, `Arrange`, `Render`,
`Io`, `List`, `Time`, `Sig`, `Math`. Core is not ambient — bring it
into scope like any library: `import Core` (qualified
`Core.Osc.sine`), `open Core` (module-qualified `Osc.sine`,
`List.map`), or `open Core.Osc` (bare `sine`). It aliases like any
module (`module C = Core`, `module L = Core.List`). Code fragments
below assume the relevant submodules are open.

Fully annotated, no inference, no recursion in v1.
`render` is the language's only effect. Files are modules (`import A`
resolves `a.synth` in the same directory — inside a library, that is a
fellow member); libraries add `import Lib` / `import Lib.Mod`
(qualified `Lib.def` / `Lib.Mod.def` access), `open Lib` /
`open Lib.Mod` (unqualified access, position-ordered shadowing), and
module aliases (`module K = Basic.Keys`). See
`examples/song/preview.synth`. Files can also namespace definitions
with **inline modules** — `module A = struct … end`, nested at will,
referenced as `A.x` (from other files `File.A.x`) or via `open A`, with
`open`s inside a `struct` scoped to it.

Ergonomic features on top of the doc's core: **labeled arguments**,
**partial application**, **lambdas**, **polymorphic signatures**,
**inline modules**, **build-time Booleans with `if`/`else`**,
**external functions in C++**, the **pipe operator**, and **local
`let ... in` bindings**:

```ocaml
open Core
open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Time

let voice ~amp:Scalar ~freq:Scalar : Scalar Signal = (sine freq) * amp ;;
let quiet : Scalar -> Scalar Signal = voice ~amp:0.25 ;;   (* curried *)

let warm : Scalar Signal =
  saw 220.0 |> lowpass ~cutoff:800.0 |> soft_clip 0.8 ;;

let pattern : Scalar Signal =
  let hit : Scalar Sample = warm |> sample ~from:0s ~to:100ms in
  let beats : Timestamp list = time_steps ~start:0s ~step:200ms ~count:5.0 in
  mix_all (List.map (place hit) beats) ;;   (* or: place_multi hit beats *)

let echoes : Scalar Signal =
  let hit : Scalar Sample = warm |> sample ~from:0s ~to:100ms in
  mix_all (List.map (fun t:Timestamp -> place hit t) [0s; 250ms; 500ms]) ;;

let _ = sample pattern ~from:0s ~to:2s |> render ~name:"warm" ~rate:48000.0 ;;
```

Labeled arguments go in any order; providing any subset of a function's
arguments — labeled, a positional prefix, or a mix — curries it over the
remaining parameters. Primitive parameters are all labeled with their
signature names, so any primitive can be called by label or partially
applied, and any function-typed expression can be applied
(`(f 1.0) 2.0`) or passed along. Lambdas (`fun x:Scalar -> ...`)
annotate their parameters like every other binding, capture enclosing
locals, and must be parenthesized when used as an argument or pipe
right-hand side. `x |> f a` desugars to `f a x` (the piped value becomes
the final positional argument).

An annotation may name **type variables**, so one definition serves every
element type the way the primitives do — the checker instantiates it
afresh at each use:

```ocaml
let dampen ~input:'a Signal : 'a Signal =
  lowpass ~cutoff:600.0 (soft_clip ~threshold:0.8 input) ;;

let mono : Scalar Signal = dampen (saw 220.0) ;;
let wide : Vector Signal = dampen (channels [saw 220.0; saw 221.0]) ;;
```

The variable is still *written*, never inferred, and inside the body it
is rigid: `'a` is whatever the caller picked, so the body can pass it
along but cannot assume it is a Scalar or a Signal. Every variable in
the result must appear in a parameter, otherwise no call site could
determine it. Polymorphic definitions curry, take and return functions
(`let twice ~f:('a -> 'a) ~x:'a : 'a = f (f x)`), and are published by
libraries like any other value. Types are erased before evaluation, so a
polymorphic definition renders bit-identically to the monomorphic one it
replaces.

**Inline modules** namespace related definitions inside one file:

```ocaml
import Core
module Voices = struct
  open Core.Osc open Core.Fx   (* scoped: end at this module's `end` *)
  let base : Scalar = 220.0 ;;
  module Wet = struct
    let damp ~input:'a Signal : 'a Signal = lowpass ~cutoff:600.0 input ;;
  end
  let lead : Scalar Signal = Wet.damp (sine base) ;;
end ;;

let mono : Scalar Signal = Voices.Wet.damp (Core.Osc.sine Voices.base) ;;
```

Bodies nest arbitrarily and see the enclosing scope; members are
addressed by dotted path (from other files, `File.Voices.base`) or
brought into scope with `open Voices`. A member is just a top-level
definition under its dotted name — same typing, evaluation, and
incremental caching as everything else.

**Booleans and `if`/`else`** make configuration part of the language.
`Bool` is a build-time value — comparisons (`< <= > >= == !=`, on two
Scalars or two Timestamps), `&&`/`||` (short-circuit), and `not` decide
it while the graph is assembled, and `if` picks a value, a signal chain,
or even which target renders. Only the taken branch evaluates; signals
themselves are never compared or branched per sample.

```ocaml
let fast : Bool = tempo >= 120.0 && not (tempo > 200.0) ;;

let voice ~freq:Scalar ~crisp:Bool : Scalar Signal =
  if crisp then highpass ~cutoff:900.0 (saw freq)
  else lowpass ~cutoff:500.0 (sine freq) ;;

let _ =
  if fast then render "fast" 48000.0 (sample mix 0s 8s)
  else render "slow" 48000.0 (sample mix 0s 16s) ;;
```

**External functions** implement a definition in C++. Declare the
signature in synth, point at a `.cpp` file next to your source, and
synthc compiles it at build time (cached by content under
`_build/externals/`, watched by the daemon like an audio input):

```ocaml
open Core.Osc
let succ a:Scalar : Scalar = external "succ.cpp" ;;
let tone : Scalar Signal = sine (succ 439.0) ;;
```

```cpp
// succ.cpp
#include <synth/external.hpp>

SYNTH_EXTERNAL(succ) {
  *result = synth::ext::Value::scalar(args[0].asScalar() + 1.0);
  return true;
}
```

Only build-time data crosses the boundary (Scalar, Timestamp, Bool,
String, Vector, lists, tuples — never signals, which stay lazy engine
graphs). This is also how Core itself is built: `stdlib/core/lib.synth`
declares every primitive as an `external` bound to the engine
implementations in `src/core/`, so `open Core` genuinely imports a
library rather than triggering compiler magic.

Signals can also be built directly: `constant 0.5` holds a level
forever, `time` is the ramp whose sample at t seconds is t, and
`signal ~f:(fun t:Scalar -> exp (0.0 - 3.0 * t))` samples a function of
time (`constant_multi` / `signal_multi` are the per-channel forms). The
math primitives `exp`, `sqrt`, `log`, and `pow ~x ~y` work on plain
Scalars and elementwise on Signals — `pow (sine 220.0) 3.0` is a
waveshaper, `sqrt time` a fade-in curve.

## Repository layout

| Path | Contents |
|------|----------|
| `src/lexer.*`, `src/parser.*`, `src/ast.hpp` | Language front-end: tokens (incl. timestamp unit-suffix literals), OCaml-like parser, AST with source spans |
| `src/types.*`, `src/checker.*` | Type system (rigid vs. free type variables, unification), fully-annotated checker with use-site instantiation of stored signatures, module resolution (files, libraries, inline `struct ... end` modules, the bundled Core) |
| `stdlib/core/lib.synth` | The Core library: every primitive's name and signature, declared in synth source as `external` bindings |
| `src/core/` | The built-in external implementations those bindings dispatch to (oscillators, effects, sampling, render, io, lists, signals, math) |
| `src/external.*` | User externals: the generated `<synth/external.hpp>` API, build-time C++ compilation, content-hash caching, dlopen binding |
| `src/signal.*` | Signal engine: lazy signal DAG, render-time discretization, sample/place windowing, filters, mixing |
| `src/eval.*` | Evaluator: reduces definitions to values, collects render targets, `load_*` build-time validation |
| `src/wav.*` | WAV read (PCM 16/24/32, float 32/64) and write (PCM 16) |
| `src/build.*` | `build.json` manifest (projects, libraries, roots), project validation, target enumeration, cached + parallel rendering, artifact + metadata emission, lint mode, watch loops (project + root daemon) |
| `src/library.*` | Library registry: dynamic discovery of `library` manifests under a root, directory-scanned member sets, `lib.synth` interface detection, dep validation, enclosing-root search |
| `src/incremental.*` | Dependency tracking: Merkle content hashes over definition closures for the build cache |
| `src/lsp.*` | `synthc lsp`: an LSP server over the front-end (diagnostics, completion, go-to-definition, hover), with unsaved-buffer overlays |
| `src/main.cpp` | `synthc` CLI (`build`, `watch`, `lint`, `lsp`) |
| `editor/vscode/` | VS Code extension: TextMate grammar for `.synth` plus a thin client that launches `synthc lsp` |
| `src/devapp/` | `synth-dev`: JSON/metadata reader, SDL audio player, ImGui shell with live refresh and `--self-test` |
| `tests/` | Unit + end-to-end tests (assert-based, run via CTest) |
| `examples/pluck/` | The design doc's §3.4 example as a buildable project |
| `examples/primitives/` | One short, audible render target per library primitive |
| `examples/lib/voices/` | The `Voices` **library**: parametric drum cores, the additive keys stack, detuned-saw pad tones, and sequencing helpers shared by every kit below |
| `examples/lib/effects/` | The `Effects` **library**: the Doppler fly-by family (control shape, swept stack, proximity, pan), parametrized by pass rate |
| `examples/basic/` | Basic instrument samples packaged as the `Basic` **library**: snare, kick, guitar pluck, piano note — presets over the `Voices` cores |
| `examples/song/` | A full 16-bar stereo song across five modules — drums, synth pad, piano, guitar, and the arrangement that imports them (`outputs/song/song.wav`) — plus `preview.synth`, the library-system demo (`"dependencies"`, `import`/`open`/`module`) |
| `examples/build.json` | The examples **root**: one `build` rule per example; `synthc build examples` builds them all |
| `examples/advanced/` | Advanced effect demos: the rapid stereo Doppler fly-by built on `Effects.Doppler` (`outputs/advanced/`) |
| `examples/darksynth/` | A ~91 s darksynth track with two drops across six modules — drums, electric bass, dark pads, distorted guitar, riser/impact FX, and the arrangement (`outputs/darksynth/`) |
| `outputs/` | Committed renders of the showcase projects (`outputs/primitives/`, `outputs/basic/`) — `.wav` to listen to and `render_vis` waveform `.svg`s to look at, without building anything; refresh with `scripts/render-outputs.sh` |

## Implementation status (design doc §12)

- [x] **Epic 0** — Repo structure, build system, C++ toolchain, and the
  language specification document
  ([docs/language-spec.md](docs/language-spec.md)).
- [x] **Epic 1** — Lexer, parser, AST, source spans, module resolution,
  parse diagnostics.
- [x] **Epic 2** — Type checker: primitive/parameterized types, annotated
  checking, use-site instantiation of primitive and user signatures
  (`'a` in a definition's annotation), higher-order arguments, operator
  typing with broadcasting, typed diagnostics.
- [x] **Epic 3** — Evaluator & signal engine: signal representation,
  pure-expression evaluation, `sample`/`place` semantics, render-time
  discretization, `.wav` writing.
- [x] **Epic 4** — Primitive library: `sine`/`saw`/`square`, ADSR +
  exponential decay, `lowpass`/`highpass`, operators + broadcasting,
  `channels`/`mix_all`/`map`/`fold`, `load_mono`/`load_multi` with
  build-time channel validation. Beyond the v1 roster: `fm`/`pm`/`am`
  modulation primitives, the feedforward `delay` the doc left under
  consideration, a Schroeder `reverb`, `resample` for time warping, a
  deterministic FM-based `noise` generator, and `hard_clip`/`soft_clip`
  distortion.
- [x] **Epic 5** — One-shot build system: manifest, project validation,
  target enumeration, metadata emission, `synthc build` + `synthc lint`.
- [x] **Epic 6** — Build daemon: `synthc watch` rebuilds on changes to
  sources, the manifest, and imported audio files (polling-based,
  whole-project rebuild — the acceptable v1 per §12). Partial-failure
  error surfacing goes through the same metadata file as one-shot builds.
- [x] **Epic 7** — Dev app: `synth-dev`, an SDL2 + Dear ImGui artifact
  browser/player. Reads build metadata (pure consumer — no compiler
  internals), lists targets with duration/rate/channels/status, plays
  artifacts through SDL audio, shows build diagnostics, and live-refreshes
  by watching the metadata file (the doc's v1 change-notification choice).
- [x] **Epic 8** — Automatic caching & incremental builds. Each render
  target is keyed by a Merkle-style content hash of its declaring
  definition's dependency closure (across modules), salted with the
  stamps of all audio inputs and an engine-version constant. The daemon
  keeps the cache across rebuilds: an edit re-renders only the targets
  whose closure actually changed. Fully automatic, no user-facing
  controls (per the doc); the cache is bounded by construction (one
  in-memory entry per live target, artifacts live on disk) and entries
  for removed targets are pruned each build.
- [x] **Epic 9** — Parallel evaluation: cache-miss targets render
  concurrently across a hardware-sized thread pool (signal graphs are
  immutable; all per-render state is per-context, so shared subgraphs are
  safe). Verified byte-identical output against fresh rebuilds.
- [x] **Epic 9.2** — Batched + intra-target parallel rendering: nodes
  compute 1024-frame blocks instead of single frames (per-node dispatch
  and memoization cost once per block, tight inner loops, and exact
  silence short-circuiting so placed samples cost nothing outside their
  windows). A planner decomposes a render's top into its combination
  spine (mixes, arithmetic, channel assembly, wrappers) plus the heavy
  subtrees hanging off it, proves the subtrees state-disjoint (grouping
  any that share nodes), and renders the groups on worker threads with a
  per-block barrier while the main thread replays the spine over the
  workers' blocks — so summation order, and therefore the output, is
  bit-identical to a sequential render. A fresh full darksynth build
  dropped from ~250 s to ~4 s wall; all showcase artifacts verified
  byte-identical to the per-frame engine's.
- [x] **Shared bus rendering across targets** — when one target's signal
  occurs as a shared subtree of another target's graph (a master summing
  the buses the stems render, an overview stacking a master lane above
  its bus lanes), the build schedules the containing target after its
  providers and serves their finished buffers to it block-by-block
  (`PreRenderedMap`), instead of re-discretizing the subtree. Deps only
  form between renders with identical windows and rates, never reach
  under placements (which remap time in private contexts), and renders
  are deterministic — so artifacts stay byte-identical. The darksynth
  master's discretization drops roughly in half again, and the stems
  overview's master lane becomes a cheap sum of its already-rendered
  lanes.
- [x] **Fused arithmetic** — `makeBinOp` no longer builds a node per
  operator: elementwise `+ - * /` chains merge into a single fused node
  holding the non-arithmetic subtrees as inputs and a small postfix
  program over them (constants become immediates, duplicate inputs
  dedup, inlining capped so shared DAGs cannot blow up). The program
  executes block-at-a-time — each operator is one tight auto-vectorized
  loop over cache-resident value-stack slabs — replaying the exact
  operator-tree order, with a per-block silence lattice reproducing each
  operator's short-circuit rule, so output is bit-identical. Benchmarked
  at ~13% off a fresh darksynth build (3.1 s → 2.7 s wall). Two smaller
  measured-and-kept follow-ups: the planner skips worker threads when
  every decomposed leaf is served from a pre-rendered window (barrier
  overhead only), and renders record per-block silence flags in
  `Rendered` so serving a shared buffer's silent stretches is a flag
  test instead of a worst-case zero scan (~2.7 s → ~2.6 s). A
  per-element interpreter variant of fusion was tried first, measured
  as a regression (it defeated SIMD auto-vectorization), and replaced.
- [x] **Sample cache across placements** — a Sample is a value, so every
  placement of it is guaranteed to replay identical content; the engine
  now exploits that instead of merely honoring it. Each render owns a
  thread-safe cache keyed by (source node, window): the first placement
  discretizes the whole window once, and every placement — at any
  timestamp, block-aligned or not, on any planner thread — serves
  slices of that buffer. The cached window records its nonzero span, so
  envelope-gated tails and pre-attack padding are silent for free. A
  placement whose entry is mid-build on another thread falls back to
  the classic private-context replay (identical values either way), and
  nested samples inside a cached window share the same cache. Measured:
  fresh darksynth build ~2.6 s → ~1.8 s wall (drums stem 1174 → 738 ms,
  master 1490 → 1088 ms); artifacts byte-identical.
- [x] **Cross-build sample cache in the daemon** — every `SigNode` now
  carries a structural content hash (set at construction, O(1) from its
  children's hashes; audio files hash their data), and the sample cache
  is keyed by (content hash, window, rate) instead of node pointers, so
  cached windows outlive the graph that created them. `buildProject`
  shares one cache across all targets of a build; the watch daemon keeps
  it across rebuilds, so an arrangement edit that dirties every target
  re-renders only the samples whose own definitions changed — usually
  none. A generation sweep keeps only what the previous build actually
  placed, and the verbose log reports it (`samples: 92 window(s) cached
  (36091 KiB), 0 rendered this build, 76 reused from previous builds`).
  Measured in watch mode on darksynth: a mix-gain edit that invalidates
  all seven targets rebuilds in ~1.4 s vs ~1.8 s cold, with zero sample
  windows re-rendered; output is bit-identical across generations
  (tested) and all artifacts verified byte-identical.

## Decisions taken on points the design doc leaves open

These are the "low-confidence" items from the doc, resolved for v1 as
follows (all easy to revisit):

- **`adsr` signature**: `adsr attack:Timestamp decay:Timestamp
  sustain:Scalar release:Timestamp hold:Timestamp : Scalar Signal`.
  Durations are Timestamps, the sustain level is a Scalar, and `hold` is
  the gate length: the envelope sustains until `hold`, then releases.
- **Higher-order arguments** may be any function-typed expression: a
  named user function or primitive, a partial application, or a lambda
  (`List.map (fun t:Timestamp -> place hit t) beats`). Lambdas capture
  enclosing locals by value.
- **Vector channel-count mismatches** are a build error, raised at graph
  construction time (before any audio is computed). Channel counts are
  static once audio files are read, so this never happens mid-render.
  v1 caps signals at 16 channels.
- **Audio file paths** in `load_mono`/`load_multi` resolve relative to the
  source file that mentions them.
- **`let _ = ...`** bindings must have type `unit` (they exist to declare
  render targets).
- **Module-level definitions must precede use** (consistent with "no
  recursion" — this also rules out mutually recursive definitions).
- **Filters** are one-pole 6 dB/oct designs evaluated statefully from the
  epoch; a placed sample's filters warm up from the source signal's own
  timeline, preserving "signals are defined from t = 0" semantics.
- **Modulation primitives** (an addition beyond the doc's v1 roster):
  - `fm carrier:Scalar modulator:Scalar Signal : Scalar Signal` — sine
    oscillator whose instantaneous frequency is `carrier + modulator(t)`
    Hz. The phase is integrated sample-by-sample from the epoch, so
    modulation depth is the modulator's amplitude (in Hz) and FM operators
    cascade (`fm 440.0 ((fm 110.0 ...) * 200.0)`).
  - `pm carrier:Scalar modulator:Scalar Signal : Scalar Signal` —
    `sin(2π·carrier·t + modulator(t))`; the modulator is in radians.
  - `am carrier:'a Signal modulator:Scalar Signal depth:Scalar :
    'a Signal` — classic AM, `carrier · (1 + depth·modulator)`; a mono
    modulator applies to every channel of a multi-channel carrier
    (ring modulation stays plain `carrier * modulator`).
  - Modulators must be mono signals; that is checked when the signal
    graph is built.
- **`delay by:Timestamp signal:'a Signal : 'a Signal`** — the feedforward
  delay the doc lists as under consideration (§6), adopted with exactly
  that signature: the input shifted `by` later in time, silence before it.
  Echoes are `mix_all [dry; (delay 250ms dry) * 0.5; ...]`. Implemented
  with a ring buffer so a subgraph shared between dry and delayed paths
  keeps its stateful nodes (filters, `fm`) consistent. Feedforward only —
  feedback/IIR delays remain future work per §13.
- **`resample input:'a Signal f:(Scalar -> Scalar) : 'a Signal`** — time
  warping: `f` is a playback-rate multiplier on the output's timeline, so
  `out(t) = input(∫₀ᵗ f)`. `1.0` is the identity, `0.5` is half speed (an
  octave down), `2.0` double. This is what expresses varispeed, tape
  flutter, and Doppler pitch shift, including on sources with no harmonic
  decomposition to `fm` (a `load_mono` file, say). The source runs in its
  own context from its own epoch, exactly like a placement, and the read
  head only moves forward: `0.0` freezes it, a negative rate clamps to
  `0.0` (reverse playback is out of scope, §13), and rates above 64 are
  rejected as runaways. Reads between source frames are linearly
  interpolated.
- **`reverb decay:Timestamp damping:Scalar mix:Scalar input:'a Signal :
  'a Signal`** — Schroeder reverb: four parallel feedback comb filters
  (damped, mutually detuned delays) into two series allpass diffusers, per
  channel. `decay` is the RT60-style tail length (comb feedback gains
  follow `g = 10^(-3d/decay)`), `damping` ∈ [0,1] rolls off highs in the
  tail, `mix` blends dry (0) to fully wet (1). The feedback loops live
  inside the primitive's per-render state — the *language-level* signal
  graph stays acyclic, exactly like the stateful one-pole filters.
  Parameters are validated at graph construction.
- **List builders** (`Core.List`) — `List.init n:Scalar f:(Scalar -> 'a) : 'a list`
  builds `[f 0.0; f 1.0; ...; f (n-1)]` (additive stacks, generated
  patterns); `List.repeat n:Scalar x:'a : 'a list` builds n copies (a
  convenience for `List.init n (fun i:Scalar -> x)`); `time_steps
  start:Timestamp step:Timestamp count:Scalar : Timestamp list` builds
  arithmetic timestamp sequences — the natural feed for `place_multi`
  (`place_multi kick (time_steps ~start:0s ~step:500ms ~count:8.0)`);
  `jitter ~seed ~spread` humanizes such a list with hash-derived (pure,
  reproducible) per-note timing deltas.
  There is no Integer type in v1: counts and indices are Scalars,
  validated at build time to be whole and non-negative.
- **`to_sec`/`to_ms`/`to_min` `x:Scalar : Timestamp`** — the computed
  counterpart of the literal unit suffixes, which can only follow a
  literal. `to_ms 250.0` is `250ms`; `to_min (1.0 / bpm)` is one beat at
  `bpm`. This is what lets a duration come out of a tempo, a loop index,
  or a parameter instead of being typed in. There is no conversion back
  to Scalar on purpose — a Timestamp that decays into a bare number is
  how unit confusion gets in.
- **`place_multi sample:'a Sample ats:Timestamp list : 'a Signal`** —
  places one sample at every timestamp in the list and mixes the
  placements (overlaps sum): `place_multi kick [0s; 500ms; 1s]`. The
  summing is unnormalized, so overlapping or dense patterns can exceed
  full scale — rendering works in doubles and only hard-clamps to
  [-1, 1] at WAV write, so manage headroom deliberately by scaling down
  or with `soft_clip`/`hard_clip`. This is
  the pattern/rhythm workhorse: equivalent to
  `mix_all (List.map (place s) ats)` (byte-identical artifacts), kept as a
  primitive for ergonomics. Each placement replays the sample from its own state, so
  stateful content (filters, `fm`, `reverb`) sounds identical at every
  timestamp — a guarantee `place` itself now also provides when the same
  sample value is placed repeatedly.
- **`hard_clip threshold:Scalar input:'a Signal : 'a Signal`** and
  **`soft_clip threshold:Scalar input:'a Signal : 'a Signal`** —
  distortion by amplitude capping. `hard_clip` clamps flat at
  ±threshold (harsh, buzzy — flat-tops the wave). `soft_clip` is smooth
  saturation `threshold·tanh(x/threshold)`: near-identity for signals
  well under the threshold, compressing progressively toward the cap
  (warm, tube-like), never quite reaching it. Both are pointwise,
  polymorphic in the element type, and reject non-positive thresholds at
  graph construction. Drive is the ordinary idiom: `soft_clip 0.5 (x * 3.0)`.
- **`render_stems name:String rate:Scalar stems:(String, 'a Sample) list
  : unit`** — stem export (the doc's §13 wish): each `(label, sample)`
  pair declares an ordinary audio target named `<name>-<label>`, so stems
  get the same parallel rendering, incremental caching, metadata and
  dev-app treatment as any target. The song example exports
  `song-drums` / `song-pad` / `song-piano` / `song-guitar`, verified to
  sum (through the soft-clip master) back to the mix.
- **`render_vis name:String rate:Scalar sample:'a Sample : unit`** — a
  second effect alongside `render`: declares a build target whose artifact
  is a waveform *image* (`<name>.svg` beside the audio artifacts) of the discretized
  window instead of audio. One lane per channel, min/max-per-column
  drawing, dependency-free SVG (viewable straight from a git host).
  Visual and audio targets share the project-wide name space, appear in
  build metadata with a `kind` field, and get the same incremental
  caching; the dev app lists them (playback stays audio-only).
- **`noise freq:Scalar : Scalar Signal`** — pseudo-noise built from
  two-step cascaded FM: an `fm` operator driven hard by a sine, itself
  modulating a second `fm` operator, with golden-ratio frequency
  relationships (so the stages never phase-lock into a periodic tone) and
  modulation indices far above 1 (smearing the sidebands into a dense,
  chaotic spectrum). `freq` sets the spectral center. There is
  deliberately no RNG: "purity by construction" requires renders to be
  deterministic and cacheable, and this noise is bit-identical on every
  build while measuring as broadband and aperiodic (near-zero
  autocorrelation at any lag beyond a fraction of a millisecond).
