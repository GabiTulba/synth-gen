#include "util.hpp"

// Time-domain effects, filters, distortion and envelopes.

using synth::ext::Value;
using namespace synth;

SYNTH_EXTERNAL(delay) {
  *result = Value::signal(makeDelay(args[0].asTime(), args[1].asSignal()));
  return true;
}
SYNTH_EXTERNAL(resample) {
  *result = Value::signal(makeResample(
      args[0].asSignal(), core::fnOfTime(ctx, args[1], "resample")));
  return true;
}
SYNTH_EXTERNAL(reverb) {
  *result = Value::signal(makeReverb(args[0].asTime(), args[1].asScalar(),
                                     args[2].asScalar(),
                                     args[3].asSignal()));
  return true;
}
SYNTH_EXTERNAL(exp_decay) {
  *result = Value::signal(makeExpDecay(args[0].asScalar()));
  return true;
}
SYNTH_EXTERNAL(adsr) {
  *result = Value::signal(makeAdsr(args[0].asTime(), args[1].asTime(),
                                   args[2].asScalar(), args[3].asTime(),
                                   args[4].asTime()));
  return true;
}
SYNTH_EXTERNAL(lowpass) {
  *result = Value::signal(
      makeFilter(FilterKind::Lowpass, args[0].asScalar(), args[1].asSignal()));
  return true;
}
SYNTH_EXTERNAL(highpass) {
  *result = Value::signal(makeFilter(FilterKind::Highpass, args[0].asScalar(),
                                     args[1].asSignal()));
  return true;
}
SYNTH_EXTERNAL(lowpass_mod) {
  *result = Value::signal(makeModFilter(FilterKind::Lowpass,
                                        args[0].toSignal(),
                                        args[1].asSignal()));
  return true;
}
SYNTH_EXTERNAL(highpass_mod) {
  *result = Value::signal(makeModFilter(FilterKind::Highpass,
                                        args[0].toSignal(),
                                        args[1].asSignal()));
  return true;
}
SYNTH_EXTERNAL(resonant) {
  *result = Value::signal(makeResonant(args[0].toSignal(),
                                       args[1].asScalar(),
                                       args[2].asSignal()));
  return true;
}
SYNTH_EXTERNAL(follow) {
  *result = Value::signal(
      makeFollow(args[0].asTime(), args[1].asTime(), args[2].asSignal()));
  return true;
}
SYNTH_EXTERNAL(feedback) {
  *result = Value::signal(makeFeedbackDelay(
      args[0].asTime(), args[1].asScalar(), args[2].asSignal()));
  return true;
}
SYNTH_EXTERNAL(hard_clip) {
  *result = Value::signal(
      makeClip(ClipKind::Hard, args[0].asScalar(), args[1].asSignal()));
  return true;
}
SYNTH_EXTERNAL(soft_clip) {
  *result = Value::signal(
      makeClip(ClipKind::Soft, args[0].asScalar(), args[1].asSignal()));
  return true;
}
