#include "util.hpp"

// Generators and modulation: the oscillator family of Core.

using synth::ext::Value;
using namespace synth;

SYNTH_EXTERNAL(sine) {
  *result = Value::signal(makeOsc(OscKind::Sine, args[0].asScalar()));
  return true;
}
SYNTH_EXTERNAL(saw) {
  *result = Value::signal(makeOsc(OscKind::Saw, args[0].asScalar()));
  return true;
}
SYNTH_EXTERNAL(square) {
  *result = Value::signal(makeOsc(OscKind::Square, args[0].asScalar()));
  return true;
}
SYNTH_EXTERNAL(noise) {
  *result = Value::signal(makeNoise(args[0].asScalar()));
  return true;
}
SYNTH_EXTERNAL(fm) {
  *result = Value::signal(makeFm(args[0].asScalar(), args[1].asSignal()));
  return true;
}
SYNTH_EXTERNAL(pm) {
  *result = Value::signal(makePm(args[0].asScalar(), args[1].asSignal()));
  return true;
}
SYNTH_EXTERNAL(am) {
  *result = Value::signal(
      makeAm(args[0].asSignal(), args[1].asSignal(), args[2].asScalar()));
  return true;
}
