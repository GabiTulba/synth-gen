# Signal engine

The engine (`src/signal.*`, with `src/wav.*` and `src/vis.*` for
artifact writing) turns lazy signal graphs into PCM frames. Its contract
is simple and strict: **every optimization must produce artifacts
byte-identical to a naive sequential, per-frame render**, and each one
is verified against that baseline.

## The signal DAG

A `Signal` value is a lazy, immutable graph; nothing is discretized
until a render target is produced, at that target's declared rate.
Signals are defined for every t ≥ 0 (the epoch). Because graphs are
immutable and all per-render state lives in per-render contexts, shared
subgraphs are safe under concurrency and sharing a subtree is free.

Time-remapping nodes create *private contexts*:

- `sample s from to` cuts the window `[from, to)`; `place smp at` embeds
  it back at `at`, silent elsewhere. Stateful primitives (filters —
  fixed, modulated and resonant — `fm`, `delay`, `feedback`, `follow`,
  `reverb`) evaluate from the epoch of their own timeline; a
  placed sample's interior state warms up from t = 0 of its source,
  preserving "signals are defined from t = 0" semantics. Feedback loops
  (`feedback`, `reverb`) live entirely inside a node's per-render
  state; the graph itself stays acyclic.
- `resample input ~f` changes how fast the source is read
  (`out(t) = input(∫₀ᵗ f)`); the source runs in its own context from its
  own epoch, exactly like a placement.

Every `SigNode` carries a structural content hash, set at construction
in O(1) from its children's hashes (audio files hash their data). These
hashes power the sample caches below.

Rendering works in doubles throughout and only hard-clamps to [-1, 1]
at WAV write. Audio files are build inputs: `load_mono`/`load_multi`
read and validate them at build time; a loaded file occupies
`[0s, duration)` and is silence afterward, linearly resampled to the
render rate. `src/wav.*` reads PCM 16/24/32 and float 32/64, and writes
16-bit PCM. `src/vis.*` writes `render_vis` targets as dependency-free
waveform SVGs — one lane per channel, min/max-per-column drawing.

## Block rendering

Nodes compute 1024-frame blocks instead of single frames: per-node
dispatch and memoization cost is paid once per block, inner loops stay
tight, and exact silence short-circuiting makes placed samples cost
nothing outside their windows.

## Intra-target parallelism (the planner)

A planner decomposes a render's top into its *combination spine*
(mixes, arithmetic, channel assembly, wrappers) plus the heavy subtrees
hanging off it, proves the subtrees state-disjoint (grouping any that
share nodes), and renders the groups on worker threads with a per-block
barrier while the main thread replays the spine over the workers'
blocks. Summation order — and therefore the output — is identical to a
sequential render. When every decomposed leaf is served from a
pre-rendered window, the planner skips worker threads entirely (barrier
overhead only).

## Fused arithmetic

`makeBinOp` does not build a node per operator: elementwise `+. -. *. /.`
chains merge into a single fused node holding the non-arithmetic
subtrees as inputs and a small postfix program over them (constants
become immediates, duplicate inputs dedup, inlining capped so shared
DAGs cannot blow up). The program executes block-at-a-time — each
operator is one tight auto-vectorized loop over cache-resident
value-stack slabs — replaying the exact operator-tree order, with a
per-block silence lattice reproducing each operator's short-circuit
rule, so output is bit-identical. (A per-element interpreter variant was
tried first, measured as a regression — it defeated SIMD
auto-vectorization — and replaced with the block form.)

Renders also record per-block silence flags, so serving a shared
buffer's silent stretches is a flag test instead of a worst-case zero
scan.

## Sample caches

A Sample is a value, so every placement of it is guaranteed to replay
identical content; the engine exploits that instead of merely honoring
it.

- **Within a render**: each render owns a thread-safe cache keyed by
  (source node, window). The first placement discretizes the whole
  window once, and every placement — at any timestamp, block-aligned or
  not, on any planner thread — serves slices of that buffer. The cached
  window records its nonzero span, so envelope-gated tails and
  pre-attack padding are silent for free. A placement whose entry is
  mid-build on another thread falls back to the classic private-context
  replay (identical values either way), and nested samples inside a
  cached window share the same cache.
- **Across targets and rebuilds**: because the cache is keyed by
  (structural content hash, window, rate) rather than node pointers,
  cached windows outlive the graph that created them. `buildProject`
  shares one cache across all targets of a build; the watch daemon keeps
  it across rebuilds, so an arrangement edit that dirties every target
  re-renders only the samples whose own definitions changed — usually
  none. A generation sweep keeps only what the previous build actually
  placed.

## Shared buses across targets

When one target's signal occurs as a shared subtree of another target's
graph (a master summing the buses the stems render, an overview stacking
a master lane above its bus lanes), the build schedules the containing
target after its providers and serves their finished buffers to it
block-by-block (`PreRenderedMap`), instead of re-discretizing the
subtree. Dependencies only form between renders with identical windows
and rates, never reach under placements (which remap time in private
contexts), and renders are deterministic — so artifacts stay
byte-identical.

## Performance reference point

On the `examples/darksynth` track (~91 s, seven targets), a fresh full
build dropped from ~250 s with the original per-frame engine to under
2 s with all of the above; in watch mode, an arrangement edit that
invalidates every target rebuilds in ~1.4 s with zero sample windows
re-rendered. All artifacts along the way were verified byte-identical to
the per-frame engine's output.
