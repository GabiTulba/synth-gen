# SynthGraph for Visual Studio Code

Language support for SynthGraph (`.synth`) source files:

- **Syntax highlighting** — keywords, timestamp literals (`500ms`),
  labeled arguments (`~cutoff:`), type variables (`'a`), module paths,
  nested `(* ... *)` comments.
- **Diagnostics** — the compiler front-end (parse + type-check) runs on
  every keystroke; errors appear inline, exactly as `synthc lint`
  reports them.
- **Completion** — names in scope (own definitions, parameters, locals,
  everything brought in by `open`), and module members after a dot
  (`Core.Osc.`).
- **Go to definition** — into the same file, sibling modules, library
  members, and the bundled Core interface (`stdlib/core/lib.synth`).
- **Hover** — the checked type of the identifier under the cursor.

Everything but the grammar is served by `synthc lsp`, the language
server built into the compiler, so editor analysis and build analysis
can never disagree. Unsaved buffers are layered over the on-disk
project: imports and libraries resolve exactly as a build after save
would see them.

## Setup

1. Build the compiler (from the repository root):

   ```sh
   cmake -B build -G Ninja && cmake --build build
   ```

2. Install the extension's one dependency and package or side-load it:

   ```sh
   cd editor/vscode
   npm install

   # Option A: run from source - open editor/vscode in VS Code and
   # press F5 (Run Extension), or symlink into your extensions dir:
   ln -s "$(pwd)" ~/.vscode/extensions/synthgraph.synthgraph-0.1.0

   # Option B: build a .vsix and install it:
   npx @vscode/vsce package
   code --install-extension synthgraph-0.1.0.vsix
   ```

3. Open a folder containing `.synth` files. The extension finds `synthc`
   in this order:
   - the `synthgraph.synthcPath` setting, if set;
   - `<workspace folder>/build/synthc`;
   - `synthc` on `PATH`.

## Notes

- The server analyzes each file the way `synthc lint` does: it locates
  the enclosing project root (a `build.json` with `build` rules, if
  any), discovers libraries under it, and resolves `import`/`open`
  through the same code paths as the build. Every discovered library is
  considered in scope, so editing never demands manifest bookkeeping
  first.
- Diagnostics come from the front-end only; nothing is rendered and no
  artifacts are written.
