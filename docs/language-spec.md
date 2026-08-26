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
  `then`, `else`, `match`, `with`, `true`, `false`. The words `struct`,
  `end`, `external`, `type`, `of` and `rec` are *contextual*: they
  matter only in their own position — inside a
  `module N = struct … end` definition, as a definition's whole
  `external "file"` body, at the start of a `type` declaration, before
  a constructor's payload, and directly between `let` and a binding
  name — and lex as ordinary identifiers everywhere else (a binding
  named `end`, `external`, `type` or `rec` keeps working).
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
- **Punctuation**: `;;` `;` `:` `=` `(` `)` `[` `]` `{` `}` `,` `.`
  `->` `~` `|>` `|` `+` `-` `*` `/` `<` `<=` `>` `>=` `==` `!=` `&&`
  `||`, and the `.`-suffixed operators `+.` `-.` `*.` `/.` `<.` `<=.`
  `>.` `>=.` `==.` `!=.` (§3). A lone `|` is the variant/match bar;
  `|>` and `||` win where they fit. Operators are longest-match, so
  `>=.` beats `>=` and `>.` beats `>`; a `.` binds to the operator on
  its left rather than starting a projection, which is unambiguous
  because a `.` never followed an operator before.

## 2. Grammar

EBNF; `{x}` is repetition, `[x]` is optionality.

```
module      ::= { top-def }
top-def     ::= import-def | open-def | alias-def | module-def
              | let-def | type-def
module-path ::= UpIdent { "." UpIdent }             (may end inside an
                                                     inline module:
                                                     Lib.File.A)
import-def  ::= "import" module-path [ ";;" ]
open-def    ::= "open" module-path [ ";;" ]
alias-def   ::= "module" UpIdent "=" module-path [ ";;" ]
module-def  ::= "module" UpIdent "=" "struct" { struct-def } "end" [ ";;" ]
struct-def  ::= open-def | module-def | let-def | type-def
let-def     ::= "let" [ "rec" ] (Ident | "_") { param } [ ":" type ] "="
                (expr | external-body) ";;"
external-body ::= "external" String                 (C++ file; §5)
param       ::= [ "~" ] Ident ":" param-type      (~ marks a labeled param)

type-def    ::= "type" [ type-params ] UpIdent
                [ "=" ( record-body | ctor-list ) ] ";;"
                                                    (no "=": abstract)
type-params ::= TypeVar | "(" TypeVar { "," TypeVar } ")"
record-body ::= "{" field { ";" field } "}"
field       ::= Ident ":" type
ctor-list   ::= [ "|" ] ctor { "|" ctor }
ctor        ::= UpIdent [ "of" type ]               (at most one payload;
                                                     tuples carry more)

type        ::= postfix-type [ "->" type ]          (right-associative)
param-type  ::= postfix-type                        (arrows need parens)
postfix-type::= atom-type { type-name }             (postfix application:
                                                     Scalar Signal,
                                                     'a list, Scalar Voice)
type-name   ::= [ module-path "." ] UpIdent | "list"
atom-type   ::= "Scalar" | "Int" | "Vector" | "Timestamp" | "String"
              | "Bool" | "unit"
              | type-name                           (a 0-parameter one)
              | TypeVar                             ('a - see §3)
              | "(" type { "," type } ")"           (tuple if >1 element;
                                                     spread across a
                                                     multi-parameter
                                                     type-name: ('a, 'b) Pair)

expr        ::= let-in | lambda | if-expr | match-expr | pipe
let-in      ::= "let" [ "rec" ] Ident { param } [ ":" type ] "=" expr
                "in" expr                           (type required for
                                                     "rec")
              | "let" (tuple-pattern | record-pattern) ":" type "="
                expr "in" expr                      (destructuring; must
                                                     be irrefutable)
lambda      ::= "fun" param { param } "->" expr
if-expr     ::= "if" expr "then" expr "else" expr   (both branches
                                                     required; extends
                                                     maximally right)
match-expr  ::= "match" expr "with" [ "|" ] arm { "|" arm }
arm         ::= pattern "->" expr                   (bodies extend
                                                     maximally right; a
                                                     nested match needs
                                                     parens)
pattern     ::= ctor-pattern | pattern-atom
ctor-pattern::= [ module-path "." ] UpIdent [ pattern-atom ]
pattern-atom::= "_" | Ident
              | [ module-path "." ] UpIdent         (payload-less ctor)
              | "(" pattern { "," pattern } ")"     (tuple if >1 element)
              | record-pattern
tuple-pattern ::= "(" pattern { "," pattern } ")"
record-pattern ::= "{" field-pat { ";" field-pat } "}"
field-pat   ::= Ident [ "=" pattern ]               (bare = punned bind)
pipe        ::= or-expr { "|>" additive }           (lowest, left-assoc)
or-expr     ::= and-expr { "||" and-expr }
and-expr    ::= comparison { "&&" comparison }
comparison  ::= additive { cmp-op additive }
cmp-op      ::= "<" | "<=" | ">" | ">=" | "==" | "!="
              | "<." | "<=." | ">." | ">=." | "==." | "!=."
additive    ::= multiplicative
                { ("+" | "-" | "+." | "-.") multiplicative }
multiplicative ::= unary { ("*" | "/" | "*." | "/.") unary }
unary       ::= ("-" | "-.") unary | app
app         ::= atom { arg }                        (application, left)
arg         ::= atom | "~" Ident ":" atom           (labeled argument)
atom        ::= atom-base { "." Ident }             (record projection;
                                                     binds tighter than
                                                     application)
atom-base   ::= Number | Time | String
              | Ident                               (unqualified name)
              | module-path "." Ident               (qualified name)
              | [ module-path "." ] UpIdent         (constructor - an
                                                     uppercase leaf)
              | "(" expr { "," expr } ")"           (tuple if >1 element)
              | "[" [ expr { ";" expr } ] "]"       (list literal)
              | "{" Ident "=" expr { ";" Ident "=" expr } "}"
                                                    (record literal)
              | "{" expr "with" Ident "=" expr
                    { ";" Ident "=" expr } "}"      (record update)
```

