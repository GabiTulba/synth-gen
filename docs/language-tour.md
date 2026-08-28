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
  (sine freq) *. (exp_decay 6.0)
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
`Time`, `Sig`, `Groove`, `Pitch`, `Tempo`, `Scale`, `Score`, `Mix`,
`Str`, `Math`. Core is not ambient — bring it into scope like
any library: `import Core` (qualified `Core.Osc.sine`), `open Core`
(module-qualified `Osc.sine`, `List.map`), or `open Core.Osc` (bare
`sine`). It aliases like any module (`module C = Core`,
`module L = Core.List`). Once the structure is familiar, the
working-file idiom for sound-design files is the `Dsp` prelude —
`open Core open Core.Dsp` re-exports the signal-tier working set under
bare names, replacing the per-submodule open block (and `synthc lint`
warns about opens a file never uses). Code fragments below assume the
relevant submodules are open.

## Types, annotations, and time

Everything is fully annotated — the checker verifies, it never guesses.
The built-in value types are `Scalar`, `Int`, `Vector` (N-channel),
`Timestamp`, `String`, `Bool`, `unit`, and tuples; `t Signal`,
`t Sample` and `t list` are ordinary *declarations* the Core library
makes ambient (and you can declare your own — see "Records" and
"Variants and match" below). A number literal with a decimal point is a `Scalar` (`440.0`);
without one it is an `Int` (`8`) — the whole-number kind that counts
and indexes. A literal with a unit suffix (`100ns`, `800ms`, `1.5s`,
`1m`) is a `Timestamp`. A *computed* Scalar enters the time domain
through `to_sec`/`to_ms`/`to_min` (`to_min (1.0 /. bpm)` is one beat),
and back out through `of_sec`/`of_ms`/`of_min` (`of_ms 250ms` is
`250.0`). The way out is deliberately a named conversion: what causes
unit confusion is a Timestamp decaying into a bare number with nobody
saying which unit it is in, so arithmetic never drops the unit on its
own. Once in the time domain you can stay there and keep computing: Timestamps add and
subtract, and scale by a Scalar, so `beat *. 4.0` is the bar,
`beat +. beat /. 2.0` the dotted note, and `bar -. beat` the upbeat
before it (results clamp at `0s`). What is left out is left out on
purpose — `1s *. 2s` is not a duration and `1s /. 500ms` would be the Scalar
conversion that does not exist. Ints convert
only explicitly: `to_scalar` exactly, and `round`/`floor`/`ceil` back
from the continuous side.

Every operator comes in two spellings: bare (`+ - * /`) for `Int`s, and
`.`-suffixed (`+. -. *. /.`) for the continuous kinds. The continuous
half lifts pointwise and broadcasts Scalars: `Signal *. Signal` is
pointwise, `Signal *. Scalar` scales, `Vector`
arithmetic is element-wise with channel counts checked when the graph is
built. `Int` arithmetic stays whole (`/` divides towards zero) and
never mixes with the continuous kinds implicitly — nothing is
overloaded across the two halves, so `7 / 2` is `3` and `7.0 /. 2.0` is
`3.5`, and neither can be mistaken for the other. See spec §3 for the
full operator table.

## Labeled arguments, currying, pipes, lambdas

```ocaml
open Core
open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Time

let voice ~amp:Scalar ~freq:Scalar : Scalar Signal = (sine freq) *. amp ;;
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
  let tone freq:Scalar : Scalar Signal = sine freq *. 0.3 in  (* local function *)
  mix_all [tone 220.0; tone 275.0; tone 330.0] ;;

let _ = sample pattern ~from:0s ~to:2s |> render ~name:"warm" ~rate:48000.0 ;;
```

- **Labeled arguments** go in any order; providing any subset of a
  function's arguments — labeled, a positional prefix, or a mix —
  curries it over the remaining parameters. Primitive parameters are all
  labeled with their signature names, so any primitive can be called by
  label or partially applied. When the value is a variable of the same
  name, the label **puns**: `~cutoff` means `~cutoff:cutoff`, so
  `adsr ~attack ~decay ~sustain ~release` says once what it would
  otherwise say twice.
