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

- **Everything lives in a panel.** A panel is one window holding some of
  the project's controls together with the waveforms of the targets they
  shape, so a knob sits next to the sound it changes. There is no
  separate controls list and no separate waveform window: drag a knob,
  the daemon rebuilds, and the waveform right under it redraws.
- Panels come from `Core.Ui.panel` declarations in the source. Anything
  no panel names — and *everything*, in a project that declares none —
  collects into one further panel, so nothing is ever unreachable and a
  project that has never heard of panels still shows all of itself in
  one window.
- Each target inside a panel shows its duration, rate, channels, frame
  count and build status, then its waveform: wheel zoom, right-drag pan,
  left-drag range selection, playback of the selection or visible range,
  and a **loop** toggle that replays the played range indefinitely
  (toggling mid-play applies immediately). Visual targets name their
  `.svg` instead; playback is audio-only.
- A panel with one target gives it the whole window; with several, each
  gets a readable fixed height and the panel scrolls. Audio is decoded
  only for targets actually on screen and released when they scroll away
  or the panel closes — a couple of minutes of stereo costs about 90 MB
  as samples, so a project with a dozen stems would otherwise decode all
  of them at launch.
- Shows the build's live controls (`Core.Control`) with the widget each
  kind asks for: `slider` and `knob` as a slider and a rotary knob,
  `int_slider` as a slider that steps whole numbers, `toggle` (and
  `opt`'s tick) as a tickbox, and `choice` as one tickbox per option,
  labelled as the build's metadata gives them. Releasing a drag — or
  the click that flips a tickbox, which has no drag to release — writes
  the value into the unit's `controls.json` (atomically); with `synthc
  watch` running next door, that write triggers a rebuild with the
  override applied — this is how the app *attaches* to a watch
  instance. A `*` marks values still waiting for their rebuild;
  `reset` / `all defaults` clear overrides. See
  [`build-system.md`](build-system.md) for the file format.
- Draws a component's parts indented under it: a panel member carries
  the depth it had in the `Controller` tree it was given, so a choice
  and the slider that only exists while one option is picked read as one
  block rather than two loose rows.
- Draws a `multi_slider` group as linked lanes under a budget bar. A
  lane's track is banded: what it has taken, the headroom it can still
  take, and the stretch the other lanes have spoken for, with a tick at
  the limit. Dragging stops there — no lane ever moves on its own, so
  lower another lane to take more here. The budget bar turns amber when
  the group's sum reaches `sum_max`, and carries a mark at `sum_min`
  when there is one.
- The backdrop window is a thin header: per unit, the project name,
  build status, diagnostics, and a `panels:` row of checkboxes that
  shows and hides each panel. Having only what you are working on on
  screen is the point. A panel you have never touched opens by default;
  what you close, where you dragged each window and how big you made it
  all persist.
- Shows build diagnostics, including for failed builds (the metadata is
  emitted either way).
- Live-refreshes by watching the metadata file, so pairing it with
  `synthc watch` gives save → rebuild → the app reflects the new
  artifacts. Because metadata is written after the artifacts, a
  metadata change also forces every waveform on screen to reload — even
  when an artifact was rewritten with the same size inside the
  filesystem's mtime granularity.
- Remembers how you left it, in **`project.json` beside `build.json`**:
  where every knob and slider is set, the main window's placement and
  size, which panels you left open and where you put them, the zoom,
  selection and loop toggle of every waveform you set up, and which
  sections you left collapsed. It is a source-tree file, not a
  build output — it survives a clean, and it is yours to commit or ignore
  as you see fit. See [`build-system.md`](build-system.md#live-controls)
  for how it relates to `controls.json`.
- The file is written only when something actually changed, at most once
  a second, and once more on exit — so dragging a window or a knob does
  not thrash the disk. It is indented and its numbers are the shortest
  form that reads back identically (`0.42`, not `0.41999999999999998`),
  because it is meant to be read and diffed. Deleting it resets
  everything; a corrupt one is ignored rather than fatal.
- Control values are therefore never lost, `synthc watch` attached or
  not. On startup the app pushes what `project.json` records into the
  unit's `controls.json`, so an attached daemon applies them; values no
  build has used yet keep their `*` pending marker until one does.
- `--self-test` reads `project.json` so it reports the values the real
  app would show, but never writes it and never writes a `controls.json`:
  a headless smoke test must not edit the project. It lists the panels a
  build declared without opening a panel window or decoding a strip, so
  the run stays deterministic.

SDL2 is the dev app's only external dependency
(`scripts/install-deps.sh`); Dear ImGui is vendored under
`third_party/`. Without SDL2 the dev app is skipped by the build and
everything else still works.
