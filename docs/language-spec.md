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
- **Keywords**: `let`, `import`.
- **Number literals**: `[0-9]+(\.[0-9]+)?` — always `Scalar`.
- **Timestamp literals**: a number literal immediately followed by a unit
  suffix: `ns` (1e-9 s), `us` (1e-6 s), `ms` (1e-3 s), `s` (1 s),
  `m` (60 s). All denote the same quantity (time since the epoch or a
  duration); e.g. `100ns`, `800ms`, `1.5s`, `1m`. An unknown suffix is a
  lexical error.
- **String literals**: `"..."` with escapes `\n`, `\t`, `\\`, `\"`.
- **Punctuation**: `;;` `;` `:` `=` `(` `)` `[` `]` `,` `.` `->` `~`
  `|>` `+` `-` `*` `/`.

## 2. Grammar

EBNF; `{x}` is repetition, `[x]` is optionality.

```
module      ::= { top-def }
top-def     ::= import-def | let-def
import-def  ::= "import" UpIdent [ ";;" ]
let-def     ::= "let" (Ident | "_") { param } [ ":" type ] "=" expr ";;"
param       ::= [ "~" ] Ident ":" param-type      (~ marks a labeled param)

type        ::= postfix-type [ "->" type ]          (right-associative)
param-type  ::= postfix-type                        (arrows need parens)
postfix-type::= atom-type { "Signal" | "Sample" | "list" }
atom-type   ::= "Scalar" | "Vector" | "Timestamp" | "String" | "unit"
              | "(" type { "," type } ")"           (tuple if >1 element)

expr        ::= let-in | pipe
let-in      ::= "let" Ident ":" type "=" expr "in" expr
pipe        ::= additive { "|>" additive }          (lowest, left-assoc)
additive    ::= multiplicative { ("+" | "-") multiplicative }
multiplicative ::= unary { ("*" | "/") unary }
unary       ::= "-" unary | app
app         ::= atom { arg }                        (application, left)
arg         ::= atom | "~" Ident ":" atom           (labeled argument)
atom        ::= Number | Time | String
              | Ident                               (unqualified name)
              | UpIdent "." Ident                   (qualified name)
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
- **Pipe**: `x |> f a b` desugars to the application `f a b x` — the
  piped value becomes the final positional argument. The right-hand side
  must be a function name or application; chains are left-associative:
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
  site. Users cannot write polymorphic definitions.
- **Definition before use, no recursion.** A definition may reference
  only parameters, *earlier* definitions of its module, imported modules'
  definitions (qualified), and primitives. Name resolution order for
  unqualified names: local binding/parameter (innermost first) → earlier
  module definition → primitive.
- **Local bindings** type-check like top-level ones: the bound expression
  must match the annotation (a var-carrying partial application of a
  polymorphic primitive resolves against it), and the name is visible
  only in the body of the `in`.
- **Labeled arguments & label-driven currying.** A parameter declared
  `~name:Type` is *labeled*; primitive parameters are all labeled with
  their signature names. At a call site, positional arguments fill the
  leftmost unfilled parameters in order, and labeled arguments
  (`f ~x:v`) fill their parameter by name, in any order. If every
  parameter is filled the call evaluates. If the remaining unfilled
  parameters are all labeled, the call is a *partial application*: its
  value is the curried function of the remaining parameters
  (`lowpass ~cutoff:800.0 : 'a Signal -> 'a Signal`). Unfilled
  *positional* parameters are an error — positional application remains
  all-or-nothing. Labels are not part of type equality.
- **Application otherwise.** A function *name* (user or primitive) may
  be passed unapplied wherever a matching function type is expected
  (`map place_pluck [...]`, `map sine [...]`); lambdas do not exist in
  v1. A stored partial application of a *polymorphic* primitive gets its
  type variables resolved against the binding's annotation
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

## 4. Modules & projects

- **File = module.** `foo.synth` defines module `Foo` (stem, first
  letter capitalized). All top-level definitions are visible to
  importers; access is qualified (`Foo.bar`).
- **`import A`** resolves `a.synth` (module name lowercased) in the same
  directory. Unresolved imports and import cycles are build errors.
- **Project**: a directory with a `.build` manifest — line-based,
  `project <name>` once plus one `source <file>` per source file; `#`
  starts a comment.
- **Render targets**: every `render` call evaluated at build time
  declares one. Names must be unique project-wide. Artifacts land in
  `build/artifacts/<name>.wav`; the machine-readable index in
  `build/metadata.json`.

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
- Audio files are build inputs: `load_mono`/`load_multi` read and
  validate them at build time (channel counts checked against the
  primitive; paths resolve relative to the source file). A loaded file
  occupies `[0s, duration)` and is silence afterward; content is
  linearly resampled to the render rate.
- Renders are deterministic; incremental rebuilds and caching rely on it.

## 6. Primitive signatures (v1 roster)

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

(* list combinators & builders *)
val map       : f:('a -> 'b) -> xs:'a list -> 'b list
val fold      : f:('a -> 'b -> 'a) -> init:'a -> xs:'b list -> 'a
val list_init : n:Scalar -> f:(Scalar -> 'a) -> 'a list   (* [f 0.0; ...; f (n-1)] *)
val repeat    : n:Scalar -> x:'a -> 'a list
val time_steps: start:Timestamp -> step:Timestamp -> count:Scalar -> Timestamp list
val jitter    : seed:Scalar -> spread:Timestamp -> steps:(Timestamp list) -> Timestamp list
```

Counts and indices are Scalars (the language's single numeric type);
counts must be whole and non-negative, validated at build time.

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
recursion and feedback (IIR-style signal cycles), lambdas and partial
application, user polymorphism, visibility control/interface files,
cross-directory imports and packaging, cache tuning knobs, native
extensions. See design doc §13.
