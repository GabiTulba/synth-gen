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
    // Deterministic pseudo-noise via two-step cascaded FM (an fm operator
    // modulating another fm operator, golden-ratio-related frequencies,
    // large indices). No RNG anywhere: renders stay reproducible, which
    // "purity by construction" requires. freq sets the spectral center.
    add(PrimId::Noise, "noise", {"freq"}, {tScalar()}, tSignal(tScalar()));
    // Modulation. fm: sine oscillator whose instantaneous frequency is
    // carrier + modulator(t) Hz (phase-integrated, so operators cascade).
    // pm: sin(2*pi*carrier*t + modulator(t)), modulator in radians.
    // am: carrier * (1 + depth * modulator); a mono modulator broadcasts
    // across a multi-channel carrier.
    add(PrimId::Fm, "fm", {"carrier", "modulator"},
        {tScalar(), tSignal(tScalar())}, tSignal(tScalar()));
    add(PrimId::Pm, "pm", {"carrier", "modulator"},
        {tScalar(), tSignal(tScalar())}, tSignal(tScalar()));
    add(PrimId::Am, "am", {"carrier", "modulator", "depth"},
        {tSignal(a), tSignal(tScalar()), tScalar()}, tSignal(a));
    // Feedforward delay (the convenience primitive the design doc leaves
    // under consideration, §6): the input shifted later in time by `by`,
    // silence before it. Echoes are delay + arithmetic + mix_all.
    add(PrimId::Delay, "delay", {"by", "signal"},
        {tTimestamp(), tSignal(a)}, tSignal(a));
    // Reverb: Schroeder-style (parallel feedback combs + series allpasses,
    // internal to the primitive - the *language* stays acyclic). decay is
    // the RT60-style tail length, damping in [0,1] rolls off highs in the
    // tail, mix in [0,1] blends dry (0) to fully wet (1).
    add(PrimId::Reverb, "reverb", {"decay", "damping", "mix", "input"},
        {tTimestamp(), tScalar(), tScalar(), tSignal(a)}, tSignal(a));
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
    // Distortion. hard_clip clamps flat at +/-threshold; soft_clip is
    // smooth saturation threshold*tanh(x/threshold) - near-identity for
    // small signals, approaching the cap asymptotically.
    add(PrimId::HardClip, "hard_clip", {"threshold", "input"},
        {tScalar(), tSignal(a)}, tSignal(a));
    add(PrimId::SoftClip, "soft_clip", {"threshold", "input"},
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
    // place_multi: place one sample at every timestamp in the list and mix
    // the placements (overlaps sum). Equivalent to mapping `place` over
    // the list - which v1 cannot express directly, since partial
    // application does not exist.
    add(PrimId::PlaceMulti, "place_multi", {"sample", "ats"},
        {tSample(a), tList(tTimestamp())}, tSignal(a));
    // The effects: render writes an audio artifact; render_vis writes a
    // waveform image (SVG) of the same discretized window instead. Both
    // declare build targets and share the project-wide name space.
    add(PrimId::Render, "render", {"name", "rate", "sample"},
        {tString(), tScalar(), tSample(a)}, tUnit());
    add(PrimId::RenderVis, "render_vis", {"name", "rate", "sample"},
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

const PrimSig* findPrimitiveById(PrimId id) {
  for (auto& p : primitives())
    if (p.id == id) return &p;
  return nullptr;
}

}  // namespace synth