Notes:

- A `let` with parameters defines a function; without, a constant. The
  return-type annotation may be omitted — the checker infers it from
  the body (see §3, local inference). `let rec` and `external` bodies
  still require it, as do destructuring bindings.
- Application binds tighter than operators: `sine 440.0 *. 0.5` is
  `(sine 440.0) *. 0.5`. `*`/`/` bind tighter than `+`/`-`, and each
  `.`-suffixed operator sits at its bare counterpart's level; all are
  left-associative.
- Unary minus negates its operand and preserves its type: defined for
  `Int`, `Scalar`, `Vector`, and `t Signal`. It is the one operator the
  discrete/continuous split (§3) leaves alone — with a single operand
  there is nothing to disambiguate — so `-` covers every numeric kind
  and `-.` is accepted as the same operator, for lines written in the
  dotted style.
- List elements are separated by `;` (OCaml style); `[]` is legal
  wherever the element type is determined — see §3.
- Record projection `r.field` binds tighter than application
  (`f r.x` is `f (r.x)`) and chains (`s.inner.gain`). A dotted path
  with an *uppercase* leaf is a constructor reference; a lowercase leaf
  is a qualified value, as before.
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

Built-in types: `Scalar`, `Int`, `Vector`, `Timestamp`, `String`,
`Bool`, `unit`, tuples `(t1, ..., tn)`, and function types
`t1 -> ... -> tn -> r` (in signatures only). Everything else is a
*declared* type: user `type` declarations (records, variants, abstract
types — see below), and the three Core declares that are ambient like
the built-in names, needing no import to write in an annotation:

- `'a list` — an ordinary recursive variant,
  `type 'a list = | Nil | Cons of ('a, 'a list)`. `[a; b; c]` literals
  are sugar for `Cons` chains and `match` takes lists apart.
- `'a Signal` — abstract: an engine-backed handle with no visible
  structure. Element types are in practice `Scalar` (mono) or `Vector`
  (N-channel).
- `'a Sample` — a record,
  `type 'a Sample = { sig : 'a Signal; from : Timestamp; to : Timestamp }`,
  so `s.from` projects and `{ s with to = 1s }` re-windows a sample.

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

- **Annotated parameters, local return-type inference.** Every
  parameter is written; return types (top-level and `let ... in`) may
  be omitted, in which case the binding takes its body's *synthesized*
  type — the checker still never guesses across definitions, and no
  constraint flows backwards. A body that leaves a type undetermined
  (an un-pinned partial application, an empty list) still demands the
  annotation, with a diagnostic saying so. `let rec` keeps its
  annotation (the recursive name is in scope in its own body at that
  type), as do `external` bodies (the annotation IS the type),
  destructuring bindings, and lambda parameters.
- **Polymorphism is written, never inferred.** A signature may name type
  variables (`'a`, `'elem`); they are instantiated (by unification) at
  each use site, exactly as a primitive's are. All occurrences of one
  name within a single top-level definition — parameters, return type,
  lambda parameters, `let ... in` annotations — are the same variable,
  and the name is scoped to that definition, so the `'a` of the next
  definition is unrelated. See "Polymorphic definitions" below.
  The math primitives (`exp`, `sqrt`, `log`, `pow`, and the trig family
  `sin`/`cos`/`tan`/`atan`/`abs`) use an unconstrained `'a`, so the
  checker admits any argument type; passing anything but a Scalar,
  Vector, or Signal is reported as a build (evaluation) error. `time`
  and `Math.pi` are the nullary primitives: values, not functions.
- **Definition before use; recursion is opt-in and self-only.** A
  definition may reference
  only parameters, *earlier* definitions of its module, imported modules'
  definitions (qualified), and names brought in by an *earlier* `open`
  — Core primitives included, under an earlier, still-enclosing
  `open Core.<Module>` (see below). Name resolution order for unqualified
  names: local binding/parameter (innermost first) → the latest earlier
  binder of the innermost enclosing scope outward (each `struct` body is
  a scope over the file's top level; binders are own/sibling definitions
  and `open`ed names, position-ordered, so an `open` shadows earlier
  same-named binders and a later definition shadows the open).
  Re-defining one of a scope's *own* names is still a
  duplicate-definition error. `let rec` (top-level or local) puts the
  binding's own name in scope in its own body, at its full annotated
  signature — see "Recursion" below; *mutual* recursion remains out.
- **The Core library.** All primitives live in `Core` — a *real
  library* bundled with the compiler (`stdlib/core/lib.synth`). Nearly
  every definition is an `external` binding to a C++ implementation
  shipped beside it (§5); the `List`, `Pitch`, `Tempo`, `Scale` and `Score`
  modules are written in SynthGraph itself, and the interface opens with the `list`/`Signal`/`Sample`
  type declarations (§3, top). Core is organized into functional
  submodules:
  `Osc` (oscillators & modulation), `Fx` (effects, filters,
  envelopes), `Arrange` (sample/place/mix), `Render` (the render
  effects), `Io` (audio import), `List`, `Time` (conversions &
  Timestamp sequences), `Sig` (signal constructors), `Groove` (the
  sequencing tier), `Pitch` (notes, temperaments &
  cents), `Tempo` (meters, note values & the beat grid),
  `Scale` (keys, degrees, chords & progressions),
  `Score` (phrases, events, dynamics & `play`), `Mix` (the stereo and
  bus vocabulary), `Str` (computed names), `Math`, and the `Dsp`
  prelude (see §6 for the roster). Core is **not ambient**: like any library
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
- **List literals** unify their elements to one type. `[]` (and a
  polymorphic `Nil`, which is the same value) leaves the element as an
  unknown the annotation or the surrounding call resolves — exactly the
  partial-application rule; if nothing determines it, that is the usual
  leftover-variable error.
- **Duplicate definitions** in a module and **duplicate parameters** in a
  signature are errors.

