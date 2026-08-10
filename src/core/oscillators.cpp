#include "util.hpp"

// Generators and modulation: the oscillator family of Core.
namespace synth::native {

Value sineImpl(Ctx&, std::vector<Value>& args) {
  return Value{makeOsc(OscKind::Sine, scalarArg(args[0]))};
}
Value sawImpl(Ctx&, std::vector<Value>& args) {
  return Value{makeOsc(OscKind::Saw, scalarArg(args[0]))};
}
Value squareImpl(Ctx&, std::vector<Value>& args) {
  return Value{makeOsc(OscKind::Square, scalarArg(args[0]))};
}
Value noiseImpl(Ctx&, std::vector<Value>& args) {
  return Value{makeNoise(scalarArg(args[0]))};
}
Value fmImpl(Ctx&, std::vector<Value>& args) {
  return Value{makeFm(scalarArg(args[0]), signalArg(args[1]))};
}
Value pmImpl(Ctx&, std::vector<Value>& args) {
  return Value{makePm(scalarArg(args[0]), signalArg(args[1]))};
}
Value amImpl(Ctx&, std::vector<Value>& args) {
  return Value{
      makeAm(signalArg(args[0]), signalArg(args[1]), scalarArg(args[2]))};
}

}  // namespace synth::native
