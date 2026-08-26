# Core library review: refactorings, extensions, and language gaps

**Status: implemented.** Every proposal in this document has landed —
A1–A4, B1–B10, C1–C6, D1–D3 — with the current state documented in
[`core-library.md`](core-library.md) (including the release-notes name
list) and the language spec; the roadmap entries this review subsumed
have left [`roadmap.md`](roadmap.md), and the open questions it
deliberately did not close (portamento's player shape, tempo maps'
`realize_map`, channel arity in types) are recorded there. Small
implementation notes against the text below: `Scale.prog_degree`
shipped alongside the `prog_*` family as the wrapped lookup they
compose from; the named `Dom13` omits the 11th (spell the full stack
with `Shape`); `Fx.resonant` clamps its cutoff near rate/6 for
stability; `Mix.vca` shipped directly as `input * gain` because D3
landed in the same round; D2 shipped return-type and `let ... in`
inference (`let rec`, externals and destructuring keep annotations);
and D1's tail-call elimination is general (any tail call, not just
self), with the depth guard now counting only nested calls. The
document is kept as the review that motivated the round.

It is the companion to [`roadmap.md`](roadmap.md): where the roadmap
lists items already accepted as future work, this document is a full
review of the `Core` library and its documentation against the way the
examples actually use them, and it proposes changes. Each proposal is
labeled — **A** (refactor what exists), **B** (extend the library, no
engine changes), **C** (extend the library, engine work required),
**D** (language-level gaps) — and the final section ranks them. Where a
proposal overlaps a roadmap entry, it says so and adds the concrete
design the roadmap left open.

Everything here was derived from reading the full documentation set
(`language-spec.md`, `core-library.md`, `language-tour.md`, `engine.md`,
`build-system.md`, `roadmap.md`), the library itself
(`stdlib/core/lib.synth` and the `stdlib/core/*.cpp` implementations,
plus the `src/ext/synth/engine.hpp` constructor surface externals can
reach), and every example project (`pluck`, `primitives`, `basic`,
`advanced`, `lib/voices`, `lib/effects`, `song`, `darksynth`).

---

## 1. How Core is actually used

### 1.1 The three tiers

The examples settle into three tiers of use, and the boundaries between
them are where the friction lives:

1. **The signal tier** — oscillators, envelopes, filters, arithmetic.
   Sound design happens here (`basic/*`, `lib/voices`, the darksynth
   instrument modules), and it works well: partial application, pipes,
   and the operator table carry it. The gaps at this tier are *missing
   vocabulary* (no trig, no phase, no resonance, no cutoff modulation),
   not missing structure.

2. **The grid tier** — `time_steps`/`jitter`/`place_multi` driven by
   `Tempo`. The whole darksynth arrangement is written here: 13 of its
   rhythm lines go through `Seq.pattern`/`Seq.humanized`, a *user
   library* wrapper (`examples/lib/voices/seq.synth`) that every project
   would have to re-write, because Core supplies the three pieces but
   not their composition.

3. **The score tier** — `Scale`/`Score` phrases realized through a
   tempo and tuning. The `song` example lives here and reads beautifully
   for melody, chords and duration-aware voices. But darksynth — the
   most demanding piece in the repository — uses it **only for the
   pads**, and its drums, bass, guitar and bells all drop down to the
   grid tier. The reason is structural, not stylistic, and it is the
   single most important finding of this review (§2, F2): *the feel
   transforms (`jitter`, `swing`) exist only on Timestamp lists, while
   duration-aware voices exist only behind `realize`* — so any part that
   wants both humanization and per-note lengths has no tier to live on.

### 1.2 Utilization: what shipped vs. what got used

Counting real call sites across all examples (comments excluded):

| Heavily used | Used | **Never used by any example** |
|---|---|---|
| `sine` `saw` `square` `noise` `fm` `exp_decay` `adsr` `lowpass` `highpass` `soft_clip` `hard_clip` `mix_all` `channels` `sample` `place` `place_multi` `render` `render_vis` `time_steps` `jitter` `List.map` `List.init` `List.repeat` `List.nth` `detune` `hz` `et12` `degree` `triad` `voicing` `value` `beats` `bar` `grid` `common` `melody` `chord` `line` `seq` `layer` `loop` `move` `realize` `play` `strike` `span` `wrap_rem` | `pm` `am` `delay` `reverb` `resample` `render_stems` `render_vis_stems` `signal` `signal_multi` `constant` `constant_multi` `time` `pow` `exp` `sqrt` `log` `to_scalar` `to_ms` `to_min` `at` `arpeggio` `staccato` `velocity` `amp` `stack` `seventh` `tones` `shift` `of_step` `step` `a440` `cents` `sum` `length` `wrap_div` | `swing` · `legato` · `in_key` · `transpose` · `snap` · `invert` · `ramp` · `Pitch.flat` · `step_hz` · `et` · `just` · `pyth` · `ratio` · `to_cents` · `List.fold` · `filter` · `zip` · `concat` · `flat_map` · `append` · `rev` · `range` · `maximum` · `load_mono` · `load_multi` |

Three readings of the right-hand column:

- **Some entries are fine.** `List.fold`/`zip`/`filter` are load-bearing
  *inside* `lib.synth`; the alternative temperaments are a deliberate
  breadth play; `to_cents`/`ratio` are inverses kept for completeness.
- **Some entries are unused because they are unusable.** `ramp` produces
  a `Scalar list` of gains, but nothing in `Score` consumes a gain list —
  applying a crescendo to a phrase requires hand-zipping `p.steps`
  against it through record literals. `swing` displaces a Timestamp
  list, but the parts that want swing are exactly the parts written as
  phrases (F2). These are shipped features with a missing last step,
  and §B5 closes them.
- **One entry is a coverage hole.** `Io` has *zero* exercise anywhere —
  no example imports audio, and no `.wav` input exists in the repo. The
  sampling workflow (load → slice → `resample` → place) is the least
  validated corner of the library and deserves a worked example
  regardless of any proposal below.

---

## 2. The recurring frictions (evidence)

Each friction is stated once here and referenced by number from the
proposals.

**F1 — The nine-line `open` preamble.** Every example file opens the
same block: `open Core` plus `Core.Osc`, `Fx`, `Arrange`, `Render`,
`Io`, `Time`, `Sig`, `Math` — even files that use almost none of them
(`darksynth/harmony.synth` opens all nine and uses three; it is pure
pitch arithmetic). Thirty-plus files carry this copy-paste block. The
opens are cheap for the compiler but expensive for the reader: they say
nothing about what the file actually touches. → A1, plus a lint aid.

