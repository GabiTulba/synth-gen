# SynthGraph Language Specification (v1)

The implementation reference for the `.synth` language (design doc
Epic 0.2). This document describes what the implementation in `src/`
accepts and how it is typed and evaluated. The design document
(`synthgraph-design-v2.pdf`) explains *why*; this file pins down *what*.

## 1. Lexical structure

- **Whitespace** separates tokens and is otherwise insignificant.
- **Comments**: `(* ... *)`, nesting allowed, may span lines.
- **Identifiers**: `[A-Za-z_][A-Za-z0-9_']*`. Lowercase-initial
  identifiers name bindings and parameters; uppercase-initial identifiers
  name modules and types. `_` is a valid binding name with special
  meaning (see §4).
- **Keywords**: `let`, `in`, `fun`, `import`, `open`, `module`.
- **Number literals**: `[0-9]+(\.[0-9]+)?` — always `Scalar`.
- **Timestamp literals**: a number literal immediately followed by a unit
  suffix: `ns` (1e-9 s), `us` (1e-6 s), `ms` (1e-3 s), `s` (1 s),
  `m` (60 s). All denote the same quantity (time since the epoch or a
  duration); e.g. `100ns`, `800ms`, `1.5s`, `1m`. An unknown suffix is a
  lexical error. A suffix only attaches to a *literal*; to carry a
  computed Scalar into the time domain use `to_sec`/`to_ms`/`to_min`
  (§5.4).
- **String literals**: `"..."` with escapes `\n`, `\t`, `\\`, `\"`.
- **Punctuation**: `;;` `;` `:` `=` `(` `)` `[` `]` `,` `.` `->` `~`
  `|>` `+` `-` `*` `/`.

## 2. Grammar

EBNF; `{x}` is repetition, `[x]` is optionality.

```
module      ::= { top-def }
top-def     ::= import-def | open-def | alias-def | let-def
module-path ::= UpIdent { "." UpIdent }             (Lib or Lib.File)
import-def  ::= "import" module-path [ ";;" ]
open-def    ::= "open" module-path [ ";;" ]
alias-def   ::= "module" UpIdent "=" module-path [ ";;" ]
let-def     ::= "let" (Ident | "_") { param } [ ":" type ] "=" expr ";;"
param       ::= [ "~" ] Ident ":" param-type      (~ marks a labeled param)

type        ::= postfix-type [ "->" type ]          (right-associative)
param-type  ::= postfix-type                        (arrows need parens)
postfix-type::= atom-type { "Signal" | "Sample" | "list" }
atom-type   ::= "Scalar" | "Vector" | "Timestamp" | "String" | "unit"
              | "(" type { "," type } ")"           (tuple if >1 element)

expr        ::= let-in | lambda | pipe
let-in      ::= "let" Ident ":" type "=" expr "in" expr
lambda      ::= "fun" param { param } "->" expr
pipe        ::= additive { "|>" additive }          (lowest, left-assoc)
additive    ::= multiplicative { ("+" | "-") multiplicative }
multiplicative ::= unary { ("*" | "/") unary }
unary       ::= "-" unary | app
app         ::= atom { arg }                        (application, left)
arg         ::= atom | "~" Ident ":" atom           (labeled argument)
atom        ::= Number | Time | String
              | Ident                               (unqualified name)
              | module-path "." Ident               (qualified name)
              | "(" expr { "," expr } ")"           (tuple if >1 element)
              | "[" [ expr { ";" expr } ] "]"       (list literal)
```

Notes:

- A `let` with parameters defines a function; without, a constant. The
  return-type annotation is mandatory except for `let _`.
- Application binds tighter than operators: `sine 440.0 * 0.5` is
  `(sine 440.0) * 0.5`. `*`/`/` bind tighter than `+`/`-`; all are
  left-associative.
- Unary minus desugars to `0 - x` (so it is Scalar-typed arithmetic).
- List elements are separated by `;` (OCaml style); the empty list `[]`
  is a parse but not a type — see §3.
- Function-typed parameters require parentheses:
  `f:(Timestamp -> Scalar Signal)`.
- **Local bindings**: `let name : Type = e in body` is an expression;
  the binding is annotated like every other binding, binds a *value*
  (no parameters in v1), scopes over `body` only, and may shadow
  parameters, module definitions, or outer locals. Chained sub-lets nest
  naturally:

  ```
  let song : Scalar Signal =
    let hit : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:100ms in
    let beats : Timestamp list = time_steps ~start:0s ~step:200ms ~count:5.0 in
    place_multi hit beats
  ;;
  ```
