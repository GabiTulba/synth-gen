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

Six proposed submodules close that gap, in two layers. Most are
**written in SynthGraph**, following the `List` precedent: `Math.pow`
already gives equal temperament, records and recursive variants give the
data structures, and `let rec` gives the traversals. Only `Rhythm.parse`
genuinely needs C++, because the language has no string operations.

| Module | Owns | Depends on | Written in |
|---|---|---|---|
| `Core.Pitch` | Notes, temperaments, cents | — | SynthGraph ✅ |
| `Core.Tempo` | BPM, meters, note values, bar/beat → Timestamp | Time | SynthGraph |
| `Core.Scale` | Keys, modes, scale degrees, chords, inversions | Pitch, List | SynthGraph |
| `Core.Rhythm` | Step patterns, Euclidean rhythms, swing | Tempo, List | C++ + SynthGraph |
| `Core.Score` | Phrases, events, articulation, `play` | all of the above | SynthGraph |
| `Core.Dyn` | Dynamic levels, crescendo, decibel gain | — | SynthGraph |

**Landed so far:** Timestamp arithmetic (language spec
[§3](language-spec.md#operators-pointwise-lifting--scalar-broadcasting)),
the wider `List` module, and `Core.Pitch`
([§6](language-spec.md#6-primitive-signatures-v1-roster)). The remaining
five modules are future work.

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

- **The examples still hold raw frequencies.** Rewriting
  `examples/song/keys.synth` is phase 2's acceptance test and wants
  `Tempo` as well, so the pitches and the grid move together rather than
  in two passes. `examples/song/pad.synth`,
  `examples/song/strings.synth`, `examples/darksynth/song.synth` and the
  `1.006` in `examples/darksynth/bass.synth` (which is
  `detune ~cents:10.4`) are all waiting on the same rewrite.
- **`Note` is inherently 12-tone**, so `shift`/`flat`/`hz` suit
  12-division temperaments; `n /= 12` ladders work in `Int` steps
  through `step_hz`. A spelling that generalizes to arbitrary `n` would
  need a different note type, and it is not clear one is worth it.
- **Scale-degree spelling** (whether `Cs` and `Df` should be
  distinguishable) stays out until something needs to render notation.

### `Core.Tempo`

```
type Meter = { beats : Int; unit : Int } ;;      (* 4/4 = { beats = 4; unit = 4 } *)
type Tempo = { bpm : Scalar; meter : Meter } ;;  (* bpm counts `unit` notes per minute *)

type Value =                                     (* recursive variant *)
  | Whole | Half | Quarter | Eighth | Sixteenth | ThirtySecond
  | Dotted of Value
  | Tuplet of (Int, Int, Value) ;;               (* 3-in-2, 5-in-4, ... *)

val Tempo.beat  : t:Tempo -> Timestamp
val Tempo.bar   : t:Tempo -> Timestamp
val Tempo.beats : t:Tempo -> n:Scalar -> Timestamp
val Tempo.value : t:Tempo -> v:Value -> Timestamp
val Tempo.at    : t:Tempo -> bar:Int -> beat:Scalar -> Timestamp
val Tempo.grid  : t:Tempo -> start:Timestamp -> step:Scalar -> count:Int
                -> Timestamp list
val Tempo.swing : amount:Scalar -> steps:Timestamp list -> Timestamp list
```

The highest-value module of the six, and the one Timestamp arithmetic
was added for. `Tempo.grid` replaces
`time_steps ~start:2s ~step:500ms ~count:28` with a call that names the
tempo, so re-tempoing a song is a one-line edit instead of recomputing
every literal across five files.

`Value` is where the type system earns its keep: as a recursive variant,
`Dotted (Dotted Quarter)` and nested tuplets fall out of a single
`let rec`, and `match` exhaustiveness catches a missing case at build
time.

`bpm` counts the meter's `unit` note per minute. That is unambiguous for
simple meters and a documented convention for compound ones — 6/8 felt
in two is `{ beats = 6; unit = 8 }` with `bpm` still counting eighths;
the dotted-quarter pulse is `value (Dotted Quarter)`.

`swing` complements the existing `jitter`: `jitter` humanizes by hashing,
`swing` displaces every odd step by a fixed proportion. Both are pure
and both clamp at the epoch.

**Open decision — the indexing base of `at`.** Musicians count bars and
beats from 1; the rest of the language counts from 0 (`List.init` yields
`0..n-1`, `List.range ~from:0`). Consistency argues for 0-based, and
mixed conventions inside one project are a durable footgun. This is a
taste call that is painful to reverse once examples are written against
it, so it should be settled before `Tempo` lands.

### `Core.Scale`

```
type Quality = | Major | Minor | Dorian | Phrygian | Lydian | Mixolydian
               | Locrian | HarmMinor | MelMinor | PentMajor | PentMinor
               | Blues | WholeTone | Chromatic ;;
type Scale = { root : Note; quality : Quality } ;;

val Scale.offsets : q:Quality -> Int list        (* semitones from the root *)
val Scale.degree  : s:Scale -> n:Int -> Note     (* wraps octaves; n<0 descends *)
val Scale.notes   : s:Scale -> from:Int -> count:Int -> Note list
val Scale.snap    : s:Scale -> note:Note -> Note (* nearest in-key note *)

type ChordQuality = | Maj | Min | Dim | Aug | Maj7 | Min7 | Dom7
                    | HalfDim7 | Dim7 | Sus2 | Sus4 | Add9 ;;
type Chord = { root : Note; quality : ChordQuality; inv : Int } ;;

val Scale.tones   : c:Chord -> Note list
val Scale.freqs   : c:Chord -> Scalar list       (* straight into channels/mix_all *)
val Scale.triad   : s:Scale -> degree:Int -> Chord      (* diatonic *)
val Scale.seventh : s:Scale -> degree:Int -> Chord
val Scale.invert  : c:Chord -> n:Int -> Chord
val Scale.voicing : c:Chord -> low:Note -> count:Int -> Note list
```

`Scale.degree` is the workhorse: a melody written as degree indices
stays in key by construction, and transposing the whole song is editing
`root`. `Scale.freqs` lands directly in the existing chord-stack idiom,
so the `Am → F → C → G` of `examples/song/pad.synth` becomes
`List.map ~f:(Scale.triad key) [0; 5; 2; 4]`.

`Quality` and `ChordQuality` are spelled differently (`Major`/`Maj`) so
both can be `open`ed at once without a constructor collision.

### `Core.Rhythm`

```
val Rhythm.parse   : pat:String -> (Scalar, Scalar) list   (* (step offset, velocity) *)
val Rhythm.euclid  : pulses:Int -> steps:Int -> rotate:Int -> Bool list
val Rhythm.offsets : hits:Bool list -> Scalar list
val Rhythm.every   : n:Int -> steps:Int -> Bool list
```

`parse` reads TR-808 grid notation — `"X..x..X.x..x..X."`, where `X` is
an accent, `x` a normal hit and `.` a rest — into step offsets with
velocities. It is the one function here that must be C++, since the
language has no string operations, and it is perhaps thirty lines. The
payoff is that a kit's entire rhythmic identity becomes four readable
string literals.

`euclid` (Bjorklund's algorithm) generates the Afro-Cuban and Balkan
rhythm families from two integers. It is cheap to implement and is
exactly the sort of thing a *programming* language for music should make
easy in a way a DAW does not.

### `Core.Score`

The sheet-music structure, in two representations on purpose:

```
(* Symbolic: positions in beats, tempo-independent *)
type Step   = { note : Note; at : Scalar; len : Scalar; vel : Scalar } ;;
type Phrase = { steps : Step list } ;;

(* Realized: positions in real time, ready to place *)
type Event  = { note : Note; at : Timestamp; dur : Timestamp; vel : Scalar } ;;

type Item = | Play of (Note, Scalar) | Rest of Scalar ;;
```

Keeping `Phrase` in beats rather than Timestamps means one phrase plays
at any tempo, and leaves room for tempo maps later without a breaking
change. `Rest` needs its own constructor because v1 has no literal
patterns, so a rest cannot be a sentinel value matched on.

```
(* builders *)
val Score.line      : items:Item list -> Phrase           (* laid end to end *)
val Score.melody    : notes:Note list -> len:Scalar -> Phrase
val Score.chord     : notes:Note list -> at:Scalar -> len:Scalar -> Phrase
val Score.arpeggio  : c:Chord -> step:Scalar -> count:Int -> Phrase

(* a phrase algebra *)
val Score.length    : p:Phrase -> Scalar
val Score.seq       : ps:Phrase list -> Phrase            (* one after another *)
val Score.stack     : ps:Phrase list -> Phrase            (* simultaneous *)
val Score.repeat    : p:Phrase -> n:Int -> Phrase
val Score.shift     : p:Phrase -> beats:Scalar -> Phrase
val Score.transpose : p:Phrase -> semitones:Int -> Phrase
val Score.in_key    : p:Phrase -> s:Scale -> Phrase       (* snap every note *)
val Score.staccato  : p:Phrase -> ratio:Scalar -> Phrase
val Score.legato    : p:Phrase -> Phrase
val Score.velocity  : p:Phrase -> f:(Scalar -> Scalar) -> Phrase

(* the bridge to audio *)
val Score.realize : t:Tempo -> p:Phrase -> Event list
val Score.play    : voice:(Scalar -> Timestamp -> Scalar -> 'a Sample)
                  -> events:Event list -> 'a Signal
val Score.strike  : voice:(Scalar -> 'a Sample)           (* fixed-length: percussion *)
                  -> events:Event list -> 'a Signal
```

`Score.play` is the single highest-value function in the proposal. A
*voice* becomes a function of `(freq, duration, velocity) -> Sample`,
and `play` places and mixes — which is what turns the hand-placed
`mix_all [...]` block of `examples/song/keys.synth` into:

```ocaml
let piano freq:Scalar dur:Timestamp vel:Scalar : Scalar Sample =
  K.strike freq
    * adsr ~attack:4ms ~decay:600ms ~sustain:0.25 ~release:350ms ~hold:dur
    * vel
  |> sample ~from:0s ~to:(dur + 350ms) ;;

let melody : Scalar Signal =
  Score.melody ~notes:(List.map ~f:(Scale.degree key) [0; 2; 4; 2; 0; -1; 0])
               ~len:1.0
    |> Score.shift ~beats:8.0
    |> Score.realize ~t:tempo
    |> Score.play ~voice:piano ;;
```

Note the duration-parameterized voice — `~hold:dur` and a window of
`dur + release`. That is what a note-length-aware library buys over
pre-baked fixed-length samples, and it is the reason Timestamp
arithmetic had to land first.

Like `place_multi`, `play` sums its placements **without
normalization**; headroom stays the composer's to manage, and the docs
should say so where they say it for `place_multi`.

`Score.strike` exists because percussion has no meaningful pitch or
duration — forcing a drum voice through the three-argument shape would
be ceremony.

### `Core.Dyn`

```
type Level = | Ppp | Pp | P | Mp | Mf | F | Ff | Fff ;;

val Dyn.amp  : l:Level -> Scalar                  (* Mf -> 0.5, ~6 dB per step *)
val Dyn.ramp : from:Level -> to:Level -> n:Int -> Scalar list
val Dyn.db   : x:Scalar -> Scalar                 (* decibels -> linear gain *)
```

Small enough to fold into `Score` if the module count wants holding
down. `Dyn.db` is worth having either way: gain expressed in decibels is
missing from Core generally, not just from songwriting.

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
2. **`Pitch`** ✅ **+ `Tempo`.** `Pitch` has shipped; `Tempo` is next,
   and rewriting `examples/song/keys.synth` against the pair is the
   acceptance test.
3. **`Scale` + `Rhythm`.** Rewrite `examples/song/pad.synth` and
   `examples/song/drums.synth`.
4. **`Score` + `Dyn`.** Rewrite `examples/song/` and
   `examples/darksynth/` against the full stack — a diff that should
   shrink both substantially and read like music.

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