### Type declarations: records, variants, abstract types

`type` declares a nominal type at the top level of a file or a
`struct` body:

```
type Env = { attack : Timestamp; release : Timestamp } ;;
type 'a Voice = { osc : 'a Signal; vel : Scalar } ;;
type ('a, 'b) Pair = { first : 'a; second : 'b } ;;
type Wave = | Sine | Saw | Pulse of Scalar ;;
type Handle ;;                                   (* abstract *)
```

- **Names and parameters.** Declared type names are uppercase-initial
  (`list` is the grandfathered lowercase spelling, declared by Core);
  parameters are written prefix (`'a Voice`, `('a, 'b) Pair`) and
  applied postfix like the built-in constructors (`Scalar Voice`,
  `(Scalar, String) Pair`). For a one-parameter type a parenthesized
  tuple is the argument (`(String, 'a Sample) list` is a list of
  pairs); for a multi-parameter one it spreads across the parameters.
  Members may use only the declared parameters; a declaration may
  reference itself (that is what makes `list` possible), and a
  declaration must precede its uses, like any definition. Declarations
  are *nominal*: two structurally identical records are different
  types, and a declaration travels under its module like a value
  (`Voices.Voice`, `open Voices`, published by a library's
  `lib.synth`).
- **Records.** A literal `{ attack = 5ms; release = 100ms }` names no
  type; it resolves to the innermost visible record declaration with
  exactly that field set (two matches in one scope are an ambiguity
  error — annotate, or reach the value another way). Projection
  `e.field` and functional update `{ e with field = v; ... }` check
  against the value's declaration; update keeps the value's type.
- **Variants.** Constructors are uppercase, carry at most one payload
  (`of T`; bundle more in a tuple), and resolve like values — bare
  when their declaration is in scope, or module-qualified with an
  uppercase leaf (`Shapes.Pulse 0.5`). They are **not first-class**: a
  payload-carrying constructor must be applied where it is used
  (`List.map ~f:(fun d:Scalar -> Pulse d)` rather than
  `List.map ~f:Pulse`). A nullary constructor of a polymorphic variant
  (`Nil`, `None`-alikes) leaves its parameters to the context, like
  `[]`.
