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

Requires a C++20 compiler and CMake ≥ 3.20. No third-party dependencies.

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

## Repository layout

| Path | Contents |
|------|----------|
| `src/lexer.*`, `src/parser.*`, `src/ast.hpp` | Language front-end: tokens (incl. timestamp unit-suffix literals), OCaml-like parser, AST with source spans |
| `src/types.*`, `src/primitives.*`, `src/checker.*` | Type system, primitive signatures, fully-annotated checker with polymorphic primitive instantiation, module resolution |
| `src/signal.*` | Signal engine: lazy signal DAG, render-time discretization, sample/place windowing, filters, mixing |
| `src/eval.*` | Evaluator: reduces definitions to values, collects render targets, `load_*` build-time validation |
| `src/wav.*` | WAV read (PCM 16/24/32, float 32/64) and write (PCM 16) |
| `src/build.*` | `.build` manifest, project validation, target enumeration, artifact + metadata emission, lint mode, watch loop |
| `src/main.cpp` | `synthc` CLI (`build`, `watch`, `lint`) |
| `tests/` | Unit + end-to-end tests (assert-based, run via CTest) |
| `examples/pluck/` | The design doc's §3.4 example as a buildable project |
| `examples/primitives/` | One short, audible render target per library primitive |
| `examples/basic/` | Basic instrument samples: snare, kick, guitar pluck, piano note |
| `outputs/` | Committed renders of the showcase projects (`outputs/primitives/`, `outputs/basic/`) — listen without building anything; refresh with `scripts/render-outputs.sh` |

## Implementation status (design doc §12)

- [x] **Epic 0** — Repo structure, build system, C++ toolchain. (0.2, the
  standalone grammar/type-rules spec document, still pending.)
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
  consideration, a Schroeder `reverb`, and a deterministic FM-based
  `noise` generator.
- [x] **Epic 5** — One-shot build system: manifest, project validation,
  target enumeration, metadata emission, `synthc build` + `synthc lint`.
- [x] **Epic 6** — Build daemon: `synthc watch` rebuilds on changes to
  sources, the manifest, and imported audio files (polling-based,
  whole-project rebuild — the acceptable v1 per §12). Partial-failure
  error surfacing goes through the same metadata file as one-shot builds.
- [ ] **Epic 7** — Dev app (SDL2 + Dear ImGui artifact browser/player).
- [ ] **Epic 8/9** — Incremental builds, caching, parallel evaluation
  (post-MVP fast-follows by design).

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
