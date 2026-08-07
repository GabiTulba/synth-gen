#pragma once
#include <optional>
#include <string>
#include <vector>

#include "types.hpp"

namespace synth {

enum class PrimId {
  Sine,
  Saw,
  Square,
  Noise,
  Fm,
  Pm,
  Am,
  Delay,
  Reverb,
  ExpDecay,
  Adsr,
  Lowpass,
  Highpass,
  HardClip,
  SoftClip,
  MixAll,
  Channels,
  Sample,
  Place,
  PlaceMulti,
  Render,
  RenderVis,
  RenderStems,
  RenderVisStems,
  LoadMono,
  LoadMulti,
  Map,
  Fold,
  ListInit,
  Repeat,
  TimeSteps,
  Jitter,
  Constant,
  ConstantMulti,
  Time,
  SignalFn,
  SignalFnMulti,
  Exp,
  Sqrt,
  Log,
  Pow,
};

struct PrimSig {
  PrimId id;
  std::string name;
  std::vector<std::string> paramNames;
  std::vector<TypePtr> paramTypes;  // may contain type Vars
  TypePtr retType;
};

const std::vector<PrimSig>& primitives();
const PrimSig* findPrimitive(const std::string& name);
const PrimSig* findPrimitiveById(PrimId id);

// The Core namespace partition. Primitives live in the built-in `Core`
// module - reachable via `open Core` or qualified `Core.name` - except
// the list functions, which live in the `Core.List` submodule under
// OCaml-style names: List.map, List.fold, List.init (= list_init),
// List.repeat.
const PrimSig* findCorePrim(const std::string& name);
const PrimSig* findCoreListPrim(const std::string& name);

}  // namespace synth
