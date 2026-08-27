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

// A sum-constrained group of controls: several named lanes, each with
// its own range and default, whose values additionally sum into
// [sum_min, sum_max]. Lanes arrive as (name, min, max, default) tuples -
// the record form the language exposes cannot cross this boundary, so
// Core.Control.multi_slider flattens it on the way in. The host
// validates the group, applies each lane's override and puts the result
// back inside the sum bounds.
SYNTH_EXTERNAL(multi_slider_lanes) {
  synth::ext::ControlGroupDecl g;
  g.name = args[0].asString();
  g.sumMin = args[1].asScalar();
  g.sumMax = args[2].asScalar();
  for (const Value& lane : args[3].asList()) {
    const std::vector<Value>& f = lane.asTuple();
    if (f.size() != 4)
      throw std::runtime_error("multi_slider: malformed lane");
    g.lanes.push_back(synth::ext::ControlGroupDecl::Lane{
        f[0].asString(), f[1].asScalar(), f[2].asScalar(), f[3].asScalar()});
  }
  std::vector<Value> values;
  for (double v : ctx.controlGroup(std::move(g)))
    values.push_back(Value::scalar(v));
  *result = Value::list(std::move(values));
  return true;
}
