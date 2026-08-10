#include "native.hpp"

namespace synth::native {

// Implementations, declared in their domain files. The (file, name)
// pairs here are exactly what stdlib/core/lib.synth's `external`
// declarations reference.
Value sineImpl(Ctx&, std::vector<Value>&);
Value sawImpl(Ctx&, std::vector<Value>&);
Value squareImpl(Ctx&, std::vector<Value>&);
Value noiseImpl(Ctx&, std::vector<Value>&);
Value fmImpl(Ctx&, std::vector<Value>&);
Value pmImpl(Ctx&, std::vector<Value>&);
Value amImpl(Ctx&, std::vector<Value>&);
Value delayImpl(Ctx&, std::vector<Value>&);
Value resampleImpl(Ctx&, std::vector<Value>&);
Value reverbImpl(Ctx&, std::vector<Value>&);
Value expDecayImpl(Ctx&, std::vector<Value>&);
Value adsrImpl(Ctx&, std::vector<Value>&);
Value lowpassImpl(Ctx&, std::vector<Value>&);
Value highpassImpl(Ctx&, std::vector<Value>&);
Value hardClipImpl(Ctx&, std::vector<Value>&);
Value softClipImpl(Ctx&, std::vector<Value>&);
Value mixAllImpl(Ctx&, std::vector<Value>&);
Value channelsImpl(Ctx&, std::vector<Value>&);
Value sampleImpl(Ctx&, std::vector<Value>&);
Value placeImpl(Ctx&, std::vector<Value>&);
Value placeMultiImpl(Ctx&, std::vector<Value>&);
Value renderImpl(Ctx&, std::vector<Value>&);
Value renderVisImpl(Ctx&, std::vector<Value>&);
Value renderStemsImpl(Ctx&, std::vector<Value>&);
Value renderVisStemsImpl(Ctx&, std::vector<Value>&);
Value loadMonoImpl(Ctx&, std::vector<Value>&);
Value loadMultiImpl(Ctx&, std::vector<Value>&);
Value mapImpl(Ctx&, std::vector<Value>&);
Value foldImpl(Ctx&, std::vector<Value>&);
Value listInitImpl(Ctx&, std::vector<Value>&);
Value repeatImpl(Ctx&, std::vector<Value>&);
Value timeStepsImpl(Ctx&, std::vector<Value>&);
Value jitterImpl(Ctx&, std::vector<Value>&);
Value constantImpl(Ctx&, std::vector<Value>&);
Value constantMultiImpl(Ctx&, std::vector<Value>&);
Value timeImpl(Ctx&, std::vector<Value>&);
Value signalFnImpl(Ctx&, std::vector<Value>&);
Value signalFnMultiImpl(Ctx&, std::vector<Value>&);
Value expImpl(Ctx&, std::vector<Value>&);
Value sqrtImpl(Ctx&, std::vector<Value>&);
Value logImpl(Ctx&, std::vector<Value>&);
Value powImpl(Ctx&, std::vector<Value>&);
Value toScalarImpl(Ctx&, std::vector<Value>&);
Value roundImpl(Ctx&, std::vector<Value>&);
Value floorImpl(Ctx&, std::vector<Value>&);
Value ceilImpl(Ctx&, std::vector<Value>&);
Value toSecImpl(Ctx&, std::vector<Value>&);
Value toMsImpl(Ctx&, std::vector<Value>&);
Value toMinImpl(Ctx&, std::vector<Value>&);
Value notImpl(Ctx&, std::vector<Value>&);

namespace {
const Impl kImpls[] = {
    {"oscillators.cpp", "sine", sineImpl},
    {"oscillators.cpp", "saw", sawImpl},
    {"oscillators.cpp", "square", squareImpl},
    {"oscillators.cpp", "noise", noiseImpl},
    {"oscillators.cpp", "fm", fmImpl},
    {"oscillators.cpp", "pm", pmImpl},
    {"oscillators.cpp", "am", amImpl},
    {"effects.cpp", "delay", delayImpl},
    {"effects.cpp", "resample", resampleImpl},
    {"effects.cpp", "reverb", reverbImpl},
    {"effects.cpp", "exp_decay", expDecayImpl},
    {"effects.cpp", "adsr", adsrImpl},
    {"effects.cpp", "lowpass", lowpassImpl},
    {"effects.cpp", "highpass", highpassImpl},
    {"effects.cpp", "hard_clip", hardClipImpl},
    {"effects.cpp", "soft_clip", softClipImpl},
    {"sampling.cpp", "mix_all", mixAllImpl},
    {"sampling.cpp", "channels", channelsImpl},
    {"sampling.cpp", "sample", sampleImpl},
    {"sampling.cpp", "place", placeImpl},
    {"sampling.cpp", "place_multi", placeMultiImpl},
    {"render.cpp", "render", renderImpl},
    {"render.cpp", "render_vis", renderVisImpl},
    {"render.cpp", "render_stems", renderStemsImpl},
    {"render.cpp", "render_vis_stems", renderVisStemsImpl},
    {"io.cpp", "load_mono", loadMonoImpl},
    {"io.cpp", "load_multi", loadMultiImpl},
    {"lists.cpp", "map", mapImpl},
    {"lists.cpp", "fold", foldImpl},
    {"lists.cpp", "init", listInitImpl},
    {"lists.cpp", "repeat", repeatImpl},
    {"lists.cpp", "time_steps", timeStepsImpl},
    {"lists.cpp", "jitter", jitterImpl},
    {"signals.cpp", "constant", constantImpl},
    {"signals.cpp", "constant_multi", constantMultiImpl},
    {"signals.cpp", "time", timeImpl},
    {"signals.cpp", "signal", signalFnImpl},
    {"signals.cpp", "signal_multi", signalFnMultiImpl},
    {"math.cpp", "exp", expImpl},
    {"math.cpp", "sqrt", sqrtImpl},
    {"math.cpp", "log", logImpl},
    {"math.cpp", "pow", powImpl},
    {"math.cpp", "to_scalar", toScalarImpl},
    {"math.cpp", "round", roundImpl},
    {"math.cpp", "floor", floorImpl},
    {"math.cpp", "ceil", ceilImpl},
    {"math.cpp", "to_sec", toSecImpl},
    {"math.cpp", "to_ms", toMsImpl},
    {"math.cpp", "to_min", toMinImpl},
    {"math.cpp", "not", notImpl},
};
}  // namespace

const Impl* find(const std::string& file, const std::string& name) {
  for (auto& impl : kImpls)
    if (name == impl.name && file == impl.file) return &impl;
  return nullptr;
}

}  // namespace synth::native
