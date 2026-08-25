# The Core library

All primitives live in **`Core`** — a real library bundled with the
compiler. Its interface is `stdlib/core/lib.synth`, which opens with
the ambient type declarations (`'a list`, the abstract `'a Signal`,
and the `'a Sample` record) and then declares the primitives. Nearly
every definition is an `external` binding to a C++ implementation
shipped beside it (`stdlib/core/*.cpp`), compiled at build time by the
same mechanism user externals use; the `List` and `Pitch` modules are
written in SynthGraph itself — plain recursive functions over the Cons/Nil
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
| `Core.Pitch` | Notes, temperaments and cents: `step`/`of_step`, `shift`, `flat`, `et`/`et12`/`just`/`pyth`, `hz`/`step_hz`/`a440`, `cents`/`detune`/`to_cents`/`ratio` | written in SynthGraph (`lib.synth`) |
| `Core.Tempo` | Meters, note values and the beat grid: `beat`/`bar`/`beats`, `value`, `at`, `grid`, `swing`, `common` | written in SynthGraph (`lib.synth`) |
| `Core.Scale` | Keys, degrees and chords: `offsets`, `degree`, `notes`, `snap`, `shape`, `tones`, `stack`/`triad`/`seventh`, `invert`, `voicing`, `freqs` | written in SynthGraph (`lib.synth`) |
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
- **`Pitch`: a note is data, a temperament is data.** The chromatic
  ladder is indexed from **C0 = 0**, so A4 is step 57 — deliberately not
  a MIDI key number. A `Tuning` carries `ratios` (one frequency ratio per
  step of the octave, counting from `root`), the `octave` the ladder
  repeats at, and a `ref_hz`/`ref_step` anchor. One formula serves every
  temperament:

  ```
  raw s = ratios[(s - root) mod n] * octave ^ floor((s - root) / n)
  hz  s = ref_hz * raw s / raw ref_step
  ```

  Dividing by `raw ref_step` is what anchors the ladder, and it makes the
  reference pitch **exact by construction** — `ref_hz` times a value over
  itself — in every temperament. `root` is the key centre, which is why
  `just` and `pyth` sound different in different keys; equal temperaments
  ignore it, their steps all being the same size. `octave` is what admits
  non-octave tunings: 13 ratios and `octave = 3.0` is Bohlen-Pierce. A
  tuning with no ratios has exactly one pitch, which keeps `step_hz`
  total.
- **`Pitch`: two ways to move a pitch, deliberately different types.**
  `shift ~by:Int` is discrete and temperament-relative — one step of
  whatever ladder the tuning defines. `detune ~cents:Scalar` is
  continuous and temperament-*in*dependent, and acts on a frequency
  rather than a note, because a `Note` has no fractional part. `cents`
  returns the bare multiplier `2^(n/1200)`, so it composes with the
  existing detune idiom (`saw (f * cents 7.0)`); `to_cents` inverts it,
  though not bit-exactly — it is a `log` undoing a `pow`.
- **`Pitch`: the tuning comes first.** `hz` and `step_hz` take `~t`
  ahead of the pitch so a temperament partially applies and disappears
  from call sites:
  `let p : Note -> Scalar = hz ~t:(just ~root:0 ~ref_hz:440.0)`. `a440`
  is the zero-ceremony path when 12-TET at A=440 is all you want.
  `Note` is inherently 12-tone, so `shift`/`flat`/`hz` suit 12-division
  temperaments; for `n /= 12` (19-EDO, Bohlen-Pierce) work in `Int`
  steps with `step_hz`, where shifting is plain `+`. The presets are
  functions rather than constants on purpose — a paramless definition is
  evaluated on every build of every project, and a tuning nobody asked
  for should not cost one.
- **`open Core.Pitch` binds twelve one- and two-letter constructors**
  (`C`, `Cs`, … `B`) as bare names. That is what makes
  `{ pc = A; oct = 4 }` read well, but in a file that is not
  pitch-heavy, `open Core` plus `Pitch.hz` and `Pitch.A` keeps the
  namespace legible.