- **Optional labels** let a call skip a parameter entirely. They are
  declared *before* every required parameter, as `?x:T` (the body then
  sees `x : T Option` — `None` when the call left it out) or with a
  default, `?(x = e : T)` (the body sees a plain, determined `T`):

  ```ocaml
  let pluck ?(bright = 0.5 : Scalar) ?vibrato:(Scalar Signal) freq:Scalar
      : Scalar Signal =
    let base : Scalar Signal = (match vibrato with
      | None -> sine freq
      | Some lfo -> fm ~carrier:freq ~modulator:lfo) in
    soft_clip ~threshold:(1.0 -. bright) base *. exp_decay 6.0 ;;

  let plain : Scalar Signal = pluck 220.0 ;;              (* defaults *)
  let hard : Scalar Signal = pluck ~bright:0.9 220.0 ;;   (* determined *)
  let wob : Scalar Signal =
    pluck ?vibrato:(Some (sine 5.0 *. 3.0)) 220.0 ;;      (* an Option through *)
  ```

  `~x:v` fills an optional parameter with a determined value; `?x:opt`
  passes a whole Option through (both spellings pun). Positional
  arguments skip optional parameters, and a call *completes* — applying
  the defaults — as soon as its required parameters are filled. The
  `Core.Option` module carries the monadic vocabulary (`map`, `bind`,
  `value`, ...) over the ambient `'a Option` variant.
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

## Records

`type` declares your own types. A record bundles named fields; a
literal names no type — it resolves to the record declaration in scope
with exactly those fields:

```ocaml
type Patch = { freq : Scalar; gain : Scalar; bite : Scalar } ;;

let lead : Patch = { freq = 440.0; gain = 0.5; bite = 0.8 } ;;
let soft : Patch = { lead with gain = 0.2 } ;;        (* functional update *)

let voice p:Patch : Scalar Signal =
  soft_clip ~threshold:p.bite (saw p.freq) *. p.gain ;;
```