- **Lambdas**: `fun x:Scalar ~y:Timestamp -> body` is an anonymous
  function expression. Parameters are annotated exactly like `let`
  parameters (function-typed ones parenthesized, `~` marks a label); the
  return type is synthesized from the body. The body extends maximally to
  the right, so a lambda used as an argument or on the right of `|>` must
  be parenthesized: `map (fun t:Timestamp -> place hit t) beats`.
- **Pipe**: `x |> f a b` desugars to the application `f a b x` — the
  piped value becomes the final positional argument. The right-hand side
  must be a function name, application, or parenthesized lambda; chains
  are left-associative:
  `saw 220.0 |> lowpass ~cutoff:800.0 |> soft_clip 0.8`.

## 3. Type system

Types: `Scalar`, `Vector`, `Timestamp`, `String`, `unit`, `t Signal`,
`t Sample`, `t list`, tuples `(t1, ..., tn)`, and function types
`t1 -> ... -> tn -> r` (in signatures only). `Signal`/`Sample` element
types are in practice `Scalar` (mono) or `Vector` (N-channel).

Rules:

- **Fully annotated, no inference.** Every parameter and every return
  type is written; the checker verifies and never guesses.
- **Monomorphic user code.** Type variables occur only in built-in
  primitive signatures and are instantiated (by unification) at each call
  site. Users cannot write polymorphic definitions. The math primitives
  (`exp`, `sqrt`, `log`, `pow`) use an unconstrained `'a`, so the checker
  admits any argument type; passing anything but a Scalar, Vector, or
  Scalar Signal is reported as a build (evaluation) error. `time` is the
  one nullary primitive: it is a `Scalar Signal` value, not a function.
- **Definition before use, no recursion.** A definition may reference
  only parameters, *earlier* definitions of its module, imported modules'
  definitions (qualified), names brought in by an *earlier* `open`, and
  Core primitives (see below). Name resolution order for unqualified
  names: local binding/parameter (innermost first) → the latest earlier
  top-level binder (an own definition or an `open`ed name —
  position-ordered, so an `open` shadows earlier same-named binders and
  a later definition shadows the open) → a Core primitive, but only
  under an earlier `open Core` (or `open Core.List` for the list
  functions). Re-defining one of the module's *own* names is still a
  duplicate-definition error.