**F2 — Two rhythm systems that cannot meet.** `Time.jitter` and
`Tempo.swing` humanize *Timestamp lists*; `Score.play` gives voices
*written durations and velocities*. A part cannot have both. Darksynth
resolves it by abandoning `Score` for every humanized part — its bass
`pump`/`sixteenths`/`eighths` grooves re-derive chord frequencies by
hand (`hz_of n = bass_note (Scale.wrap_rem ~n:n ~k:4)`,
`examples/darksynth/song.synth:184`) precisely because they need
`Seq.humanized`, which only speaks Timestamp lists. Meanwhile `swing`
has zero users: the grid parts that could take it are drum grids where
straight time was chosen, and the phrase parts that want shuffle can't
reach it. → B5 (`Score.humanize`, `Score.shuffle`), enabled by B1
(`Math.hash`).

**F3 — Every project rebuilds the same timing/harmony scaffolding.**
`song/timing.synth` and `darksynth/timing.synth` both define: wrappers
that fix `~t` (`dur`, `beats`, `at`, `grid`), a bars→beats bridge for
Score (`let span n:Int : Scalar = Math.to_scalar (n * tempo.meter.beats)`
— *identical line* in both files, `song/timing.synth:32`,
`darksynth/timing.synth:37`), and hand-derived per-bar hit counts
(`quarters`/`halves`/`eighths`/`sixteenths` off `meter.beats`,
`darksynth/timing.synth:42-45`, correct for simple meters only — the
roadmap already flags this as `Tempo.per_bar`). `song/harmony.synth` and
`darksynth/harmony.synth` both define: a degree cycle with wrapping
lookup (`chord_degree`), a re-rooted key (`octave ~oct`), voiced chords
over a register floor (`parts ~i ~low ~count`), and — most tellingly — a
*register fold* implemented by abusing `voicing ~count:1` + `List.nth`
to pull one note into the octave above `low` (`root ~i ~low`, both
files). ~80 lines of near-identical infrastructure per project is the
library telling us what it is missing. → B2, B3, B4.

**F4 — Stereo is entirely manual, and there is no pan.** Every bus in
both songs is `channels [x * gL; x * gR]` with hand-balanced gain pairs
(`song/song.synth:75-81`, `darksynth/song.synth:402-425`); "bells lean
right, chains lean left" is expressed as eight coupled literals. The
Doppler library hand-rolls a linear pan law
(`examples/lib/effects/doppler.synth:50-52`), and `primitives.synth`
demos `signal_multi` with a hand-written crossfade. Nothing in Core
places a mono source in a stereo field. → B6; C4 for the missing inverse
(`channels` has no way back out of a `Vector Signal`).

**F5 — Mixing has no per-part gain, and dynamics live in the wrong
place.** Mixes are written `intro * 0.8 + build + drop1 + breakdown * 0.8`
(`darksynth/song.synth:256`) — fine at small scale, but `mix_all` takes
no weights, and the only dB vocabulary in the library (`Score.db`,
`Score.amp`) is namespaced under *scores*, invisible to the mixing code
that wants it. → B6.

**F6 — Percussion through `Score` needs a dummy note.** `song/song.synth:38`:
`let hit : Note = Harmony.key.tonic ;;` exists only so `melody`/`line`
can be fed something for `strike` to ignore. Accenting a hat line
requires *layering two phrases at different levels displaced by half a
beat* (`song/song.synth:50-56`) because neither `melody` nor `line`
carries velocity, and `velocity ~f` maps uniformly — there is no
per-step velocity path. This is also why `ramp` is unusable (§1.2).
→ B5 (`rhythm`, `hits`, `vels`).

**F7 — A cosine costs a hard-coded π/2 through `pm`.**
`examples/lib/effects/doppler.synth:27`:
`pm ~carrier:rate ~modulator:(constant 1.5707963)` is the only way to
get a phase-offset oscillator, because `Math` has no `sin`/`cos`/`pi`
and oscillators have no phase parameter. The same absence means
`signal ~f` (whose body may only use arithmetic and Math primitives)
cannot express any trigonometric shape, and an equal-power pan curve
has no natural spelling. → B1, C5.

**F8 — Filter parameters are frozen at graph construction.** `lowpass`/
`highpass` take `cutoff:Scalar`; nothing in the library modulates a
filter over time. The darksynth riser — the genre's canonical *filter*
sweep — had to be built as an FM sweep plus noise swell instead
(`darksynth/fx.synth:17-24`). There is also no resonance anywhere in the
library, which walls off entire electronic idioms (acid lines, wubs,
formant-ish sweeps). This is the largest *sound* gap the examples
reveal. → C1.

**F9 — A mono control signal cannot scale a stereo bus.** The operator
table has `t Signal ⊗ t Signal` and `t Signal ⊗ Scalar`, but not
`Scalar Signal ⊗ Vector Signal`. Fading out a `Vector` master with an
envelope, or ducking a stereo pad under a kick, has no direct spelling —
the workaround is `am ~modulator:(env - 1.0) ~depth:1.0` (which computes
`carrier * env` through the AM formula) or restructuring the graph to
apply gain per-channel before `channels`. `am`'s mono-modulator
broadcast rule is the precedent that this is wanted. → D3 (operator
rule), B6 (`vca`/`duck` in the meantime).

**F10 — The voice-window arithmetic is repeated in every voice.** Every
duration-aware voice computes some variant of
`adsr ~hold:(dur - release)` or `~hold:dur` and
`sample ~from:0s ~to:(dur + release)` — with *two different conventions*
across the examples for whether `dur` includes the release tail
(`song/keys.synth:29-35` vs. `song/pad.synth:33-40`). → B10 (`Fx.gated`),
plus a documentation note fixing one convention.

**F11 — Echo stacks are hand-unrolled.** The
`dry + delay¹·g + delay²·g² …` pattern appears three times
(`darksynth/song.synth:169-171`, `:373-378`,
`primitives/primitives.synth` delay demo). → B10 (`Fx.echoes`).

**F12 — Programmatic render names are impossible.** `render_stems`
concatenates `"<name>-<label>"` *in C++*, but the language cannot: there
is no string operation of any kind. Rendering one artifact per section
of a song (the natural audition loop for iterating on an arrangement) is
therefore impossible — target names must be unique, and names cannot be
computed. → B8.

**F13 — Dense scores can hit the 4096-call ceiling.** All List/Score
combinators recurse once per element against `kMaxApplyDepth = 4096`
(`src/eval.cpp:479`). The C++-side builders (`time_steps`, `jitter`,
`place_multi`) are immune, but the Score path is not: `realize` and
`play` each `List.map` over every step, so a *single phrase* of ≥4096
steps fails the build. At 120 BPM in 4/4 that is one sixteenth-note
line sustained for ~8.5 minutes — inside the range of real techno/ambient
pieces, and `seq`/`loop` currently do O(n·m) repositioning work on top
(each `seq` level re-`move`s the entire suffix). Musical lists "sit far
under" the limit today because the examples are 90–120 s long. → A3, D1.

