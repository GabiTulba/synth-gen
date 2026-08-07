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

# Stream the build log (-v works for watch too): phase timings, one line
# per artifact with its worker thread and discretize/write durations, and
# per-target dependency statistics (direct deps, dependents, closure):
build/synthc build examples/basic -v

# Dev app: browse and play a project's rendered artifacts; live-updates
# whenever a build rewrites the metadata (pair it with `synthc watch`):
build/synth-dev examples/pluck
```

### The `build.json` manifest

A single JSON object per directory. A standalone project:

```json
{ "project": "pluck-demo",
  "description": "optional free text (JSON has no comments)",
  "sources": ["pluck.synth", "other.synth"] }
```

A **library** — a reusable, importable unit. `"expose"` marks the public
files (internal `"sources"` files are importable only within the
library); `"dependencies"` names other libraries it uses:

```json
{ "library": "Basic",
  "expose": ["keys.synth"],
  "sources": ["internal.synth"],
  "dependencies": ["Fx"] }
```

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
`strike`, and `module K = Basic.Keys` + `K.strike` all reach a
library's exposed modules. A dependency's own render targets are not
re-rendered into the consumer's build.

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
open Core

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

All primitives live in the built-in **`Core`** module — files start
with `open Core` (bare `sine`, `render`, ...), or use qualified
`Core.sine`; the list functions form the `Core.List` submodule
(`List.map`, `List.fold`, `List.init`, `List.repeat`; `open Core.List`
makes them bare). `Core` aliases like any module (`module C = Core`).

Fully annotated, no inference, no Booleans/branching/recursion in v1.
`render` is the language's only effect. Files are modules (`import A`
resolves `a.synth` in the same directory, or a sibling listed by the
enclosing library); libraries add `import Lib` / `import Lib.File`
(qualified `Lib.File.def` access), `open Lib` / `open Lib.File`
(unqualified access, position-ordered shadowing), and module aliases
(`module K = Basic.Keys`). See `examples/song/preview.synth`.

Ergonomic features on top of the doc's core: **labeled arguments**,
**partial application**, **lambdas**, the **pipe operator**, and **local
`let ... in` bindings**:

```ocaml
open Core

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
| `src/types.*`, `src/primitives.*`, `src/checker.*` | Type system, primitive signatures, fully-annotated checker with polymorphic primitive instantiation, module resolution |
| `src/signal.*` | Signal engine: lazy signal DAG, render-time discretization, sample/place windowing, filters, mixing |
| `src/eval.*` | Evaluator: reduces definitions to values, collects render targets, `load_*` build-time validation |
| `src/wav.*` | WAV read (PCM 16/24/32, float 32/64) and write (PCM 16) |
| `src/build.*` | `build.json` manifest (projects, libraries, roots), project validation, target enumeration, cached + parallel rendering, artifact + metadata emission, lint mode, watch loops (project + root daemon) |
| `src/library.*` | Library registry: dynamic discovery of `library` manifests under a root, dep validation, enclosing-root search |
| `src/incremental.*` | Dependency tracking: Merkle content hashes over definition closures for the build cache |
| `src/main.cpp` | `synthc` CLI (`build`, `watch`, `lint`) |
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
  checking, primitive instantiation, higher-order arguments, operator
  typing with broadcasting, typed diagnostics.
- [x] **Epic 3** — Evaluator & signal engine: signal representation,
  pure-expression evaluation, `sample`/`place` semantics, render-time
  discretization, `.wav` writing.
- [x] **Epic 4** — Primitive library: `sine`/`saw`/`square`, ADSR +
  exponential decay, `lowpass`/`highpass`, operators + broadcasting,
  `channels`/`mix_all`/`map`/`fold`, `load_mono`/`load_multi` with
  build-time channel validation. Beyond the v1 roster: `fm`/`pm`/`am`
  modulation primitives, the feedforward `delay` the doc left under
  consideration, a Schroeder `reverb`, a deterministic FM-based
  `noise` generator, and `hard_clip`/`soft_clip` distortion.
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
