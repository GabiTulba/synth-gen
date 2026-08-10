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
- **Type variables**: `'` immediately followed by an identifier: `'a`,
  `'elem`. They appear only in type annotations (§3). A `'` *starts* a
  token only here — inside an identifier it is an ordinary continuation
  character, so `x'` remains one name.
- **Keywords**: `let`, `in`, `fun`, `import`, `open`, `module`, `if`,
  `then`, `else`, `true`, `false`. The words `struct`, `end` and
  `external` are *contextual*: they matter only inside a
  `module N = struct … end` definition (resp. as a definition's whole
  `external "file"` body) and lex as ordinary identifiers everywhere
  else (a binding named `end` or `external` keeps working).
- **Number literals**: `[0-9]+(\.[0-9]+)?`. A literal *with* a decimal
  point is a `Scalar` (`440.0`, `0.5`); one *without* is an `Int`
  (`8`, `440`). Ints are the discrete kind (counts, indices); Scalars
  the continuous one (frequencies, gains, levels). Neither converts to
  the other implicitly (§3).
- **Timestamp literals**: a number literal immediately followed by a unit
  suffix: `ns` (1e-9 s), `us` (1e-6 s), `ms` (1e-3 s), `s` (1 s),
  `m` (60 s). All denote the same quantity (time since the epoch or a
  duration); e.g. `100ns`, `800ms`, `1.5s`, `1m`. An unknown suffix is a
  lexical error. A suffix only attaches to a *literal*; to carry a
  computed Scalar into the time domain use `to_sec`/`to_ms`/`to_min`
  (§5.4).
- **Boolean literals**: `true`, `false` — always `Bool`.
- **String literals**: `"..."` with escapes `\n`, `\t`, `\\`, `\"`.
- **Punctuation**: `;;` `;` `:` `=` `(` `)` `[` `]` `,` `.` `->` `~`
  `|>` `+` `-` `*` `/` `<` `<=` `>` `>=` `==` `!=` `&&` `||`.

## 2. Grammar

EBNF; `{x}` is repetition, `[x]` is optionality.