**F14 — `Quality` and `ChordQuality` are closed.** `offsets` and `shape`
are exhaustive matches over fixed enums, and `Scale`/`Chord` require a
constructor from those enums. A custom scale — harmonic major, double
harmonic, Hirajoshi, an octatonic, a maqam approximation, anything the
fourteen shipped qualities don't cover — **cannot be expressed at all**,
short of bypassing `Scale` entirely and hand-writing `Note list`s
(losing `degree`, `snap`, `stack`, `triad`, and `in_key`). Same for
chord vocabularies beyond the twelve shipped shapes (no 6ths, 9ths,
11ths, 13ths, no minMaj7 *as a name*). This is the hardest
"non-standard songs impossible to represent" wall in the current
library. → B4.

---

## 3. Representability audit

The direct answer to "what can't be said, and what is merely painful."
**Blocked** = not expressible today; **Awkward** = expressible with
significant ceremony; **OK** = well served.

| Technique | Status | Today / proposal |
|---|---|---|
| Custom scales & modes (harmonic major, octatonic, Hirajoshi, …) | **Blocked** | `Quality` is a closed enum (F14) → B4 |
| Extended/named jazz chords (6, 9, 11, 13, mMaj7…) | **Blocked** (as names) | hand-built `Note list`s only → B4 |
| Resonant filter sweeps, acid/wub | **Blocked** | no resonance, no cutoff modulation (F8) → C1 |
| Value-level randomness (generative choices, humanized velocity/detune) | **Blocked** | `jitter`'s hash is locked in the time domain; Timestamps deliberately never convert back to Scalar, so its randomness cannot be extracted → B1 `Math.hash` |
| Programmatic target names (per-section renders) | **Blocked** | no string ops (F12) → B8 |
| Per-channel processing of imported stereo audio | **Blocked** | `Vector Signal` has no channel extraction → C4 |
| Portamento/glide between successive melody notes | **Blocked** | a voice sees one `freq` with no note context → B5 (open design) |
| Quarter-tones / per-note inflection inside a phrase | **Blocked** | `Step` has no fractional pitch; `Note` is 12-tone → B5 (`bend`) |
| True sidechain compression (program-dependent) | **Blocked** | needs envelope follower + signal-level control (roadmap) → C2 |
| Feedback delays, flangers, Karplus–Strong | **Blocked** | roadmap (feedforward only) → C3 |
| Reverse playback / scrubbing | **Blocked** | roadmap (`resample` reads forward) |
| Swing/humanize on a `Score` phrase | **Awkward→Blocked** | only on Timestamp lists (F2) → B5 |
| Crescendo / per-note dynamics | **Awkward** | `ramp` has no consumer; hand-zip steps (F6) → B5 |
| Sidechain-style pumping on a fixed grid | **Awkward** | expressible as `am ~modulator:(place_multi dip kicks - …)` tricks; worth canonizing → B6 `duck` |
| Mixed meters / metric modulation | **Awkward** | second `Tempo` + computed offsets; workable, verbose → B3 `marks` helps |
| Tempo curves, rubato, accel/rit | **Awkward** | roadmap; `resample` covers audio-side warp, `Phrase` realization missing → B5 sketch |
| Non-octave & n≠12 EDO tunings | **OK / Awkward** | `Tuning.octave` + `step_hz` are genuinely good; but the `Score` tier can't reach them (`Step` holds 12-tone `Note`s) → B5 `realize_with` |
| Polyrhythm & polymeter | **OK** | two `Tempo`s / interleaved grids compose cleanly |
| Odd tuplets, nested tuplets, dotted values | **OK** | `Value` is a small triumph — recursive, exhaustive, exact |
| Euclidean/world rhythm patterns | **Awkward** | writable via `List` recursion; common enough to ship → B7 |
| Just intonation with key-dependent color | **OK** | `Tuning.root` design is exactly right (and un-exercised: no example uses `just`/`pyth` — worth one) |
| Drone/spectral/additive | **OK** | additive stacks + `signal ~f`; trig absence pinches (F7) → B1 |
| Musique concrète / sampling | **Untested** | `Io` + `resample` exist with zero example coverage (§1.2) |

---

## 4. Proposals A — refactor what exists

### A1. `Core.Dsp`: a prelude for the signal tier (F1)

Add one alias module to `lib.synth` re-exporting the sound-design
working set under bare names — the oscillators, envelopes, filters,
clips, `mix_all`/`channels`/`sample`/`place`/`place_multi`, the `Sig`
constructors, `render`/`render_vis`, `to_sec`/`to_ms`/`to_min`, and the
Math family — so the standard preamble becomes:

```
open Core
open Core.Dsp
```

Mechanics: eta-expanded aliases inside `module Dsp = struct … end`
(`let lowpass ~cutoff:Scalar ~input:'a Signal : 'a Signal =
Fx.lowpass ~cutoff:cutoff ~input:input ;;` and so on) — no language
change, labels preserved, ~60 lines written once in Core. The
submodules stay the canonical, documented homes; `Dsp` is a view. The
tour keeps teaching `open Core.Osc` first so the structure stays
learnable, and `Dsp` is introduced as the working-file idiom.

Deliberately *excluded* from `Dsp`: `Io` (rare), `render_stems`
variants (arrangement-file vocabulary), and all of
`Pitch`/`Tempo`/`Scale`/`Score` (their `open`s are meaningful
decisions — the name-collision management described in
`core-library.md` depends on opening them consciously).

Companion tooling change: `synthc lint` should warn on an `open` that
binds nothing the file uses. That, more than the prelude, is what stops
the nine-line block from being cargo-culted forward (F1's
`harmony.synth` case).

### A2. Promote the sequencing idiom into Core (F2, tier 2)

`Voices.Seq` (`examples/lib/voices/seq.synth`) is generic infrastructure
living in an example: `pattern` (hit × grid) and `humanized` (hit × grid
× jitter) are how *all thirteen* darksynth rhythm lines are written. Move
the idiom into a new `Core.Groove` module (name avoids `Seq` vs.
`Score.seq` confusion):

```
val Groove.pattern   : hit:'a Sample -> steps:Timestamp list -> 'a Signal
val Groove.humanized : hit:'a Sample -> steps:Timestamp list
                         -> seed:Scalar -> spread:Timestamp -> 'a Signal
```

Note the shape change from `Voices.Seq`: take the **step list**, not
`~start ~step ~count`. The example version bakes in arithmetic grids;
taking the list keeps it composable with `Tempo.grid`, `swing`,
`jitter`, masks, and Euclid patterns (B7) without a parameter explosion
— `Groove.pattern kick (grid ~t ~from ~step:Quarter ~count:32)` reads
as well as the current call and generalizes. (`pattern` is then a
one-line synonym for `place_multi` with the arguments flipped for
pipes; that is fine — the point is the named pair, and `humanized`
carries real logic.)

