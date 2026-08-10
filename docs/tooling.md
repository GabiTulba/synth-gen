# Tooling

Everything around the compiler: the linter, the language server, the
VS Code extension, and the dev app. A common thread runs through all of
it — **one front-end, many surfaces**: `lint`, `lsp`, and `build` share
the same lexer, parser, checker, and module resolution, so editor
analysis and build analysis can never disagree.

## `synthc lint`

```sh
synthc lint path/to/file.synth
```

Front-end checks only: parse and type-check with full module resolution,
no artifacts written, nothing rendered. It locates the enclosing project
root (a `build.json` with `build` rules, if any), discovers libraries
under it, and resolves `import`/`open` through the same code paths as
the build — so a file lints exactly as it would build.

## The language server (`synthc lsp`)

`src/lsp.*` implements an LSP server (JSON-RPC over stdio) over the
compiler front-end:

- **Diagnostics** on every keystroke, exactly as `synthc lint` reports
  them.
- **Completion** — names in scope (own definitions, parameters, locals,
  everything brought in by `open`), and module members after a dot
  (`Core.Osc.`).
- **Go to definition** — into the same file, sibling modules, library
  members, and the bundled Core interface (`stdlib/core/lib.synth`).
- **Hover** — the checked type of the identifier under the cursor.

Unsaved buffers are layered over the on-disk project: imports and
libraries resolve exactly as a build after save would see them. Every
discovered library is considered in scope, so editing never demands
manifest bookkeeping first.

## The VS Code extension (`editor/vscode/`)

A TextMate grammar for `.synth` (syntax highlighting for keywords,
timestamp literals, labeled arguments, type variables, module paths,
nested comments) plus a thin client that launches `synthc lsp` for
everything else. Setup instructions are in
[`editor/vscode/README.md`](../editor/vscode/README.md).

## The dev app (`synth-dev`)

```sh
build/synth-dev examples/pluck
```

`src/devapp/` is an SDL2 + Dear ImGui artifact browser/player. It is a
*pure consumer* of build output — it reads `metadata.json`, never
compiler internals:

- Lists targets with duration, rate, channels, and build status
  (including visual targets; playback is audio-only).
- Plays artifacts through SDL audio.
- Shows build diagnostics, including for failed builds (the metadata is
  emitted either way).
- Live-refreshes by watching the metadata file, so pairing it with
  `synthc watch` gives save → rebuild → the app reflects the new
  artifacts.
- `--self-test` exercises the metadata reader and player headlessly.

SDL2 is the dev app's only external dependency
(`scripts/install-deps.sh`); Dear ImGui is vendored under
`third_party/`. Without SDL2 the dev app is skipped by the build and
everything else still works.