Projection (`p.freq`) binds tighter than application, updates keep the
record's type, and declarations can be polymorphic
(`type 'a Voice = { osc : 'a Signal; vel : Scalar }`, used as
`Scalar Voice`) or *abstract* (`type Handle ;;` — no visible
structure; Core's `Signal` is declared exactly like that). Core's
`Sample` is itself a record, `{ sig; from; to }`, so `s.from` and
`{ s with to = 1s }` work out of the box. Types are nominal: two
same-shaped records are still different types, and declarations travel
with modules like values do.

## Variants and match

A variant enumerates shapes; `match` picks them apart. Constructors
carry at most one payload (bundle more in a tuple), and matches must be
exhaustive — the checker names a missing case concretely:

```ocaml
type Wave = | Sine | Saw | Pulse of Scalar ;;

let osc w:Wave ~freq:Scalar : Scalar Signal =
  match w with
  | Sine -> sine ~freq:freq
  | Saw -> saw ~freq:freq
  | Pulse duty -> square ~freq:freq *. duty ;;
```

Patterns nest (`| Full (Pulse d) -> ...`), destructure tuples and
records (`| (t, s) -> ...`, `{ attack; release = r }` — punned or
renamed, any subset of the fields), and bind without annotations: the
scrutinee's type already says everything. Only the taken arm
evaluates, so an arm can even `render`. An irrefutable pattern also
works straight in a binding:
`let (lo, hi) : (Scalar, Scalar) = bounds in ...`.

## Recursion — and lists are just a variant

`let rec` puts a function's own name in scope in its body. That, plus
recursive type declarations, is exactly what lists are made of: Core
declares `type 'a list = | Nil | Cons of ('a, 'a list)`, makes it
ambient, and writes the whole `List` module (`map`, `fold`, `init`,
`repeat`, `length`, `append`, `nth`, `rev`, `filter`, `concat`,
`flat_map`, `zip`, `range`, `sum`, `maximum`) in SynthGraph —
`[a; b; c]` is sugar for a `Cons` chain:

```ocaml
let rec swell xs:Scalar Signal list ~gain:Scalar : Scalar Signal =
  match xs with
  | Nil -> constant 0.0
  | Cons (x, rest) -> x *. gain +. swell rest ~gain:(gain /. 2.0) ;;

let stack : Scalar Signal = swell [sine 110.0; sine 220.0; sine 440.0] ~gain:0.5 ;;
```

Recursion is self-only (no mutual groups), needs at least one
parameter, and is guarded: past 4096 genuinely *nested* calls the
build fails with a recursion-limit diagnostic instead of running away.
Tail calls — through `let ... in` bodies, `if` branches and `match`
arms — are eliminated, so accumulator-shaped recursion (every Core
`List` combinator is written that way) runs at constant depth however
long the list.

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
comparisons (`<. <=. >. >=. ==. !=.`, on two Scalars or two Timestamps;
the bare `< <= > >= == !=` compare two Ints),
`&&`/`||` (short-circuit), and `not` decide it while the graph is
assembled, and `if` picks a value, a signal chain, or even which target
renders. Only the taken branch evaluates; signals themselves are never
compared or branched per sample.

```ocaml
let fast : Bool = tempo >=. 120.0 && not (tempo >. 200.0) ;;

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
t seconds is t, and `signal ~f:(fun t:Scalar -> exp (0.0 -. 3.0 *. t))`
samples a function of time (`constant_multi` / `signal_multi` are the
per-channel forms). The math primitives `exp`, `sqrt`, `log`, and
`pow ~x ~y` work on plain Scalars and elementwise on Signals —
`pow (sine 220.0) 3.0` is a waveshaper, `sqrt time` a fade-in curve.

## Live controls

`Control.slider ~name:"cutoff" ~min:100.0 ~max:4000.0 ~default:900.0`
(and `Control.knob`, drawn as a rotary dial) declares a named tweakable
parameter and evaluates to an ordinary Scalar: the default, or the
override the `synth-dev` app wrote while you dragged its slider with a
`synthc watch` daemon running. The value is fixed for one whole build —
evaluation stays pure and renders stay deterministic; moving a slider
is a rebuild, not a modulation (use signals for movement *within* a
render). `examples/controls` is the worked example.

## Rhythm and humanization

`time_steps ~start ~step ~count` builds arithmetic timestamp sequences —
the natural feed for `place_multi`
(`place_multi kick (time_steps ~start:0s ~step:500ms ~count:8)`);
`jitter ~seed ~spread` humanizes such a list with hash-derived (pure,
reproducible) per-note timing deltas.

`Core.Tempo` gives the same grid a name for the tempo behind it. A
`Tempo` is a BPM and a meter (`common ~bpm:120.0` is 4/4), `beat`/`bar`/
`beats ~n` are its pulse, and `value ~v:Quarter` — or `Dotted Quarter`,
or `Tuplet (3, 2, Eighth)` — is a note value in Timestamps. `grid ~t
~from ~step:Quarter ~count:28` replaces the precomputed
`time_steps ~start:2s ~step:500ms ~count:28`, so re-tempoing a piece is
one edit instead of a sweep over literals; `at ~bar:4 ~beat:2.0` names a
position (bars and beats count from 0, as an offset rather than a ruler
label); and `swing ~amount ~step` displaces every offbeat by a fixed
proportion where `jitter` scatters by hashing.

`Core.Scale` puts the pitches in a key. A `Scale` is a `tonic` and a
`quality` (`{ tonic = { pc = A; oct = 3 }; quality = Minor }`), and
`degree ~s ~n` is the note `n` steps up its ladder — 0-based, wrapping
at the scale's own length, descending for negative `n`. A melody written
in degrees stays in key by construction, and transposing the piece is
editing `tonic`. `triad ~degree` and `seventh ~degree` stack every other
degree, so the chord's quality falls out of the key rather than being
named; `tones ~c` does the same for a chord you *do* name
(`{ root = ...; quality = Dom7 }`). Everything hands back a
`Pitch.Note list`, which `invert`, `voicing ~low ~count` and
`freqs ~t` all consume — `snap` pulls a stray note back into the key on
the way.

`Core.Score` is where those two meet. A `Phrase` is a score in *beats* —
`melody ~notes ~len`, `chord ~notes ~at ~len`, or `line ~items` with
`Play` and `Rest` laid end to end — and the transforms on it are pure
edits: `move`, `transpose`, `in_key`, `staccato`, `legato`, `velocity`,
with `seq` joining phrases end to end and `layer` sounding them at once.
Nothing has a time or a frequency yet. `realize ~tempo ~tuning` is the
single bridge to `Event`s, and `play ~voice` places and mixes them,
handing each voice `(freq, duration, velocity)` — so an envelope's
`~hold` follows the *written* note length:

```ocaml
let piano freq:Scalar dur:Timestamp vel:Scalar : Scalar Sample =
  K.strike freq
    *. adsr ~attack:4ms ~decay:600ms ~sustain:0.25 ~release:350ms ~hold:dur
    *. vel
  |> sample ~from:0s ~to:(dur +. 350ms) ;;

melody ~notes:(List.map ~f:(Scale.degree ~s:key) [0; 2; 4; 2; 0])
       ~len:1.0
  |> move ~beats:8.0
  |> realize ~tempo:tempo ~tuning:tuning
  |> play ~voice:piano
```

Dynamics live in `Score` too: `amp ~l:Mf` is a velocity on a 4 dB ladder
anchored at `Fff = 1.0`, and `ramp ~from ~to ~n` is a crescendo
interpolated in decibels. Like `place_multi`, `play` sums without
normalizing — the headroom is yours. `List.init` / `List.repeat` build
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