The `Voices` library keeps thin deprecated wrappers for one release of
the examples, then drops them.

### A3. Make `Score.seq`/`loop` accumulate instead of re-walking (F13)

`seq` currently rebuilds the suffix at every level —
`Cons (p, rest) → p.steps ++ (move (seq rest) (span p)).steps` — so
each step is re-positioned once per preceding phrase: O(n·m) work, and
`loop ~n` inherits O(n²) in total steps. Rewrite as a left fold carrying
`(offset, steps-so-far)`:

```
let seq ~ps:Phrase list : Phrase =
  let step acc:(Scalar, Step list) p:Phrase : (Scalar, Step list) =
    let (off, ss) : (Scalar, Step list) = acc in
    (off + span ~p:p, List.append ~xs:ss ~ys:(move ~p:p ~beats:off).steps) in
  let (off, ss) : (Scalar, Step list) =
    List.fold ~f:step ~init:(0.0, Nil) ~xs:ps in
  { steps = ss } ;;
```

Same observable results (the build tests that pin `seq` by rendered
values must keep passing unchanged), linear work, and recursion depth
becomes one `fold` frame per *phrase* rather than per phrase **plus**
the nested `move` maps. `legato` has a similar O(n²) inner fold
(next-attack search per step); acceptable at musical sizes, worth a
comment stating so. This does not lift the depth ceiling — that is D1 —
but it removes the quadratic constant that makes long `loop`s expensive
before the ceiling is even reached.

### A4. Documentation restructure

The information architecture splits each primitive across two documents:
signatures in spec §6, semantics in `core-library.md`'s bullet list —
and the bullets have outgrown the format (the
`Pitch`/`Tempo`/`Scale`/`Score` notes are now ~150 lines of prose one
long unordered list deep). Proposal:

- Give `core-library.md` one **subsection per submodule** with the
  signatures inlined next to their semantics (the spec keeps the
  canonical roster; duplication is mechanical and `lib.synth` is the
  single source of truth for both).
- Add an **"idioms" section per tier**: the voice-window convention
  (F10 — pick and document *one*: "`dur` is the sounding length; windows
  are `dur + release`"), the ducking pattern (F9/B6), the
  section-landmark pattern (`timing.synth`), and a pointer from every
  primitive to its `primitives.synth` render.
- Document what is currently discoverable only by reading C++:
  **`adsr`'s segments are linear** (`src/signal.cpp:445` — musically
  relevant; a note on faking exponential decays with `exp_decay`
  products belongs here), `sample` rejects inverted windows, negative
  computed Timestamps **clamp to `0s` silently** (a placement computed
  as `at - lead - length` near the origin moves rather than errors —
  worth a caution box), and `time_steps` caps counts at 1,000,000.
- The `Io`/`resample` sampling workflow gets a worked example (§1.2's
  coverage hole) — this is an examples change, but the doc should link
  it.

---

## 5. Proposals B — library extensions (no engine changes)

Everything in B is implementable in SynthGraph in `lib.synth` or as a
value-level external in the existing `.cpp` files, following the
four-edit process (`core-library.md`: definition, submodule table, spec
§6 roster, checker + build tests).

### B1. `Math`: the missing quarter (F7, and the randomness wall)

```
val Math.pi    : Scalar                            (* 3.14159265358979312 *)
val Math.sin   : x:'a -> 'a
val Math.cos   : x:'a -> 'a
val Math.tan   : x:'a -> 'a
val Math.atan  : x:'a -> 'a
val Math.abs   : x:'a -> 'a
val Math.min   : a:Scalar -> b:Scalar -> Scalar
val Math.max   : a:Scalar -> b:Scalar -> Scalar
val Math.clamp : lo:Scalar -> hi:Scalar -> x:Scalar -> Scalar
val Math.lerp  : a:Scalar -> b:Scalar -> t:Scalar -> Scalar
val Math.hash  : seed:Scalar -> i:Int -> Scalar    (* uniform [0, 1) *)
```

- `sin`/`cos`/`tan`/`atan`/`abs` follow the `exp`/`sqrt`/`log` pattern
  (polymorphic over Scalar/Vector/Signal). The Scalar/Vector cases are
  pure `math.cpp` additions; the Signal-elementwise case needs four rows
  in `SigUnaryOp` — see C5, a few dozen engine lines. If C5 waits,
  shipping the Scalar/Vector half first is still worth it (pan laws,
  phase math, `signal ~f` bodies — `fnOfTime`'s symbolic substitution
  means every new Math primitive automatically extends what `signal ~f`
  can express).
- `pi` is a plain constant. The "presets are functions" rule exists
  because a paramless definition is evaluated on every build of every
  project — evaluating one number literal is free, and `pi` as a
  function would poison every arithmetic expression it appears in.
- `abs`/`min`/`max`/`clamp`/`lerp` are the four-line functions everyone
  writes inline today (`Scale.snap` hand-rolls `abs` twice in its
  `closer` fold; every `if x > acc then x else acc` in `lib.synth` is
  `max`).
- **`hash` is the important one.** It is `jitter`'s splitmix64
  (`stdlib/core/lists.cpp`) exposed as a value: pure, platform-stable,
  seeded — determinism and cacheability fully preserved. Today the
  library's only randomness is imprisoned in the time domain (a
  Timestamp can never become a Scalar, by design, so `jitter`'s deltas
  cannot be repurposed). `hash` unlocks humanized velocity, per-note
  detune drift, round-robin sample choice via `nth`, generative
  arrangement choices via build-time `if` — the entire aleatoric corner
  of the audit — in one primitive, and B5's `Score.humanize` is written
  over it. Document the sharing: `jitter` = `hash` applied in the time
  domain, same bits.

### B2. `Time.div` / `Time.rem`: the missing quotient (F3)

The spec is right that `1s / 500ms` must not hand back a bare Scalar —
"a Timestamp that decays into a bare number is how unit confusion gets
in." But the *quotient of two durations* is not a decayed number; it is
a dimensionless **count**, and its absence is why `timing.synth` derives
per-bar hit counts by meter arithmetic that is wrong outside simple
meters:

```
val Time.div : num:Timestamp -> den:Timestamp -> Int        (* floor *)
val Time.rem : num:Timestamp -> den:Timestamp -> Timestamp  (* num - den*div *)
```

`div` answers "how many of these fit" (a count, so an `Int` — it lands
exactly where `~count` parameters want it); `rem` answers "and what's
left" (still a duration, still a Timestamp). No unit ever decays.
Division by `0s` is a build error. Two `math.cpp` externals. This
subsumes the roadmap's `Tempo.per_bar` (B3 implements it in one line)
and closes F3's count derivations: `Time.div Timing.ending (dur Whole)`
is "how many bars long is the piece" with no meter caveat. The floor
convention matches `wrap_div`; document that pairing.

### B3. `Tempo`: finish the grid vocabulary (F3)