- **Abstract types** (`type Handle ;;`) have no visible structure: no
  literals, no patterns ("type 'Signal' is abstract - it has no
  constructors to match"). They are the declaration form for
  engine-backed handles; Core's `Signal` is the canonical one.
- Editing a declaration invalidates every definition typed by it: type
  declarations participate in the incremental dependency graph like
  value definitions.

### `match` and patterns

```
let osc w:Wave ~freq:Scalar : Scalar Signal =
  match w with
  | Sine -> sine ~freq:freq
  | Saw -> saw ~freq:freq
  | Pulse duty -> square ~freq:freq *. duty
;;
```

- **Typing is destructuring, not inference.** The scrutinee's type is
  already known; patterns only take it apart, so pattern binders carry
  no annotations. Patterns are: `_`, a lowercase binder, a constructor
  (with its payload pattern), a tuple, or a record pattern
  (`{ attack; release = r }` — a subset of the fields, a bare name
  binding punned under itself). No literal patterns in v1; use `if`
  for value tests. Matching is only defined at types with structure to
  match — a rigid `'a` or an abstract type is an error.
- **Exhaustiveness and redundancy are hard errors.** A non-exhaustive
  match names a concrete missing example ("for example, Full (Pulse _)
  is not covered"); an arm the earlier arms already cover is
  "unreachable".
- **Arms agree like `if` branches** (unifying when one still carries
  variables), and **only the taken arm evaluates** — the untaken arms'
  errors and render effects never fire.
- Arm bodies extend maximally right; parenthesize a `match` nested in
  an arm. The scrutinee is evaluated exactly once.
- **Destructuring let**: `let (lo, hi) : (Scalar, Scalar) = e in body`
  and `let { attack; release = r } : Env = e in body` bind through a
  pattern; the pattern must be irrefutable (cover every value of the
  annotated type), so constructor patterns of multi-constructor
  variants belong in a `match` instead.

### Recursion (`let rec`)

`let rec` makes the binding's own name visible in its body, at its
full annotated signature:

```
let rec fact n:Int : Int = if n <= 1 then 1 else n * fact (n - 1) ;;

let rec sum xs:Scalar list : Scalar =
  match xs with
  | Nil -> 0.0
  | Cons (x, rest) -> x +. sum rest
;;
```

- Works at the top level and for local functions
  (`let rec go i:Int : 'a list = ... in go 0`); requires at least one
  parameter (a recursive constant could never terminate) and a full
  annotation — the recursive name is in scope in its own body at that
  declared type.
- Inside the body the name binds like a parameter: its `'a` stays
  rigid, so a definition cannot call itself at a different type (no
  polymorphic recursion).
- *Mutual* recursion (and recursive *signal* feedback) remains out of
  scope — see §7.
- **Tail calls are eliminated.** A call in tail position — through
  `let ... in` bodies, `if` branches and `match` arms — reuses the
  current frame, so accumulator-shaped recursion (`List.fold`, and
  every Core combinator, which are written in accumulator form) runs at
  constant depth however long the list. Only genuinely *nested* (non-
  tail) recursion counts against the runaway guard: beyond 4096 nested
  calls the build fails with a recursion-limit diagnostic, and a tail
  chain that never terminates trips its own (much larger) iteration
  brake instead of hanging the build.

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
  included: `x *. 2.0` on an `'a` is not defined (on an `'a Signal` it is,
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

Every operator comes in two spellings, and each accepts one half of the
numeric kinds:

- **bare** — `+ - * /` and `< <= > >= == !=` — takes two **Int**s;
- **`.`-suffixed** — `+. -. *. /.` and `<. <=. >. >=. ==. !=.` — takes
  the **continuous** kinds: Scalar, Timestamp, Vector and Signal.

Nothing is overloaded across that divide, so an operator's result kind
never depends on inference, and truncating `7 / 2` (`3`) can never be
mistaken for `7.0 /. 2.0` (`3.5`). The two spellings share a precedence
level, so switching a line between them never reparenthesizes it.

For `+. -. *. /.` with operand types L and R:

| L | R | Result |
|---|---|--------|
| Scalar | Scalar | Scalar |
| Vector | Vector | Vector (element-wise; channel counts must match at build time) |
| Vector | Scalar (either order) | Vector |
| t Signal | t Signal | t Signal (pointwise) |
| t Signal | Scalar (either order) | t Signal (broadcast) |
| t Signal | Scalar Signal (either order) | t Signal (mono broadcast: the Scalar Signal lifts across the other side's channels, mirroring `am`'s mono-modulator rule — `bus *. envelope` fades a stereo bus) |
| Timestamp | Timestamp | Timestamp (`+.` `-.` only) |
| Timestamp | Scalar | Timestamp (`*.` either order, `/.` with the Timestamp on the left) |

`+ - * /` take `Int | Int -> Int`, where `/` divides towards zero and
division by zero is a build error.

Anything else is a type error: `1s +. sine 440.0` (no row above),
`2 +. 3.0` or `1s *. 2` (an Int never reaches a `.` operator), and
`0.5 * 2.0` (a Scalar never reaches a bare one — the error names `*.`).
Ints do not broadcast and do not convert implicitly; cross over with
`to_scalar`.

The Timestamp rows are a *dimensional* rule, not a numeric one:
durations add and subtract, and a duration scales by a dimensionless
Scalar, so musical time can be written as musical time —
`beat *. 1.5` is a dotted note, `bar -. beat` is the upbeat before it,
`to_ms 250.0 +. 100ms` composes a window. The combinations left out are
left out on purpose. `1s *. 2s` is not a duration; `1s /. 500ms` would
hand back the bare Scalar that `to_sec`/`to_ms`/`to_min` deliberately
have no inverse for (§6); `1s +. 2.0` mixes a duration with a number —
convert the Scalar first.

Every Timestamp result **clamps at the epoch**. The timeline starts at
`0s` and a negative instant has no meaning, so `100ms -. 900ms` is `0s`
rather than an error or a negative Timestamp — the same clamping
`jitter` applies to a humanized step (§6). Unary `-` on a Timestamp
stays a type error for the same reason: there is nothing for it to
denote.
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
- **Comparisons** split the same way as the arithmetic operators:
  `<` `<=` `>` `>=` `==` `!=` take two **Int**s, and `<.` `<=.` `>.`
  `>=.` `==.` `!=.` take two **Scalar**s or two **Timestamp**s. Both
  produce a `Bool`. Signals are *not* comparable: a lazy
  signal has no single value, and a sample-wise select would be a
  different, signal-producing operation (deliberately absent in v1 —
  see §7). Comparing under `signal ~f`'s symbolic substitution is
  likewise a build-time error. Chained comparisons (`a <. b <. c`) parse
  left-associatively and are rejected by typing (Bool has no ordering).
- **`&&` / `||`** combine Bools and short-circuit: only the deciding
  operand is evaluated. `not` is a Core primitive (`b:Bool -> Bool`).
  Precedence, loosest to tightest: `|>`, `||`, `&&`, comparisons,
  `+ - +. -.`, `* / *. /.`, application — each `.` operator sits at the
  same level as its bare counterpart.
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
    open Core.Osc open Core.Fx             (* scoped: ends at `end` *)
    let base : Scalar = 220.0 ;;
    module Fx = struct
      let damp ~input:'a Signal : 'a Signal =
        lowpass ~cutoff:600.0 input ;;
    end
    let lead : Scalar Signal = Fx.damp (sine base) ;;
  end ;;

  let mono : Scalar Signal =
    Voices.Fx.damp (Core.Osc.sine Voices.base) ;;
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

There is one mechanism, and the bundled Core library uses it too. The
file string names a C++ file resolved relative to the declaring
`.synth` file. At build time synthc compiles it once with `$CXX`
(default `c++`) into a shared object cached by content — edits
recompile, rebuilds reuse; user code's objects live under the
project's `_build/externals/`, the bundled stdlib's in a shared
per-user cache — then loads it and binds the exported entry point. The
C++ file is a build input: the watch daemon rebuilds when it changes.
Every definition in `stdlib/core/lib.synth` is such an external over
the `.cpp` files shipped beside it: the primitives' signatures live in
synth source, their bodies in library C++, and `open Core` is a plain
library open.

An implementation includes the shipped `<synth/external.hpp>` and
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

**Every value crosses the boundary.** Data — Scalar, Int, Timestamp,
Bool, String, Vector, unit, and lists/tuples of those — arrives as
transparent `synth::ext::Value`s. Signals and Samples arrive as lazy
engine graph handles that combine with the `<synth/engine.hpp>`
constructors, the same ones the engine itself uses (the symbols resolve
against the host process when the object is loaded). Functions (and
type-variable-typed values generally) arrive as opaque handles, callable
through the context's `apply` service or passable back unchanged. The
context also offers `loadAudio` (audio files as build inputs) and
`render` (declaring render targets) — Core's own `Io.load_mono` and
`Render.render` are ordinary externals built on exactly these. External
names must form C++ symbols (letters, digits, `_`).

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
val Osc.saw_bl: freq:Scalar -> Scalar Signal         (* bandlimited (PolyBLEP) *)
val Osc.square_bl: freq:Scalar -> Scalar Signal      (* bandlimited (PolyBLEP) *)
val Osc.noise: freq:Scalar -> Scalar Signal          (* two-step FM; deterministic *)

(* envelopes *)
val Fx.exp_decay: rate:Scalar -> Scalar Signal          (* e^(-rate*t) *)
val Fx.adsr: attack:Timestamp -> decay:Timestamp -> sustain:Scalar
             -> release:Timestamp -> hold:Timestamp -> Scalar Signal

(* filters; the modulated forms take the cutoff (Hz) as a mono signal *)
val Fx.lowpass: cutoff:Scalar -> input:'a Signal -> 'a Signal
val Fx.highpass: cutoff:Scalar -> input:'a Signal -> 'a Signal
val Fx.lowpass_mod: cutoff:Scalar Signal -> input:'a Signal -> 'a Signal
val Fx.highpass_mod: cutoff:Scalar Signal -> input:'a Signal -> 'a Signal
val Fx.resonant: cutoff:Scalar Signal -> q:Scalar -> input:'a Signal
               -> 'a Signal          (* 2-pole SVF lowpass; q rings *)

(* signal-level control *)
val Fx.follow: attack:Timestamp -> release:Timestamp
             -> input:Scalar Signal -> Scalar Signal  (* envelope follower *)
val Sig.select: gate:Scalar Signal -> threshold:Scalar -> above:'a Signal
              -> below:'a Signal -> 'a Signal  (* sample-wise choice *)

(* distortion *)
val Fx.hard_clip: threshold:Scalar -> input:'a Signal -> 'a Signal  (* clamp at +/-threshold *)
val Fx.soft_clip: threshold:Scalar -> input:'a Signal -> 'a Signal  (* threshold*tanh(x/threshold) *)

(* modulation *)
val Osc.fm: carrier:Scalar -> modulator:Scalar Signal -> Scalar Signal
val Osc.pm: carrier:Scalar -> modulator:Scalar Signal -> Scalar Signal
val Osc.am: carrier:'a Signal -> modulator:Scalar Signal -> depth:Scalar -> 'a Signal

(* time effects *)
val Fx.delay: by:Timestamp -> signal:'a Signal -> 'a Signal
val Fx.feedback: by:Timestamp -> gain:Scalar -> input:'a Signal
               -> 'a Signal  (* out(t) = in(t) + gain*out(t-by); |gain| < 1 *)
val Fx.resample: input:'a Signal -> f:(Scalar -> Scalar) -> 'a Signal  (* f is a playback-rate multiplier *)
val Fx.reverb: decay:Timestamp -> damping:Scalar -> mix:Scalar
             -> input:'a Signal -> 'a Signal

(* voice sugar (written in SynthGraph over the primitives above) *)
val Fx.gated: attack:Timestamp -> decay:Timestamp -> sustain:Scalar
            -> release:Timestamp -> hold:Timestamp -> input:Scalar Signal
            -> Scalar Sample   (* input * adsr, cut to the envelope's end *)
val Fx.echoes: by:Timestamp -> gain:Scalar -> n:Int -> input:'a Signal
             -> 'a Signal      (* input + sum delay(by*i) * gain^i *)

(* combination *)
val Arrange.mix_all: signals:'a Signal list -> 'a Signal
val Arrange.channels: chans:Scalar Signal list -> Vector Signal
val Arrange.channel: n:Int -> input:Vector Signal -> Scalar Signal
  (* the inverse of channels; the index is validated at graph build *)

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
val Math.sin: x:'a -> 'a             (* radians; the same polymorphic rule *)
val Math.cos: x:'a -> 'a
val Math.tan: x:'a -> 'a
val Math.atan: x:'a -> 'a
val Math.abs: x:'a -> 'a
val Math.pi: Scalar                  (* a plain constant *)
val Math.min: a:Scalar -> b:Scalar -> Scalar
val Math.max: a:Scalar -> b:Scalar -> Scalar
val Math.clamp: lo:Scalar -> hi:Scalar -> x:Scalar -> Scalar
val Math.lerp: a:Scalar -> b:Scalar -> t:Scalar -> Scalar
val Math.hash: seed:Scalar -> i:Int -> Scalar
  (* pure, platform-stable, uniform in [0, 1); jitter's splitmix64
     exposed as a value - the library's one source of randomness *)

(* Int <-> Scalar conversions: to_scalar is exact; the way back names
   its fraction policy. *)
val Math.to_scalar: n:Int -> Scalar
val Math.round: x:Scalar -> Int
val Math.floor: x:Scalar -> Int
val Math.ceil: x:Scalar -> Int

(* Core.Pitch: notes, temperaments & cents - written in SynthGraph.
   The chromatic ladder is indexed from C0 = 0, so A4 is step 57; this
   is not a MIDI key number. Types travel under the module, so they are
   Pitch.Note / Pitch.Tuning / Pitch.PitchClass when qualified. *)
type PitchClass = | C | Cs | D | Ds | E | F | Fs | G | Gs | A | As | B
type Note   = { pc : PitchClass; oct : Int }
type Tuning = { ref_hz : Scalar; ref_step : Int; root : Int;
                ratios : Scalar list; octave : Scalar }

val Pitch.step    : note:Note -> Int
val Pitch.of_step : step:Int -> Note
val Pitch.shift   : note:Note -> by:Int -> Note      (* by ladder steps *)
val Pitch.flat    : note:Note -> Note
val Pitch.wrap_to : note:Note -> low:Note -> Note
  (* moved by whole octaves into [low, low + octave): the register fold *)

(* temperaments; presets are functions, not constants *)
val Pitch.et    : n:Int -> ref_hz:Scalar -> ref_step:Int -> Tuning
val Pitch.et12  : ref_hz:Scalar -> Tuning
val Pitch.just  : root:Int -> ref_hz:Scalar -> Tuning      (* 5-limit *)
val Pitch.pyth  : root:Int -> ref_hz:Scalar -> Tuning      (* 3-limit *)

(* pitch -> frequency; the tuning comes first so it partially applies *)
val Pitch.hz      : t:Tuning -> note:Note -> Scalar
val Pitch.step_hz : t:Tuning -> step:Int -> Scalar    (* for n /= 12 *)
val Pitch.a440    : note:Note -> Scalar               (* 12-TET, A4 = 440 *)

(* cents: continuous, and the same in every temperament *)
val Pitch.cents    : n:Scalar -> Scalar               (* multiplier 2^(n/1200) *)
val Pitch.detune   : freq:Scalar -> cents:Scalar -> Scalar
val Pitch.to_cents : ratio:Scalar -> Scalar
val Pitch.ratio    : num:Int -> den:Int -> Scalar     (* 3/2, 5/4 *)

(* Core.Tempo: meters, note values & the beat grid - written in
   SynthGraph. `bpm` counts the meter's `unit` note per minute. Bars and
   beats count from 0: `at` is an offset, not a ruler label. Types travel
   under the module, so they are Tempo.Meter / Tempo.Tempo / Tempo.Value
   when qualified - a module and a type may share a name. *)
type Meter = { beats : Int; unit : Int }
type Tempo = { bpm : Scalar; meter : Meter }
type Value = | Whole | Half | Quarter | Eighth | Sixteenth | ThirtySecond
             | Dotted of Value                  (* x1.5, and it nests *)
             | Tuplet of (Int, Int, Value)      (* n of v per m of v *)

(* the pulse; `t` comes first so a tempo partially applies *)
val Tempo.beat  : t:Tempo -> Timestamp              (* one `unit` note *)
val Tempo.bar   : t:Tempo -> Timestamp              (* `beats` of them *)
val Tempo.beats : t:Tempo -> n:Scalar -> Timestamp  (* fractional is fine *)
val Tempo.bars  : t:Tempo -> n:Scalar -> Timestamp  (* n bars *)

(* a whole note is `unit` beats, whatever the meter *)
val Tempo.value : t:Tempo -> v:Value -> Timestamp
val Tempo.per_bar : t:Tempo -> v:Value -> Int
  (* how many of v fit in one bar - meter-correct, via Time.div *)
val Tempo.bar_beats : t:Tempo -> n:Int -> Scalar
  (* n bars in *beats*: the bridge Score.move/chord ~len need *)

(* positions and grids *)
val Tempo.at    : t:Tempo -> bar:Int -> beat:Scalar -> Timestamp
val Tempo.grid  : t:Tempo -> from:Timestamp -> step:Value -> count:Int
                    -> Timestamp list
val Tempo.swing : amount:Scalar -> step:Timestamp -> steps:Timestamp list
                    -> Timestamp list
val Tempo.swung_grid : t:Tempo -> from:Timestamp -> step:Value -> count:Int
                         -> amount:Scalar -> Timestamp list
  (* grid |> swing in one call, so the step is named once *)
val Tempo.marks : t:Tempo -> bars:Int list -> Timestamp list
  (* section lengths (bars) -> section starts: n+1 entries, ending last *)

val Tempo.common : bpm:Scalar -> Tempo               (* 4/4; a function *)

(* Core.Scale: keys, degrees & chords - written in SynthGraph. Degrees
   count from 0 and wrap at the ladder's own length, so degree 5 is the
   octave of a pentatonic scale and degree 7 the octave of a heptatonic
   one; negative degrees descend. A scale has a `tonic` and a chord a
   `root` - both carry a `quality`, and a record literal resolves by its
   field names. Types are Scale.Scale / Scale.Chord / Scale.Quality /
   Scale.ChordQuality when qualified. *)
type Quality = | Major | Minor | Dorian | Phrygian | Lydian | Mixolydian
               | Locrian | HarmMinor | MelMinor | PentMajor | PentMinor
               | Blues | WholeTone | Chromatic
               | CustomQ of Int list         (* any semitone ladder *)
type Scale = { tonic : Pitch.Note; quality : Quality }
type ChordQuality = | Maj | Min | Dim | Aug | Maj7 | Min7 | Dom7
                    | HalfDim7 | Dim7 | Sus2 | Sus4 | Add9
                    | Sixth | Min6 | Dom9 | Maj9 | Min9 | MinMaj7
                    | Dom7b9 | Dom13
                    | Shape of Int list      (* any chord shape *)
type Chord = { root : Pitch.Note; quality : ChordQuality }
type Prog = { key : Scale; degrees : Int list }   (* a degree cycle *)

(* degrees of the key *)
val Scale.offsets : q:Quality -> Int list        (* semitones from the tonic *)
val Scale.degree  : s:Scale -> n:Int -> Pitch.Note      (* n<0 descends *)
val Scale.notes   : s:Scale -> from:Int -> count:Int -> Pitch.Note list
val Scale.snap    : s:Scale -> note:Pitch.Note -> Pitch.Note

(* chords: named by quality, or stacked out of the key *)
val Scale.shape   : q:ChordQuality -> Int list   (* semitones from the root *)
val Scale.tones   : c:Chord -> Pitch.Note list
val Scale.stack   : s:Scale -> from:Int -> count:Int -> Pitch.Note list
val Scale.triad   : s:Scale -> degree:Int -> Pitch.Note list  (* stack of 3 *)
val Scale.seventh : s:Scale -> degree:Int -> Pitch.Note list  (* stack of 4 *)

(* progressions: a key plus a degree cycle, with wrapping lookup *)
val Scale.prog_len    : p:Prog -> Int
val Scale.prog_degree : p:Prog -> i:Int -> Int          (* wraps *)
val Scale.prog_root   : p:Prog -> i:Int -> Pitch.Note
val Scale.prog_chord  : p:Prog -> i:Int -> Pitch.Note list
val Scale.prog_stack  : p:Prog -> i:Int -> count:Int -> Pitch.Note list

(* voicing, and the one exit to frequencies *)
val Scale.invert  : notes:Pitch.Note list -> n:Int -> Pitch.Note list
val Scale.voicing : notes:Pitch.Note list -> low:Pitch.Note -> count:Int
                      -> Pitch.Note list
val Scale.freqs   : t:Pitch.Tuning -> notes:Pitch.Note list -> Scalar list

(* floor division and its non-negative remainder; there is no `%` *)
val Scale.wrap_div : n:Int -> k:Int -> Int
val Scale.wrap_rem : n:Int -> k:Int -> Int

(* Core.Score: phrases, events and playing them - written in
   SynthGraph. A Phrase is symbolic (beats and Notes); an Event is
   realized (Timestamps and a frequency); `realize` is the only bridge,
   and the only place a tempo and a tuning are named. `span`/`layer`/
   `loop`/`move` are spelled to avoid List.length, Scale.stack,
   List.repeat and Pitch.shift, which are usually open beside this
   module. Types are Score.Step / Score.Phrase / Score.Event /
   Score.Item / Score.Level when qualified. *)
type Step   = { note : Pitch.Note; at : Scalar; len : Scalar;
                vel : Scalar; bend : Scalar }         (* beats; bend in
                                                         cents, default 0 *)
type Phrase = { steps : Step list }
type Event  = { freq : Scalar; at : Timestamp; dur : Timestamp;
                vel : Scalar }                        (* real time *)
type Item   = | Play of (Pitch.Note, Scalar) | Rest of Scalar
type Level  = | Ppp | Pp | Piano | Mp | Mf | Forte | Ff | Fff

(* dynamics: a 4 dB ladder anchored at Fff = 1.0 *)
val Score.db    : x:Scalar -> Scalar             (* decibels -> gain *)
val Score.amp   : l:Level -> Scalar
val Score.ramp  : from:Level -> to:Level -> n:Int -> Scalar list
                                                 (* interpolated in dB *)

(* builders *)
val Score.line     : items:Item list -> Phrase   (* laid end to end *)
val Score.melody   : notes:Pitch.Note list -> len:Scalar -> Phrase
val Score.chord    : notes:Pitch.Note list -> at:Scalar -> len:Scalar
                       -> Phrase
val Score.arpeggio : notes:Pitch.Note list -> step:Scalar -> count:Int
                       -> Phrase
val Score.rhythm   : lens:Scalar list -> Phrase  (* unpitched, end to end *)
val Score.hits     : n:Int -> len:Scalar -> Phrase

(* the phrase algebra - every one a pure edit *)
val Score.span      : p:Phrase -> Scalar              (* in beats *)
val Score.seq       : ps:Phrase list -> Phrase        (* one after another *)
val Score.layer     : ps:Phrase list -> Phrase        (* simultaneous *)
val Score.loop      : p:Phrase -> n:Int -> Phrase
val Score.move      : p:Phrase -> beats:Scalar -> Phrase
val Score.transpose : p:Phrase -> semitones:Int -> Phrase
val Score.in_key    : p:Phrase -> s:Scale.Scale -> Phrase
val Score.staccato  : p:Phrase -> ratio:Scalar -> Phrase
val Score.legato    : p:Phrase -> Phrase       (* stretch to next attack *)
val Score.velocity  : p:Phrase -> f:(Scalar -> Scalar) -> Phrase
val Score.vels      : p:Phrase -> vs:Scalar list -> Phrase
  (* per-step dynamics: vs cycles, scaling each velocity *)
val Score.crescendo : p:Phrase -> from:Level -> to:Level -> Phrase
val Score.bend      : p:Phrase -> f:(Int -> Scalar) -> Phrase
  (* per-note inflection in cents, applied at realization *)
val Score.humanize  : p:Phrase -> seed:Scalar -> spread:Scalar -> Phrase
  (* feel in beats, via Math.hash - composes with duration-aware voices *)
val Score.shuffle   : p:Phrase -> grid:Scalar -> amount:Scalar -> Phrase
  (* displaces steps on odd multiples of grid beats *)

(* the bridge to audio; play and strike sum without normalization.
   realize is sugar for realize_with ~pitch:(Pitch.hz ~t:tuning); any
   Note -> Scalar mapping realizes a phrase. *)
val Score.realize : tempo:Tempo.Tempo -> tuning:Pitch.Tuning -> p:Phrase
                      -> Event list
val Score.realize_with : tempo:Tempo.Tempo -> pitch:(Pitch.Note -> Scalar)
                           -> p:Phrase -> Event list
val Score.play    : voice:(Scalar -> Timestamp -> Scalar -> 'a Sample)
                      -> events:Event list -> 'a Signal
val Score.strike  : voice:(Scalar -> 'a Sample)   (* velocity only *)
                      -> events:Event list -> 'a Signal

(* Core.List: list combinators & builders - written in SynthGraph
   (recursive functions over the Cons/Nil variant), not C++ *)
val List.map    : f:('a -> 'b) -> xs:'a list -> 'b list
val List.mapi   : f:(Int -> 'a -> 'b) -> xs:'a list -> 'b list
val List.fold   : f:('a -> 'b -> 'a) -> init:'a -> xs:'b list -> 'a  (* folds left *)
val List.scan   : f:('a -> 'b -> 'a) -> init:'a -> xs:'b list -> 'a list
  (* running fold: n+1 entries, init first *)
val List.init   : n:Int -> f:(Int -> 'a) -> 'a list   (* [f 0; ...; f (n-1)] *)
val List.repeat : n:Int -> x:'a -> 'a list

(* structure *)
val List.length : xs:'a list -> Int
val List.append : xs:'a list -> ys:'a list -> 'a list
val List.nth    : xs:'a list -> i:Int -> default:'a -> 'a  (* out of range -> default *)
val List.rev    : xs:'a list -> 'a list

(* selection & expansion *)
val List.filter   : f:('a -> Bool) -> xs:'a list -> 'a list
val List.concat   : xss:'a list list -> 'a list
val List.flat_map : f:('a -> 'b list) -> xs:'a list -> 'b list
val List.zip      : xs:'a list -> ys:'b list -> ('a, 'b) list  (* stops at the shorter *)
val List.take     : n:Int -> xs:'a list -> 'a list
val List.drop     : n:Int -> xs:'a list -> 'a list

(* numeric *)
val List.range   : from:Int -> count:Int -> Int list   (* [from; ...; from+count-1] *)
val List.sum     : xs:Scalar list -> Scalar
val List.maximum : xs:Scalar list -> least:Scalar -> Scalar  (* empty -> least *)
val List.iter    : f:('a -> unit) -> xs:'a list -> unit
  (* iterate renders over a list; implemented in C++ - `unit` has no
     literal, so a synth-side iterator could not exist *)

(* Core.Time: timestamp construction & sequences *)
val Time.to_sec: x:Scalar -> Timestamp
val Time.to_ms: x:Scalar -> Timestamp
val Time.to_min: x:Scalar -> Timestamp
val Math.not: b:Bool -> Bool
val Time.time_steps: start:Timestamp -> step:Timestamp -> count:Int -> Timestamp list
val Time.jitter: seed:Scalar -> spread:Timestamp -> steps:(Timestamp list) -> Timestamp list

(* the duration quotient: a count and a remainder, no unit ever decays.
   Floor convention; num == den * div + rem, 0s <= rem < den; division
   by 0s is a build error. *)
val Time.div: num:Timestamp -> den:Timestamp -> Int
val Time.rem: num:Timestamp -> den:Timestamp -> Timestamp

(* Core.Groove: the sequencing tier - written in SynthGraph. Everything
   takes the step *list*, so grids, swing, jitter, masks and euclid
   compose freely. *)
val Groove.pattern   : hit:'a Sample -> steps:Timestamp list -> 'a Signal
val Groove.humanized : hit:'a Sample -> steps:Timestamp list
                         -> seed:Scalar -> spread:Timestamp -> 'a Signal
val Groove.mask      : keep:Bool list -> steps:Timestamp list
                         -> Timestamp list    (* keep cycles *)
val Groove.euclid    : hits:Int -> steps:Timestamp list -> Timestamp list
                         (* Bjorklund selection over the given grid *)

(* Core.Mix: the stereo and bus vocabulary - written in SynthGraph.
   channels/mix_all stay in Arrange; this is the musical layer above. *)
val Mix.pan     : pos:Scalar -> input:Scalar Signal -> Vector Signal
                    (* -1 left .. +1 right; equal-power *)
val Mix.pan_sig : pos:Scalar Signal -> input:Scalar Signal -> Vector Signal
val Mix.mix     : parts:(Scalar, 'a Signal) list -> 'a Signal
val Mix.db      : x:Scalar -> Scalar        (* re-export of Score.db *)
val Mix.gain_db : x:Scalar -> input:'a Signal -> 'a Signal
val Mix.vca     : gain:Scalar Signal -> input:'a Signal -> 'a Signal
val Mix.duck    : ats:Timestamp list -> depth:Scalar -> dip:Timestamp
                    -> recover:Timestamp -> input:'a Signal -> 'a Signal

(* Core.Str: the minimum for computed render-target names *)
val Str.cat    : a:String -> b:String -> String
val Str.of_int : n:Int -> String
```

`Core.Dsp` is a *view*: the sound-design working set (oscillators,
envelopes, filters, clips, `mix_all`/`channels`/`channel`/`sample`/
`place`/`place_multi`, the `Sig` constructors, `render`/`render_vis`,
`to_sec`/`to_ms`/`to_min`, and the Math family) re-exported under bare
names, so a working file's preamble is `open Core open Core.Dsp`. The
submodules above stay the canonical homes; `Io`, the stems renders and
`Pitch`/`Tempo`/`Scale`/`Score` are deliberately not in it.

Counts and indices are Ints, so wholeness is guaranteed by the type
system; `List.init`/`List.repeat` treat a negative computed count as
zero (the empty list), while `time_steps` still rejects one as a build
(evaluation) error.

The `List` combinators are **total**: every one of them answers for the
empty list, so a builder that legitimately produces nothing needs no
special case at the call site. Where a partial function would normally
be, the signature names the answer instead — `nth` takes the `default`
for an out-of-range index and `maximum` the value an empty list yields
(which doubles as a floor), rather than failing the build. `zip` stops
at the shorter list. All of them are written in SynthGraph in
`lib.synth`, as ordinary recursive functions over `Cons`/`Nil`.

`Pitch` is written in SynthGraph too. One formula turns a ladder step
into a frequency in every temperament —
`raw s = ratios[(s - root) mod n] * octave ^ floor((s - root) / n)` and
`hz s = ref_hz * raw s / raw ref_step` — so the reference pitch is exact
by construction, `root` is the key centre that makes just and
Pythagorean tunings key-dependent, and `octave` admits non-octave
tunings. `shift` moves by discrete ladder steps; `cents`/`detune` are
continuous and act on frequencies, since a `Note` has no fractional
part. See [`core-library.md`](core-library.md) for the details.

`to_sec`/`to_ms`/`to_min` are the computed counterpart of the literal
suffixes — `to_ms 250.0` is `250ms` — and are what a duration derived
from a tempo, a loop index, or a parameter has to go through:
`to_min (1.0 /. bpm)` is one beat. There is deliberately no conversion
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

*Mutual* recursion and user-defined signal feedback (IIR-style signal
*cycles* — the `Fx.feedback` and `reverb` primitives hold their loops
inside per-render state, and the language-level graph stays acyclic),
literal patterns in `match` (use `if` for value tests), *global* type
inference (parameters and polymorphism are still written out; return
types and `let ... in` annotations are locally inferred, §3),
per-definition visibility control (a library's `lib.synth` publishes
whole modules or re-exported values, but a published module exposes all
of its definitions — type declarations included), reverse playback
(`resample` reads its source only forward, so a negative rate clamps to
zero rather than rewinding), cache tuning knobs. See design doc §13.
(Lambdas, general partial application, cross-directory
imports/packaging — via libraries, `open` and module aliases —
user-written polymorphism, inline modules, build-time Booleans with
`if`/`else`, native extensions — `external` functions, §5 —
user-defined types with pattern matching and self-recursion — `type`
declarations, `match`, `let rec`, with `list` and `Signal`/`Sample`
now ordinary Core declarations — and signal-level *choice* — comparisons
and `if` stay build-time-only, but `Sig.select` and `Fx.follow` are
the sample-wise select and the envelope follower as signal-producing
primitives, §6 — were listed here originally and are now in the
language; see §2, §3, §4 and §6.)
