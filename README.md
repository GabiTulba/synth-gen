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
# One-shot build of a project directory (contains a .build manifest):
build/synthc build examples/pluck
# -> examples/pluck/build/artifacts/demo.wav
# -> examples/pluck/build/metadata.json

# Build daemon: watch sources, .build and imported audio files, rebuild
# on change (save file -> rebuild -> dev app reflects new artifacts):
build/synthc watch examples/pluck

# Front-end checks only (for editor integration):
build/synthc lint path/to/file.synth

# Dev app: browse and play a project's rendered artifacts; live-updates
# whenever a build rewrites the metadata (pair it with `synthc watch`):
build/synth-dev examples/pluck
```

### The `.build` manifest (v1)

```
# comment
project pluck-demo
source pluck.synth
source other.synth
```

### Build outputs

Artifacts are written to `<project>/build/artifacts/<name>.wav` (16-bit
PCM). Build metadata — the machine-readable index the dev app consumes — is
written to `<project>/build/metadata.json` and is emitted for failed builds
too, with diagnostics included.

## Language at a glance

```ocaml
(* pluck.synth *)
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
  mix_all (map place_pluck [0s; 500ms; 1s; 1500ms])
;;

let _ = render "demo" 48000.0 (sample song 0s 2s)
;;
```

Fully annotated, no inference, no Booleans/branching/recursion in v1.
`render` is the language's only effect. Files are modules (`import A`
resolves `a.synth` in the same directory).

Three ergonomic features on top of the doc's core: **labeled arguments**
with label-driven currying, the **pipe operator**, and **local
`let ... in` bindings**:

```ocaml
let voice ~amp:Scalar ~freq:Scalar : Scalar Signal = (sine freq) * amp ;;
let quiet : Scalar -> Scalar Signal = voice ~amp:0.25 ;;   (* curried *)

let warm : Scalar Signal =
  saw 220.0 |> lowpass ~cutoff:800.0 |> soft_clip 0.8 ;;

let pattern : Scalar Signal =
  let hit : Scalar Sample = warm |> sample ~from:0s ~to:100ms in
  let beats : Timestamp list = time_steps ~start:0s ~step:200ms ~count:5.0 in
  place_multi hit beats ;;

let _ = sample pattern ~from:0s ~to:2s |> render ~name:"warm" ~rate:48000.0 ;;
```

Labeled arguments go in any order; providing a subset curries the
function. Primitive parameters are all labeled with their signature
names, so any primitive can be called by label or partially applied.
`x |> f a` desugars to `f a x` (the piped value becomes the final
positional argument).

## Repository layout

| Path | Contents |
|------|----------|
| `src/lexer.*`, `src/parser.*`, `src/ast.hpp` | Language front-end: tokens (incl. timestamp unit-suffix literals), OCaml-like parser, AST with source spans |
| `src/types.*`, `src/primitives.*`, `src/checker.*` | Type system, primitive signatures, fully-annotated checker with polymorphic primitive instantiation, module resolution |
| `src/signal.*` | Signal engine: lazy signal DAG, render-time discretization, sample/place windowing, filters, mixing |
| `src/eval.*` | Evaluator: reduces definitions to values, collects render targets, `load_*` build-time validation |
| `src/wav.*` | WAV read (PCM 16/24/32, float 32/64) and write (PCM 16) |
| `src/build.*` | `.build` manifest, project validation, target enumeration, cached + parallel rendering, artifact + metadata emission, lint mode, watch loop |
| `src/incremental.*` | Dependency tracking: Merkle content hashes over definition closures for the build cache |
| `src/main.cpp` | `synthc` CLI (`build`, `watch`, `lint`) |
| `src/devapp/` | `synth-dev`: JSON/metadata reader, SDL audio player, ImGui shell with live refresh and `--self-test` |
| `tests/` | Unit + end-to-end tests (assert-based, run via CTest) |
| `examples/pluck/` | The design doc's §3.4 example as a buildable project |
| `examples/primitives/` | One short, audible render target per library primitive |
| `examples/basic/` | Basic instrument samples: snare, kick, guitar pluck, piano note |
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
  Sub-expression-level scheduling (9.2) within a single target remains
  open — worthwhile only for projects dominated by one large target.

## Decisions taken on points the design doc leaves open

These are the "low-confidence" items from the doc, resolved for v1 as
follows (all easy to revisit):

- **`adsr` signature**: `adsr attack:Timestamp decay:Timestamp
  sustain:Scalar release:Timestamp hold:Timestamp : Scalar Signal`.
  Durations are Timestamps, the sustain level is a Scalar, and `hold` is
  the gate length: the envelope sustains until `hold`, then releases.
- **Higher-order arguments are named functions only** — no lambdas in v1,
  as the doc leans. Any user function whose signature matches may be
  passed (e.g. to `map`/`fold`).
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
- **List builders** — `list_init n:Scalar f:(Scalar -> 'a) : 'a list`
  builds `[f 0.0; f 1.0; ...; f (n-1)]` (additive stacks, generated
  patterns); `repeat n:Scalar x:'a : 'a list` builds n copies (not
  expressible via `list_init` without lambdas); `time_steps
  start:Timestamp step:Timestamp count:Scalar : Timestamp list` builds
  arithmetic timestamp sequences — the natural feed for `place_multi`
  (`place_multi kick (time_steps ~start:0s ~step:500ms ~count:8.0)`).
  There is no Integer type in v1: counts and indices are Scalars,
  validated at build time to be whole and non-negative.
- **`place_multi sample:'a Sample ats:Timestamp list : 'a Signal`** —
  places one sample at every timestamp in the list and mixes the
  placements (overlaps sum): `place_multi kick [0s; 500ms; 1s]`. This is
  the pattern/rhythm workhorse `mix_all (map (place s) ats)` would be —
  which v1 cannot write directly, since partial application doesn't
  exist. Each placement replays the sample from its own state, so
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
- **`render_vis name:String rate:Scalar sample:'a Sample : unit`** — a
  second effect alongside `render`: declares a build target whose artifact
  is a waveform *image* (`build/artifacts/<name>.svg`) of the discretized
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
