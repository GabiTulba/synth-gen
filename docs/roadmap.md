# Roadmap

Future work only: things deliberately left out of v1 (design doc §13,
spec [§7](language-spec.md#7-out-of-scope-in-v1)) and improvements under
consideration. Nothing listed here is implemented; the rest of the
documentation describes only what is. An item lands by leaving this file
— the `Core.Pitch`/`Tempo`/`Scale`/`Score` write-ups that used to fill
the composition-libraries section went that way, and git history has
them.

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

The songwriting layer — `Core.Pitch`, `Core.Tempo`, `Core.Scale`,
`Core.Score` — is built and documented in
[`core-library.md`](core-library.md) and the language spec
[§6](language-spec.md#6-primitive-signatures-v1-roster). What is left is
work on top of it:

- **A note-value *count*.** `Tempo.value` says how long a note value is,
  as a `Timestamp`, and Timestamps do not divide
  ([§3](language-spec.md#operators-pointwise-lifting--scalar-broadcasting))
  — so there is no way to ask how many eighths fill eight bars, which is
  exactly the number a grid's `~count` wants.
  `examples/darksynth/timing.synth` derives its own
  `quarters`/`halves`/`eighths`/`sixteenths` off `meter.beats`, correct
  for simple meters only. A `Tempo.per_bar ~t ~v:Value : Int` is a few
  lines over the same `frac` that `value` already walks, and it is the
  one gap the example rewrite turned up.
- **Tempo maps and rubato.** A `Tempo` is one fixed BPM. A tempo *curve*
  — accelerando, ritardando, a click that drifts — is expressible as a
  `Scalar -> Scalar` rate over the timeline, which is precisely what
  `resample` already consumes, so the primitive exists and only the
  `Phrase`-level realization is missing. Keeping `Phrase` symbolic (in
  beats) is what leaves the door open.
- **`Note` is inherently 12-tone**, so `shift`/`flat`/`hz` suit
  12-division temperaments; `n /= 12` ladders work in `Int` steps
  through `step_hz`. A spelling that generalizes to arbitrary `n` would
  need a different note type, and it is not clear one is worth it.
- **Scale-degree spelling** (whether `Cs` and `Df` should be
  distinguishable) stays out until something needs to render notation.
- **`Score.seq` nests rather than folds**, one call level per phrase, so
  a sequence of thousands of phrases would reach the 4096-call recursion
  limit. One level per *event* is well inside it, so no realistic score
  reaches this today.
- **Notation stays out of scope.** `Phrase` is a lean note list with an
  algebra over it, not a notation model: ties, slurs, key signatures,
  repeat barlines, multi-staff layout and beaming are all absent. They
  matter for *rendering staves*, which the language does not do, and
  each one would add a case to every combinator. The variants are
  extensible if that call turns out to be wrong.
- **`Render.render_score`.** A piano-roll or staff SVG artifact
  alongside `render_vis`, reusing the existing artifact machinery
  exactly. It would make the "music sheet" framing literal, and a
  dependency-free SVG of a `Phrase` is viewable straight from a git host
  like the waveforms already are.

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
