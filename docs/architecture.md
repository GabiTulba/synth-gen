# Architecture

SynthGraph is a compiler (`synthc`), a signal-rendering engine, a build
system, and a small constellation of tools around them. This document
maps the components and how data flows between them; each component has
its own detailed document (linked below).

## The pipeline

```
.synth sources ── lexer ── parser ── AST
                                      │
                        module resolution + type checker
                                      │
                                  evaluator ── signal graphs + render targets
                                      │
                    build system (caching, scheduling, parallelism)
                                      │
                             engine discretization
                                      │
              .wav / .svg artifacts  +  metadata.json ── dev app
```

1. **Front-end** — the lexer and parser turn a `.synth` file into an AST
   with source spans; the checker verifies the fully-annotated types and
   resolves modules (sibling files, libraries, inline modules, the
   bundled Core). Diagnostics carry spans and render as compiler errors,
   lint output, or LSP diagnostics from the same code path.
2. **Evaluation** — the evaluator reduces definitions to values.
   Everything is pure except `render` (and its `render_vis` /
   `render_stems` variants), which records a *render target*: a named
   sample window plus a rate. `Control.slider`/`knob` declare *live
   controls* — build-time Scalar parameters resolved here from the
   unit's override file, fixed for the whole build. Signals stay lazy
   graphs throughout.
3. **Build** — the build system enumerates targets across the project,
   consults the incremental cache (Merkle content hashes over each
   target's definition closure), and renders cache-miss targets in
   parallel.
4. **Discretization** — the engine walks each target's signal graph and
   produces PCM frames, block by block, writing `.wav` (or `.svg` for
   visual targets) artifacts plus a `metadata.json` index.
5. **Consumption** — the dev app reads the metadata (a pure consumer, no
   compiler internals) and lets you browse and play artifacts; the watch
   daemon loops the whole pipeline on file changes. Live controls close
   the loop in the other direction: the dev app renders them as sliders
   and knobs and writes overrides into the unit's `controls.json`, a
   build input the daemon rebuilds on.

## Components

| Component | Sources | Documentation |
|-----------|---------|---------------|
| Language front-end | `src/lexer.*`, `src/parser.*`, `src/ast.hpp`, `src/diagnostics.*` | [`language-spec.md`](language-spec.md) §1–2 |
| Type system & module resolution | `src/types.*`, `src/checker.*` | [`language-spec.md`](language-spec.md) §3–4 |
| Evaluator | `src/eval.*` | [`language-spec.md`](language-spec.md) §5 |
| Signal engine | `src/signal.*`, `src/wav.*`, `src/vis.*` | [`engine.md`](engine.md) |
| Core library | `stdlib/core/` | [`core-library.md`](core-library.md) |
| External functions | `src/external.*`, `src/ext/` | [`core-library.md`](core-library.md) |
| Build system | `src/build.*`, `src/library.*`, `src/incremental.*`, `src/json.*` | [`build-system.md`](build-system.md) |
| CLI | `src/main.cpp` | [`build-system.md`](build-system.md) |
| Language server | `src/lsp.*` | [`tooling.md`](tooling.md) |
| VS Code extension | `editor/vscode/` | [`tooling.md`](tooling.md), [`editor/vscode/README.md`](../editor/vscode/README.md) |
| Dev app | `src/devapp/` | [`tooling.md`](tooling.md) |
| Tests | `tests/` | assert-based unit + end-to-end tests, run via CTest |

## Design invariants

A few properties hold across every component and are worth knowing
before reading any of them:

- **Purity by construction.** Evaluation is deterministic and effect-free
  except for declaring render targets. There is no RNG (even `noise` is
  deterministic FM), no I/O during evaluation beyond build-time audio
  file loading, and no hidden state. This is what makes caching,
  parallel rendering, and reproducible builds trivial to trust.
- **Signals are values; discretization is late.** A `Signal` is a lazy,
  immutable graph. Nothing touches audio frames until a render target is
  discretized at its declared rate. Sharing a subgraph is free and safe.
- **Byte-identical output is the contract.** Every engine optimization
  (parallelism, block rendering, fusion, sample caches, shared buses) is
  required to produce artifacts byte-identical to a naive sequential
  render, and is verified against it.
- **One front-end, many surfaces.** `synthc build`, `synthc lint`, and
  `synthc lsp` share the same lexer/parser/checker, so editor analysis
  and build analysis can never disagree.
- **Core is a real library.** Every primitive is declared in synth
  source (`stdlib/core/lib.synth`) as an `external` binding to an engine
  implementation — `open Core` is a plain library open, not compiler
  magic.

## Repository layout

| Path | Contents |
|------|----------|
| `src/lexer.*`, `src/parser.*`, `src/ast.hpp` | Language front-end: tokens (incl. timestamp unit-suffix literals), OCaml-like parser, AST with source spans |
| `src/types.*`, `src/checker.*` | Type system (rigid vs. free type variables, unification), fully-annotated checker with use-site instantiation of stored signatures, module resolution (files, libraries, inline `struct ... end` modules, the bundled Core) |
| `src/diagnostics.*` | Diagnostic representation and rendering with source spans |
| `stdlib/core/` | The Core library: `lib.synth` declares the ambient types (`list`, `Signal`, `Sample`) and every primitive's name and signature — mostly `external` bindings over the implementation `.cpp` files beside it, with the List module written in SynthGraph (oscillators, effects, sampling, render, io, lists, signals, math) |
| `src/external.*` | External-function loading: build-time C++ compilation, content-hash caching, dlopen binding — for Core and user code alike |
| `src/ext/` | The public API headers externals compile against: `<synth/external.hpp>` (values, context services, entry-point macro) and `<synth/engine.hpp>` (signal-graph constructors) |
| `src/signal.*` | Signal engine: lazy signal DAG, render-time discretization, sample/place windowing, filters, mixing, block rendering, fusion |
| `src/eval.*` | Evaluator: reduces definitions to values, collects render targets, `load_*` build-time validation |
| `src/wav.*` | WAV read (PCM 16/24/32, float 32/64) and write (PCM 16) |
| `src/vis.*` | Waveform SVG rendering for `render_vis` targets |
| `src/build.*` | `build.json` manifest (projects, libraries, roots), project validation, target enumeration, cached + parallel rendering, artifact + metadata emission, lint mode, watch loops (project + root daemon) |
| `src/library.*` | Library registry: dynamic discovery of `library` manifests under a root, directory-scanned member sets, `lib.synth` interface detection, dep validation, enclosing-root search |
| `src/incremental.*` | Dependency tracking: Merkle content hashes over definition closures for the build cache |
| `src/lsp.*` | `synthc lsp`: an LSP server over the front-end (diagnostics, completion, go-to-definition, hover, find-references, rename, outline, formatting), with unsaved-buffer overlays |
| `src/main.cpp` | `synthc` CLI (`build`, `watch`, `lint`, `lsp`) |
| `editor/vscode/` | VS Code extension: TextMate grammar for `.synth` plus a thin client that launches `synthc lsp` |
| `src/devapp/` | `synth-dev`: JSON/metadata reader, SDL audio player, ImGui shell with live refresh and `--self-test` |
| `tests/` | Unit + end-to-end tests (assert-based, run via CTest) |
| `examples/` | A buildable project root: per-primitive demos, reusable libraries, and full arranged tracks (see the README) |
| `outputs/` | Committed renders of the showcase projects; refresh with `scripts/render-outputs.sh` |
| `docs/` | This documentation and the original design document |
| `third_party/` | Vendored dependencies (Dear ImGui for the dev app) |
