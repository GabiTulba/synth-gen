# Language tour

A guided walk through the `.synth` language. This is the informal
companion to the [language specification](language-spec.md), which pins
down the exact lexical structure, grammar, typing rules, and evaluation
semantics.

## The model

A sound is a **signal**: a pure function of time, represented as a lazy
graph and never discretized until rendered. A **sample** is a signal cut
to a window (`sample s 0s 800ms`); placing samples back onto a timeline
(`place`, `place_multi`) and mixing (`mix_all`) is how patterns, phrases,
and whole songs are arranged. A source file is a module of fully
annotated, non-recursive definitions; `render` is the language's only
effect, and each call declares one build target.

```ocaml
(* pluck.synth *)
open Core            (* submodule names: Osc, Fx, Arrange, List, ... *)
open Core.Osc open Core.Fx open Core.Arrange open Core.Render

let pluck freq:Scalar : Scalar Signal =
  (sine freq) * (exp_decay 6.0)
;;

let pluck_sample freq:Scalar : Scalar Sample =
  sample (pluck freq) 0s 800ms
;;

let place_pluck at:Timestamp : Scalar Signal =
  place (pluck_sample 440.0) at
;;

let song : Scalar Signal =
  mix_all (List.map place_pluck [0s; 500ms; 1s; 1500ms])
;;

let _ = render "demo" 48000.0 (sample song 0s 2s)
;;
```

All primitives live in **`Core`** — a real library bundled with the
compiler (see [`core-library.md`](core-library.md)) organized into
functional submodules: `Osc`, `Fx`, `Arrange`, `Render`, `Io`, `List`,
`Time`, `Sig`, `Math`. Core is not ambient — bring it into scope like
any library: `import Core` (qualified `Core.Osc.sine`), `open Core`
(module-qualified `Osc.sine`, `List.map`), or `open Core.Osc` (bare
`sine`). It aliases like any module (`module C = Core`,
`module L = Core.List`). Code fragments below assume the relevant
submodules are open.

## Types, annotations, and time

Everything is fully annotated — the checker verifies, it never guesses.
The value types are `Scalar`, `Int`, `Vector` (N-channel), `Timestamp`,
`String`, `Bool`, `unit`, plus `t Signal`, `t Sample`, `t list`, and
tuples. A number literal with a decimal point is a `Scalar` (`440.0`);
without one it is an `Int` (`8`) — the whole-number kind that counts
and indexes. A literal with a unit suffix (`100ns`, `800ms`, `1.5s`,
`1m`) is a `Timestamp`. A *computed* Scalar enters the time domain
through `to_sec`/`to_ms`/`to_min` (`to_min (1.0 / bpm)` is one beat) —
and there is deliberately no conversion back, because a Timestamp that
decays into a bare number is how unit confusion gets in. Ints convert
only explicitly: `to_scalar` exactly, and `round`/`floor`/`ceil` back
from the continuous side.

Arithmetic (`+ - * /`) lifts pointwise and broadcasts Scalars:
`Signal * Signal` is pointwise, `Signal * Scalar` scales, `Vector`
arithmetic is element-wise with channel counts checked when the graph is
built. `Int` arithmetic stays whole (`/` divides towards zero) and
never mixes with the continuous kinds implicitly. See spec §3 for the
full operator table.

## Labeled arguments, currying, pipes, lambdas

