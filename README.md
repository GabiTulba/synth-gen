# SynthGraph

A functional, text-first language and build system for creating audio
samples and full songs through composable, pure mathematical
transformations on sound.

A sound — from a short sample up to a fully arranged song — is an
ordinary source file (`.synth`), a composition of pure functions that
generate, transform, slice, and arrange signals. A project is compiled by
a build system; the resulting audio artifacts are browsed and played in a
companion dev app. No editor is shipped — the product surface is a
compiler, a linter, a language server, and a build daemon.

The full design rationale is in
[`docs/synthgraph-design-v2.pdf`](docs/synthgraph-design-v2.pdf).

## Quick start

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build                # run the test suite

build/synthc build examples/pluck     # one-shot build
# -> examples/_build/pluck/artifacts/demo.wav
# -> examples/_build/pluck/metadata.json

build/synthc watch examples/pluck     # rebuild on change
build/synth-dev examples/pluck        # browse & play the artifacts
```

Requires a C++20 compiler and CMake ≥ 3.20. The compiler, build system
and daemon have no third-party dependencies. The dev app (`synth-dev`)
additionally needs SDL2 (`scripts/install-deps.sh`); Dear ImGui is
vendored. Without SDL2 the dev app is skipped and everything else still
builds.

The whole toolchain also runs on an Android phone under
[Termux](https://termux.dev) with the Termux:X11 app as the display.
After a one-time setup (`scripts/install-deps.sh`, plus the Termux:X11
APK from its GitHub releases), `scripts/android-dev.sh <project>` starts
the display and audio servers, a `synthc watch`, and the dev app
fullscreen; see that script's header for details.

## A taste of the language

```ocaml
(* pluck.synth *)
open Core
open Core.Osc open Core.Fx open Core.Arrange open Core.Render

let pluck freq:Scalar : Scalar Signal =
  (sine freq) *. (exp_decay 6.0)
;;

let pluck_sample freq:Scalar : Scalar Sample =
  sample (pluck freq) 0s 800ms
;;

let song : Scalar Signal =
  mix_all (List.map (place (pluck_sample 440.0)) [0s; 500ms; 1s; 1500ms])
;;

let _ = render "demo" 48000.0 (sample song 0s 2s)
;;
```

Annotated parameters with locally inferred return types — with
records, variants and `match`, and `let rec` for self-recursion — and
`render` is the language's only effect.
Signals are lazy graphs; nothing is discretized until a render target is
produced. See the [language tour](docs/language-tour.md) for the full
feature walk-through.

## Documentation

| Document | Contents |
|----------|----------|
| [`docs/architecture.md`](docs/architecture.md) | Component map: how the compiler, engine, build system, and tools fit together; repository layout |
| [`docs/language-tour.md`](docs/language-tour.md) | Guided tour of the language: signals, samples, modules, labels, polymorphism, conditionals, externals |
| [`docs/language-spec.md`](docs/language-spec.md) | The v1 language specification: lexical structure, grammar, type system, evaluation semantics, primitive roster |
| [`docs/build-system.md`](docs/build-system.md) | The `synthc` CLI, `build.json` manifests (projects, libraries, roots), build outputs, incremental caching, the watch daemon |
| [`docs/engine.md`](docs/engine.md) | The signal engine: lazy signal DAGs, discretization, parallel and block-based rendering, fusion, sample caches |
| [`docs/core-library.md`](docs/core-library.md) | The bundled `Core` library: submodule organization, primitive semantics, and external functions in C++ |
| [`docs/tooling.md`](docs/tooling.md) | Developer tooling: `synthc lint`, the language server, the VS Code extension, and the `synth-dev` app |
| [`docs/roadmap.md`](docs/roadmap.md) | Future work: everything deliberately left out of v1 and the improvements under consideration |

## Examples

`examples/` is a buildable project root — `build/synthc build examples`
builds everything in it, from single-primitive demos
(`examples/primitives/`) through reusable libraries
(`examples/lib/voices/`, `examples/lib/effects/`, `examples/basic/`) up
to full arranged tracks (`examples/song/`, `examples/darksynth/`).
Committed renders of the showcase projects live in `outputs/` (`.wav` to
listen to and waveform `.svg`s to look at, without building anything);
refresh them with `scripts/render-outputs.sh`.
