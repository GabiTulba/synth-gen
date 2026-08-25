# The Core library

All primitives live in **`Core`** — a real library bundled with the
compiler. Its interface is `stdlib/core/lib.synth`, which opens with
the ambient type declarations (`'a list`, the abstract `'a Signal`,
and the `'a Sample` record) and then declares the primitives. Nearly
every definition is an `external` binding to a C++ implementation
shipped beside it (`stdlib/core/*.cpp`), compiled at build time by the
same mechanism user externals use; the `List` module is written in
SynthGraph itself — plain recursive functions over the Cons/Nil
variant. So `open Core` genuinely imports a library rather than
triggering compiler magic: primitive signatures live in synth source,
their bodies in library C++ (or synth), and the same `external`
mechanism is available to users (see below).

Core is always discoverable, always an allowed dependency (no manifest
entry needed), and **not ambient** — bring it into scope like any
library: `import Core` (qualified `Core.Osc.sine`), `open Core`
(module-qualified `Osc.sine`, `List.map`), or `open Core.Osc` (bare
`sine`). It aliases like any module (`module C = Core`,
`module L = Core.List`). The name `Core` is reserved.

The full signature roster is in the
[language specification §6](language-spec.md#6-primitive-signatures-v1-roster).
This document describes the submodule organization and the exact
semantics of the less obvious primitives.

## Submodules

| Submodule | Contents | Engine sources |
|-----------|----------|----------------|
| `Core.Osc` | Oscillators (`sine`, `saw`, `square`, `noise`) and modulation (`fm`, `pm`, `am`) | `stdlib/core/oscillators.cpp` |
| `Core.Fx` | Envelopes (`exp_decay`, `adsr`), filters (`lowpass`, `highpass`), distortion (`hard_clip`, `soft_clip`), time effects (`delay`, `resample`, `reverb`) | `stdlib/core/effects.cpp` |
| `Core.Arrange` | Combination and arrangement: `mix_all`, `channels`, `sample`, `place`, `place_multi` | `stdlib/core/sampling.cpp` |
| `Core.Render` | The effects: `render`, `render_vis`, `render_stems`, `render_vis_stems` | `stdlib/core/render.cpp` |
| `Core.Io` | Audio import: `load_mono`, `load_multi` | `stdlib/core/io.cpp` |
| `Core.List` | List combinators & builders: `map`, `fold`, `init`, `repeat`, `length`, `append`, `nth`, `rev`, `filter`, `concat`, `flat_map`, `zip`, `range`, `sum`, `maximum` | written in SynthGraph (`lib.synth`) |
| `Core.Time` | Timestamp construction & sequences: `to_sec`/`to_ms`/`to_min`, `time_steps`, `jitter` | `stdlib/core/lists.cpp` |
| `Core.Sig` | Signal constructors: `constant`, `constant_multi`, `time`, `signal`, `signal_multi` | `stdlib/core/signals.cpp` |
| `Core.Math` | `exp`, `sqrt`, `log`, `pow` — polymorphic over Scalars and (elementwise) Signals — plus the Int conversions `to_scalar`, `round`, `floor`, `ceil`, and `not` | `stdlib/core/math.cpp` |

## Primitive semantics

The precise behavior of the primitives whose signatures alone don't tell
the whole story:

- **`adsr attack decay sustain release hold`** — durations are
  Timestamps, the sustain level is a Scalar, and `hold` is the gate
  length: the envelope sustains until `hold`, then releases.
- **Filters** (`lowpass`, `highpass`) are one-pole 6 dB/oct designs
  evaluated statefully from the epoch; a placed sample's filters warm up
  from the source signal's own timeline, preserving "signals are defined
  from t = 0" semantics.
- **`fm carrier modulator`** — sine oscillator whose instantaneous
  frequency is `carrier + modulator(t)` Hz. The phase is integrated
  sample-by-sample from the epoch, so modulation depth is the
  modulator's amplitude (in Hz) and FM operators cascade
  (`fm 440.0 ((fm 110.0 ...) * 200.0)`).
- **`pm carrier modulator`** — `sin(2π·carrier·t + modulator(t))`; the
  modulator is in radians.
- **`am carrier modulator depth`** — classic AM,
  `carrier · (1 + depth·modulator)`; a mono modulator applies to every
  channel of a multi-channel carrier. Ring modulation stays plain
  `carrier * modulator`. Modulators must be mono signals; that is
  checked when the signal graph is built.
- **`delay by signal`** — feedforward delay: the input shifted `by`
  later in time, silence before it. Echoes are
  `mix_all [dry; (delay 250ms dry) * 0.5; ...]`. Implemented with a ring
  buffer so a subgraph shared between dry and delayed paths keeps its
  stateful nodes (filters, `fm`) consistent. Feedforward only —
  feedback/IIR delays are future work ([roadmap](roadmap.md)).
- **`resample input ~f`** — time warping: `f` is a playback-rate
  multiplier on the output's timeline, so `out(t) = input(∫₀ᵗ f)`.
  `1.0` is the identity, `0.5` is half speed (an octave down), `2.0`
  double. This is what expresses varispeed, tape flutter, and Doppler
  pitch shift, including on sources with no harmonic decomposition to
  `fm` (a `load_mono` file, say). The source runs in its own context
  from its own epoch, exactly like a placement, and the read head only
  moves forward: `0.0` freezes it, a negative rate clamps to `0.0`
  (reverse playback is future work), and rates above 64 are rejected as
  runaways. Reads between source frames are linearly interpolated, so
  warping is not lossless.
- **`reverb decay damping mix input`** — Schroeder reverb: four parallel
  feedback comb filters (damped, mutually detuned delays) into two
  series allpass diffusers, per channel. `decay` is the RT60-style tail
  length (comb feedback gains follow `g = 10^(-3d/decay)`), `damping`
  ∈ [0,1] rolls off highs in the tail, `mix` blends dry (0) to fully wet
  (1). The feedback loops live inside the primitive's per-render state —
  the *language-level* signal graph stays acyclic, exactly like the
  stateful one-pole filters. Parameters are validated at graph
  construction.
- **`noise freq`** — pseudo-noise built from two-step cascaded FM: an
  `fm` operator driven hard by a sine, itself modulating a second `fm`
  operator, with golden-ratio frequency relationships (so the stages
  never phase-lock into a periodic tone) and modulation indices far
  above 1 (smearing the sidebands into a dense, chaotic spectrum).
  `freq` sets the spectral center. There is deliberately no RNG: "purity
  by construction" requires renders to be deterministic and cacheable,
  and this noise is bit-identical on every build while measuring as
  broadband and aperiodic.
- **`hard_clip threshold input`** clamps flat at ±threshold (harsh,
  buzzy — flat-tops the wave); **`soft_clip threshold input`** is smooth
  saturation `threshold·tanh(x/threshold)`: near-identity for signals
  well under the threshold, compressing progressively toward the cap
  (warm, tube-like), never quite reaching it. Both are pointwise,
  polymorphic in the element type, and reject non-positive thresholds at
  graph construction. Drive is the ordinary idiom:
  `soft_clip 0.5 (x * 3.0)`.
- **`place_multi sample ats`** — places one sample at every timestamp
  and mixes the placements; equivalent to
  `mix_all (List.map (place s) ats)` (byte-identical artifacts), kept as
  a primitive for ergonomics. Summing is unnormalized — manage headroom
  deliberately. Each placement replays the sample from its own state, so
  stateful content (filters, `fm`, `reverb`) sounds identical at every
  timestamp.
- **`render_stems name rate stems`** — each `(label, sample)` pair
  declares an ordinary audio target named `<name>-<label>`, so stems get
  the same parallel rendering, incremental caching, metadata and dev-app
  treatment as any target. `render_vis_stems` renders *one* SVG artifact
  with a labeled waveform lane per stem on a shared time axis.
- **`render_vis name rate sample`** — declares a build target whose
  artifact is a waveform *image* (`<name>.svg`) of the discretized
  window instead of audio: one lane per channel, min/max-per-column
  drawing, dependency-free SVG (viewable straight from a git host).
- **`to_sec`/`to_ms`/`to_min`** — the computed counterpart of the
  literal unit suffixes (which can only follow a literal): `to_ms 250.0`
  is `250ms`, `to_min (1.0 / bpm)` is one beat at `bpm`. There is no
  conversion back to Scalar on purpose — a Timestamp that decays into a
  bare number is how unit confusion gets in. Once you have a Timestamp,
  the operators carry it the rest of the way: Timestamps add and
  subtract, and scale by a Scalar, so a tempo becomes a grid without
  ever leaving the unit — `let beat : Timestamp = to_min (1.0 / bpm)`,
  then `beat * 4.0` for the bar and `beat + beat / 2.0` for the dotted
  note. Results clamp at `0s` (language spec
  [§3](language-spec.md#operators-pointwise-lifting--scalar-broadcasting)).
- **`List` edge cases** — the combinators are total, so a score
  builder that legitimately produces nothing needs no special case at
  the call site. `nth` answers with its `default` when the index is out
  of range (there is no option type, and a partial primitive would push
  a bounds check onto every caller); `maximum` takes the value an empty
  list answers with, which doubles as a floor; `zip` stops at the
  shorter list, so pairing a melody against a rhythm never invents an
  element neither side had; `concat`/`flat_map`/`filter` on an empty
  list are empty. `range ~from ~count` is the Int counterpart of
  `time_steps`.
- **`jitter ~seed ~spread steps`** — humanizes a rhythm: each timestamp
  moves by a delta in `[-spread, +spread]` (clamped at `0s`) derived by
  hashing `(seed, index)`. Statistically random but pure — the same seed
  always yields the same feel, so builds stay reproducible; give each
  layer its own seed so they drift independently.
- **Audio file paths** in `load_mono`/`load_multi` resolve relative to
  the source file that mentions them; channel counts are validated at
  build time.
- **Counts and indices** (`List.init`, `List.repeat`, `time_steps`) are
  `Int`s: wholeness is guaranteed by the type system, and `List.init`
  hands its function an Int index (`fun i:Int -> ...`). A negative
  *computed* count yields the empty list from `List.init`/`List.repeat`
  and is a build error in `time_steps`. `Math.to_scalar` takes an
  Int into Scalar arithmetic exactly; `Math.round`/`floor`/`ceil` come
  back from a Scalar with an explicit fraction policy.

## External functions in C++

`let name params : Type = external "file.cpp" ;;` binds a definition to
a C++ implementation instead of a synth body. Externals are ordinary
values: they curry, take labels, and are published by libraries like
anything else. There is one mechanism (`src/external.*`), and Core uses
it too: the string names a C++ file resolved relative to the declaring
`.synth` file. At build time synthc compiles it once with `$CXX`
(default `c++`) into a shared object cached by content — edits
recompile, rebuilds reuse; user code's objects live under the project's
`_build/externals/`, the bundled stdlib's in a shared per-user cache —
then loads it with `dlopen` and binds the exported entry point. The C++
file is a build input: the watch daemon rebuilds when it changes. Every
Core definition is such an external over the `stdlib/core/*.cpp` files
shipped beside `lib.synth`.

An implementation includes the shipped `<synth/external.hpp>` and
defines one entry point per external, named after the definition:

```cpp
#include <synth/external.hpp>

SYNTH_EXTERNAL(succ) {
  *result = synth::ext::Value::scalar(args[0].asScalar() + 1.0);
  return true;
}
```

Arguments arrive fully applied, in declaration order; return `true` with
`*result` set, or report failure (fill `*error` and return `false`, or
throw) — failures become build diagnostics on the declaring definition.
One `.cpp` may implement several externals.

**Every value crosses the boundary.** Data — Scalar, Int, Timestamp,
Bool, String, Vector, unit, and lists/tuples of those — arrives as
transparent `synth::ext::Value`s. Signals and Samples arrive as lazy
engine graph handles that combine with the `<synth/engine.hpp>`
constructors, the same ones the engine itself uses (the symbols resolve
against the host process when the object is loaded). Functions (and
type-variable-typed values generally) arrive as opaque handles, callable
through the context's `apply` service or passable back unchanged. The
context also offers `loadAudio` (audio files as build inputs) and
`render` (declaring render targets) — `Io.load_mono` and
`Render.render` are ordinary externals built on exactly these. External
names must form C++ symbols (letters, digits, `_`).