```ocaml
open Core
open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Time

let voice ~amp:Scalar ~freq:Scalar : Scalar Signal = (sine freq) * amp ;;
let quiet : Scalar -> Scalar Signal = voice ~amp:0.25 ;;   (* curried *)

let warm : Scalar Signal =
  saw 220.0 |> lowpass ~cutoff:800.0 |> soft_clip 0.8 ;;

let pattern : Scalar Signal =
  let hit : Scalar Sample = warm |> sample ~from:0s ~to:100ms in
  let beats : Timestamp list = time_steps ~start:0s ~step:200ms ~count:5 in
  mix_all (List.map (place hit) beats) ;;   (* or: place_multi hit beats *)

let echoes : Scalar Signal =
  let hit : Scalar Sample = warm |> sample ~from:0s ~to:100ms in
  mix_all (List.map (fun t:Timestamp -> place hit t) [0s; 250ms; 500ms]) ;;

let chord : Scalar Signal =
  let tone freq:Scalar : Scalar Signal = sine freq * 0.3 in  (* local function *)
  mix_all [tone 220.0; tone 275.0; tone 330.0] ;;

let _ = sample pattern ~from:0s ~to:2s |> render ~name:"warm" ~rate:48000.0 ;;
```

- **Labeled arguments** go in any order; providing any subset of a
  function's arguments — labeled, a positional prefix, or a mix —
  curries it over the remaining parameters. Primitive parameters are all
  labeled with their signature names, so any primitive can be called by
  label or partially applied.
- Any function-typed expression can be applied (`(f 1.0) 2.0`) or passed
  along — a bare name, a partial application, a parameter, a lambda.
- **Lambdas** (`fun x:Scalar -> ...`) annotate their parameters like
  every other binding, capture enclosing locals by value, and must be
  parenthesized when used as an argument or pipe right-hand side.
- **Pipe**: `x |> f a` desugars to `f a x` — the piped value becomes the
  final positional argument.
- **Local bindings**: `let name : Type = e in body` is an expression;
  chained sub-lets nest naturally. With parameters
  (`let tone freq:Scalar : Scalar Signal = ... in ...`) a local binding
  defines a **local function** — sugar for binding a lambda, so it
  captures enclosing locals and parameters, takes labels, and curries
  like any other function.

## Polymorphic signatures

An annotation may name **type variables**, so one definition serves
every element type the way the primitives do — the checker instantiates
it afresh at each use:

```ocaml
let dampen ~input:'a Signal : 'a Signal =
  lowpass ~cutoff:600.0 (soft_clip ~threshold:0.8 input) ;;

let mono : Scalar Signal = dampen (saw 220.0) ;;
let wide : Vector Signal = dampen (channels [saw 220.0; saw 221.0]) ;;
```

The variable is still *written*, never inferred, and inside the body it
is rigid: `'a` is whatever the caller picked, so the body can pass it
along but cannot assume it is a Scalar or a Signal. Every variable in
the result must appear in a parameter, otherwise no call site could
determine it. Polymorphic definitions curry, take and return functions
(`let twice ~f:('a -> 'a) ~x:'a : 'a = f (f x)`), and are published by
libraries like any other value. Types are erased before evaluation, so a
polymorphic definition renders bit-identically to the monomorphic one it
replaces.

## Modules: files, libraries, inline modules

Files are modules: `import A` resolves `a.synth` in the same directory
(inside a library, a fellow member) and gives qualified access
(`A.def`). Libraries add `import Lib` / `import Lib.Mod`, `open Lib` /
`open Lib.Mod` (unqualified access, position-ordered shadowing), and
module aliases (`module K = Basic.Keys`). What a library publishes is
declared in its `lib.synth` interface file — see
[`build-system.md`](build-system.md) for how libraries are declared and
discovered, and spec §4 for resolution rules. See
`examples/song/preview.synth` for a working demonstration.

Files can also namespace definitions with **inline modules** —
`module A = struct … end`, nested at will, referenced as `A.x` (from
other files `File.A.x`) or via `open A`, with `open`s inside a `struct`
scoped to it:

```ocaml
import Core
module Voices = struct
  open Core.Osc open Core.Fx   (* scoped: end at this module's `end` *)
  let base : Scalar = 220.0 ;;
  module Wet = struct
    let damp ~input:'a Signal : 'a Signal = lowpass ~cutoff:600.0 input ;;
  end
  let lead : Scalar Signal = Wet.damp (sine base) ;;
end ;;

let mono : Scalar Signal = Voices.Wet.damp (Core.Osc.sine Voices.base) ;;
```

