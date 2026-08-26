#include "util.hpp"

// Direct signal constructors: constants, the time ramp, and sampled
// functions of time.

using synth::core::fnOfTime;
using synth::ext::Value;
using namespace synth;

SYNTH_EXTERNAL(constant) {
  *result = Value::signal(makeConst(args[0].asScalar()));
  return true;
}

SYNTH_EXTERNAL(constant_multi) {
  std::vector<SigPtr> chans;
  for (auto& x : args[0].asList()) chans.push_back(makeConst(x.asScalar()));
  if (chans.empty())
    throw std::runtime_error("constant_multi: needs at least one level");
  *result = Value::signal(makeChannels(std::move(chans)));
  return true;
}

SYNTH_EXTERNAL(time) {
  *result = Value::signal(makeTime());
  return true;
}

SYNTH_EXTERNAL(signal) {
  *result = Value::signal(fnOfTime(ctx, args[0], "signal"));
  return true;
}

SYNTH_EXTERNAL(select) {
  *result = Value::signal(makeSelect(args[0].toSignal(), args[1].asScalar(),
                                     args[2].asSignal(),
                                     args[3].asSignal()));
  return true;
}

SYNTH_EXTERNAL(signal_multi) {
  std::vector<SigPtr> chans;
  for (auto& f : args[0].asList())
    chans.push_back(fnOfTime(ctx, f, "signal_multi"));
  if (chans.empty())
    throw std::runtime_error("signal_multi: needs at least one function");
  *result = Value::signal(makeChannels(std::move(chans)));
  return true;
}