```
module      ::= { top-def }
top-def     ::= import-def | open-def | alias-def | module-def | let-def
module-path ::= UpIdent { "." UpIdent }             (may end inside an
                                                     inline module:
                                                     Lib.File.A)
import-def  ::= "import" module-path [ ";;" ]
open-def    ::= "open" module-path [ ";;" ]
alias-def   ::= "module" UpIdent "=" module-path [ ";;" ]
module-def  ::= "module" UpIdent "=" "struct" { struct-def } "end" [ ";;" ]
struct-def  ::= open-def | module-def | let-def
let-def     ::= "let" (Ident | "_") { param } [ ":" type ] "="
                (expr | external-body) ";;"
external-body ::= "external" String                 (C++ file; §5)
param       ::= [ "~" ] Ident ":" param-type      (~ marks a labeled param)

type        ::= postfix-type [ "->" type ]          (right-associative)
param-type  ::= postfix-type                        (arrows need parens)
postfix-type::= atom-type { "Signal" | "Sample" | "list" }
atom-type   ::= "Scalar" | "Int" | "Vector" | "Timestamp" | "String"
              | "Bool" | "unit"
              | TypeVar                             ('a - see §3)
              | "(" type { "," type } ")"           (tuple if >1 element)

expr        ::= let-in | lambda | if-expr | pipe
let-in      ::= "let" Ident { param } ":" type "=" expr "in" expr
lambda      ::= "fun" param { param } "->" expr
if-expr     ::= "if" expr "then" expr "else" expr   (both branches
                                                     required; extends
                                                     maximally right)
pipe        ::= or-expr { "|>" additive }           (lowest, left-assoc)
or-expr     ::= and-expr { "||" and-expr }
and-expr    ::= comparison { "&&" comparison }
comparison  ::= additive { cmp-op additive }
cmp-op      ::= "<" | "<=" | ">" | ">=" | "==" | "!="
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
- Unary minus negates its operand and preserves its type: defined for
  `Int`, `Scalar`, `Vector`, and `t Signal`.
- List elements are separated by `;` (OCaml style); the empty list `[]`
  is a parse but not a type — see §3.
- Function-typed parameters require parentheses:
  `f:(Timestamp -> Scalar Signal)`.
- **Local bindings**: `let name : Type = e in body` is an expression;
  the binding is annotated like every other binding, scopes over `body`
  only, and may shadow parameters, module definitions, or outer locals.
  Chained sub-lets nest naturally:

  ```
  let song : Scalar Signal =
    let hit : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:100ms in
    let beats : Timestamp list = time_steps ~start:0s ~step:200ms ~count:5 in
    place_multi hit beats
  ;;
  ```

  With parameters a local binding defines a *local function*; the
  annotation after the parameters is the return type, exactly as at the
  top level:

  ```
  let chord : Scalar Signal =
    let tone freq:Scalar ~gain:Scalar : Scalar Signal = sine freq * gain in
    mix_all [tone 220.0 ~gain:0.5; tone 275.0 ~gain:0.3; tone 330.0 ~gain:0.3]
  ;;
  ```

  A local function is sugar for binding a lambda —
  `let f x:T : R = e in body` is exactly
  `let f : T -> R = fun x:T -> e in body` — so it captures earlier
  locals and enclosing parameters by value, its labeled parameters
  (`~gain`) fill by name at call sites, and it partially applies like
  any other function.
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

Types: `Scalar`, `Int`, `Vector`, `Timestamp`, `String`, `Bool`, `unit`,
`t Signal`, `t Sample`, `t list`, tuples `(t1, ..., tn)`, and function
types `t1 -> ... -> tn -> r` (in signatures only). `Signal`/`Sample` element
types are in practice `Scalar` (mono) or `Vector` (N-channel).

`Int` is the build-time integer: a 64-bit signed whole number for
counts, indices, and other discrete quantities. The primitives that
take counts (`List.init`, `List.repeat`, `time_steps`) take Ints, so
"whole and non-negative" is enforced by the type system rather than by
build-time validation (a *negative* count is still a build error —
the type cannot see the sign of a computed value). Ints never convert
implicitly: `Math.to_scalar` goes to the continuous side exactly, and
`Math.round`/`Math.floor`/`Math.ceil` come back with an explicit
fraction policy (§6).

Rules:

- **Fully annotated, no inference.** Every parameter and every return
  type is written; the checker verifies and never guesses.
- **Polymorphism is written, never inferred.** A signature may name type
  variables (`'a`, `'elem`); they are instantiated (by unification) at
  each use site, exactly as a primitive's are. All occurrences of one
  name within a single top-level definition — parameters, return type,
  lambda parameters, `let ... in` annotations — are the same variable,
  and the name is scoped to that definition, so the `'a` of the next
  definition is unrelated. See "Polymorphic definitions" below.
  The math primitives (`exp`, `sqrt`, `log`, `pow`) use an unconstrained
  `'a`, so the checker admits any argument type; passing anything but a
  Scalar, Vector, or Scalar Signal is reported as a build (evaluation)
  error. `time` is the one nullary primitive: it is a `Scalar Signal`
  value, not a function.
- **Definition before use, no recursion.** A definition may reference
  only parameters, *earlier* definitions of its module, imported modules'
  definitions (qualified), names brought in by an *earlier* `open`, and
  Core primitives (see below). Name resolution order for unqualified
  names: local binding/parameter (innermost first) → the latest earlier
  binder of the innermost enclosing scope outward (each `struct` body is
  a scope over the file's top level; binders are own/sibling definitions
  and `open`ed names, position-ordered, so an `open` shadows earlier
  same-named binders and a later definition shadows the open) → a Core
  primitive, but only under an earlier, still-enclosing `open Core` (or
  `open Core.List` for the list functions). Re-defining one of a
  scope's *own* names is still a duplicate-definition error.
