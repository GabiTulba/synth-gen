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
build/synth-dev --scale 2 --size 1400x900 examples/pluck
```

`src/devapp/` is an SDL2 + Dear ImGui artifact browser/player. It is a
*pure consumer* of build output — it reads `metadata.json`, never
compiler internals. `--scale N` magnifies the whole UI, `--size WxH`
opens at a given size rather than the remembered one, and
`--fullscreen` fills the display.

### The shell

The window manager is modelled on **i3**: numbered tabs, each holding a
tree of split containers whose leaves are windows. Nothing floats and
nothing overlaps — the tree is the only authority on where a window is
and how big it is.

- **Everything lives in a window.** A window is one panel: some of the
  project's controls together with the waveforms of the targets they
  shape, so a knob sits next to the sound it changes. One further window
  is the **overview**: per unit the project name, build status,
  diagnostics, and the tick list of its panels. Ticking one there is how
  a window comes into existence; the row also says which tab is holding
  it. Those rows are elements like any other — each wears its key, `Tab`
  steps through them and `Enter` ticks them — and ticking one keeps the
  focus on the overview, so a run of them is a run of keystrokes.
- **Tabs** are numbered and can be named (`Alt+,`). `Alt+<n>` goes to a
  tab, creating it empty if it is not there yet — an empty tab says how
  to fill it. `Alt+Shf+<n>` sends the focused window to a tab. A tab
  you empty and walk away from closes itself; naming one keeps it, and
  so does standing on it.
- **The tree** splits horizontally and vertically, nested freely. A new
  window divides the focused one along its *longer* side (i3's
  `default_orientation auto`), so opening four panels gives a grid
  rather than four slivers in a row; `Alt+b` / `Alt+v` override that for
  the next one. Every container draws a thin border in a colour for its
  depth, so the nesting is visible in the layout itself, and `Alt+t`
  writes the focused tab's tree out as an outline with the focused
  window marked.
- **Resizing** is `Alt+r` (then `h/j/k/l`, `Shift` for a coarser step)
  or dragging the gutter between two windows. No child of a split goes
  below 5% of it.
- **The focus is a node, not a window.** `Alt+a` steps out to the
  container holding the focused window — it draws a ring around itself
  and lights its windows' titles — and from there *everything* applies
  to the whole container: move it, resize it, close it (all its windows
  go), send it to another tab (it arrives laid out as it was), or open a
  window into it (the newcomer becomes one of its children, as in i3).
  `Alt+Shf+a` steps back in.
- **Select mode** (`Alt+s`) reshapes the tree. `h/j/k/l` gather the
  neighbours next to the focus into a run — adjacent siblings of one
  container, which is the only shape that can be grouped without
  reordering anything — and then `b` groups the run side by side, `v`
  groups it stacked. `x` flattens the focused container: its windows
  join the one outside it, sharing out the space it had, which is the
  way back from a grouping. `a` / `Shf+a` walk to the container and
  back without leaving the mode. The tab bar counts what is gathered,
  everything gathered is ringed, and `Alt+t`'s outline marks both the
  focus and the run. There is one ring and it means one thing: this is
  what the next command acts on.
  - Grouping a run of *one* is i3's plain `split`: nothing is wrapped,
    and the next window opened here goes that way instead.
  - Grouping *everything* in a container re-orients that container
    rather than nesting a copy of it inside itself.
- Each window **scrolls** its own contents (`Up` / `Down`, `Ctrl+d` /
  `Ctrl+u` for half a page, `Ctrl+Left` / `Ctrl+Right` sideways — the
  bare arrows belong to the selected row, so the window's own scrolling
  takes the modifier it already uses for paging and scaling), so a panel
  with more in it than fits is never truncated,
  and **draws at its own scale**: `Ctrl+=` and `Ctrl+-` make one
  window's contents bigger or smaller, `Ctrl+0` puts it back. This is a
  re-render, not a magnification — the padding, spacing, scrollbars,
  rounding and grab sizes scale with the font (the same `ScaleAllSizes`
  the global `--scale` uses), and so do the sizes the app picks in raw
  pixels, so a knob at 80% is a smaller knob rather than a shrunken
  picture of one. **The zoom steps are the font's own baked sizes** —
  the font is rasterised at a ladder of them and a step moves between
  them, so glyphs are always drawn at a size they were baked at, one
  texel per pixel. Nothing is ever stretched, which is what keeps
  scaled text as sharp as unscaled text; a step is therefore about 8%
  near the middle of the range rather than a round number. The title
  says the percentage while it is not 100%, and the setting is
  remembered with the rest of the layout.

### The keyboard

`Alt` is the mod key. The table below is the whole map for normal mode;
it lives in `src/devapp/keymap.cpp`, and both help surfaces are rendered
from it, so it cannot drift out of date.

| Keys | |
|---|---|
| `Alt+h/j/k/l`, `Alt+←↓↑→` | move focus |
| `Alt+Shf+h/j/k/l` | move the focused window (at the tree's edge it is promoted to the root) |
| `Alt+1` … `Alt+9` | go to a tab; `Alt+Shf+<n>` sends the window there |
| `Alt+n` / `Alt+,` | new tab / rename this one |
| `Alt+b` / `Alt+v` | the next window opens to the right of / below this one |
| `Alt+r` | resize mode |
| `Alt+a` / `Alt+Shf+a` | focus the container this is in / step back into it |
| `Alt+s` | select mode: `h/j/k/l` gather neighbours, `b` / `v` group them, `x` flatten |
| `Alt+q` / `Alt+o` | close what is focused (a container takes its windows with it) / put the overview here |
| `Alt+d`, `Alt+/` | search |
| `Alt+t` / `Alt+w` | the tree outline / the which-key pane |
| `?` | the shortcuts that apply right here |
| a letter | select the row that shows it |
| a digit | focus the window that shows it |
| `Tab` / `Shf+Tab` | select the next row / the one before |
| `Up` / `Down`, `Ctrl+d` / `Ctrl+u` | scroll this window, a line or half a page |
| `Ctrl+Left` / `Ctrl+Right` | scroll it sideways, when it is narrower than its rows |
| `Ctrl+=` / `Ctrl+-` / `Ctrl+0` | draw this window bigger / smaller / at 100% |
| `Left` `Right` (`Shf` for finer) | nudge the selected control; `Alt+Backspace` puts it back to its default |
| `Enter` | flip a tickbox, take the next option, or show/hide a panel from the overview |
| `Space`, `-` `=`, `Alt+0`, `Alt+p` | on a selected waveform: play/stop, zoom, fit, loop |

- **Search** (`Alt+d`) matches, in one list: tabs by name *or* index (a
  bare number goes straight to that tab), windows by panel name —
  including panels no tab is showing, which is how you open one — and
  the individual controls and waveforms inside them. Accepting an
  element result focuses its window *and* selects that element, so
  `Alt+d`, `sust`, `Enter`, `l` finds a knob and turns it up.
- **Every row wears its key**, at the head of its name: `[s] adsr
  sustain amp`. Press the letter and that row is selected — no mode, no
  modifier, nothing to summon. A panel can reserve a row's letter with
  `Core.Ui.key` where it lists the controller; everything else takes the
  first free letter in panel order, so a reservation costs no other row
  its key. A `multi_slider` heading is the exception, and wears none:
  there is nothing on it to select, so the letters run past it to its
  lanes, which are rows like any other. Automatic keys are all the same length and never start with a
  reserved letter, so the typing is never ambiguous.
- **Every window wears its number**, counted in tree order — the same
  order the layout reads in. A bare digit focuses that window. Numbers
  and letters divide the bare keys between them: digits address the
  windows of this tab, letters address the rows of the focused one, and
  normal mode binds neither, so a row's key can never be shadowed by a
  command.
- A `multi_slider` **lane is a row of its own**, keyed and nudged
  separately, under the budget bar its group draws. Nudging one is
  clamped to the band the other lanes leave it, exactly as dragging it
  is, so no lane ever moves because another one did. The group's own row
  is that budget bar and holds no value, so `Tab` steps over it onto the
  first lane — its key still selects it.
- The keys sit in a **gutter down the left of the window**, so they line
  up in a column and every row's widget starts at the same place. A row
  with no key still gets the gutter, which is what keeps them aligned.
- A keyboard edit goes through exactly the same path as a drag: it
  writes the unit's `controls.json` and shows the same pending `*`.

### Help, as you type

- **`?`** works in every mode that has a map — normal, resize, select —
  and lists the shortcuts of the mode you pressed it in, filtered by
  what is selected: a waveform's keys appear when a waveform is
  selected, a control's when a control is. Any key closes it, and you
  come back to the mode you asked from rather than being dropped into
  normal.
- **`Alt` is a prefix you can tap**, emacs-style, not only a key to
  hold: press and release it on its own and the pane comes up at once
  listing the whole `Alt` map, and the next key is read as though `Alt`
  were still down (`Alt`, then `2`, goes to tab 2). `Esc` spends the
  prefix without doing anything else, so `Esc Esc` gets you out of one
  you did not mean to start. Holding `Alt` still works exactly as
  before.
- **The which-key pane** (`Alt+w`, remembered between runs) is emacs'
  `which-key`: it lists what can come next, with an explanation of each
  - the `Alt` map after a tap or a hold, that mode's keys in resize or
  select mode, and what would complete a part-typed sequence. It is fed
  by the same table as `?`, sizes its columns to the entries it actually
  has, and says how many it could not fit rather than running off the
  screen.

### Inside a window

- Panels come from `Core.Ui.panel` declarations in the source. Anything
  no panel names — and *everything*, in a project that declares none —
  collects into one further panel, so nothing is ever unreachable and a
  project that has never heard of panels still shows all of itself.
- Each target shows its duration, rate, channels, frame count and build
  status, then its waveform: wheel zoom, right-drag pan, left-drag range
  selection, playback of the selection or visible range, and a **loop**
  toggle that replays the played range indefinitely (toggling mid-play
  applies immediately). The canvas owns the wheel while the pointer is
  over it, so zooming never scrolls the window out from under you.
  Visual targets name their `.svg` instead; playback is audio-only.
- **Waveforms are sized to the window they are in.** Everything else in
  a panel — the control rows, each waveform's heading, format line and
  transport row, the `defaults` button at the bottom — costs the same
  whatever height the waveforms are drawn at, so the body's height is
  that fixed cost plus one share per target. The panel measures what it
  actually came to last frame and corrects the share from the slack it
  had left over. One target takes the lot; four share it. A panel
  reaches for a scrollbar only when its own rows do not fit, not merely
  because it has waveforms in it — and never for the few pixels a
  button's descender would otherwise cost it.

  The share is a whole number of pixels, and is only reopened when a
  whole pixel is going spare for every waveform. Dividing the slack out
  exactly would be right if a panel's height were a continuous function
  of the height it is handed, and it is not — every row's position is
  truncated to a pixel, so half a pixel handed to each of two waveforms
  comes back unspent and is offered again next frame. A panel that did
  that would alternate between two heights a pixel or two apart for as
  long as it was on screen, which does not read as movement: it reads
  as a bottom edge drawn twice, slightly out of register.

  The split stops at a floor: a canvas divides its height between the
  channels it has, and a lane below about 28px stops showing a shape at
  all, so a stereo target holds out for twice the room a mono one does.
  Under that floor the panel gives up and scrolls, which is the honest
  answer for a window too short for what is in it — make it taller, or
  send a panel to a tab of its own.

  Across, a panel has no such lever: a knob is the size it is and a name
  is as long as it was written, and the widgets that size themselves to
  the window already stop at a minimum width. So a window narrower than
  its widest row gets a horizontal scrollbar rather than quietly cutting
  the row off at the edge. The waveforms are drawn to the panel's whole
  width rather than to the part of it in view, so scrolling sideways
  reveals more of them instead of carrying them off the left edge — and
  they are the one thing that does not *set* that width, or the two
  would push each other wider every frame. A target scrolled out of
  view holds the width it had when it was last drawn, for the same
  reason: if being scrolled away made a panel a different width from
  being on screen, the scrollbar that came and went with it would take
  a strip off the bottom of the panel, which would change what was
  scrolled away.

  Audio is decoded only for targets actually on screen and released when they scroll away
  or the window closes — a couple of minutes of stereo costs about 90 MB
  as samples.
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
  `reset` / `defaults` clear overrides. See
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
- Shows build diagnostics, including for failed builds (the metadata is
  emitted either way). A failed build never costs you the layout: only a
  build that succeeded is believed about which panels exist, so a typo
  cannot close your windows.
- Live-refreshes by watching the metadata file, so pairing it with
  `synthc watch` gives save → rebuild → the app reflects the new
  artifacts. Because metadata is written after the artifacts, a
  metadata change also forces every waveform on screen to reload — even
  when an artifact was rewritten with the same size inside the
  filesystem's mtime granularity.

### What it remembers

- **`project.json` beside `build.json`**: where every knob and slider is
  set, the main window's placement and size, **the tabs — their names,
  their trees, the size of every split and which one you were on** —
  whether the outline and the which-key pane were open, the zoom,
  selection and loop toggle of every waveform you set up, and which
  sections you left collapsed. It is a source-tree file, not a build
  output — it survives a clean, and it is yours to commit or ignore as
  you see fit. See [`build-system.md`](build-system.md#live-controls)
  for how it relates to `controls.json`.
- A settings file written before tabs existed is migrated: tab 1 takes
  the overview plus every panel that was open, tiled. A panel that
  disappears from a build is dropped from its tab, and comes back where
  it was if its declaration returns.
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
  app would show, but never writes it and never writes a
  `controls.json`: a headless smoke test must not edit the project. It
  builds a fresh layout rather than the saved one, reports the controls,
  the panels and each tab's tree, and draws one frame in every mode — so
  an unbalanced overlay is an assert in CI rather than a crash in front
  of you. It then *types*: a scripted run of real key events through
  ImGui's own input queue — enter select mode, gather a neighbour, group
  them, tap `Alt` and switch tab, label the rows and pick one — and
  reports what they left behind. That is the only coverage the seam
  between a key event and a chord has, and it runs against two projects,
  one of which has several windows and rows to address.
- It also checks that every panel has **come to rest** by the time the
  typing stops: that its height, width, waveform height and scroll
  extents were the same on the last several frames as on the ones
  before. A layout that solves itself from what it measured last frame
  can hunt between two answers instead of settling on one, and a panel
  redrawn at two heights on alternate frames does not look like it is
  moving — it looks like its bottom edge is drawn twice, a pixel or two
  out of register. No single frame is wrong, so only a run can catch it.
  Whether it hunts depends on the window size, which is why one of the
  runs passes `--size` and `--scale` to squeeze the panels until they
  scroll both ways.

### How it is put together

`synthdev_core` is the headless half, and everything with a rule in it
lives there: the container tree and its i3 operations
(`layout.{hpp,cpp}`), the shortcut table and its state machine
(`keymap.{hpp,cpp}`), the search index, the window-element enumeration
and the row keys (`search.{hpp,cpp}`), plus metadata, project state,
the audio player and the waveform maths.

**One machine owns every key press.** `KeyMachine::dispatch` is the only
way in, and it is a stack of small machines tried in a fixed order — the
first to claim a press ends it:

1. **capture** — the press addressed something on screen (a row's key, a
   window's number) or the mode read it as typing (a search query, a
   tab's new name);
2. **overlay** — the help page, which any press closes;
3. **capture's leftovers** — in such a mode only its control keys go on,
   so a letter never reaches the map behind it;
4. **prefix** — a tapped `Alt`, or a part-typed sequence;
5. **map** — the binding table for the current mode.

Nothing outside gets a second say, which is what stops a press acting at
two levels: selecting the row keyed `s` and *then* being read as a
command. The one thing the app decides for itself is whether a press
addressed something — only it knows the rows it just drew — and it tells
the machine, which does the rest. Tests pin the ordering, and two pin
the invariants underneath it: a capture mode may not bind a bare letter,
since it could never see one, and **normal mode binds no bare letter or
digit at all**, since those belong to the rows and the windows. `devapp_tests` links that and
nothing else, so the whole navigation model is unit-tested without a
window. `app.cpp` is the shell that drives it and `widgets.cpp` what
goes inside a window; between them they are the only files that mention
ImGui.

SDL2 is the dev app's only external dependency
(`scripts/install-deps.sh`); Dear ImGui is vendored under
`third_party/`. Without SDL2 the dev app is skipped by the build and
everything else still works.
