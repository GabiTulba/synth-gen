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
