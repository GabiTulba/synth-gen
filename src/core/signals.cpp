#include "util.hpp"

// Direct signal constructors: constants, the time ramp, and sampled
// functions of time.
namespace synth::native {

Value constantImpl(Ctx&, std::vector<Value>& args) {
  return Value{makeConst(scalarArg(args[0]))};
}

Value constantMultiImpl(Ctx&, std::vector<Value>& args) {
  std::vector<SigPtr> chans;
  for (auto& x : std::get<ListV>(args[0].v).items)
    chans.push_back(makeConst(scalarArg(x)));
  if (chans.empty())
    throw EvalError("constant_multi: needs at least one level");
  return Value{makeChannels(std::move(chans))};
}

Value timeImpl(Ctx&, std::vector<Value>&) { return Value{makeTime()}; }

Value signalFnImpl(Ctx& ctx, std::vector<Value>& args) {
  return Value{fnOfTime(ctx, args[0], "signal")};
}

Value signalFnMultiImpl(Ctx& ctx, std::vector<Value>& args) {
  std::vector<SigPtr> chans;
  for (auto& f : std::get<ListV>(args[0].v).items)
    chans.push_back(fnOfTime(ctx, f, "signal_multi"));
  if (chans.empty())
    throw EvalError("signal_multi: needs at least one function");
  return Value{makeChannels(std::move(chans))};
}

}  // namespace synth::native
