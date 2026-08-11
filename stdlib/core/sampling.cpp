#include "util.hpp"

// Mixing, channel assembly, and the sample/place arrangement family.

using synth::ext::Sample;
using synth::ext::Value;
using namespace synth;

SYNTH_EXTERNAL(mix_all) {
  std::vector<SigPtr> items;
  for (auto& x : args[0].asList()) items.push_back(x.asSignal());
  *result = Value::signal(makeMix(std::move(items)));
  return true;
}
SYNTH_EXTERNAL(channels) {
  std::vector<SigPtr> items;
  for (auto& x : args[0].asList()) items.push_back(x.asSignal());
  *result = Value::signal(makeChannels(std::move(items)));
  return true;
}
SYNTH_EXTERNAL(sample) {
  Sample s;
  s.signal = args[0].asSignal();
  s.from = args[1].asTime();
  s.to = args[2].asTime();
  if (s.from < 0 || s.to < s.from)
    throw std::runtime_error(
        "sample: invalid window (from=" + std::to_string(s.from) +
        "s, to=" + std::to_string(s.to) + "s)");
  *result = Value::sample(std::move(s));
  return true;
}
SYNTH_EXTERNAL(place) {
  const Sample& s = args[0].asSample();
  *result = Value::signal(makePlace(s.signal, s.from, s.to,
                                    args[1].asTime()));
  return true;
}
SYNTH_EXTERNAL(place_multi) {
  const Sample& s = args[0].asSample();
  std::vector<SigPtr> placed;
  for (auto& t : args[1].asList())
    placed.push_back(makePlace(s.signal, s.from, s.to, t.asTime()));
  *result = Value::signal(makeMix(std::move(placed)));
  return true;
}
