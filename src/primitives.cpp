#include "primitives.hpp"

namespace synth {

const std::vector<PrimSig>& primitives() {
  static const std::vector<PrimSig> prims = [] {
    std::vector<PrimSig> p;
    TypePtr a = tVar(0), b = tVar(1);
    auto add = [&](PrimId id, std::string name,
                   std::vector<std::string> paramNames,
                   std::vector<TypePtr> paramTypes, TypePtr ret) {
      p.push_back({id, std::move(name), std::move(paramNames),
                   std::move(paramTypes), std::move(ret)});
    };
    // Generators
    add(PrimId::Sine, "sine", {"freq"}, {tScalar()}, tSignal(tScalar()));
    add(PrimId::Saw, "saw", {"freq"}, {tScalar()}, tSignal(tScalar()));
    add(PrimId::Square, "square", {"freq"}, {tScalar()}, tSignal(tScalar()));
    // Envelopes
    add(PrimId::ExpDecay, "exp_decay", {"rate"}, {tScalar()},
        tSignal(tScalar()));
    // ADSR: attack/decay/release are durations, sustain is a level, hold is
    // the gate length (release begins at `hold`). See README for rationale;
    // the design doc leaves exact parameters open.
    add(PrimId::Adsr, "adsr", {"attack", "decay", "sustain", "release", "hold"},
        {tTimestamp(), tTimestamp(), tScalar(), tTimestamp(), tTimestamp()},
        tSignal(tScalar()));
    // Filters
    add(PrimId::Lowpass, "lowpass", {"cutoff", "input"},
        {tScalar(), tSignal(a)}, tSignal(a));
    add(PrimId::Highpass, "highpass", {"cutoff", "input"},
        {tScalar(), tSignal(a)}, tSignal(a));
    // Combination
    add(PrimId::MixAll, "mix_all", {"signals"}, {tList(tSignal(a))},
        tSignal(a));
    add(PrimId::Channels, "channels", {"chans"}, {tList(tSignal(tScalar()))},
        tSignal(tVector()));
    // Slicing & arrangement
    add(PrimId::Sample, "sample", {"signal", "from", "to"},
        {tSignal(a), tTimestamp(), tTimestamp()}, tSample(a));
    add(PrimId::Place, "place", {"sample", "at"}, {tSample(a), tTimestamp()},
        tSignal(a));
    // The single effect
    add(PrimId::Render, "render", {"name", "rate", "sample"},
        {tString(), tScalar(), tSample(a)}, tUnit());
    // File import
    add(PrimId::LoadMono, "load_mono", {"path"}, {tString()},
        tSignal(tScalar()));
    add(PrimId::LoadMulti, "load_multi", {"path"}, {tString()},
        tSignal(tVector()));
    // List combinators
    add(PrimId::Map, "map", {"f", "xs"}, {tFun({a}, b), tList(a)}, tList(b));
    add(PrimId::Fold, "fold", {"f", "init", "xs"},
        {tFun({a, b}, a), a, tList(b)}, a);
    return p;
  }();
  return prims;
}

const PrimSig* findPrimitive(const std::string& name) {
  for (auto& p : primitives())
    if (p.name == name) return &p;
  return nullptr;
}

}  // namespace synth