- **`Tempo`: `bpm` counts the meter's `unit` note per minute.** That is
  unambiguous for simple meters and the usual convention for compound
  ones: 6/8 felt in two is `{ beats = 6; unit = 8 }` with `bpm` still
  counting eighths, and the dotted-quarter pulse is
  `value ~v:(Dotted Quarter)`. Everything else is derived from
  `beat = to_min (1 / bpm)` — `bar` is `beats` of them, `beats ~n` takes
  a `Scalar` so half-beats need no ceremony. `common ~bpm` is 4/4; any
  other meter is a plain record literal, and like `Pitch`'s temperaments
  it is a function rather than a constant so an unused tempo costs no
  build time.
- **`Tempo`: a whole note is `unit` beats, whatever the meter.** Four
  beats in 4/4, eight eighths in 6/8 — so `value` is the beat times
  `unit` times a dimensionless fraction of a whole note. `Value` is a
  recursive variant, which is where the type system earns its keep:
  `Dotted` multiplies by 1.5 and composes
  (`Dotted (Dotted Quarter)` is 2.25 quarters), `Tuplet (n, m, v)` is
  *n* of `v` in the time of *m* (`Tuplet (3, 2, Eighth)` is the
  eighth-note triplet) and nests, and a `match` that forgets a case is a
  build-time error rather than a silently wrong duration.
- **`Tempo`: bars and beats count from 0.** `at ~bar:0 ~beat:0.0` is the
  origin. Read it as an offset — "four bars and two beats in" — not as a
  ruler label, and it agrees with every other index in the language
  rather than with the convention that numbers the downbeat 1. `grid` is
  the tempo-aware `time_steps`: the call names the tempo and the note
  value instead of a precomputed step, so re-tempoing is a one-line edit.
  A `Tempo` carries one meter; a piece that changes meter binds a second
  one and offsets from a computed start.
- **`Tempo.swing` complements `jitter`.** `jitter` humanizes by hashing;
  `swing` displaces every odd-indexed entry later by `step * amount`
  (`0.0` straight, `1/3` triplet swing, `0.5` dotted). It takes `step`
  rather than reading it off consecutive gaps, so the last entry is not a
  special case and a non-uniform grid still behaves predictably. Both are
  pure, so both stay cacheable.
- **`Scale`: degrees count from 0, and wrap at the ladder's length.**
  Degree 0 is the tonic; degree 7 of a seven-note scale is the tonic an
  octave up, but degree *5* is the octave in a pentatonic one, because
  `degree` wraps at `List.length (offsets q)` rather than at a hardcoded
  seven. Negative degrees descend — the case a truncating divide gets
  wrong, which is why `Scale` carries its own `wrap_div`/`wrap_rem` (the
  language has no `%`). `notes ~from ~count` is the run of consecutive
  degrees; `snap` pulls an out-of-key note to the nearest one in it,
  taking the lower when a note sits exactly between two.
- **`Scale`: a scale has a `tonic`, a chord has a `root`.** Those are
  the musically correct words, and they are also what keeps the two
  record types apart — both carry a `quality`, and a record literal
  resolves by its field names, so identical field sets would be
  ambiguous. `Quality` is spelled long (`Major`) and `ChordQuality`
  short (`Maj`) so both can be `open`ed at once.
- **`Scale`: a diatonic stack is notes, a named chord is a `Chord`.**
  `triad`/`seventh`/`stack` take every other degree of the key and hand
  back a `Pitch.Note list`: the quality falls out of the ladder instead
  of being named, which is what lets harmonic minor produce its minMaj7
  without `ChordQuality` growing a case for it. `Chord` is for chords
  you name (`{ root = ...; quality = Min7 }`), and a progression is an
  ordinary `Chord list`. `invert`, `voicing` and `freqs` work on the
  note list from either source, so the two paths converge immediately.
- **`Scale`: `invert` rotates, `voicing` spreads.** `invert ~n:1` lifts
  the bottom note an octave (first inversion), `~n:-1` drops the top one
  instead, and `~n:k` on a `k`-note chord is the whole chord an octave
  up. `voicing ~low ~count` cycles upward through the chord for `count`
  parts, starting at the lowest octave that puts its first note at or
  above `low` — three chord tones become the four-part pad stack.
  `freqs ~t` is the one exit to Scalars and names its temperament, as
  `Pitch.hz` does.
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