- **The Core library.** All primitives live in `Core` — a *real
  library* bundled with the compiler (`stdlib/core/lib.synth`), whose
  every definition is an `external` binding to an implementation
  compiled into synthc (§5), organized into functional submodules:
  `Osc` (oscillators & modulation), `Fx` (effects, filters,
  envelopes), `Arrange` (sample/place/mix), `Render` (the render
  effects), `Io` (audio import), `List`, `Time` (conversions &
  Timestamp sequences), `Sig` (signal constructors), and `Math`
  (see §6 for the roster). Core is **not ambient**: like any library
  it must be brought into scope — `import Core` for qualified access
  (`Core.Osc.sine`), `open Core` for module-qualified access
  (`Osc.sine`, `List.map`), or `open Core.Osc` for bare names
  (`sine`). It is always discoverable and always an allowed dependency
  (no manifest entry needed), and it aliases like any module
  (`module C = Core` then `C.Osc.sine`; `module L = Core.List` then
  `L.map`). The name `Core` is reserved — a user file or library named
  Core is shadowed by the bundled one. Code fragments elsewhere in
  this document assume the relevant submodules are open.
- **Module references** (`import`/`open`/alias targets and qualified
  names) resolve their first segment as: an inline module or module
  alias bound earlier in an enclosing scope (innermost first, later
  binders override) → a *sibling* file module — any
  `.synth` file in the same directory, which inside a library is
  exactly its member set (a sibling wins over a library of the same
  name) → a discovered library (which must be `dep`-declared). A
  library member therefore reaches every other member by short name,
  with nothing listed anywhere; the library's own `lib.synth` is not a
  sibling and naming it (or a `Lib.Member` path into one's own
  library) is an error. From outside, only what `lib.synth` binds is
  reachable: `Lib.X` resolves through the `module X = …` bindings
  there, under the name the interface gives it. Any further segments
  descend into inline modules of the module reached so far
  (`Lib.X.A.def`, see §4).
- **Local bindings** type-check like top-level ones: the bound expression
  must match the annotation (a var-carrying partial application of a
  polymorphic primitive resolves against it), and the name is visible
  only in the body of the `in`. A parameterized local binding checks as
  the lambda it desugars to: its parameters scope over the bound
  expression only, and type variables in its annotation are the
  enclosing top-level definition's (see above), rigid like everywhere
  else in the body.
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

### Polymorphic definitions

A definition is polymorphic when its annotation names type variables. The
effect is instantiation, not inference: the definition is checked once,
and every use picks its own types.

```
let dampen ~input:'a Signal : 'a Signal =
  lowpass ~cutoff:600.0 (soft_clip ~threshold:0.8 input)
;;

let mono : Scalar Signal = dampen (saw 220.0) ;;
let wide : Vector Signal = dampen (channels [saw 220.0; saw 221.0]) ;;
```

Two rules make one check stand for every instantiation:

- **Inside the body a variable is rigid.** It denotes one fixed but
  unknown type, chosen by the caller, so the body may only pass such a
  value along — never assume it is a Scalar, a Signal, or another
  variable. `let f ~x:'a : Scalar = x` and
  `let f ~x:'a : 'a = lowpass ~cutoff:1.0 x` are both rejected: each
  would decide something that is the caller's to decide. Operators are
  included: `x * 2.0` on an `'a` is not defined (on an `'a Signal` it is,
  by the Signal/Scalar row of the table below).
- **Every variable in the result must occur in a parameter**, so the call
  site can determine it. `let bad ~x:Scalar : 'a Signal = sine x` is
  rejected up front — nothing at a call site could ever fix `'a`. The
  parameters of a function-typed annotation count:
  `let damp : 'a Signal -> 'a Signal = lowpass ~cutoff:600.0` is fine.

Otherwise polymorphic definitions behave like any other. They can be
higher-order (`let twice ~f:('a -> 'a) ~x:'a : 'a = f (f x)`), partially
applied, passed to polymorphic primitives
(`List.map dampen [sine 330.0; square 220.0]`), and published by a
library's `lib.synth` like any other value.

Nothing about a definition's *evaluation* depends on its types: a
polymorphic definition renders bit-identically to the monomorphic one it
generalizes.

### Operators (pointwise lifting + Scalar broadcasting)

For `+ - * /` with operand types L and R:

| L | R | Result |
|---|---|--------|
| Scalar | Scalar | Scalar |
| Int | Int | Int (`/` divides towards zero; division by zero is a build error) |
| Vector | Vector | Vector (element-wise; channel counts must match at build time) |
| Vector | Scalar (either order) | Vector |
| t Signal | t Signal | t Signal (pointwise) |
| t Signal | Scalar (either order) | t Signal (broadcast) |