```
val Tempo.bars      : t:Tempo -> n:Scalar -> Timestamp   (* n bars, fractional ok *)
val Tempo.per_bar   : t:Tempo -> v:Value -> Int          (* roadmap item; = Time.div (bar t) (value t v) *)
val Tempo.bar_beats : t:Tempo -> n:Int -> Scalar         (* n bars in *beats* — the Score bridge *)
val Tempo.marks     : t:Tempo -> bars:Int list -> Timestamp list
val Tempo.swung_grid: t:Tempo -> from:Timestamp -> step:Value
                        -> count:Int -> amount:Scalar -> Timestamp list
```

- `bars` is the sibling `beats` already has (`darksynth`'s
  `seg n = bar ~t:tempo * (2.0 * Math.to_scalar n)` becomes
  `bars ~t ~n:(2.0 * …)`).
- `bar_beats` is the **identical** `span` helper both projects wrote
  (F3) — the bars→beats bridge `Score.move`/`chord ~len` need. Naming it
  `bar_beats` rather than `span` keeps `Score.span` unshadowed, per the
  library's own collision policy.
- `marks` turns section lengths into section starts:
  `marks ~t ~bars:[8; 8; 8; 8; 12; 4; 8; 4]` is the darksynth landmark
  table as one fold, returning n+1 Timestamps (each start plus the
  ending). The named landmarks stay user `let`s — Core cannot name
  `drop2` — but the cumulative arithmetic, the part that breaks when a
  section is re-lengthened mid-writing, becomes one edit. (A `scan` in
  `List` — B9 — is the general tool; `marks` is its tempo-aware face.)
- `swung_grid` exists because the composed form names the step twice
  (`grid ~step:Eighth … |> swing ~step:(value ~t ~v:Eighth)`) — a
  disagreement between the two is a silent groove bug. One call, one
  step. This may finally give `swing` its first user.

### B4. `Scale`/`Pitch`: open the enums, name the progression (F3, F14)

**Open `Quality` and `ChordQuality` with a payload case each:**

```
type Quality      = | Major | Minor | … | Chromatic | CustomQ of Int list ;;
type ChordQuality = | Maj | Min | … | Add9 | Shape of Int list ;;
```

`offsets` gains `| CustomQ os -> os`; `shape` gains `| Shape ss -> ss`.
Every existing function — `degree`, `snap`, `notes`, `stack`, `triad`,
`seventh`, `tones`, `in_key` — works over custom ladders *unchanged*,
because they all already go through `offsets`/`shape`. Harmonic major is
`CustomQ [0; 2; 4; 5; 7; 8; 11]`; a 13th chord is
`Shape [0; 4; 7; 10; 14; 17; 21]`. This is the cheapest possible fix to
the hardest representability wall (F14). Compatibility note: adding a
constructor breaks user code that exhaustively `match`es these types —
expected to be rare (they are parameter enums; the examples never match
them) and the checker names each missing case concretely.

Also extend the named roster with the standing-vocabulary chords the
twelve shapes miss: `Sixth`, `Min6`, `Dom9`, `Maj9`, `Min9`, `MinMaj7`,
`Dom7b9`, `Dom13` (each one row in `shape`). Named chords are
documentation; `Shape` is the escape hatch.

**Name the progression** — the structure both `harmony.synth` files
re-implement:

```
type Prog = { key : Scale; degrees : Int list } ;;

val Scale.prog_len   : p:Prog -> Int
val Scale.prog_chord : p:Prog -> i:Int -> Pitch.Note list   (* wraps; triad on degrees[i mod len] *)
val Scale.prog_stack : p:Prog -> i:Int -> count:Int -> Pitch.Note list
val Scale.prog_root  : p:Prog -> i:Int -> Pitch.Note
```

with wrapping via the existing `wrap_rem`, so darksynth's
`cycle`/`chord_degree`/`parts`/`root` collapse into
`let prog : Prog = { key = key; degrees = [0; 5; 3; 4] }` plus calls.

**Add the register fold both projects faked** (the `voicing ~count:1` +
`nth` idiom):

```
val Pitch.wrap_to : note:Note -> low:Note -> Note
```

— the note moved by whole octaves into `[low, low + octave)`. Three
lines over `step`/`of_step`/`wrap_rem`, and `Scale.voicing` itself can
be re-expressed over it. `Prog`'s `~low`-taking variants
(`prog_chord ~low ~count` voiced form) then compose from parts that each
mean something.

### B5. `Score`: rhythm, dynamics, feel, and the tuning valve (F2, F6)

The additions that let darksynth's drums/bass/bells be *written as
phrases* — closing the tier split — plus the last-step consumers for
shipped-but-stranded features:

```
(* rhythm without a dummy note *)
val Score.rhythm : lens:Scalar list -> Phrase       (* unpitched steps, laid end to end *)
val Score.hits   : n:Int -> len:Scalar -> Phrase    (* = rhythm (repeat n len) *)

(* per-step dynamics: vs cycles, so [1.0; 0.6] alternates accents;
   finally the consumer `ramp` never had *)
val Score.vels      : p:Phrase -> vs:Scalar list -> Phrase
val Score.crescendo : p:Phrase -> from:Level -> to:Level -> Phrase
                        (* = vels with ramp ~n:(step count) *)

(* feel, in beats, on the symbolic side (needs Math.hash) *)
val Score.humanize : p:Phrase -> seed:Scalar -> spread:Scalar -> Phrase
val Score.shuffle  : p:Phrase -> grid:Scalar -> amount:Scalar -> Phrase
                        (* displaces steps landing on odd multiples of grid *)

(* the tuning valve: realize against any note->frequency mapping *)
val Score.realize_with : tempo:Tempo.Tempo -> pitch:(Pitch.Note -> Scalar)
                           -> p:Phrase -> Event list
```

