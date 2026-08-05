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
  ExpDecay,
  Adsr,
  Lowpass,
  Highpass,
  MixAll,
  Channels,
  Sample,
  Place,
  Render,
  LoadMono,
  LoadMulti,
  Map,
  Fold,
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

}  // namespace synth