- **The Core namespace.** All primitives live in the built-in `Core`
  module — except the list functions, which live in its `Core.List`
  submodule under OCaml-style names: `List.map`, `List.fold`,
  `List.init` (= the doc's `list_init`), `List.repeat`. Files start
  with `open Core`, which makes primitives callable bare and binds
  `List` (so `List.map f xs` works); `open Core.List` additionally
  makes the list functions bare. Qualified access (`Core.sine`,
  `Core.List.map`) always works, with no open or import. `Core`
  aliases like any module (`module C = Core` then `C.sine`,
  `C.List.map`); the name `Core` is reserved — a user file or library
  named Core is not reachable.
- **Module references** (`import`/`open`/alias targets and qualified
  names) resolve their first segment as: module alias bound earlier in
  the file (later aliases override) → a *sibling* file module — any
  `.synth` file in the same directory, which inside a library is
  exactly its member set (a sibling wins over a library of the same
  name) → a discovered library (which must be `dep`-declared). A
  library member therefore reaches every other member by short name,
  with nothing listed anywhere; the library's own `lib.synth` is not a
  sibling and naming it (or a `Lib.Member` path into one's own
  library) is an error. From outside, only what `lib.synth` binds is
  reachable: `Lib.X` resolves through the `module X = …` bindings
  there, under the name the interface gives it.
- **Local bindings** type-check like top-level ones: the bound expression
  must match the annotation (a var-carrying partial application of a
  polymorphic primitive resolves against it), and the name is visible
  only in the body of the `in`.
- **Labeled arguments & partial application.** A parameter declared
  `~name:Type` is *labeled*; primitive parameters are all labeled with
  their signature names. At a call site, positional arguments fill the
  leftmost unfilled parameters in order, and labeled arguments
  (`f ~x:v`) fill their parameter by name, in any order. If every
  parameter is filled the call evaluates. Otherwise the call is a
  *partial application*: its value is the curried function of the
  remaining parameters, in declaration order, keeping their labels
  (`lowpass ~cutoff:800.0 : 'a Signal -> 'a Signal`,
  `place hit : at:Timestamp -> Scalar Signal`). Any subset may be left
  unfilled — a positional prefix, a labeled subset, or a mix. Labels are
  not part of type equality.
- **Application otherwise.** Any expression of function type may be
  applied or passed where a matching function type is expected: a bare
  name (`map place_pluck [...]`, `map sine [...]`), a partial
  application (`map (place hit) beats`, `(f 1.0) 2.0`), a
  function-typed parameter, or a lambda. Primitive signatures'
  type variables are instantiated fresh at every call site, so partial
  applications of polymorphic primitives can flow directly into other
  polymorphic calls (`map (lowpass ~cutoff:600.0) sigs`); a *stored*
  one also resolves against the binding's annotation
  (`let damp : Scalar Signal -> Scalar Signal = lowpass ~cutoff:600.0`).
- **`let _` is the effect form.** Its body must have type `unit`, and
  the render primitives (`render`, `render_vis`) are the only sources of
  `unit`.
- **Empty list literals are rejected** ("cannot determine the element
  type"); list elements must all have one type.
- **Duplicate definitions** in a module and **duplicate parameters** in a
  signature are errors.

### Operators (pointwise lifting + Scalar broadcasting)

For `+ - * /` with operand types L and R:

| L | R | Result |
|---|---|--------|
| Scalar | Scalar | Scalar |
| Vector | Vector | Vector (element-wise; channel counts must match at build time) |
| Vector | Scalar (either order) | Vector |
| t Signal | t Signal | t Signal (pointwise) |
| t Signal | Scalar (either order) | t Signal (broadcast) |

Anything else (e.g. `Timestamp + Signal`) is a type error. Channel-count
mismatches between Vector Signals are detected when the signal graph is
built — before any audio is computed — and signals are capped at 16
channels in v1.

## 4. Modules, libraries & projects

- **File = module.** `foo.synth` defines module `Foo` (stem, first
  letter capitalized). All top-level definitions are visible to
  importers; access is qualified (`Foo.bar`). A library member's
  canonical module id is `Lib.Foo`.
- **`import A`** resolves the module `A` in the current context:
  `a.synth` next to the importing file (which inside a library means a
  fellow member), or a discovered library. `import Lib` brings in the
  library's interface module, making everything its `lib.synth`
  publishes reachable as `Lib.def` and `Lib.X.def`; `import Lib.X`
  imports one published module. Unresolved imports and import cycles
  are build errors.
- **`open`** brings names into scope, shadowing by position (see §3):
  `open Lib` injects the interface module's own definitions *and* the
  module names it binds (so `def` and `X.def` work unqualified at the
  file level); `open File` / `open Lib.X` injects that module's
  definitions directly. An `open` implies the corresponding import.
- **`module K = Path`** binds (or overrides) a module name: the target
  may be a file module by any spelling in scope, an earlier alias, or a
  whole library (`module B = Basic` then `B.Keys.def`). At a library
  interface's top level it also *publishes* `K` (see below).
- **Library**: a directory whose `build.json` declares
  `"library": "<Name>"` and, optionally, `"dependencies"`. It lists no
  files: every `.synth` file in the directory is a member, and members
  import each other freely by short name. The directory must contain a
  `lib.synth` **interface file**, which is the library itself — module
  `Lib`, not `Lib.Lib` — and declares the entire public surface:

  ```
  open Core
  import Doppler

  module Doppler = Doppler ;;                (* publish a member module *)
  module Echo    = Delay ;;                  (* ...possibly renamed *)

  let fly_by ~rate:Scalar : Scalar Signal =  (* or a value of its own *)
    Doppler.pan (Doppler.swept_saw ~rate:rate) rate
  ;;
  ```

  A member not bound by `lib.synth` is internal: reachable from its
  siblings, invisible outside. `lib.synth` is not itself a member, so
  members cannot import it — nor may they refer to their own library by
  name (`Own.Sibling` inside `Own` is an error; use `Sibling`).
  Libraries may declare render targets; building the library renders
  them into its own directory under the root's `_build/`, and
  consumers' builds do NOT re-render a dependency's targets.
- **Project**: a directory with a `build.json` manifest — one JSON
  object with `"project": "<name>"` plus a `"sources"` array (and an
  optional `"dependencies"` array); an optional `"description"` string
  carries free-form prose (JSON has no comments).
- **Root**: a `build.json` with `"project": "<name>"` and a `"build"`
  array naming one rule per buildable unit (a project/library directory
  or a single `.synth` file). Libraries are discovered dynamically by
  recursively scanning the tree under the root (skipping `_build/`
  output, legacy `build/`, and hidden directories); dependency names
  resolve against that discovered set, wherever the library lives in
  the tree. Duplicate library names, unknown dependencies and library
  dependency cycles are build errors. Building a dependency-carrying
  directory on its own finds its enclosing root automatically; `synthc
  build`/`watch` at the root builds/watches every rule.
- **Render targets**: every `render` call evaluated at build time
  declares one. Names must be unique per build unit. All outputs land
  in a single `_build/` tree at the root, mirroring the source layout:
  rule `song` writes artifacts to `<root>/_build/song/artifacts/
  <name>.wav` and the machine-readable index to
  `<root>/_build/song/metadata.json`; a file rule `tunes/t.synth`
  writes under `_build/tunes/t/`. A unit built outside any root uses
  its own directory as the root.

## 5. Evaluation semantics

- Top-level definitions evaluate in order (imports first, in dependency
  order). Everything is pure except `render`, which records a target.
- A `Signal` value is a lazy graph; nothing is discretized until a render
  target is produced, at that target's declared rate. Signals are defined
  for every t ≥ 0 (the epoch).
- `sample s from to` cuts the window `[from, to)`; `place smp at` embeds
  it back at `at`, silent elsewhere. Stateful primitives (filters,
  `fm`, `delay`, `reverb`) evaluate from the epoch of their own timeline;
  a placed sample's interior state warms up from t = 0 of its source.
- `resample input ~f` remaps time the other way: instead of moving a
  window, it changes how fast the source is read. `f` is a *rate
  multiplier* evaluated on the output's timeline, so
  `out(t) = input(phi(t))` where `phi` is the running integral of `f` -
  `fun t -> 1.0` is the identity, `0.5` halves the speed (an octave down),
  `2.0` doubles it. Like a placement, the source runs on its own timeline
  from its own epoch. The read head only ever moves forward: `0.0` freezes
  it on the current sample and a negative rate clamps to `0.0` rather than
  playing backwards. Rates above 64 are rejected as runaways. Values
  between source frames are linearly interpolated, so warping is not
  lossless.
- Audio files are build inputs: `load_mono`/`load_multi` read and
  validate them at build time (channel counts checked against the
  primitive; paths resolve relative to the source file). A loaded file
  occupies `[0s, duration)` and is silence afterward; content is
  linearly resampled to the render rate.
- Renders are deterministic; incremental rebuilds and caching rely on it.

## 6. Primitive signatures (v1 roster)

Everything below lives in the built-in `Core` module (`open Core` for
bare names, or `Core.sine`), except the four `Core.List` functions
listed with their `List.` names.

```
(* generators *)
val sine      : freq:Scalar -> Scalar Signal
val saw       : freq:Scalar -> Scalar Signal
val square    : freq:Scalar -> Scalar Signal
val noise     : freq:Scalar -> Scalar Signal          (* two-step FM; deterministic *)

(* envelopes *)
val exp_decay : rate:Scalar -> Scalar Signal          (* e^(-rate*t) *)
val adsr      : attack:Timestamp -> decay:Timestamp -> sustain:Scalar
             -> release:Timestamp -> hold:Timestamp -> Scalar Signal

(* filters *)
val lowpass   : cutoff:Scalar -> input:'a Signal -> 'a Signal
val highpass  : cutoff:Scalar -> input:'a Signal -> 'a Signal

(* distortion *)
val hard_clip : threshold:Scalar -> input:'a Signal -> 'a Signal  (* clamp at +/-threshold *)
val soft_clip : threshold:Scalar -> input:'a Signal -> 'a Signal  (* threshold*tanh(x/threshold) *)

(* modulation *)
val fm        : carrier:Scalar -> modulator:Scalar Signal -> Scalar Signal
val pm        : carrier:Scalar -> modulator:Scalar Signal -> Scalar Signal
val am        : carrier:'a Signal -> modulator:Scalar Signal -> depth:Scalar -> 'a Signal

(* time effects *)
val delay     : by:Timestamp -> signal:'a Signal -> 'a Signal
val resample  : input:'a Signal -> f:(Scalar -> Scalar) -> 'a Signal  (* f is a playback-rate multiplier *)
val reverb    : decay:Timestamp -> damping:Scalar -> mix:Scalar
             -> input:'a Signal -> 'a Signal

(* combination *)
val mix_all   : signals:'a Signal list -> 'a Signal
val channels  : chans:Scalar Signal list -> Vector Signal

(* slicing & arrangement *)
val sample    : signal:'a Signal -> from:Timestamp -> to:Timestamp -> 'a Sample
val place     : sample:'a Sample -> at:Timestamp -> 'a Signal
val place_multi : sample:'a Sample -> ats:Timestamp list -> 'a Signal  (* mix of placements; overlaps sum *)

(* the effects *)
val render    : name:String -> rate:Scalar -> sample:'a Sample -> unit
val render_vis: name:String -> rate:Scalar -> sample:'a Sample -> unit  (* waveform SVG *)
val render_stems : name:String -> rate:Scalar
                -> stems:(String, 'a Sample) list -> unit
  (* one audio target per stem, named "<name>-<label>" *)
val render_vis_stems : name:String -> rate:Scalar
                    -> stems:(String, 'a Sample) list -> unit
  (* ONE svg artifact: a labeled waveform lane per stem, shared time axis *)

(* file import *)
val load_mono : path:String -> Scalar Signal
val load_multi: path:String -> Vector Signal

(* signal constructors *)
val constant  : value:Scalar -> Scalar Signal
val constant_multi : levels:Scalar list -> Vector Signal  (* one level per channel *)
val time      : Scalar Signal          (* nullary: the ramp t, in seconds *)
val signal    : f:(Scalar -> Scalar) -> Scalar Signal
  (* samples f over time; the body is limited to arithmetic and the math
     primitives - nothing else maps a Scalar to a Scalar *)
val signal_multi : fs:(Scalar -> Scalar) list -> Vector Signal

(* math: polymorphic over Scalars and (elementwise) Signals; anything
   else is a build-time error. Domain follows IEEE: log of a
   non-positive value or sqrt of a negative one yield -inf/NaN samples. *)
val exp       : x:'a -> 'a
val sqrt      : x:'a -> 'a
val log       : x:'a -> 'a             (* natural *)
val pow       : x:'a -> y:Scalar -> 'a

(* Core.List: list combinators & builders *)
val List.map    : f:('a -> 'b) -> xs:'a list -> 'b list
val List.fold   : f:('a -> 'b -> 'a) -> init:'a -> xs:'b list -> 'a
val List.init   : n:Scalar -> f:(Scalar -> 'a) -> 'a list   (* [f 0.0; ...; f (n-1)] *)
val List.repeat : n:Scalar -> x:'a -> 'a list

(* timestamp construction & list utilities (Core proper) *)
val to_sec    : x:Scalar -> Timestamp
val to_ms     : x:Scalar -> Timestamp
val to_min    : x:Scalar -> Timestamp
val time_steps: start:Timestamp -> step:Timestamp -> count:Scalar -> Timestamp list
val jitter    : seed:Scalar -> spread:Timestamp -> steps:(Timestamp list) -> Timestamp list
```

Counts and indices are Scalars (the language's single numeric type);
counts must be whole and non-negative, validated at build time.

`to_sec`/`to_ms`/`to_min` are the computed counterpart of the literal
suffixes — `to_ms 250.0` is `250ms` — and are what a duration derived
from a tempo, a loop index, or a parameter has to go through:
`to_min (1.0 / bpm)` is one beat. There is deliberately no conversion
back: a Timestamp that can decay into a bare number is how unit
confusion gets in.

`jitter` humanizes a rhythm: each timestamp moves by a delta in
`[-spread, +spread]` (clamped at `0s`) derived by hashing
`(seed, index)`. The deltas are statistically random but the function is
pure - the same seed always yields the same feel, so builds stay
reproducible and cacheable; give each layer its own seed so they drift
independently:

```
place_multi hat (time_steps ~start:0s ~step:250ms ~count:32.0
                   |> jitter ~seed:7.0 ~spread:8ms)
```

Semantics details for the doc's open points (also in the README):
`adsr` sustains until `hold` then releases; `fm` integrates
`carrier + modulator(t)` Hz from the epoch; `pm`'s modulator is radians;
`am` computes `carrier * (1 + depth*modulator)` with a mono modulator
broadcast across carrier channels; `delay` is feedforward only; `reverb`
is a per-channel Schroeder bank (RT60-style `decay`, `damping`/`mix` in
[0,1]); `noise` is deterministic cascaded FM. `place_multi` sums its
placements without normalization, so overlapping or dense patterns can
exceed full scale — rendering works in doubles and only hard-clamps to
[-1, 1] at WAV write; scale down or use `soft_clip`/`hard_clip` to
manage headroom deliberately.

## 7. Out of scope in v1

Booleans and control flow, pattern matching, user-defined types,
recursion and feedback (IIR-style signal cycles), user polymorphism,
per-definition visibility control (a library's `lib.synth` publishes
whole modules or re-exported values, but a published module exposes all
of its definitions), reverse playback (`resample` reads its source only
forward, so a negative rate clamps to zero rather than rewinding),
cache tuning knobs, native extensions. See design
doc §13. (Lambdas, general partial application, and cross-directory
imports/packaging — via libraries, `open` and module aliases — were
listed here originally and are now in the language; see §2, §3 and §4.)