Anything else (e.g. `Timestamp + Signal`, or `Int + Scalar` — Ints do
not broadcast; convert explicitly with `to_scalar`) is a type error.
Channel-count
mismatches between Vector Signals are detected when the signal graph is
built — before any audio is computed — and signals are capped at 16
channels in v1.

### Booleans & conditionals

Everything here happens at *build time* — a `Bool` is one value decided
while the graph is assembled, never a per-sample stream.

- **`Bool`** is an atomic type with literals `true`/`false`. It works
  everywhere a type does: parameters (`~crisp:Bool`), lists, tuples,
  `'a` instantiation.
- **Comparisons** `<` `<=` `>` `>=` `==` `!=` take two Ints, two
  Scalars, or two Timestamps and produce a `Bool`. Signals are *not* comparable: a lazy
  signal has no single value, and a sample-wise select would be a
  different, signal-producing operation (deliberately absent in v1 —
  see §7). Comparing under `signal ~f`'s symbolic substitution is
  likewise a build-time error. Chained comparisons (`a < b < c`) parse
  left-associatively and are rejected by typing (Bool has no ordering).
- **`&&` / `||`** combine Bools and short-circuit: only the deciding
  operand is evaluated. `not` is a Core primitive (`b:Bool -> Bool`).
  Precedence, loosest to tightest: `|>`, `||`, `&&`, comparisons,
  `+ -`, `* /`, application.
- **`if c then a else b`** is an expression: `c` must be `Bool` (a rigid
  `'a` does not qualify — the caller never promised a Bool), both
  branches are required and must have the same type, which is the
  result type; a var-carrying branch (a partial application of a
  polymorphic callee) unifies against the other. Only the taken branch
  is evaluated, so the untaken branch's errors and render effects never
  fire — `if loud then render "a" … else render "b" …` renders exactly
  one target. Branches extend maximally right; parenthesize an `if`
  used as an argument or operand.

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
  may be a file module by any spelling in scope, an earlier alias, a
  whole library (`module B = Basic` then `B.Keys.def`), or an inline
  module (`module L = Core.List` then `L.map`). A whole-module binding
  at a library interface's top level also *publishes* `K` (see below);
  an inline-module alias stays scope-local — libraries publish whole
  modules, and inline members travel with them under their dotted
  paths.
- **Inline modules.** `module A = struct … end ;;` defines a module
  inside a file. The body holds `let` definitions, `open`s, and nested
  `module … = struct` definitions (imports and aliases stay at the top
  level); everything is position-ordered exactly as at the file's top
  level, and the body sees the enclosing scope's earlier binders.

  ```
  module Voices = struct
    open Core                              (* scoped: ends at `end` *)
    let base : Scalar = 220.0 ;;
    module Fx = struct
      let damp ~input:'a Signal : 'a Signal =
        lowpass ~cutoff:600.0 input ;;
    end
    let lead : Scalar Signal = Fx.damp (sine base) ;;
  end ;;

  let mono : Scalar Signal = Voices.Fx.damp (Core.sine Voices.base) ;;
  ```

  Members are referenced by dotted path (`Voices.base`,
  `Voices.Fx.damp`; from another file, `File.Voices.base`), or brought
  into scope with `open Voices`, which injects the module's immediate
  values and sub-module names. An `open` (of anything) inside a
  `struct` ends at that module's `end`. Inline modules of an imported
  or opened module come along automatically; a definition inside an
  inline module shadows same-named outer binders for the rest of its
  body, and an inline module shadows a same-named file module or
  library from its point of definition. Semantically an inline module
  is pure namespacing: a member is a top-level definition whose full
  name is its dotted path — same typing (including polymorphic
  signatures), same evaluation, same incremental hashing.
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

### External functions

`let name params : Type = external "file.cpp" ;;` binds a definition to
a C++ implementation instead of a synth body. The annotation is the
complete type (params and result are always written; result variables
must still be bound by parameters). Externals are ordinary values: they
curry, take labels, and are published by libraries like anything else.

Two kinds exist, split by where the declaration lives:

