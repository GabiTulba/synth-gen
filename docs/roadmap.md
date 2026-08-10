# Roadmap

Future work only: things deliberately left out of v1 (design doc §13,
spec [§7](language-spec.md#7-out-of-scope-in-v1)) and improvements under
consideration. Everything here is *not* implemented today; the rest of
the documentation describes only what is.

## Language

- **Signal-level branching.** Comparisons and `if` are build-time only.
  A sample-wise select/gate over signals would be a new signal-producing
  primitive (e.g. threshold gates, envelope followers driving choices).
- **Pattern matching** and **user-defined types** — today the only
  aggregate types are lists and tuples, consumed positionally.
- **Recursion.** Definitions must precede use and cannot be (mutually)
  recursive. Lifting this interacts with the "no feedback" rule below
  and with incremental hashing, so it needs its own design pass.
- **Type inference.** Every binding is fully annotated; polymorphism is
  written out. Local inference (return types, `let ... in` annotations,
  lambda parameters) would remove most annotation weight without
  changing the checked language.
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
- **Richer LSP.** The server covers diagnostics, completion,
  go-to-definition, and hover; rename, find-references, document
  outline, and formatting are natural next steps.
- **Dev app playback features.** The artifact browser plays targets
  start-to-finish; seeking, looping, A/B between builds, and inline
  waveform display (the data already exists for `render_vis`) would make
  it a better auditioning tool.
- **Native extensions beyond build-time externals.** User externals run
  at build time on build-time data only; letting user C++ participate in
  *signal* processing (custom DSP nodes) is a separate, larger feature
  with caching and determinism obligations.