- `rhythm`/`hits` bake the placeholder note in (steps carry
  `of_step ~step:0`, documented as ignored by `strike`), so a drum part
  is `hits ~n:4 ~len:1.0 |> vels [1.0]` — and the hat-accent workaround
  (F6's two-layer trick) becomes `hits ~n:8 ~len:0.5 |> vels [1.0; amp ~l:Forte]`.
- `humanize`/`shuffle` operate on `at` in **beats**, before `realize`,
  which is what makes feel compatible with duration-aware voices — the
  structural fix for F2. (`shuffle`, not `swing`: the module's
  name-dodging convention, since `Tempo.swing` stays for Timestamp
  lists.) Both are pure via `Math.hash`, same reproducibility contract
  as `jitter`, "give each layer its own seed" guidance carried over.
- `realize_with` decouples `Score` from `Pitch.Tuning`: `realize` stays
  and becomes sugar for `realize_with ~pitch:(Pitch.hz ~t:tuning)`.
  Scordatura, well-temperament tables, per-key inflection maps, or a
  `step_hz`-backed mapping become one lambda, without `Score` learning
  anything about tuning — the audit's "Score can't reach n≠12" row
  reduces to "write the mapping" (the 12-tone `Note` in `Step` is still
  the score alphabet; see §8 for why that stays).
- **Per-note inflection** (`bend`): adding `bend : Scalar` (cents,
  default `0.0`) to `Step`, applied by `realize*` via `Pitch.cents`, is
  the honest fix for quarter-tones and blue notes inside phrases. It is
  the one *breaking* record change in this document (a record literal
  must name exactly the declared field set, so hand-written `Step`
  literals — rare; the examples build steps only through builders — gain
  a field). Builders default it to `0.0`; one new transform
  `Score.bend ~p ~f:(Int -> Scalar)` sets it by index. Worth the break
  now, while `Step` literals are rare in the wild.
- **Portamento stays open.** A glide needs the *next* event's frequency;
  a `voice` sees one note. The clean shape is probably an alternative
  player — `play_legato ~voice:(Scalar -> Scalar -> Timestamp -> Scalar -> 'a Sample)`
  handing `(freq, next_freq, dur, vel)` for the voice to sweep with
  `resample` or `fm` — but it should wait for C1/C2 experience before
  committing; flagged as the known gap it is rather than half-designed
  here.

The tempo-map roadmap entry also lands at this seam when it comes:
`realize` is the single beats→Timestamps bridge, so a
`realize_map ~tempos:(Scalar, Tempo.Tempo) list` (tempo changes at beat
positions, piecewise) needs no change to `Phrase` or any transform —
worth recording as the intended shape even though it is not proposed for
this round.

### B6. `Core.Mix`: the stereo and bus vocabulary (F4, F5, F9)

A new submodule — the current library stops one abstraction short of
where every arrangement file actually works:

```
val Mix.pan     : pos:Scalar -> input:Scalar Signal -> Vector Signal
                    (* -1 left … +1 right; equal-power *)
val Mix.pan_sig : pos:Scalar Signal -> input:Scalar Signal -> Vector Signal
                    (* automated pan: autopan, Doppler sweeps *)
val Mix.mix     : parts:(Scalar, 'a Signal) list -> 'a Signal
                    (* weighted mix_all *)
val Mix.db      : x:Scalar -> Scalar        (* re-export of Score.db *)
val Mix.gain_db : x:Scalar -> input:'a Signal -> 'a Signal
val Mix.vca     : gain:Scalar Signal -> input:'a Signal -> 'a Signal
                    (* mono gain over any bus; via am until D3 *)
val Mix.duck    : ats:Timestamp list -> depth:Scalar -> dip:Timestamp
                    -> recover:Timestamp -> input:'a Signal -> 'a Signal
```

- `pan` is equal-power via `sqrt` (`channels [in * sqrt ((1.0 - pos) * 0.5);
  in * sqrt ((1.0 + pos) * 0.5)]`) — expressible today, wanted
  everywhere, written nowhere. `pan_sig` is the same formula with
  `sqrt` elementwise over a position *signal* — also expressible today
  (`sqrt` already lifts), and it makes the Doppler library's hand-rolled
  pan a one-liner. With B1's trig, a `sin`/`cos` law variant can follow;
  `sqrt` needs nothing.
- `mix` is the weighted `mix_all` every master section approximates with
  `a * 0.5 + b * 0.4 + …`; pairs keep the gains readable next to their
  parts, and `Mix.db`/`gain_db` let them be written as decibels
  (`mix [(db (-6.0), drums); …]`) — dynamics vocabulary finally
  reachable from mixing code without `open Core.Score` (F5).
- `vca` is the mono-scales-a-bus operation F9 documents:
  implemented as `am ~carrier:input ~modulator:(gain - 1.0) ~depth:1.0`
  (exactly `input * gain`) until the operator table learns the broadcast
  row (D3), at which point the body becomes `input * gain` and nothing
  downstream changes.
- `duck` canonizes the grid-sidechain idiom: a `1 → 1-depth → 1`
  dip envelope placed at every timestamp, multiplied onto the bus via
  `vca` — pumping pads under a four-floor kick in one call, no
  envelope follower needed (that remains C2, for *program-dependent*
  sidechaining).

`channels` and `mix_all` stay in `Arrange` untouched; `Mix` is the
musical layer above them. (The right inverse — extracting a channel —
cannot be written in synth and is C4.)

### B7. `Groove` extensions: masks and Euclid (with A2)

```
val Groove.mask   : keep:Bool list -> steps:Timestamp list -> Timestamp list
                      (* keep cycles; the step-sequencer row *)
val Groove.euclid : hits:Int -> steps:Timestamp list -> Timestamp list
                      (* Bjorklund selection over the given grid *)
```

`mask ~keep:[true; false; false; true]` over a sixteenth grid is the
x..x row of a step sequencer; `euclid ~hits:5` over 16 steps is the
world-rhythm generator half of electronic music leans on. Both are pure
`List` work (~15 lines each), both compose with `swing`/`jitter`
upstream and `Groove.pattern`/`place_multi` downstream, and both make
the *irregular* patterns that today get written as bare literal lists
(`darksynth`'s chain rattles) derivable when they have structure.

### B8. `Str`: two functions, one workflow (F12)

```
val Str.cat    : a:String -> b:String -> String
val Str.of_int : n:Int -> String
val List.iter  : f:('a -> unit) -> xs:'a list -> unit
```

`cat`/`of_int` are two `math.cpp`-style externals. This is not a string
library — it is the minimum for *computed target names*, which is the
minimum for "render every section of the song as its own artifact":

```
let _ = List.iter
  ~f:(fun i:Int ->
        render ~name:(Str.cat "section-" (Str.of_int i)) ~rate:48000.0
               ~sample:(section_sample i))
  ~xs:(List.range ~from:0 ~count:8) ;;
```

`List.iter` rides along because the review turned up a quiet
impossibility: **effects cannot be iterated in pure SynthGraph at
all**. `unit` exists only as a type — there is no `()` literal, and
`render` is the language's only unit producer — so a fold over renders
has no `~init`, a `unit list` from `map` does not satisfy `let _`'s
unit body, and even a hand-written recursive iterator has nothing to
return in its `Nil` arm. `iter` therefore *must* be an external (C++
can mint the unit value; synth cannot), which is also why it, unlike
the rest of `List`, lives in `lists.cpp`. (The alternative — a `()`
literal — is a language change with only this use case; the external
is cheaper and sufficient. Recorded in D5.)

The audition loop this enables (build → listen to just the section
being worked on, at section rather than track granularity, with
incremental caching doing the rest) is arguably the biggest *workflow*
improvement available for the cost. Anything more (`length`, `sub`,
comparison) waits for a real need.

### B9. `List`: four gaps in the combinator set

```
val List.mapi : f:(Int -> 'a -> 'b) -> xs:'a list -> 'b list
val List.take : n:Int -> xs:'a list -> 'a list
val List.drop : n:Int -> xs:'a list -> 'a list
val List.scan : f:('a -> 'b -> 'a) -> init:'a -> xs:'b list -> 'a list
                  (* running fold: n+1 entries, init first *)
```

Evidence: index-aware mapping is currently spelled
`zip (range …) xs |> map` or restructured around `init` (darksynth's
per-segment seeds thread indices by hand); `take`/`drop` are how a
progression gets truncated to a section (`less`-style count arithmetic
at `darksynth/song.synth:192-196` is a `take` in disguise); `scan` is
the cumulative-starts engine under B3's `marks` and any "each length
follows from the previous" structure. All total, all in the existing
style.

### B10. `Fx` sugar: `gated` and `echoes` (F10, F11)

```
val Fx.gated  : attack:Timestamp -> decay:Timestamp -> sustain:Scalar
                  -> release:Timestamp -> hold:Timestamp
                  -> input:Scalar Signal -> Scalar Sample
                  (* input * adsr, cut to the envelope's own end:
                     [0s, max hold (attack + decay) + release) *)
val Fx.echoes : by:Timestamp -> gain:Scalar -> n:Int
                  -> input:'a Signal -> 'a Signal
                  (* input + Σ_{i=1..n} delay(by·i) · gainⁱ *)
```

`gated` writes the F10 arithmetic once and fixes the convention (the
window ends where the envelope does; the doc note in A4 points here). A
voice
becomes `fun freq:Scalar dur:Timestamp vel:Scalar -> voice freq * vel
|> gated ~attack:3ms ~decay:110ms ~sustain:0.5 ~release:60ms ~hold:dur`.
`echoes` replaces three hand-unrolled stacks and is where feedforward
echo lives until C3 gives it feedback.

---

## 6. Proposals C — engine-backed extensions (ranked)

Each of these needs new `SigNode` kinds (or new node parameters) behind
new `engine.hpp` constructors, then ordinary externals. Ranked by
songwriting value per engine effort.

### C1. Modulated and resonant filters — the top engine ask (F8)

```
val Fx.lowpass_mod  : cutoff:Scalar Signal -> input:'a Signal -> 'a Signal
val Fx.highpass_mod : cutoff:Scalar Signal -> input:'a Signal -> 'a Signal
val Fx.resonant     : cutoff:Scalar Signal -> q:Scalar
                        -> input:'a Signal -> 'a Signal   (* 2-pole SVF lowpass *)
```

Two steps: (a) signal-rate cutoff on the existing one-poles (coefficient
recompute per sample or per block — the node is already stateful and
per-context, so the engine's determinism and warm-up rules apply
unchanged; the mono-modulator validation rule from `am`/`resample`
carries over verbatim); (b) a state-variable filter with resonance —
the single most-missed subtractive-synthesis element in the library.
Together they unlock filter risers (F8's fake becomes real), wah/acid
lines (`resonant` swept by an envelope — note today that envelope can
only be built from `exp_decay`/`adsr`/`signal ~f` math, which is
sufficient), and the "opening filter" gesture that defines the
darksynth genre the flagship example is written in. The existing
`lowpass`/`highpass` stay: fixed-cutoff one-poles are cheaper and most
call sites are tone-shaping, not gestures.

### C2. Envelope follower + select: signal-level control, concretely

The roadmap's "signal-level branching" entry, reduced to the two
primitives the audit actually needs:

```
val Fx.follow : attack:Timestamp -> release:Timestamp
                  -> input:Scalar Signal -> Scalar Signal   (* rectified, smoothed *)
val Sig.select : gate:Scalar Signal -> threshold:Scalar
                   -> above:'a Signal -> below:'a Signal -> 'a Signal
```

`follow` + arithmetic + `Mix.vca` is a compressor, a program-dependent
sidechain (`vca ~gain:(1.0 - follow kick_bus * depth)`), a gate, an
auto-wah driving C1. `select` is the sample-wise choice the spec
deliberately excluded from `if` — as its own primitive, per the spec's
own framing ("a sample-wise select/gate over signals would be a new
signal-producing primitive", §7). Both are ordinary stateful nodes with
per-context state; neither touches the language's build-time `Bool`.

### C3. Feedback delay (roadmap, endorsed with a signature)

```
val Fx.feedback : by:Timestamp -> gain:Scalar -> input:'a Signal -> 'a Signal
```

Internal feedback state like `reverb`'s (the language-level graph stays
acyclic), `|gain| < 1` validated at construction, minimum `by` bounded
by the block size story. Flangers, dub delays, and Karplus–Strong-style
plucks stop being blocked. General user-defined signal cycles remain
out (rightly — that is a language design, not a primitive).

### C4. Channel extraction

```
val Arrange.channel : n:Int -> input:Vector Signal -> Scalar Signal
```

The inverse `channels` never had (F4): per-channel processing of
`load_multi` material, mid/side tricks via `Mix`, width control.
Channel index validated at graph build like every other channel rule.
One trivial node.

### C5. Elementwise trig rows

The engine half of B1: `Sin`/`Cos`/`Tan`/`Atan`/`Abs` rows in
`SigUnaryOp` (and the fused-arithmetic interpreter's dispatch). Small,
and it completes `signal ~f` as a usable custom-LFO/waveshaper facility.

### C6. Bandlimited oscillators (quality flag, not a blocker)

`saw`/`square` are naive (`src/signal.cpp:158-159`: `2·frac − 1`,
`frac < 0.5 ? 1 : −1`) and alias audibly on bright, high-fundamental
material; the examples mask it by lowpassing nearly every saw. PolyBLEP
variants are cheap; the catch is the byte-identical contract — changing
the waveform is a *versioned* engine change, which the incremental
cache's engine-version salt already accommodates (`build-system.md`).
Worth doing deliberately, once, with the version bump called out —
possibly as new `Osc.saw_bl`/`square_bl` first if bit-stability of
existing artifacts matters more than defaults.

Not proposed: oscillator phase parameters (`pm ~carrier ~modulator:(constant φ)`
already provides sine-with-phase, and B1's `pi` makes it readable —
document the idiom instead), reverb pre-delay (composes today as
`delay |> reverb`), reverse playback and higher channel counts (already
on the roadmap with the right framing).

---

## 7. Proposals D — language-level gaps, ranked by songwriting impact

D-items are language/evaluator work, listed here because the review
surfaced concrete musical evidence for (or against) prioritizing them.

**D1. Self-tail-call elimination in the evaluator (F13).** The 4096
frame guard is the right runaway brake, but `List.fold`'s recursive call
is already in tail position, and `map`/`append`/`init` have standard
accumulator forms. With self-TCO in `eval.cpp`, the `List` module (and
A3's fold-based `seq`) can be restated so that *depth* stops scaling
with list length entirely — the guard then only catches genuine runaway
recursion, and an 8½-minute sixteenth line stops being a cliff. This is
the highest-value language change for *scaling* scores; without it,
every B5 adoption push nudges long pieces closer to F13.

**D2. Local type inference (roadmap, endorsed with evidence).** The
annotation tax is real in songwriting flow: every two-line `let` in an
arrangement carries a type the right-hand side already states
(`let d2 : Timestamp = Timing.drop2`,
`darksynth/song.synth:312-313`), and the annotation load is heaviest
exactly in the tweak-listen-tweak loop where friction costs the most.
Return types and `let … in`
annotations (the roadmap's own shortlist) would cut most of it without
touching the checked language. Worth scheduling *before* the library
grows further — every B-proposal adds call sites that will be written
under whichever rule is in force.

**D3. Broadcast rule: `Scalar Signal ⊗ Vector Signal` (F9).** Add the
operator-table row lifting a mono signal across a Vector signal's
channels for `*` (and plausibly `+`/`-`), mirroring the Scalar
broadcast row and `am`'s mono-modulator precedent. Checker: one row;
engine: `makeBinOp` accepting 1×N with broadcast (its channel logic
already understands broadcast-only nodes). `Mix.vca`'s body then
becomes `input * gain`, and bus automation stops needing an identity
disguised as AM.

**D4. Not needed after B1/B2.** Two candidate language changes dissolve
into library items: value randomness needs no `random` effect
(`Math.hash` preserves purity by construction), and Timestamp division
needs no operator (`Time.div`'s Int-returning quotient is *better* than
a `/` returning Scalar, because a count is not a number of seconds).
Recording the non-need is the point: both stay out of the language.

**D5. Lower priority, unchanged.** Mutual recursion (no musical case
surfaced — `Score` composes fine without it), literal patterns (`match`
on `Item` payloads never wanted them in practice), per-definition
visibility (library authors managed with `lib.synth` interfaces),
signal comparisons (C2's `select` is the principled form), and a `()`
unit literal (B8's `List.iter` external covers the one workflow that
wanted it). The review found no reason to re-rank these above the
roadmap's current placement.

**D6. A consideration, not a proposal: channel arity in types.**
`Vector` erases channel count, so `channels [l; r]` vs. a 5-channel bus
meet the checker identically and diverge at graph build. A
`Stereo`-style refinement (or a type-level count) would catch pan/bus
mistakes earlier, but it infects every polymorphic signature in the
library. The build-time channel check plus C4/B6 keeping stereo
construction behind named functions is probably the right cost line;
recorded so the option isn't lost.

---

## 8. What deliberately stays as it is

The review also confirms several designs that examples validate
*against* plausible "improvements" — future proposals should treat these
as settled unless new evidence appears:

- **No Timestamp → Scalar conversion.** Every friction that seemed to
  want it (F3's counts) is better served by `Time.div`'s dimensioned
  quotient. The one-way door works.
- **Purity, no RNG.** `jitter`'s reproducible feel is exactly right in
  practice (seeds per layer, cacheable renders); `Math.hash` extends the
  same contract instead of breaking it.
- **Unnormalized summing** (`mix_all`, `place_multi`, `play`). Headroom
  as the composer's responsibility is used deliberately everywhere
  (`soft_clip ~threshold:0.9` as master glue); auto-normalization would
  fight the gain-staging the examples do on purpose.
- **`Note` stays 12-tone; `Value` stays recursive; presets stay
  functions.** The n≠12 path through `Int` steps + `step_hz` +
  `realize_with` (B5) is honest about what a `Note` is; `Dotted`/`Tuplet`
  nesting has already paid for itself; and the build-cost argument for
  function presets holds (with `Math.pi` as the documented
  single-constant exception).
- **Notation stays out of scope** — ties, spelling, staves. `bend` (B5)
  is a *sound* feature, not a notation feature; the line holds.
- **`Score`'s collision-dodging names** (`span`/`layer`/`loop`/`move`)
  — B3/B5 additions follow the same rule (`bar_beats` not `span`,
  `shuffle` not `swing`).

---

## 9. Compatibility and process

- **Additive and invisible to existing code:** A1, A2 (new module), B1,
  B2, B3, B6, B7, B8, B9, B10, C1–C5, D1. New bare names under existing
  `open`s cannot break user files (position-ordered shadowing: a
  file's own later definitions win), though additions to *opened* Core
  modules can shadow a user's earlier same-named definition — worth a
  release-notes list of every new name.
- **Behavior-preserving refactors:** A3 (pinned by the existing
  literal-render build tests — the `core-library.md` testing note about
  types pinning nothing and renders pinning values applies in full).
- **Breaking, checker-guided:** B4's new variant constructors (breaks
  exhaustive user matches on `Quality`/`ChordQuality`; none exist in
  the examples) and B5's `Step.bend` field (breaks hand-written `Step`
  literals/patterns; the examples have none — all steps flow through
  builders). Both fail loudly at check time with concrete messages,
  which is this language's best case for a break.
- **Versioned-output:** C6 changes rendered bytes and rides the
  engine-version cache salt.
- Every addition follows the four-edit process from `core-library.md`
  (definition; submodule table; spec §6 roster; checker + build tests),
  and the SynthGraph-implemented modules take the literal-render build
  tests seriously — `humanize`/`shuffle`/`euclid`/`marks` all need
  value-pinning tests, not just type coverage. New docs sections per A4
  land with the code they describe.

---

## 10. Suggested phasing

| Phase | Items | Theme |
|---|---|---|
| **1 — pure library, high leverage** | A1, B1 (Scalar/Vector half), B2, B3, B9, B10, A2+B7, B6 (pan/mix/db/duck via am) | Everything the examples visibly re-implement, shippable entirely in `lib.synth` plus a handful of value-level externals |
| **2 — the score tier completed** | B5 (rhythm/vels/humanize/shuffle/realize_with), B4, A3, A4, B8 | Darksynth's drums become phrases; custom scales open; docs restructured |
| **3 — engine round** | C1, C5 (completing B1), C2, C4, D3 | Filters that move, control that listens, buses that fade |
| **4 — language round** | D1, D2, then C3, C6 | Depth ceiling and annotation tax; then feedback and bandlimiting |
| **Continuous** | Io/sampling worked example; `lint` unused-open warning; release-notes name lists | Coverage and hygiene |

The one-sentence version: **the signal tier needs vocabulary (trig,
pan, resonance), the grid tier needs promotion (`Groove`), the score
tier needs completion (rhythm, dynamics, feel), the harmony layer needs
the two abstractions every project rebuilt (`Prog`, `wrap_to`,
`Time.div` counts), and the two hard walls — closed scale enums and
immovable filters — each fall to one focused change (B4, C1).**
