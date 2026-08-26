# Roadmap

Future work only: things deliberately left out of v1 (design doc §13,
spec [§7](language-spec.md#7-out-of-scope-in-v1)) and improvements under
consideration. Everything here is *not* implemented today unless a line
says otherwise; the rest of the documentation describes only what is.

## Language

- **Signal-level branching.** Comparisons and `if` are build-time only.
  A sample-wise select/gate over signals would be a new signal-producing
  primitive (e.g. threshold gates, envelope followers driving choices).
- **Mutual recursion.** `let rec` covers self-recursion (and recursive
  type declarations cover `list`-like shapes), but a group of
  definitions cannot refer to each other: definitions still precede
  use, which the checker's scope replay, the evaluator's ordering, the
  incremental dependency graph, and the LSP all lean on. An
  `and`-group form could lift this without breaking existing syntax.
- **Literal patterns.** `match` covers constructors, tuples, records,
  binders and wildcards; matching on a literal value (`| 0 -> ...`)
  is deliberately absent — use `if` — and would need a design for
  exhaustiveness over open domains.
- **Per-declaration visibility for types.** A published module exposes
  its type declarations like its values; abstract-on-export (hiding a
  record's fields outside its library) is unexplored.
- **Type inference.** Every binding is fully annotated; polymorphism is
  written out. Local inference (return types, `let ... in` annotations,
  lambda parameters, `match` scrutinee-directed shortcuts) would remove
  most annotation weight without changing the checked language.
- **Per-definition visibility control.** A library's `lib.synth`
  publishes whole modules or re-exported values, but a published module
  exposes all of its definitions — there is no `private` below module
  granularity.

## Engine & primitives

- **Feedback / IIR delays.** `delay` is feedforward only; feedback
  echoes, resonators, and IIR-style signal cycles need a language-level
  story for cyclic graphs (today only primitives like `reverb` may hold
  internal feedback state).
- **Reverse playback.** `resample` reads its source only forward — a
  negative rate clamps to zero rather than rewinding. Reverse (and
  scrubbing generally) requires a different read-head model.
- **Higher channel counts.** Signals are capped at 16 channels in v1.
- **More output formats.** WAV *reading* handles PCM 16/24/32 and float
  32/64, but artifacts are always written as 16-bit PCM; 24-bit or
  float output (and formats beyond WAV) would preserve the engine's
  double-precision headroom.

## Composition libraries

Core is strong on *synthesis* and thin on *songwriting*. Everything a
composition needs beyond a raw signal — that a pitch is a note, that a
position is a bar and a beat, that four frequencies are a chord — lives
today in comments rather than in code:

```ocaml
note 523.25 |> place ~at:5s ;;                            (* C5 *)
place_multi kick (time_steps ~start:2s ~step:500ms ~count:28) ;;
let am : Scalar Sample = bar ~a:110.0 ~b:164.81 ~c:220.0 ~d:261.63 ;;
```

Every pitch is a hand-computed frequency, every position a hand-computed
offset from a tempo that appears nowhere in the source, and a chord is
four unrelated floats. Re-tempoing or transposing means recomputing
every literal in every file by hand.

Four submodules close that gap, in two layers, and all four are
**written in SynthGraph**, following the `List` precedent: `Math.pow`
already gives equal temperament, records and recursive variants give the
data structures, and `let rec` gives the traversals. No C++ was needed
anywhere in the stack.

| Module | Owns | Depends on | Written in |
|---|---|---|---|
| `Core.Pitch` | Notes, temperaments, cents | — | SynthGraph ✅ |
| `Core.Tempo` | BPM, meters, note values, bar/beat → Timestamp | Time | SynthGraph ✅ |
| `Core.Scale` | Keys, modes, scale degrees, chords, inversions | Pitch, List | SynthGraph ✅ |
| `Core.Score` | Phrases, events, articulation, dynamics, `play` | all of the above | SynthGraph ✅ |

**Landed so far:** Timestamp arithmetic (language spec
[§3](language-spec.md#operators-pointwise-lifting--scalar-broadcasting)),
the wider `List` module, and `Core.Pitch`, `Core.Tempo`, `Core.Scale`
and `Core.Score`
([§6](language-spec.md#6-primitive-signatures-v1-roster)) — the whole
stack, none of it C++.

### `Core.Pitch` — **shipped**

Implemented in `stdlib/core/lib.synth`; the roster lives in the language
spec [§6](language-spec.md#6-primitive-signatures-v1-roster) and the
semantics in [`core-library.md`](core-library.md). It landed with two
changes to what this section originally proposed:

- **Temperament is first-class**, rather than the "beyond the first
  pass" item it was filed as. `Tuning` is an ordinary record —
  `ratios` per step of the octave, the `octave` the ladder repeats at,
  a `root` key centre, and a `ref_hz`/`ref_step` anchor — and one
  formula serves equal, just, Pythagorean and non-octave tunings alike.
- **No MIDI.** The integer form of a note is a plain step index on the
  chromatic ladder with C0 = 0, so A4 is 57. Nothing in Core speaks
  MIDI.

Still open, deliberately:

- ~~**The examples still hold raw frequencies and raw Timestamps.**~~
  **Done.** Both showcase projects now name their pitches and their note
  values. Each grew a `timing.synth` holding the one `Tempo`
  everything derives from, and `examples/darksynth/bass.synth`'s
  `1.006`/`1.009` are `detune ~cents:10.4`/`15.5` (`guitar.synth`'s
  three followed in the phase-4 pass). The timing half was verified by
  rendering: it is bit-identical to the pre-migration artifacts. The
  pitch half moved five committed renders under `outputs/`, and only by
  the rounding — replacing each 2-decimal literal with exact 12-TET
  shifts every pitch by under 0.02 cents, which shows up as phase drift
  about 28 dB below the signal. Restoring the rounding reproduces the
  old artifacts byte for byte.
- **`Note` is inherently 12-tone**, so `shift`/`flat`/`hz` suit
  12-division temperaments; `n /= 12` ladders work in `Int` steps
  through `step_hz`. A spelling that generalizes to arbitrary `n` would
  need a different note type, and it is not clear one is worth it.
- **Scale-degree spelling** (whether `Cs` and `Df` should be
  distinguishable) stays out until something needs to render notation.

### `Core.Tempo` — **shipped**

Implemented in `stdlib/core/lib.synth`; the roster lives in the language
spec [§6](language-spec.md#6-primitive-signatures-v1-roster) and the
semantics in [`core-library.md`](core-library.md). It shipped as
proposed, with three refinements:

- **The indexing base of `at` is 0**, and the open decision this section
  used to record is settled. Bars and beats count from 0 like every
  other index in the language, and `at ~bar:4 ~beat:2.0` is read as an
  offset — "four bars and two beats in" — rather than as a ruler label,
  which sidesteps the musicians-count-from-1 clash instead of picking a
  side.
- **`grid` takes `~from` and a `Value` step**, not `~start` and a
  `Scalar`: `grid ~t ~from:2s ~step:Quarter ~count:28` is exactly the
  call that replaces `time_steps ~start:2s ~step:500ms ~count:28`, and
  the note value is the thing worth naming.
- **`swing` takes `~step` explicitly** rather than reading it off
  consecutive gaps, so the last entry of a grid is not a special case
  and a non-uniform grid still behaves predictably. `Tempo.common ~bpm`
  is the 4/4 preset, a function rather than a constant for the same
  reason `Pitch.et12` is.

Still open, deliberately:

- **No note-value *count*.** `value` answers how long a note value is,
  as a `Timestamp`, and Timestamps do not divide — so there is no way to
  ask how many eighths fill eight bars, which is exactly the number a
  `grid`'s or a `Seq.humanized`'s `~count` wants.
  `examples/darksynth/timing.synth` derives its own
  `quarters`/`halves`/`eighths`/`sixteenths` off `meter.beats`, correct
  for simple meters only. A `Tempo.per_bar ~t ~v:Value : Int` is three
  lines over the same `frac` `value` already walks, and it is the one
  gap the example rewrite turned up.

`value` rests on one observation that is easy to get wrong: a whole note
is `unit` beats, whatever the meter — four beats in 4/4, eight eighths
in 6/8 — so the only recursive part is a dimensionless fraction of a
whole note. `Dotted` and `Tuplet` then compose without further
machinery, and `match` exhaustiveness catches a missing case at build
time.

### `Core.Scale` — **shipped**

Implemented in `stdlib/core/lib.synth`; the roster lives in the language
spec [§6](language-spec.md#6-primitive-signatures-v1-roster) and the
semantics in [`core-library.md`](core-library.md). Two departures from
what this section originally proposed, both forced by trying to write
it:

- **Diatonic stacks are `Note list`s, not `Chord`s.** `triad` and
  `seventh` were to return a `Chord`, which means naming the quality —
  but harmonic minor's tonic seventh is a minMaj7 and its third is an
  augMaj7, neither of which is in `ChordQuality`, and pentatonic and
  whole-tone stacks have no name at all. Classifying would have meant
  either an ever-growing enum or a lossy fallback. So a stack hands back
  the notes the key produced, `Chord` stays for chords you *name*, and
  `invert`/`voicing`/`freqs` work on the list from either source. The
  `inv` field of `Chord` went with it: inversion is `invert`, applicable
  to any note list, rather than a field only named chords can carry.
- **A scale has a `tonic`, not a `root`.** The musically correct word,
  and also the necessary one: a record literal resolves by its field
  names, so `Scale` and `Chord` both having `{ root; quality }` would
  have been ambiguous at every use.

`Scale.freqs` also takes a `~t:Tuning` rather than assuming 12-TET,
matching `Pitch.hz` — a just-intonation triad really is rational.

Degrees count from 0, settling the same question `Tempo.at` raised, and
wrap at the ladder's own length rather than at seven: degree 5 is the
octave of a pentatonic scale. Negative degrees descend, which is why
`Scale` carries a floor division and remainder of its own.

Since shipping:

- ~~**The examples do not use it yet.**~~ **Done**, with the `Score`
  rewrite in phase 4. Each showcase project grew a `harmony.synth` — the
  tonal counterpart of its `timing.synth` — holding the one `Scale`
  everything derives from, the chord cycle as a list of degrees, and two
  register-folding helpers (`parts`, `root`) over `Scale.voicing`. Both
  progressions came out of the key rather than out of chord names:
  `examples/song/`'s `Am → F → C → G` is `[0; 5; 2; 6]` of A minor, and
  `examples/darksynth/`'s `Am → F → Dm → E` is `[0; 5; 3; 4]` of A
  *harmonic* minor — so the E major that makes the track dark is the
  mode's own degree 4 rather than a hand-spelled G♯.

  The darksynth voicings came back bit-identical: folding each triad up
  from a fixed `low` reproduces the hand-written register exactly, as do
  the bass, guitar and bell roots at their own floors. The song's pad
  did not, and that is the musical edit this item was filed for —
  `Scale.voicing ~count:4` spells the triad plus its octave where the
  original had a hand-voiced spread — so `outputs/song/` moved.

### `Core.Score` — **shipped**, with dynamics folded in

Implemented in `stdlib/core/lib.synth`; the roster lives in the language
spec [§6](language-spec.md#6-primitive-signatures-v1-roster) and the
semantics in [`core-library.md`](core-library.md).

The two representations survived the implementation unchanged, and they
are the design. A `Phrase` is **symbolic** — positions and lengths in
beats, notes as `Pitch.Note`s — so one phrase plays at any tempo in any
temperament and every transform is a pure edit of the score. An `Event`
is **realized**: a frequency, a Timestamp, a duration. `realize` is the
one bridge, and it is the only place a tempo and a tuning are named.
Keeping `Phrase` in beats is also what leaves room for tempo maps later
without a breaking change.

`play` is what the whole stack was for. A voice becomes a function of
`(freq, duration, velocity) -> Sample` — note the *duration*, which is
what a note-length-aware library buys over pre-baked fixed-length
samples, and the reason Timestamp arithmetic had to land first:

```ocaml
let piano freq:Scalar dur:Timestamp vel:Scalar : Scalar Sample =
  K.strike freq
    * adsr ~attack:4ms ~decay:600ms ~sustain:0.25 ~release:350ms ~hold:dur
    * vel
  |> sample ~from:0s ~to:(dur + 350ms) ;;

melody ~notes:(List.map ~f:(Scale.degree ~s:key) [0; 2; 4; 2; 0; -1; 0])
       ~len:1.0
  |> move ~beats:8.0
  |> realize ~tempo:tempo ~tuning:tuning
  |> play ~voice:piano
```

Like `place_multi`, `play` sums without normalization; headroom stays
the composer's.

Departures from what this section originally proposed:

- **`Core.Dyn` is folded in**, as this section always allowed it might
  be: a level is only ever a velocity, and `db` is three lines. `Level`
  spells `Piano` and `Forte` out where the rest are abbreviated,
  because a bare `F` would collide with `Pitch`'s pitch class of the
  same name. The ladder is 4 dB per step anchored at `Fff = 1.0`, which
  makes `Piano` exactly a tenth of it and keeps every level at or below
  unity — the originally sketched "Mf → 0.5, ~6 dB per step" cannot do
  both, since six 6 dB steps above 0.5 overflow.
- **`Event` carries a frequency, not a `Note`.** `realize` resolves
  pitch as well as time, so a voice never has to know about
  temperament and `play` needs no `~tuning` of its own.
- **Four names are spelled to stay out of the way** of modules usually
  open beside this one: `span` (not `length`, which is `List.length`),
  `layer` (not `stack`, which is `Scale.stack`), `loop` (not `repeat`,
  `List.repeat`) and `move` (not `shift`, `Pitch.shift`). Shadowing is
  legal, so these were avoidable footguns rather than errors — but a
  library that quietly captures a name its neighbours already own is a
  bad neighbour.
- **`arpeggio` takes a `Note list`, not a `Chord`**, matching the
  decision `Scale` made: it cycles through whatever `Scale.tones` or
  `Scale.stack` produced.

Since shipping:

- ~~**The examples do not use it yet.**~~ **Done.** Both showcase
  projects are written against the full stack, and the pass turned out
  to shrink them without moving the audio much:

  - `examples/song/pad.synth` and `examples/darksynth/pads.synth` are
    `Score.chord` phrases laid end to end by `seq` and sounded by
    `play`, so a pad voice is `(freq, dur, vel) -> Sample` and its
    envelope plateau and sample window follow the written length instead
    of a hardcoded 3300 ms. The dark pad's shimmer octave became a
    second phrase rather than a fourth chord tone, because it runs a
    wider detune — one phrase means one voice.
  - `examples/song/keys.synth` is a `Score.melody` of eleven scale
    degrees (negative ones descend) at a `Level`, and
    `examples/song/strings.synth` is a `Score.arpeggio` over
    root · fifth · octave · fifth. The arpeggio reproduces the three
    hand-placed layers it replaced *exactly*, at a third of the length.
  - `examples/song/song.synth`'s kit is one bar of `Score.line` per
    part — `Rest` for the backbeat's gaps, `layer` for the accented
    eighth hats — `loop`ed over the song and sounded by `Score.strike`.
    That is the one place the note in a `Play` goes unread, which is
    what `strike` is for.
  - `examples/darksynth/song.synth` keeps its humanized grids (`Score`
    has no jitter, deliberately) but takes every pitch from the key: the
    bass, guitar and bell roots are cycle positions at three different
    floors, and the ten guitar lead runs are scale degrees rather than
    thirty frequencies that had to agree with the chords. Its
    *positions* went the same way in a second pass: `timing.synth` now
    holds the eight section landmarks as segment counts
    (`drop2 = seg 16`), and every start in the arrangement is a landmark
    plus a segment, a bar or a beat — `on Timing.drop2 3`,
    `Timing.tolling + Timing.beats 11.0`. Every `count` is a bar count
    times the hits a bar holds, and every riser is placed by working
    back from the arrival it announces and from its own length
    (`Fx.riser.to - Fx.riser.from`, because a `Sample` is an ordinary
    record). That took the file from 178 written-out durations to 39:
    13 `0s` sample origins, 14 humanizing `~spread`s and one echo time
    in milliseconds — microtiming, not musical position — and 11 in the
    wall-clock listening guide in the header comment. Not one position
    in the arrangement is a literal.

  `examples/lib/voices/pads.synth` lost `place4` with this: laying four
  chords end to end is `Score.seq`, not a library helper. The darksynth
  render is bit-identical apart from `guitar.synth`'s three detunes
  moving from ratios to cents (0.17 % of samples, ~89 dB down); the song
  render moved with the pad voicing, as the `Scale` section above
  records.

### Cross-cutting design constraints

Four properties of the language shape all six modules and should be
settled before any of them is written:

- **Record literals resolve by field set** (spec
  [§3](language-spec.md#type-declarations-records-variants-abstract-types)),
  and these modules will be `open`ed together. `Scale { root; quality }`
  and `Chord { root; quality; inv }` differ, so they disambiguate — but
  a user writing `{ root = ...; quality = ... }` while meaning a chord
  silently gets a `Scale`. The full field-name matrix across all six
  modules (plus `Sample`'s `{ sig; from; to }`) needs an audit, and
  `Chord` probably wants a distinct field name.
- **Constructors are not first-class**, so `List.map ~f:Play` is
  illegal and a user must write `fun n:Note -> Play (n, 1.0)`. The
  builder API is shaped around that: `Score.melody` and `Score.chord`
  exist precisely so the common cases never touch `Item`.
- **No mutual recursion.** Every traversal must be self-recursive or go
  through a local `go`, and the module dependency order
  (Pitch → Scale → Score) has to be a strict DAG. It is, but a later
  addition that wants `Scale` to consult `Score` would not be writable.
- **The 4096-call recursion limit** bounds phrase length. One level per
  event is well inside it for any realistic score, but `Score.seq` over
  thousands of phrases should fold rather than nest.

### Scope: what "sheet-like" deliberately excludes

`Phrase` is a lean note list with an algebra over it, not a notation
model. Ties, slurs, key signatures, repeat barlines, multi-staff layout,
and beaming are all out. They matter for *rendering staves*, which the
language does not do; composition needs none of them, and each one adds
a case to every combinator. The variants are extensible if that call
turns out to be wrong.

### Sequencing

1. ~~Timestamp arithmetic and the wider `List` module.~~ **Done** —
   nothing else compiles without them, and both are useful on their own.
2. ~~**`Pitch` + `Tempo`.**~~ **Done**, examples included: both
   showcase projects are written against the pair.
3. ~~**`Scale`.**~~ **Done.** (A `Rhythm` module of step patterns and
   Euclidean generators was proposed here and has been dropped from the
   plan — it was the only piece that would have needed C++, and it is
   not wanted.)
4. ~~**`Score`, dynamics included.**~~ **Done**, example rewrite
   included: `examples/song/` and `examples/darksynth/` are written
   against the full stack, each with a `harmony.synth` beside its
   `timing.synth`.

Each phase carries the same obligations as any Core work: the submodule
table in [`core-library.md`](core-library.md), the roster in the
language spec [§6](language-spec.md#6-primitive-signatures-v1-roster),
and coverage in `tests/test_checker.cpp` and `tests/test_build.cpp`.
Because these modules are written in SynthGraph, only a render pins
their *values* — the build tests must compare against hand-written
literal renders, as the `List` combinator tests do.

### Beyond the first pass

Three things the stack above makes cheap, none of them worth building
before it exists:

- **`Render.render_score`.** A piano-roll or staff SVG artifact
  alongside `render_vis`, reusing the existing artifact machinery
  exactly. It would make the "music sheet" framing literal, and a
  dependency-free SVG of a `Phrase` is viewable straight from a git
  host like the waveforms already are.
- **Tempo maps and rubato.** `Tempo` is one fixed BPM. A tempo *curve*
  (accelerando, ritardando, a click that drifts) is expressible as a
  `Scalar -> Scalar` rate over the timeline, which is precisely what
  `resample` already consumes — so the primitive exists and only the
  `Phrase`-level realization is missing. Keeping `Phrase` symbolic (in
  beats) is what leaves the door open.
- **Tunings beyond 12-TET.** `Pitch.cents` and `Pitch.just` cover
  microtonal detuning pointwise, but a first-class
  `type Tuning = { ref_hz : Scalar; edo : Int; offsets : Scalar list }`
  threaded through `Pitch.hz` would express Scala-style scales,
  well temperaments, and non-octave tunings as data rather than as
  per-call arithmetic.

## Build system & tooling

- **Native file-system events in the watch daemon.** Watching is
  polling-based in v1; inotify/FSEvents would cut latency and idle cost.
- **Finer-grained watch rebuilds.** The daemon rebuilds the whole
  project per change (acceptable in v1 because target- and sample-level
  caching make the rebuild cheap); front-end work could also be scoped
  to the changed module's dependents.
- **Cache tuning knobs.** Caching is fully automatic and bounded by
  construction; there are deliberately no user-facing controls yet
  (size limits, pinning, on-disk persistence of sample windows across
  daemon restarts).
- **Richer LSP, continued.** The server now covers diagnostics,
  completion, go-to-definition, hover, find-references, rename, document
  outline, and formatting; still open are signature help, semantic
  tokens, workspace-wide symbol search, renaming modules and labeled
  parameters (a labeled parameter's name is call-site syntax, so its
  rename must rewrite every `~label:` argument too), and a formatter
  that re-flows lines rather than only normalizing spacing within the
  author's layout.
- **Dev app playback features.** The artifact browser plays targets
  start-to-finish; seeking, looping, A/B between builds, and inline
  waveform display (the data already exists for `render_vis`) would make
  it a better auditioning tool.
- **Native extensions beyond build-time externals.** User externals run
  at build time on build-time data only; letting user C++ participate in
  *signal* processing (custom DSP nodes) is a separate, larger feature
  with caching and determinism obligations.