- **Core externals.** Every definition in the bundled
  `stdlib/core/lib.synth` is external; the file string names the
  `src/core/*.cpp` translation unit compiled into synthc that implements
  it. This is what the primitives *are* now: their signatures live in
  synth source, their bodies in the engine, and `open Core` is a plain
  library open.
- **User externals.** Anywhere else, the string names a C++ file
  resolved relative to the declaring `.synth` file. At build time synthc
  compiles it once into a shared object cached under
  `_build/externals/` (keyed by file content — edits recompile, rebuilds
  reuse), loads it, and binds the exported entry point. The C++ file is
  a build input: the watch daemon rebuilds when it changes. The
  compiler is `$CXX` (default `c++`).

A user implementation includes the generated `<synth/external.hpp>` and
defines one entry point per external, named after the definition:

```cpp
#include <synth/external.hpp>

SYNTH_EXTERNAL(succ) {
  *result = synth::ext::Value::scalar(args[0].asScalar() + 1.0);
  return true;
}
```

Arguments arrive fully applied, in declaration order; return `true` with
`*result` set, or report failure (fill `*error` and return `false`, or
throw) — failures become build diagnostics on the declaring definition.
One `.cpp` may implement several externals.

**Only data crosses the boundary**: Scalar, Int, Timestamp, Bool,
String, Vector, unit, and lists/tuples of those. Signals, Samples, functions and
type variables cannot appear in a user external's signature (checked at
type time) — signals are lazy engine graphs, not values, and stay on the
host side. Core externals are exempt (their implementations *are* the
engine). External names must form C++ symbols (letters, digits, `_`).

## 6. Primitive signatures (v1 roster)

Everything below lives in the bundled `Core` library, organized into
functional submodules and listed here under its full path. Bring what
you need into scope explicitly: `import Core` (then `Core.Osc.sine`),
`open Core` (then `Osc.sine`), or `open Core.Osc` (then `sine`).

