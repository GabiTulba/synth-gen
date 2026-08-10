#include "util.hpp"

// Mixing, channel assembly, and the sample/place arrangement family.
namespace synth::native {

Value mixAllImpl(Ctx&, std::vector<Value>& args) {
  std::vector<SigPtr> items;
  for (auto& x : std::get<ListV>(args[0].v).items)
    items.push_back(signalArg(x));
  return Value{makeMix(std::move(items))};
}
Value channelsImpl(Ctx&, std::vector<Value>& args) {
  std::vector<SigPtr> items;
  for (auto& x : std::get<ListV>(args[0].v).items)
    items.push_back(signalArg(x));
  return Value{makeChannels(std::move(items))};
}
Value sampleImpl(Ctx&, std::vector<Value>& args) {
  SampleV s;
  s.sig = signalArg(args[0]);
  s.from = timeArg(args[1]);
  s.to = timeArg(args[2]);
  if (s.from < 0 || s.to < s.from)
    throw EvalError("sample: invalid window (from=" + std::to_string(s.from) +
                    "s, to=" + std::to_string(s.to) + "s)");
  return Value{std::move(s)};
}
Value placeImpl(Ctx&, std::vector<Value>& args) {
  const SampleV& s = std::get<SampleV>(args[0].v);
  double at = timeArg(args[1]);
  return Value{makePlace(s.sig, s.from, s.to, at)};
}
Value placeMultiImpl(Ctx&, std::vector<Value>& args) {
  const SampleV& s = std::get<SampleV>(args[0].v);
  std::vector<SigPtr> placed;
  for (auto& t : std::get<ListV>(args[1].v).items)
    placed.push_back(makePlace(s.sig, s.from, s.to, timeArg(t)));
  return Value{makeMix(std::move(placed))};
}

}  // namespace synth::native
