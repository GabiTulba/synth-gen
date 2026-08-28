#include "util.hpp"

#include <cmath>
#include <cstdio>

// Live controls: named build-time parameters - a Scalar on a slider or a
// knob, an Int on a whole-step slider, a Bool in a tickbox, or one
// option picked out of a list. Each entry point here resolves the value;
// the `Core.Control` function of the same name without `_value` wraps it
// with the `Controller` a panel shows it with. Each call declares the control and
// evaluates to its current value: the override an attached dev tool
// wrote into the unit's controls.json (clamped to the range, snapped to
// a whole number where the kind is discrete), or the default. Pure from
// the language's point of view: the value is fixed for one whole build;
// the host validates ranges and rejects conflicting redeclarations.

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

SYNTH_EXTERNAL(slider_value) {
  *result = declareControl(ctx, args, ControlDecl::Kind::Slider);
  return true;
}

SYNTH_EXTERNAL(knob_value) {
  *result = declareControl(ctx, args, ControlDecl::Kind::Knob);
  return true;
}

// A slider quantized to whole steps: bounds and value are Ints on both
// sides of the boundary, and the host snaps an override to a whole
// number the same way it clamps one to the range.
SYNTH_EXTERNAL(int_slider_value) {
  ControlDecl c;
  c.kind = ControlDecl::Kind::IntSlider;
  c.name = args[0].asString();
  c.min = (double)args[1].asInt();
  c.max = (double)args[2].asInt();
  c.def = (double)args[3].asInt();
  *result = Value::integer(std::llround(ctx.control(std::move(c))));
  return true;
}

// A tickbox: one Bool, carried as the 0-or-1 every control value is.
SYNTH_EXTERNAL(toggle_value) {
  ControlDecl c;
  c.kind = ControlDecl::Kind::Toggle;
  c.name = args[0].asString();
  c.min = 0;
  c.max = 1;
  c.def = args[1].asBool() ? 1.0 : 0.0;
  *result = Value::boolean(ctx.control(std::move(c)) >= 0.5);
  return true;
}

// What the dev app writes beside one option's tickbox. An option reads
// as its own value whenever a value has a reading - a String, a number,
// a Timestamp, a Bool, or the constructor name an opaque variant lends
// as its tag (Linear, Exponential) - and falls back to its position in
// the list when it has none (a signal, a record, a function).
static std::string optionLabel(const Value& v, size_t i) {
  char buf[32];
  switch (v.kind) {
    case Value::Kind::String:
      if (!v.str.empty()) return v.str;
      break;
    case Value::Kind::Bool: return v.num != 0.0 ? "true" : "false";
    case Value::Kind::Int: return std::to_string(v.inum);
    case Value::Kind::Scalar:
      std::snprintf(buf, sizeof buf, "%.6g", v.num);
      return buf;
    case Value::Kind::Time:
      std::snprintf(buf, sizeof buf, "%.6gs", v.num);
      return buf;
    case Value::Kind::Opaque:
      if (!v.str.empty()) return v.str;
      break;
    default: break;
  }
  return std::to_string(i + 1);
}

// One choice out of a list, drawn as a tickbox per option. Only the
// index crosses the boundary - the host has no reading of the option
// values and does not need one - so the control is an ordinary numeric
// control over [0, n-1] and the implementation indexes its own list
// with the result, handing the chosen value back untouched (whatever
// its type: the list arrived transparent or opaque, and leaves the same
// way). The first option is the default; an empty list has nothing to
// choose and is a build error.
SYNTH_EXTERNAL(choice_value) {
  const std::vector<Value>& options = args[1].asList();
  ControlDecl c;
  c.kind = ControlDecl::Kind::Choice;
  c.name = args[0].asString();
  if (options.empty())
    throw std::runtime_error("choice '" + c.name +
                             "': the option list is empty");
  for (size_t i = 0; i < options.size(); i++)
    c.options.push_back(optionLabel(options[i], i));
  c.min = 0;
  c.max = (double)(options.size() - 1);
  c.def = 0;
  long long picked = std::llround(ctx.control(std::move(c)));
  if (picked < 0) picked = 0;
  if (picked >= (long long)options.size())
    picked = (long long)options.size() - 1;
  *result = options[(size_t)picked];
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
