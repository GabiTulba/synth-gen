#include "util.hpp"

// Live controls: named build-time Scalar parameters with a range and a
// default. Each call declares the control and evaluates to its current
// value - the override an attached dev tool wrote into the unit's
// controls.json (clamped to the range), or the default. Pure from the
// language's point of view: the value is fixed for one whole build; the
// host validates ranges and rejects conflicting redeclarations.

using synth::ext::ControlDecl;
using synth::ext::Value;

static Value declareControl(synth::ext::Ctx& ctx, const Value* args,
                            ControlDecl::Kind kind) {
  ControlDecl c;
  c.kind = kind;
  c.name = args[0].asString();
  c.min = args[1].asScalar();
  c.max = args[2].asScalar();
  c.def = args[3].asScalar();
  return Value::scalar(ctx.control(std::move(c)));
}

SYNTH_EXTERNAL(slider) {
  *result = declareControl(ctx, args, ControlDecl::Kind::Slider);
  return true;
}

SYNTH_EXTERNAL(knob) {
  *result = declareControl(ctx, args, ControlDecl::Kind::Knob);
  return true;
}