Bodies nest arbitrarily and see the enclosing scope; a member is just a
top-level definition under its dotted name — same typing, evaluation,
and incremental caching as everything else.

## Booleans and `if`/`else`

Configuration is part of the language. `Bool` is a *build-time* value —
comparisons (`< <= > >= == !=`, on two Scalars or two Timestamps),
`&&`/`||` (short-circuit), and `not` decide it while the graph is
assembled, and `if` picks a value, a signal chain, or even which target
renders. Only the taken branch evaluates; signals themselves are never
compared or branched per sample.

```ocaml
let fast : Bool = tempo >= 120.0 && not (tempo > 200.0) ;;

let voice ~freq:Scalar ~crisp:Bool : Scalar Signal =
  if crisp then highpass ~cutoff:900.0 (saw freq)
  else lowpass ~cutoff:500.0 (sine freq) ;;

let _ =
  if fast then render "fast" 48000.0 (sample mix 0s 8s)
  else render "slow" 48000.0 (sample mix 0s 16s) ;;
```

## External functions

An external implements a definition in C++. Declare the signature in
synth, point at a `.cpp` file next to your source, and synthc compiles
it at build time (cached by content under `_build/externals/`, watched
by the daemon like an audio input):

```ocaml
open Core.Osc
let succ a:Scalar : Scalar = external "succ.cpp" ;;
let tone : Scalar Signal = sine (succ 439.0) ;;
```

```cpp
// succ.cpp
#include <synth/external.hpp>

SYNTH_EXTERNAL(succ) {
  *result = synth::ext::Value::scalar(args[0].asScalar() + 1.0);
  return true;
}
```

Every value crosses the boundary: data (Scalar, Int, Timestamp, Bool,
String, Vector, lists, tuples) transparently, Signals and Samples as
lazy engine graph handles you combine with the `<synth/engine.hpp>`
constructors, and functions as opaque handles callable through the
context. This is also how Core itself is built — see
[`core-library.md`](core-library.md).

## Building signals directly

`constant 0.5` holds a level forever, `time` is the ramp whose sample at
t seconds is t, and `signal ~f:(fun t:Scalar -> exp (0.0 - 3.0 * t))`
samples a function of time (`constant_multi` / `signal_multi` are the
per-channel forms). The math primitives `exp`, `sqrt`, `log`, and
`pow ~x ~y` work on plain Scalars and elementwise on Signals —
`pow (sine 220.0) 3.0` is a waveshaper, `sqrt time` a fade-in curve.

## Rhythm and humanization

`time_steps ~start ~step ~count` builds arithmetic timestamp sequences —
the natural feed for `place_multi`
(`place_multi kick (time_steps ~start:0s ~step:500ms ~count:8)`);
`jitter ~seed ~spread` humanizes such a list with hash-derived (pure,
reproducible) per-note timing deltas. `List.init` / `List.repeat` build
generated lists; counts and indices are `Int`s, so wholeness is the
type system's business (`List.init 6 (fun i:Int -> ...)`), and the
index reaches Scalar arithmetic through `to_scalar`.

`place_multi` sums its placements without normalization, so overlapping
or dense patterns can exceed full scale — rendering works in doubles and
only hard-clamps to [-1, 1] at WAV write, so manage headroom
deliberately by scaling down or with `soft_clip`/`hard_clip`.

## Where to next

- [`language-spec.md`](language-spec.md) — the precise rules and the
  full primitive roster (spec §6).
- [`core-library.md`](core-library.md) — what each primitive does,
  including the exact semantics of `adsr`, `fm`/`pm`/`am`, `delay`,
  `resample`, `reverb`, `noise`, and the render effects.
- [`build-system.md`](build-system.md) — manifests, projects, libraries,
  and build outputs.