```
(* generators *)
val Osc.sine: freq:Scalar -> Scalar Signal
val Osc.saw: freq:Scalar -> Scalar Signal
val Osc.square: freq:Scalar -> Scalar Signal
val Osc.noise: freq:Scalar -> Scalar Signal          (* two-step FM; deterministic *)

(* envelopes *)
val Fx.exp_decay: rate:Scalar -> Scalar Signal          (* e^(-rate*t) *)
val Fx.adsr: attack:Timestamp -> decay:Timestamp -> sustain:Scalar
             -> release:Timestamp -> hold:Timestamp -> Scalar Signal

(* filters *)
val Fx.lowpass: cutoff:Scalar -> input:'a Signal -> 'a Signal
val Fx.highpass: cutoff:Scalar -> input:'a Signal -> 'a Signal

(* distortion *)
val Fx.hard_clip: threshold:Scalar -> input:'a Signal -> 'a Signal  (* clamp at +/-threshold *)
val Fx.soft_clip: threshold:Scalar -> input:'a Signal -> 'a Signal  (* threshold*tanh(x/threshold) *)

(* modulation *)
val Osc.fm: carrier:Scalar -> modulator:Scalar Signal -> Scalar Signal
val Osc.pm: carrier:Scalar -> modulator:Scalar Signal -> Scalar Signal
val Osc.am: carrier:'a Signal -> modulator:Scalar Signal -> depth:Scalar -> 'a Signal

(* time effects *)
val Fx.delay: by:Timestamp -> signal:'a Signal -> 'a Signal
val Fx.resample: input:'a Signal -> f:(Scalar -> Scalar) -> 'a Signal  (* f is a playback-rate multiplier *)
val Fx.reverb: decay:Timestamp -> damping:Scalar -> mix:Scalar
             -> input:'a Signal -> 'a Signal

(* combination *)
val Arrange.mix_all: signals:'a Signal list -> 'a Signal
val Arrange.channels: chans:Scalar Signal list -> Vector Signal

(* slicing & arrangement *)
val Arrange.sample: signal:'a Signal -> from:Timestamp -> to:Timestamp -> 'a Sample
val Arrange.place: sample:'a Sample -> at:Timestamp -> 'a Signal
val Arrange.place_multi: sample:'a Sample -> ats:Timestamp list -> 'a Signal  (* mix of placements; overlaps sum *)

(* the effects *)
val Render.render: name:String -> rate:Scalar -> sample:'a Sample -> unit
val Render.render_vis: name:String -> rate:Scalar -> sample:'a Sample -> unit  (* waveform SVG *)
val Render.render_stems: name:String -> rate:Scalar
                -> stems:(String, 'a Sample) list -> unit
  (* one audio target per stem, named "<name>-<label>" *)
val Render.render_vis_stems: name:String -> rate:Scalar
                    -> stems:(String, 'a Sample) list -> unit
  (* ONE svg artifact: a labeled waveform lane per stem, shared time axis *)

(* file import *)
val Io.load_mono: path:String -> Scalar Signal
val Io.load_multi: path:String -> Vector Signal

(* signal constructors *)
val Sig.constant: value:Scalar -> Scalar Signal
val Sig.constant_multi: levels:Scalar list -> Vector Signal  (* one level per channel *)
val Sig.time: Scalar Signal          (* nullary: the ramp t, in seconds *)
val Sig.signal: f:(Scalar -> Scalar) -> Scalar Signal
  (* samples f over time; the body is limited to arithmetic and the math
     primitives - nothing else maps a Scalar to a Scalar *)
val Sig.signal_multi: fs:(Scalar -> Scalar) list -> Vector Signal

(* math: polymorphic over Scalars and (elementwise) Signals; anything
   else is a build-time error. Domain follows IEEE: log of a
   non-positive value or sqrt of a negative one yield -inf/NaN samples. *)
val Math.exp: x:'a -> 'a
val Math.sqrt: x:'a -> 'a
val Math.log: x:'a -> 'a             (* natural *)
val Math.pow: x:'a -> y:Scalar -> 'a

(* Int <-> Scalar conversions: to_scalar is exact; the way back names
   its fraction policy. *)
val Math.to_scalar: n:Int -> Scalar
val Math.round: x:Scalar -> Int
val Math.floor: x:Scalar -> Int
val Math.ceil: x:Scalar -> Int

(* Core.List: list combinators & builders *)
val List.map    : f:('a -> 'b) -> xs:'a list -> 'b list
val List.fold   : f:('a -> 'b -> 'a) -> init:'a -> xs:'b list -> 'a
val List.init   : n:Int -> f:(Int -> 'a) -> 'a list   (* [f 0; ...; f (n-1)] *)
val List.repeat : n:Int -> x:'a -> 'a list

(* Core.Time: timestamp construction & sequences *)
val Time.to_sec: x:Scalar -> Timestamp
val Time.to_ms: x:Scalar -> Timestamp
val Time.to_min: x:Scalar -> Timestamp
val Math.not: b:Bool -> Bool
val Time.time_steps: start:Timestamp -> step:Timestamp -> count:Int -> Timestamp list
val Time.jitter: seed:Scalar -> spread:Timestamp -> steps:(Timestamp list) -> Timestamp list
```

Counts and indices are Ints, so wholeness is guaranteed by the type
system; a negative computed count is still a build (evaluation) error.

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
(* with open Core.Arrange and open Core.Time *)
place_multi hat (time_steps ~start:0s ~step:250ms ~count:32
                   |> jitter ~seed:7.0 ~spread:8ms)
```

Semantics details for the doc's open points (expanded in
[`core-library.md`](core-library.md)):
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

Signal-level branching (comparisons and `if` are build-time only; a
sample-wise select/gate over signals would be a new signal-producing
primitive), pattern matching, user-defined types, recursion and feedback
(IIR-style signal cycles), type *inference* (every binding is still
annotated; polymorphism is written out, §3), per-definition visibility
control (a library's `lib.synth` publishes whole modules or re-exported
values, but a published module exposes all of its definitions), reverse
playback (`resample` reads its source only forward, so a negative rate
clamps to zero rather than rewinding), cache tuning knobs, native
extensions. See design doc §13. (Lambdas, general partial application,
cross-directory imports/packaging — via libraries, `open` and module
aliases — user-written polymorphism, inline modules, and build-time
Booleans with `if`/`else` were listed here originally and are now in the
language; see §2, §3 and §4.)
