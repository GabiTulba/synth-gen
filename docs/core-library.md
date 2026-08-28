# The Core library

All primitives live in **`Core`** — a real library bundled with the
compiler. Its interface is `stdlib/core/lib.synth`, which opens with
the ambient type declarations (`'a list`, the abstract `'a Signal`,
and the `'a Sample` record) and then declares the primitives. Many
definitions are `external` bindings to C++ implementations shipped
beside it (`stdlib/core/*.cpp`), compiled at build time by the same
mechanism user externals use; the `List`, `Groove`, `Pitch`, `Tempo`,
`Scale`, `Score` and `Mix` modules (and a handful of sugar inside the
external modules) are written in SynthGraph itself. So `open Core`
genuinely imports a library rather than triggering compiler magic:
primitive signatures live in synth source, their bodies in library C++
(or synth), and the same `external` mechanism is available to users
(see the end of this document).

Core is always discoverable, always an allowed dependency (no manifest
entry needed), and **not ambient** — bring it into scope like any
library: `import Core` (qualified `Core.Osc.sine`), `open Core`
(module-qualified `Osc.sine`, `List.map`), or `open Core.Osc` (bare
`sine`). It aliases like any module (`module C = Core`,
`module L = Core.List`). The name `Core` is reserved.

The full signature roster is in the
[language specification §6](language-spec.md#6-primitive-signatures-v1-roster).
This document describes each submodule with its semantics, then the
idioms that tie them together.

## Submodules

| Submodule | Contents | Engine sources |
|-----------|----------|----------------|
| `Core.Math` | `exp` `sqrt` `log` `pow`, trig (`sin` `cos` `tan` `atan`), `abs`, `pi`, `min`/`max`/`clamp`/`lerp`, the pure hash (`hash`), Int conversions (`to_scalar`, `round`/`floor`/`ceil`), `not` | `stdlib/core/math.cpp` + SynthGraph sugar |
| `Core.List` | List combinators & builders: `map`, `mapi`, `fold`, `scan`, `init`, `repeat`, `length`, `append`, `nth`, `nth_opt`, `rev`, `filter`, `concat`, `flat_map`, `zip`, `take`, `drop`, `range`, `sum`, `maximum`, `iter` | written in SynthGraph (`lib.synth`); `iter` in `lists.cpp` |
| `Core.Option` | The vocabulary over `'a Option`: `some`, `is_some`/`is_none`, `value`/`fold`, the monadic core (`map`, `bind`, `join`, `map2`), `or_else`, `filter`, `to_list` | written in SynthGraph (`lib.synth`) |
| `Core.Osc` | Oscillators (`sine`, `saw`, `square`, their bandlimited `saw_bl`/`square_bl` variants, `noise`) and modulation (`fm`, `pm`, `am`) | `stdlib/core/oscillators.cpp` |
| `Core.Time` | Timestamp construction & sequences: `to_sec`/`to_ms`/`to_min` and their inverses `of_sec`/`of_ms`/`of_min`, the duration quotient (`div`/`rem`), `time_steps`, `jitter` | `stdlib/core/math.cpp`, `lists.cpp` |
| `Core.Arrange` | Combination and arrangement: `mix_all`, `channels`, `channel`, `sample`, `place`, `place_multi` | `stdlib/core/sampling.cpp` |
| `Core.Fx` | Envelopes (`exp_decay`, `adsr` and its primitive `adsr_curved`, with the `Curve` shapes `Linear`/`Exponential`), filters (`lowpass`, `highpass`, the modulated `lowpass_mod`/`highpass_mod`, the resonant `resonant`), control (`follow`), distortion (`hard_clip`, `soft_clip`), time effects (`delay`, `feedback`, `resample`, `reverb`), and the voice sugar (`gated`, `echoes`) | `stdlib/core/effects.cpp` + SynthGraph sugar |
| `Core.Render` | The effects: `render`, `render_vis`, `render_stems`, `render_vis_stems` | `stdlib/core/render.cpp` |
| `Core.Control` | Live controls: `slider`, `knob`, `int_slider`, `toggle`, `choice`, `opt`, `multi_slider`, and the component combinators `map`/`nest` — named build-time parameters the dev app can override between rebuilds, each yielding an `'a Control` | `stdlib/core/control.cpp` + SynthGraph sugar |
| `Core.Io` | Audio import: `load_mono`, `load_multi` | `stdlib/core/io.cpp` |
| `Core.Sig` | Signal constructors: `constant`, `constant_multi`, `time`, `signal`, `signal_multi`, `select` | `stdlib/core/signals.cpp` |
| `Core.Groove` | The sequencing tier: `pattern`, `humanized`, `mask`, `euclid` | written in SynthGraph (`lib.synth`) |
| `Core.Pitch` | Notes, temperaments and cents: `step`/`of_step`, `shift`, `flat`, `wrap_to`, `et`/`et12`/`just`/`pyth`, `hz`/`step_hz`/`a440`, `cents`/`detune`/`to_cents`/`ratio` | written in SynthGraph (`lib.synth`) |
| `Core.Tempo` | Meters, note values and the beat grid: `beat`/`bar`/`beats`/`bars`, `value`, `per_bar`, `bar_beats`, `at`, `grid`, `swing`, `swung_grid`, `marks`, `common` | written in SynthGraph (`lib.synth`) |
| `Core.Scale` | Keys, degrees and chords: `offsets`, `degree`, `notes`, `snap`, `shape`, `tones`, `stack`/`triad`/`seventh`, progressions (`Prog`, `prog_len`/`prog_degree`/`prog_root`/`prog_chord`/`prog_stack`), `invert`, `voicing`, `freqs`, `wrap_div`/`wrap_rem` | written in SynthGraph (`lib.synth`) |
| `Core.Score` | Phrases in beats, events in time, and the bridge between: builders (`line`/`melody`/`chord`/`arpeggio`/`rhythm`/`hits`), the algebra (`span`, `seq`/`layer`/`loop`/`move`, `transpose`/`in_key`/`staccato`/`legato`/`velocity`/`vels`/`crescendo`/`bend`/`humanize`/`shuffle`), the bridge (`realize`, `realize_with`), `play`/`strike`, and dynamics (`amp`, `ramp`, `db`) | written in SynthGraph (`lib.synth`) |
| `Core.Mix` | The stereo and bus vocabulary: `pan`/`pan_sig`, `mix`, `db`/`gain_db`, `vca`, `duck` | written in SynthGraph (`lib.synth`) |
| `Core.Str` | Computed names: `cat`, `of_int` | `stdlib/core/str.cpp` |
| `Core.Dsp` | The signal-tier prelude: the sound-design working set re-exported under bare names | written in SynthGraph (`lib.synth`) |

Adding to Core means four edits, not one: the definition in
`stdlib/core/lib.synth` (or the `.cpp` behind an external), the row
above, the roster in the language spec
[§6](language-spec.md#6-primitive-signatures-v1-roster), and coverage in
`tests/test_checker.cpp` and `tests/test_build.cpp`. For a submodule
written in SynthGraph the last one carries extra weight: types alone pin
almost nothing, so only a render pins a definition's *values*, and the
build tests compare against hand-written literal renders the way the
`List` combinator tests do.

## The three tiers, and the `Dsp` prelude

The library serves three tiers of writing. The **signal tier** —
oscillators, envelopes, filters, arithmetic — is where sound design
happens. The **grid tier** — `Tempo.grid` and friends feeding
`Groove` — is where rhythm lines live. The **score tier** —
`Scale`/`Score` phrases realized through a tempo and a tuning — is
where melody, harmony and duration-aware voices live. Feel
(`humanize`, `shuffle`, `swing`, `jitter`) exists on both the grid and
score tiers, so a part never has to abandon written durations to get a
human groove.

`Core.Dsp` is the working prelude for the signal tier: one view
re-exporting the sound-design working set under bare names, so a
typical voice or effect file opens with

```
open Core
open Core.Dsp
```

instead of the historical nine-line `open` block. `Dsp` deliberately
excludes `Io`, the stems renders, and all of
`Pitch`/`Tempo`/`Scale`/`Score` — those opens are meaningful decisions
(they carry name-collision management of their own, below), and the
submodules stay the canonical, documented homes. `synthc lint` warns
about opens that bind nothing a file uses.

## Module-by-module semantics

### `Core.Math`

- **`exp` / `sqrt` / `log` / `pow` / `sin` / `cos` / `tan` / `atan` /
  `abs`** are polymorphic over Scalars, Vectors and Signals
  (elementwise); anything else is a build-time error. Domain follows
  IEEE: `log` of a non-positive input or `sqrt` of a negative one yield
  `-inf`/NaN. Angles are radians. Because `signal ~f`'s body is applied
  symbolically to the time ramp, every one of these extends what
  `signal ~f` can express — a custom LFO is
  `signal ~f:(fun t -> sin (2.0 *. pi *. rate *. t))`.
- **`pi`** is a plain constant — the documented exception to "presets
  are functions": evaluating one number literal is free, and `pi` as a
  function would poison every arithmetic expression it appears in.
- **`min` / `max` / `clamp` / `lerp`** are the four-line Scalar helpers
  everyone used to write inline.
- **`hash ~seed ~i`** is value-level randomness done purely: jitter's
  splitmix64 exposed as a value — a platform-stable function of
  `(seed, index)`, uniform in `[0, 1)`. Same bits as
  `Time.jitter` (jitter *is* `hash` applied in the time domain), same
  reproducibility contract: same seed, same feel, renders stay
  cacheable. It unlocks humanized velocity, per-note detune drift,
  round-robin sample choice via `List.nth`, and generative arrangement
  choices via build-time `if` — no RNG anywhere.
- **`to_scalar` / `round` / `floor` / `ceil`** — Ints never decay
  implicitly; going to the continuous side is exact, and coming back
  names its fraction policy. `floor` is a true floor (toward −∞).

### `Core.List`

The combinators are **total**: every one answers for the empty list, so
a builder that legitimately produces nothing needs no special case at
the call site. `nth` answers with its `default` when the index is out
of range (`nth_opt` is the `Option`-returning spelling for callers that
want to observe absence); `maximum` takes the value an empty
list answers with, which doubles as a floor; `zip` stops at the shorter
list; `take`/`drop` of a count past either end answer with what is
there (negative counts take/drop nothing). `mapi` is index-aware `map`;
`scan` is the running fold (n+1 entries, `init` first) — the
cumulative-starts engine under `Tempo.marks`. `range ~from ~count` is
the Int counterpart of `time_steps`.

Every combinator recurses in tail position (accumulate, then reverse),
and the evaluator eliminates tail calls, so list length never
approaches the recursion guard — a phrase of tens of thousands of steps
evaluates at constant depth.

**`iter`** runs an effectful function over a list and is the one List
function implemented in C++: `unit` has no literal, so a synth-side
iterator would have nothing to return in its `Nil` arm. Its use is
computed render names (see `Str`).

### `Core.Option`

`'a Option` is the ambient two-constructor variant
(`type 'a Option = | None | Some of 'a`, declared at the top of
`lib.synth` beside `list`) for a value that may be absent: what an
optional parameter without a default (`?x:T`) delivers to its body,
what a `?x:` call-site argument passes through, and the return shape of
partial lookups (`List.nth_opt`). Options are plain variant values, so
`match` always works; the module exists so a pipeline can thread
absence without spelling the match out each time.

- **`is_some` / `is_none`** observe; **`value ~default`** and
  **`fold ~none ~some`** leave the option (`fold` is the catamorphism —
  everything else here is a special case of it).
- **`map`**, **`bind`**, **`join`** and **`map2`** are the monadic
  core: transform a present value, chain computations that may
  themselves come up empty, flatten one level of nesting, and combine
  two options (absent if either side is).
- **`or_else ~alt`** picks the first present option; **`filter ~f`**
  empties a value that fails a predicate; **`to_list`** bridges to the
  zero-or-one-element list; **`some`** is the `Some` constructor as a
  function (constructors are not first-class, so this is what flows
  into `List.map` and friends).

A defaulted optional parameter (`?(x = e : T)`) subsumes the common
`value` call: the body already sees a determined `T`, and a caller
holding an option passes it through with `?x:opt` — "use the caller's
choice if there is one, mine otherwise".

### `Core.Osc`

- **`sine` / `saw` / `square`** are the classic naive shapes —
  bit-stable and cheap, and they alias audibly on bright,
  high-fundamental material.
- **`saw_bl` / `square_bl`** are bandlimited (PolyBLEP) variants:
  identical to the naive shapes away from their discontinuities, with
  a two-sample polynomial correction at each edge that suppresses the
  aliasing. They are *new names* rather than new defaults so existing
  artifacts stay byte-identical; prefer them for bright unfiltered
  material.
- **`noise ~freq`** — pseudo-noise built from two-step cascaded FM with
  golden-ratio frequency relationships and modulation indices far above
  1. `freq` sets the spectral center. There is deliberately no RNG:
  renders are deterministic and cacheable, and this noise is
  bit-identical on every build while measuring as broadband and
  aperiodic.
- **`fm carrier modulator`** — sine oscillator whose instantaneous
  frequency is `carrier + modulator(t)` Hz, phase integrated from the
  epoch, so FM operators cascade.
- **`pm carrier modulator`** — `sin(2π·carrier·t + modulator(t))`; the
  modulator is in radians. `pm ~modulator:(constant (pi /. 2.0))` is the
  sine-with-phase idiom (a cosine).
- **`am carrier modulator depth`** — classic AM,
  `carrier · (1 + depth·modulator)`; a mono modulator applies to every
  channel of a multi-channel carrier. Ring modulation stays plain
  `carrier *. modulator`. Modulators must be mono; checked at graph
  build.

### `Core.Time`

- **`to_sec` / `to_ms` / `to_min`** — the computed counterpart of the
  literal unit suffixes. Once you have a Timestamp the operators carry
  it: durations add, subtract and scale, and results clamp at `0s`
  (see the caution box under Idioms).
- **`of_sec` / `of_ms` / `of_min`** — the way back out, exact inverses
  of the three above (`of_ms 250ms` is `250.0`). The conversion is
  deliberately *named*: what causes unit confusion is a Timestamp
  decaying into a bare number with nobody saying which unit it is in,
  so arithmetic still never drops the unit on its own — `1s /. 500ms`
  remains an error, and `of_sec 1s /. of_sec 500ms` is how you mean
  it. Reach for `div`/`rem` first when you want a count of durations;
  reach for these when the duration has to drive ordinary Scalar
  arithmetic (a rate, a curve exponent, a display value).
- **`div ~num ~den` / `rem ~num ~den`** — the missing quotient. The
  ratio of two durations is not a decayed number; it is a
  dimensionless **count**, so `div` answers "how many of these fit"
  as an `Int` (exactly where `~count` parameters want it) and `rem`
  answers "what is left" as a Timestamp. Floor convention, pairing
  with `Scale.wrap_div`: `num == den * div + rem` with
  `0s <= rem < den`. Division by `0s` is a build error. No unit ever
  decays — this is deliberately not `Timestamp /. Timestamp → Scalar`.
- **`time_steps ~start ~step ~count`** — the arithmetic grid; counts
  are capped at 1,000,000.
- **`jitter ~seed ~spread ~steps`** — humanizes a rhythm: each
  timestamp moves by a delta in `[-spread, +spread]` (clamped at `0s`)
  derived by hashing `(seed, index)` — `Math.hash` in the time domain,
  same bits. Pure, so builds stay reproducible; give each layer its own
  seed so they drift independently.

### `Core.Arrange`

- **`mix_all`** sums without normalization — headroom is the
  composer's (see Idioms).
- **`channels`** assembles mono signals into a Vector signal (16
  channels max in v1); **`channel ~n`** is the inverse it never had —
  one channel of a multichannel signal as mono, index validated against
  the static channel count at graph build. Per-channel processing of
  `load_multi` material and mid/side tricks compose from it.
- **`sample ~signal ~from ~to`** cuts the window `[from, to)`; an
  inverted window is rejected at build time. **`place`** embeds a
  sample at a timestamp, silence elsewhere; each placement replays its
  source from the source's own epoch, so stateful content (filters,
  `fm`, `reverb`) sounds identical at every placement.
- **`place_multi ~sample ~ats`** places one sample at every timestamp
  and mixes the placements — byte-identical to
  `mix_all (List.map (place s) ats)`, kept as a primitive for
  ergonomics. Summing is unnormalized.

### `Core.Fx`

- **`adsr ?decay_curve ?release_curve attack decay sustain release hold`**
  — durations are Timestamps, the sustain level a Scalar, and `hold` is
  the gate length: the envelope sustains until `hold`, then releases.
  The envelope is identically zero past
  `max hold (attack +. decay) +. release`, which is what gates placed
  tails into structural silence.
  The two optional curves are `Fx.Curve` values — `Linear` (the
  default) or `Exponential k` — and shape the falling segments
  independently:

  ```
  type Curve = | Linear | Exponential of Scalar
  ```

  `Exponential k` follows `e^-kx`, dropping fast and tailing off — the
  fall of a plucked string — with `k` saying how sharply: around 3 is a
  gentle bend, 5 the analog-ish middle, 10 and up nearly a spike. A
  negative `k` is the mirror (slow first, then steep), `Exponential 0.0`
  *is* `Linear`, and `|k|` is bounded by 64 — past that the shape is a
  step in all but name, and far past it `e^-k` would overflow into NaN
  samples, so the build stops instead. Every curve is normalized to its
  segment's endpoints, so a decay still lands exactly on `sustain` and a
  release exactly on silence whatever `k` is. **The attack is always
  linear**, and so is everything else unless a curve says otherwise (an
  existing call's output is unchanged). `adsr_curved` is the primitive
  underneath, taking the two shapes as the bare curvatures they stand
  for (`~decay_curvature` / `~release_curvature`, `0.0` for a ramp) —
  the form the external boundary carries; `adsr` is the spelling to
  reach for.
- **`lowpass` / `highpass`** are one-pole 6 dB/oct designs evaluated
  statefully from the epoch; a placed sample's filters warm up from the
  source's own timeline. Fixed cutoffs are cheap tone-shaping.
- **`lowpass_mod` / `highpass_mod`** are the same one-poles with a
  **signal-rate cutoff** (Hz, mono — the same rule as `am`'s
  modulator): filter sweeps as a *gesture* — risers, wah motions,
  spectral fades — with the coefficient recomputed per sample. A
  constant cutoff signal reproduces the fixed filter bit for bit.
- **`resonant ~cutoff ~q ~input`** — a two-pole state-variable lowpass
  with resonance: `q` around 0.7 is flat, higher values ring at the
  cutoff — acid lines, wubs, the opening-filter gesture. The cutoff
  signal clamps to a stable fraction of the render rate (about
  rate/6), so a sweep that overshoots stays a filter rather than a
  runaway. The sweep envelope is built from `exp_decay`/`adsr`/
  `signal ~f` math like any other control signal.
- **`follow ~attack ~release ~input`** — envelope follower: |input|
  smoothed with separate attack/release one-poles. The listening half
  of compressors, gates, auto-wahs and program-dependent sidechains:
  `vca ~gain:(1.0 -. follow ... *. depth)` ducks a bus under whatever
  the followed signal does. The input must be mono — follow a bus
  after mixing it down.
- **`delay ~by ~signal`** — feedforward delay: the input shifted `by`
  later, silence before it. Implemented with a ring buffer so a
  subgraph shared between dry and delayed paths keeps its stateful
  nodes consistent.
- **`feedback ~by ~gain ~input`** — feedback delay:
  `out(t) = in(t) + gain · out(t − by)`, `|gain| < 1` validated at
  construction, `by` positive and bounded below by one output frame.
  Dub delays, flangers, Karplus–Strong-style plucks. The loop lives
  inside the node's per-render state, exactly like `reverb`'s — the
  *language-level* graph stays acyclic; general user-defined signal
  cycles remain out of scope.
- **`resample ~input ~f`** — time warping: `f` is a playback-rate
  multiplier on the output's timeline, so `out(t) = input(∫₀ᵗ f)`.
  `1.0` identity, `0.5` an octave down, `2.0` up. The source runs in
  its own context from its own epoch; the read head only moves forward
  (`0.0` freezes, negative clamps — reverse playback is roadmap), rates
  above 64 are rejected, reads are linearly interpolated.
- **`reverb ~decay ~damping ~mix ~input`** — Schroeder reverb: four
  parallel damped feedback combs into two series allpasses, per
  channel. `decay` is RT60-style tail length, `damping`/`mix` in
  [0,1], validated at construction.
- **`hard_clip`** clamps flat at ±threshold; **`soft_clip`** saturates
  as `threshold·tanh(x/threshold)`. Thresholds must be positive. Drive
  is the ordinary idiom: `soft_clip 0.5 (x *. 3.0)`.
- **`gated ?decay_curve ?release_curve ~attack ~decay ~sustain ~release
  ~hold ~input`** — the voice-window idiom written once:
  `input *. adsr ...`, cut to the envelope's own end
  `[0s, max hold (attack +. decay) +. release)`. This fixes the window
  convention (see Idioms): `hold` is the sounding length, the window is
  the envelope's end. The curves are `adsr`'s, passed straight through
  (an unfilled one takes `adsr`'s default, `Linear`).
- **`echoes ~by ~gain ~n ~input`** — the feedforward echo stack
  `input + Σᵢ delay(by·i) · gainⁱ` for `i` in 1..n, replacing the
  hand-unrolled `dry + delay¹·g + delay²·g²` pattern. Feedforward;
  `feedback` is the regenerating tail.

### `Core.Render` and `Core.Io`

- **`render name rate sample`** declares a build target (the language's
  only effect); names share one project-wide namespace.
  **`render_vis`** renders a waveform SVG instead of audio.
  **`render_stems name rate stems`** — each `(label, sample)` pair
  declares an ordinary audio target named `<name>-<label>`;
  **`render_vis_stems`** renders *one* SVG with a labeled lane per
  stem. Computed names come from `Str` (below).
- **`load_mono` / `load_multi`** read audio files at build time as
  build inputs; paths resolve relative to the source file that names
  them, channel counts are validated (`load_mono` insists on 1). A
  loaded file occupies `[0s, duration)` and is silence afterward. The
  worked example is `examples/sampling`.

### `Core.Control`

Every control here yields an **`'a Control`** — a record pairing the value
this build resolved with the controller a panel shows it with:

```
type Controller = | Widget of String
                  | Keyed of (String, Controller)
                  | Nested_controller of Controller list
type 'a Control = { value : 'a; ui : Controller }
```

`c.value` is what the graph uses; `c.ui` is what `Ui.panel` takes. The name
is written once, where the control is declared — a panel never spells it
again — and a control some branch never declared leaves no controller for a
panel to name.

- **`slider name min max default`** / **`knob name min max default`**
  declare a named live control and evaluate to its value for this
  build: the override an attached `synth-dev` wrote into the unit's
  `controls.json` (clamped to `[min, max]`), or the default. The two
  differ only in how the dev app draws them. `max` must exceed `min`,
  the default must lie in range, and names share one project-wide name
  space — redeclaring a name with the same kind and range yields the
  same value, a conflicting redeclaration is a build error. The value
  is an ordinary value, fixed for the whole build, so evaluation stays
  pure and renders stay deterministic; moving a slider is a rebuild,
  not a modulation (use signals for that). The worked example is
  `examples/controls`; the file format and daemon wiring live in
  [`build-system.md`](build-system.md).

- **`int_slider name min max default`** is the same control quantized
  to whole steps: bounds, default and value are `Int`s, so it is the
  one to reach for when the parameter is a count rather than an amount
  (voices, divisions, repeats, an index into a list). An override lands
  on a step — a fractional value in a hand-edited `controls.json`
  rounds — and the bounds follow the slider rules (`max` must exceed
  `min`, the default must be in range).

- **`toggle name default`** is a tickbox: one `Bool`, on or off.

- **`choice name options`** picks one option out of a list, drawn as a
  tickbox per option, and evaluates to **the option itself** — of
  whatever type the list holds, so a choice can be a name, a frequency,
  or a whole voice:

  ```
  let voice : Scalar Signal =
    Control.choice ~name:"voice" ~options:[sine 220.0; saw 220.0] ;;
  let shape : String =
    Control.choice ~name:"shape" ~options:["soft"; "hard"] ;;
  ```

  The first option is the default, and an empty list has nothing to
  choose: that is a build error. Only the selected *index* crosses to
  the host, so the dev app labels each tickbox with the option's own
  value where a value reads as text (`String`, number, `Timestamp`,
  `Bool`) and with its position (`1`, `2`, …) otherwise. Redeclaring a
  choice under the same name requires the same options.

- **`opt ?on name value`** puts a tickbox in front of a value: ticked it
  is `Some value`, unticked `None` — the control an optional parameter
  takes directly.

  ```
  let voice : Scalar Signal =
    pluck ?depth:(Control.opt ~name:"depth" ~value:0.4) 220.0 ;;
  ```

  `?on` is where the tickbox starts, ticked unless said otherwise
  (`~on:false`). It is `toggle` plus a payload — the tickbox is an
  ordinary control named `name` — and `value` is an ordinary
  expression, evaluated whether or not the tick is on.

- **`map ~f ~c`** keeps a control's widget and changes the value it
  stands for; **`nest ~value ~parts`** makes several controllers one
  component with a value of its own. A component's first part is its
  head and draws where the panel put it, the rest one level in — so a
  component of one part looks exactly like the plain control it wraps.
  Together they are all a composite needs:

  ```
  type CurveShape = | Linear | Exponential ;;

  (* The rate slider is declared inside the arm that needs it, so while
     Linear is picked it does not exist at all. *)
  let curve_control ~label:String : Fx.Curve Control =
    let shape : CurveShape Control =
      Control.choice ~name:label ~options:[Linear; Exponential] in
    match shape.value with
    | Linear -> Control.nest ~value:Fx.Linear ~parts:[shape.ui]
    | Exponential ->
        let rate : Scalar Control =
          Control.slider ~name:(Str.cat label " rate") ~min:0.5 ~max:12.0
                         ~default:5.0 in
        Control.nest ~value:(Fx.Exponential rate.value)
                     ~parts:[shape.ui; rate.ui] ;;
  ```

  The panel that names `(curve_control ~label:"decay").ui` gets the row
  and — only while Exponential is picked — the slider indented under it.
  `examples/controls` runs this.

- **The `*_value` primitives** (`slider_value`, `knob_value`,
  `int_slider_value`, `toggle_value`, `choice_value`, and
  `multi_slider_lanes`) are the externals underneath, resolving the value
  alone. The functions above are their spelling; reach for a primitive
  only to build a control vocabulary of your own.

- **`multi_slider name sum_min sum_max lanes`** declares several
  controls whose values are *related*: each lane is a named Scalar in
  its own `[min, max]`, and the lane values additionally sum into
  `[sum_min, sum_max]`. It evaluates to one value per lane, in lane
  order — index them with `List.nth`. The shape an envelope wants,
  where each duration has its own range and the total has a budget:

  ```
  open Core.Control
  let env : Scalar list =
    Control.multi_slider ~name:"env" ~sum_min:0.0 ~sum_max:1.0
      ~lanes:[{ name = "attack"; min = 0.0; max = 0.4; default = 0.02 };
               { name = "decay"; min = 0.0; max = 0.5; default = 0.18 }] ;;
  let attack : Scalar = List.nth ~xs:env ~i:0 ~default:0.0 ;;
  ```

  A lane is a `Control.Lane` record — a record literal resolves against
  the innermost visible declaration, so the lanes need `open
  Core.Control` (the way `{ pc = C; oct = 4 }` needs `open Core.Pitch`).

  Each lane becomes an ordinary control named **`"<group>.<lane>"`**
  (`env.attack`), so lanes override, rebuild and appear in the metadata
  exactly like sliders. A group name shares the same project-wide space
  as a plain control's, with the same rule: redeclaring a group with
  identical lanes and bounds yields the same values, and a conflicting
  redeclaration is a build error.

  The declaration must be satisfiable — every lane's default in its own
  range, the defaults' sum inside `[sum_min, sum_max]`, and lane ranges
  that can reach those bounds at all — otherwise the build fails.
  Overrides are applied per lane, clamped to that lane's range, and then
  the group is put back inside its sum bounds by giving up (or taking
  up) the difference in proportion to each lane's remaining room. That
  projection is what makes a hand-edited `controls.json` safe;
  `synth-dev` never needs it, because it stops each lane at the budget
  while you drag.

### `Core.Ui`

- **`panel name controls targets`** groups part of a project for the dev
  app: the given controllers and the named render targets are shown
  together in one window — the knobs on top, each target's waveform
  beneath. A panel is the dev app's whole unit of UI, so this is how you
  decide what is shown beside what, and it is what keeps a project with
  five instruments and a dozen stems legible. Anything no panel names
  collects into one further panel, so declaring none costs you nothing
  and declaring some never hides the rest.

  ```
  open Core.Ui
  let _ = Ui.panel ~name:"Drums" ~controls:[gain.ui; decay.ui]
                   ~targets:["song-drums"] ;;
  ```

  Controls are given as **controllers** — the `ui` half of what a
  declaration handed back — so a member cannot be misspelled and a
  control the build never declared cannot be named. A controller may be
  a whole component (`Nested_controller`), which draws as its head plus
  its parts one level in; a `multi_slider` group is a single controller
  that draws as the usual linked budget bar. Since controllers are
  values, a panel comes *after* the declarations it shows.

  Targets are still given by **name**: a `render` evaluates to unit, so
  its name is the only handle it leaves behind. Every target name must
  resolve — a typo fails the build rather than silently dropping a
  waveform — and that check runs after evaluation, so a panel may name a
  target declared further down the file. A hand-built `Widget "name"`
  member is checked the same way.

- **`key k c`** reserves the key the panel's window reaches a
  controller by. Every row shows the key at the head of its name, and
  with that window focused pressing it selects the row — so this one
  always answers to `k`, wherever it sits in the list and whatever is
  added above it.

  ```
  let _ = Ui.panel ~name:"Kick"
            ~controls:[ Ui.key ~k:"e" ~c:env.ui ;
                        Ui.key ~k:"s" ~c:sustain.ui ;
                        decay_curve.ui ;
                        noise_amp.ui ]
            ~targets:["kick"] ;;
  ```

  It is written **where the panel lists the controller**, not where the
  control is declared: the key is a fact about the window showing it, so
  the same control listed in two panels can answer to a different key in
  each, and a control's declaration says nothing about the layout.

  `k` is one **letter**: the dev app addresses its windows by digit, so
  a digit here is a build error. On a component it lands on the head row
  — the one the panel shows the component by. Rows nobody has spoken for take
  the first free key in panel order, so reserving one never costs
  another row its label, and a window with no reservations labels
  exactly as it did before they existed.

  A panel that reserves one key twice, or gives one controller two keys,
  is a build error — the labels are what its window shows, so each can
  only reach one row:

  ```
  error: panel 'Kick' reserves the key 'd' twice: for 'adsr decay curve'
         and for 'adsr release curve'
  ```

  Two different panels may each use `d`, since the labels belong to a
  window.

  Panel names have their own project-wide name space, separate from
  controls and targets. Unlike a control, redeclaring a panel is always
  an error, even identically: a panel yields no value, so there is no
  reason to declare one twice. Listing a member twice is likewise an
  error — it would draw the same widget twice.

  Panels are **presentation only**. The declaration never reaches the
  engine, never changes an artifact, and never enters a cache key, so
  renaming one costs nothing to rebuild. A project that declares no
  panel behaves exactly as it did before panels existed, down to the
  bytes of its `metadata.json`. See [`tooling.md`](tooling.md) for what
  the dev app does with them.

### `Core.Sig`

- **`constant` / `constant_multi` / `time`** — constants and the ramp
  whose sample at t seconds is t.
- **`signal ~f` / `signal_multi ~fs`** — sample a `Scalar -> Scalar`
  function over time. The body is applied *symbolically* to the time
  ramp, so whatever it builds becomes the graph: arithmetic, the Math
  primitives (the trig family included), and comparisons / `if` /
  `&&` / `||` / `not`, which take their sample-wise meaning here — a
  comparison is a 0/1 condition signal and an `if` on one is a
  `select`. That makes ordinary SynthGraph functions written with `if`
  (`Math.min`, `Math.max`, `Math.clamp`) usable inside the lambda.
  Because nothing skips a branch per sample, such an `if` builds
  *both* branches and both must be Scalars or Signals.
- **`select ~gate ~threshold ~above ~below`** — the sample-wise choice,
  written out: `gate(t) >= threshold`
  picks `above`, else `below`, per sample. The gate must be mono; all
  three children advance in lockstep whichever side is chosen. With
  `follow` this is signal-level control: gates, program-dependent
  switching, `follow`-driven wahs into `resonant`.

### `Core.Groove`

The sequencing tier, promoted from the examples: a hit placed on a
Timestamp grid. Everything takes the **step list** rather than baking
in grid arithmetic, so it composes with `Tempo.grid`, `swing`,
`jitter`, `mask` and `euclid` without a parameter explosion.

- **`pattern ~hit ~steps`** — `place_multi` with the arguments shaped
  for pipes: `Groove.pattern kick (grid ~t ~from ~step:Quarter
  ~count:32)`.
- **`humanized ~hit ~steps ~seed ~spread`** — `pattern` with `jitter`
  applied at the placement; same seed contract (pure, per-layer seeds).
- **`mask ~keep ~steps`** — `keep` cycles over the steps: the `x..x`
  row of a step sequencer. An empty `keep` keeps everything.
- **`euclid ~hits ~steps`** — Euclidean rhythm: `hits` onsets spread as
  evenly as possible over the given grid (Bjorklund selection — step
  `i` stays when `(i·hits) mod n < hits`, so the pattern starts on a
  hit). `euclid ~hits:5` over 16 steps is the world-rhythm generator
  half of electronic music leans on.

### `Core.Pitch`

- **A note is data, a temperament is data.** The chromatic ladder is
  indexed from **C0 = 0**, so A4 is step 57 — deliberately not a MIDI
  key number. A `Tuning` carries `ratios` (one frequency ratio per
  step, counting from `root`), the `octave` the ladder repeats at, and
  a `ref_hz`/`ref_step` anchor. One formula serves every temperament:

  ```
  raw s = ratios[(s - root) mod n] * octave ^ floor((s - root) / n)
  hz  s = ref_hz * raw s / raw ref_step
  ```

  Dividing by `raw ref_step` anchors the ladder and makes the reference
  pitch **exact by construction** in every temperament. `root` is the
  key centre (why `just` and `pyth` sound different in different keys;
  equal temperaments ignore it). `octave` admits non-octave tunings: 13
  ratios and `octave = 3.0` is Bohlen-Pierce. A tuning with no ratios
  has exactly one pitch, which keeps `step_hz` total.
- **Two ways to move a pitch, deliberately different types.**
  `shift ~by:Int` is discrete and temperament-relative;
  `detune ~cents:Scalar` is continuous, temperament-independent, and
  acts on a frequency (a `Note` has no fractional part — for
  *phrase-level* inflection see `Score`'s `bend`). `cents` returns the
  bare multiplier `2^(n/1200)`; `to_cents` inverts it (a `log` undoing
  a `pow`, so not bit-exactly).
- **`wrap_to ~note ~low`** — the register fold: the note moved by whole
  octaves into `[low, low + octave)`. This is the `voicing ~count:1` +
  `nth` idiom both example songs used to fake, named; basslines and
  chord roots over a register floor compose from it.
- **The tuning comes first.** `hz`/`step_hz` take `~t` ahead of the
  pitch so a temperament partially applies:
  `let p : Note -> Scalar = hz ~t:(just ~root:0 ~ref_hz:440.0)`.
  `a440` is the zero-ceremony 12-TET path. `Note` is inherently
  12-tone; for `n ≠ 12` work in `Int` steps with `step_hz` — and reach
  the score tier through `Score.realize_with` with a
  `step_hz`-backed mapping.
- **`open Core.Pitch` binds twelve one- and two-letter constructors**
  (`C`, `Cs`, … `B`) as bare names — great in pitch-heavy files, worth
  keeping qualified elsewhere.
- Presets (`et12`, `just`, `pyth`, …) are functions, not constants: a
  paramless definition is evaluated on every build of every project,
  and a tuning nobody asked for should not cost one.

### `Core.Tempo`

- **`bpm` counts the meter's `unit` note per minute** — unambiguous for
  simple meters, the usual convention for compound ones (6/8 felt in
  two is `{ beats = 6; unit = 8 }`, the dotted-quarter pulse is
  `value ~v:(Dotted Quarter)`). Everything derives from
  `beat = to_min (1.0 /. bpm)`; `bars ~n` is the sibling `beats ~n`
  always had, fractional included.
- **A whole note is `unit` beats, whatever the meter**, so `value` is
  the beat times `unit` times a dimensionless fraction. `Value` is a
  recursive variant: `Dotted` multiplies by 1.5 and composes,
  `Tuplet (n, m, v)` is *n* of `v` in the time of *m* and nests, and a
  `match` that forgets a case is a build-time error.
- **`per_bar ~t ~v`** — how many of a note value fit in one bar, via
  `Time.div`, so it is correct in *any* meter, compound included — the
  count a grid's `~count` wants. **`bar_beats ~t ~n`** is the
  bars→beats bridge `Score.move`/`chord ~len` need (named `bar_beats`,
  not `span`, so `Score.span` stays unshadowed).
- **Bars and beats count from 0.** `at ~bar:4 ~beat:2.0` is an offset —
  "four bars and two beats in" — not a ruler label. `grid` is the
  tempo-aware `time_steps`. A `Tempo` carries one meter; a piece that
  changes meter binds a second one and offsets from a computed start.
- **`marks ~t ~bars`** turns section lengths into section starts: n+1
  Timestamps (each start plus the ending), so re-lengthening one
  section mid-writing is one edit. The named landmarks stay yours to
  bind — Core cannot name `drop2`. (`List.scan` is the general tool;
  `marks` is its tempo-aware face.)
- **`swing ~amount ~step ~steps`** displaces every odd-indexed entry
  later by `step *. amount` (`0.0` straight, `1/3` triplet, `0.5`
  dotted); it takes `step` rather than reading consecutive gaps so the
  last entry is not a special case. **`swung_grid`** is `grid` and
  `swing` in one call so the step is named once — a disagreement
  between the two is a silent groove bug. `jitter` humanizes by
  hashing; `swing` is deterministic displacement — both pure, both
  cacheable.

### `Core.Scale`

- **Degrees count from 0 and wrap at the ladder's own length** —
  degree 5 is the octave of a pentatonic scale, degree 7 of a
  heptatonic one; negative degrees descend (the case a truncating
  divide gets wrong, which is why `Scale` carries `wrap_div`/`wrap_rem`
  — the language has no `%`). `snap` pulls an out-of-key note to the
  nearest one in it, taking the lower when exactly between two.
- **The quality enums are open.** Fourteen named scale qualities plus
  **`CustomQ of Int list`** — any semitone ladder: harmonic major is
  `CustomQ [0; 2; 4; 5; 7; 8; 11]`, and Hirajoshi, octatonics or a
  maqam approximation are the same one line. Twenty named chord shapes
  (the classic twelve plus `Sixth`, `Min6`, `Dom9`, `Maj9`, `Min9`,
  `MinMaj7`, `Dom7b9`, `Dom13`) plus **`Shape of Int list`** — any
  chord: a full 13th is `Shape [0; 4; 7; 10; 14; 17; 21]` (the named
  `Dom13` omits the 11th, as played). Every function — `degree`,
  `snap`, `notes`, `stack`, `triad`, `in_key` — works over custom
  ladders unchanged, because they all go through `offsets`/`shape`.
  Named qualities are documentation; the payload cases are the escape
  hatch. (A user `match` over these enums must now cover the payload
  case — the checker names it concretely.)
- **A scale has a `tonic`, a chord has a `root`** — the musically
  correct words, and what keeps the two record types apart when a
  literal resolves by field names. `Quality` is spelled long (`Major`)
  and `ChordQuality` short (`Maj`) so both can be opened at once.
- **A diatonic stack is notes, a named chord is a `Chord`.**
  `triad`/`seventh`/`stack` take every other degree and hand back a
  `Pitch.Note list` — the quality falls out of the key (harmonic minor
  yields its minMaj7 without a name). `Chord` is for chords you *name*.
  `invert`, `voicing` and `freqs` work on the list from either source.
- **`Prog`** names the progression every project used to rebuild: a
  `key` plus a `degrees` cycle, with wrapping lookup. `prog_degree` is
  the wrapped lookup; `prog_root`/`prog_chord`/`prog_stack` compose
  from it, so a song's harmony collapses to
  `let prog : Prog = { key = key; degrees = [0; 5; 3; 4] }` plus calls.
- **`invert` rotates, `voicing` spreads.** `invert ~n:1` lifts the
  bottom note an octave; `voicing ~low ~count` cycles upward through
  the chord for `count` parts from the lowest octave at or above `low`.
  `freqs ~t` is the one exit to Scalars and names its temperament.

### `Core.Score`

- **A `Phrase` is symbolic, an `Event` is not.** A phrase holds beats
  and `Pitch.Note`s (and per-step `vel` and `bend`), so one phrase
  plays at any tempo in any temperament and every transform is a pure
  edit. An `Event` carries a frequency, a Timestamp and a duration —
  a voice never has to know about temperament.
- **`realize ~tempo ~tuning` and `realize_with ~tempo ~pitch`** are the
  bridge — `realize` is sugar for
  `realize_with ~pitch:(Pitch.hz ~t:tuning)`. `realize_with` accepts
  *any* `Note -> Scalar` mapping: scordatura, a well-temperament
  table, per-key inflection maps, or a `step_hz`-backed `n ≠ 12` ladder
  are one lambda away, and `Score` never learns about tuning.
- **`bend`** is per-note inflection in cents, applied at realization
  through `Pitch.cents` — quarter-tones and blue notes inside phrases.
  Builders default it to `0.0` (which multiplies the frequency by
  exactly 1.0); `Score.bend ~p ~f` sets it by step index. This is a
  *sound* feature, not a notation feature.
- **`rhythm` and `hits`** are percussion without a dummy note: unpitched
  steps laid end to end (they carry `of_step 0`, which `strike`
  ignores). `hits ~n:8 ~len:0.5 |> vels [1.0; amp ~l:Forte]` is the
  accented hat line that used to take two layered phrases.
- **`vels` and `crescendo`** are per-step dynamics — the consumer
  `ramp` never had. `vs` cycles over the steps in order, *scaling* each
  velocity (so `[1.0; 0.6]` alternates accents and transforms
  compose); `crescendo` is `vels` over `ramp ~n:(step count)`,
  interpolated in decibels, which is the shape a crescendo actually
  has.
- **`humanize` and `shuffle`** are feel on the symbolic side — in
  beats, before `realize` — which is what finally makes feel compose
  with duration-aware voices. Both are pure via `Math.hash` (same
  contract as `jitter`: per-layer seeds; positions clamp at beat 0).
  `shuffle ~grid ~amount` displaces steps landing on odd multiples of
  `grid` beats — named `shuffle`, not `swing`, because `Tempo.swing`
  stays for Timestamp lists (the module's name-dodging convention).
- **`play` hands a voice the note's duration.** A voice is
  `(freq, duration, velocity) -> Sample` — `Fx.gated` writes its
  window arithmetic in one call. `strike` is the percussion form,
  taking velocity alone. Both **sum without normalization**.
- **`seq` and `layer`** are the two ways to combine: `seq` starts each
  phrase where the last ended (a left fold — linear work, constant
  depth); `layer` leaves phrases at their own positions; `loop ~n` is
  `seq` of n copies. `line` lays `Item`s end to end (`Rest` advances
  the cursor without a step). `legato` stretches each note to the next
  attack (a fold per step — quadratic, fine at musical sizes);
  `staccato ~ratio` scales written lengths.
- **Names dodge their neighbours:** `span` not `length`, `layer` not
  `stack`, `loop` not `repeat`, `move` not `shift`, `shuffle` not
  `swing`. `Forte`/`Piano` are spelled out because a bare `F` would be
  Pitch's F.
- **Dynamics are a decibel ladder:** `Level` is 4 dB per step anchored
  at `Fff = 1.0`, so `Piano` is exactly a tenth of `Fff`.
  `ramp ~from ~to ~n` interpolates in decibels; `db` converts decibels
  to linear gain.
- **Portamento stays open.** A glide needs the *next* event's
  frequency and a voice sees one note; the likely shape is an
  alternative player handing `(freq, next_freq, dur, vel)`, deferred
  until the modulated-filter idioms settle.

### `Core.Mix`

The musical layer above `channels`/`mix_all` (which stay in `Arrange`
untouched):

- **`pan ~pos ~input`** places a mono source in the stereo field with
  the equal-power law (`pos` −1 left … +1 right, via `sqrt`);
  **`pan_sig`** is the same law with the position automated — autopans,
  Doppler sweeps.
- **`mix ~parts`** is the weighted `mix_all`: `(gain, signal)` pairs
  keep the gains readable next to their parts, and with **`db`** (a
  re-export of `Score.db`, so mixing code needs no
  `open Core.Score`) they read as decibels:
  `mix [(db (-6.0), drums); ...]`. **`gain_db`** applies one.
- **`vca ~gain ~input`** is mono-gain-over-any-bus — the operator
  table's broadcast row (`bus *. envelope`), named. Fading a Vector
  master with an envelope is `vca ~gain:env`.
- **`duck ~ats ~depth ~dip ~recover ~input`** canonizes the grid
  sidechain: a `1 → 1−depth → 1` dip placed at every timestamp,
  multiplied onto the bus — pumping pads under a four-floor kick with
  no envelope follower. Overlapping dips sum; keep `depth × overlaps`
  under 1. Program-*dependent* sidechaining is `Fx.follow` + `vca`.

### `Core.Str`

Two functions, one workflow: **`cat`** and **`of_int`** exist so render
target names can be computed —
`render ~name:(Str.cat "section-" (Str.of_int i))` — and `List.iter`
exists so the renders can be iterated. Rendering one artifact per
*section* of a song is the natural audition loop for arrangement work,
with incremental caching doing the rest. This is deliberately not a
string library; anything more waits for a real need.

## Idioms

- **The voice-window convention (pick one, this one):** `dur`/`hold`
  is the *sounding* length; the sample window is the envelope's end,
  `max hold (attack +. decay) +. release`. `Fx.gated` writes it for you —
  a voice is
  `fun freq:Scalar dur:Timestamp vel:Scalar -> osc freq * vel |> gated
  ~attack:3ms ~decay:110ms ~sustain:0.5 ~release:60ms ~hold:dur`.
- **The ducking pattern:** on a fixed grid, `Mix.duck`. Program
  dependent: `vca ~gain:(1.0 - follow ~attack ~release kick_bus *
  depth)`. Both are ordinary graphs — no side-chain routing concept
  needed.
- **The section-landmark pattern:** name one `Tempo`, derive every
  position from it, and let `Tempo.marks ~bars:[8; 8; 12; 4]` compute
  the section starts; bind the names (`intro`, `drop`, …) with
  `List.nth` over the result. Per-section renders via
  `Str` + `List.iter` complete the audition loop.
- **Caution: Timestamps clamp at the epoch silently.** A placement
  computed as `at -. lead -. length` near the origin *moves* to `0s`
  rather than erroring (the same clamp `jitter` applies). If an early
  hit sounds doubled on the downbeat, look for a subtraction that went
  negative.
- **Headroom is yours.** `mix_all`, `place_multi`, `play`, `strike`
  sum without normalization; rendering works in doubles and only
  hard-clamps at WAV write. Gain-stage deliberately —
  `soft_clip ~threshold:0.9` as master glue is the examples'
  convention.
- **Randomness is `hash`, and seeds are per-layer.** Same seed, same
  feel, same bytes: `jitter`/`humanized`/`Score.humanize` for time,
  `hash` directly for velocity, detune, choices. There is no RNG and
  no `random` effect on purpose.
- Every primitive has a rendered demonstration in
  `examples/primitives`; the sampling workflow (`Io` → slice →
  `resample` → place) is worked through in `examples/sampling`.

## New in the core-library review round

For release notes and shadow-checking (additions to *opened* Core
modules can shadow a user's earlier same-named definitions): the names
added by this round are `Math.sin`, `Math.cos`, `Math.tan`,
`Math.atan`, `Math.abs`, `Math.pi`, `Math.min`, `Math.max`,
`Math.clamp`, `Math.lerp`, `Math.hash`; `List.mapi`, `List.take`,
`List.drop`, `List.scan`, `List.iter`; `Osc.saw_bl`, `Osc.square_bl`;
`Time.div`, `Time.rem`; `Arrange.channel`; `Fx.lowpass_mod`,
`Fx.highpass_mod`, `Fx.resonant`, `Fx.follow`, `Fx.feedback`,
`Fx.gated`, `Fx.echoes`; `Sig.select`; the `Groove`, `Mix`, `Str` and
`Dsp` modules; `Pitch.wrap_to`; `Tempo.bars`, `Tempo.per_bar`,
`Tempo.bar_beats`, `Tempo.marks`, `Tempo.swung_grid`;
`Scale.CustomQ`, `Scale.Shape`, the eight named chords, `Scale.Prog`
and its `prog_*` family; `Score.rhythm`, `Score.hits`, `Score.vels`,
`Score.crescendo`, `Score.bend`, `Score.humanize`, `Score.shuffle`,
`Score.realize_with`, and the `Step.bend` field (a *breaking* change
for hand-written `Step` literals and exhaustive matches over
`Quality`/`ChordQuality` — both fail loudly at check time with concrete
messages).

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
external Core definition is such a binding over the `stdlib/core/*.cpp`
files shipped beside `lib.synth`.

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
