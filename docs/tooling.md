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

Lint additionally **warns on unused opens**: an `open` that binds
nothing the file actually uses (no value, type, constructor, or module
name resolved through it) is reported as a warning on the files you
named — imports stay quiet, and builds never emit it. This is what
keeps the historical nine-line `open` preamble from being cargo-culted
forward; most sound-design files now want just
`open Core open Core.Dsp`.

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
- **Find references** — for parameters, locals (shadowing respected),
  and top-level definitions. Because the checker rewrites every resolved
  reference to canonical form, matching is exact — never textual. The
  search covers the open documents plus every `.synth` file under the
  enclosing project root, so uses are found in files that import the
  definition even when they are not open.
- **Rename** — built on the same reference search, returned as a
  workspace edit across all affected files. Renames that cannot be done
  safely are refused: labeled parameters (the label is part of every
  call site's syntax), Core definitions (the bundled stdlib is
  read-only), and new names that are not value identifiers. Collision
  with an existing name in scope is not checked — the diagnostics on the
  next keystroke report any breakage.
- **Document outline** — modules and definitions as a nested symbol
  tree, each with its checked type.
- **Formatting** — deliberately conservative and purely lexical: the
  author's line breaks, indentation, comments, and alignment columns are
  kept, while token spacing is normalized (operators spaced, delimiters
  tight, annotation colons as written), trailing whitespace stripped,
  blank-line runs collapsed, and the final newline fixed. Token text is
  copied verbatim, so formatting can never change what a file means; a
  file the lexer cannot read is left untouched. The whole `examples/`
  and `stdlib/` tree is already a fixed point of the formatter.

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
- Opens each audio target's waveform in its own floating window — drag
  it anywhere, resize it, close it from the title bar. Inside: wheel
  zoom, right-drag pan, left-drag range selection, playback of the
  selection or visible range, and a **loop** toggle that replays the
  played range indefinitely (toggling mid-play applies immediately).
- Shows the build's live controls (`Core.Control.slider` / `knob`) as
  sliders and rotary knobs. Releasing a drag writes the value into the
  unit's `controls.json` (atomically); with `synthc watch` running next
  door, that write triggers a rebuild with the override applied — this
  is how the app *attaches* to a watch instance. A `*` marks values
  still waiting for their rebuild; `reset` / `all defaults` clear
  overrides. See [`build-system.md`](build-system.md) for the file
  format.
- Shows build diagnostics, including for failed builds (the metadata is
  emitted either way).
- Live-refreshes by watching the metadata file, so pairing it with
  `synthc watch` gives save → rebuild → the app reflects the new
  artifacts. Because metadata is written after the artifacts, a
  metadata change also forces every open waveform window to reload —
  even when an artifact was rewritten with the same size inside the
  filesystem's mtime granularity.
- `--self-test` exercises the metadata reader and player headlessly.

SDL2 is the dev app's only external dependency
(`scripts/install-deps.sh`); Dear ImGui is vendored under
`third_party/`. Without SDL2 the dev app is skipped by the build and
everything else still works.
