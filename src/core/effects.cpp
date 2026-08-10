#include "util.hpp"

// Time-domain effects, filters, distortion and envelopes.
namespace synth::native {

Value delayImpl(Ctx&, std::vector<Value>& args) {
  return Value{makeDelay(timeArg(args[0]), signalArg(args[1]))};
}
Value resampleImpl(Ctx& ctx, std::vector<Value>& args) {
  return Value{
      makeResample(signalArg(args[0]), fnOfTime(ctx, args[1], "resample"))};
}
Value reverbImpl(Ctx&, std::vector<Value>& args) {
  return Value{makeReverb(timeArg(args[0]), scalarArg(args[1]),
                          scalarArg(args[2]), signalArg(args[3]))};
}
Value expDecayImpl(Ctx&, std::vector<Value>& args) {
  return Value{makeExpDecay(scalarArg(args[0]))};
}
Value adsrImpl(Ctx&, std::vector<Value>& args) {
  return Value{makeAdsr(timeArg(args[0]), timeArg(args[1]),
                        scalarArg(args[2]), timeArg(args[3]),
                        timeArg(args[4]))};
}
Value lowpassImpl(Ctx&, std::vector<Value>& args) {
  return Value{
      makeFilter(FilterKind::Lowpass, scalarArg(args[0]), signalArg(args[1]))};
}
Value highpassImpl(Ctx&, std::vector<Value>& args) {
  return Value{
      makeFilter(FilterKind::Highpass, scalarArg(args[0]), signalArg(args[1]))};
}
Value hardClipImpl(Ctx&, std::vector<Value>& args) {
  return Value{
      makeClip(ClipKind::Hard, scalarArg(args[0]), signalArg(args[1]))};
}
Value softClipImpl(Ctx&, std::vector<Value>& args) {
  return Value{
      makeClip(ClipKind::Soft, scalarArg(args[0]), signalArg(args[1]))};
}

}  // namespace synth::native
