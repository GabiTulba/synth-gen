#include <algorithm>
#include <cstring>

#include "util.hpp"

// The Timestamp sequence helpers that drive rhythm patterns. (The
// Core.List combinators are written in SynthGraph itself, in
// lib.synth - lists are ordinary Cons/Nil variants there; they cross
// this boundary as plain vectors.)

using synth::core::countArg;
using synth::ext::Value;

SYNTH_EXTERNAL(time_steps) {
  double start = args[0].asTime();
  double step = args[1].asTime();
  std::int64_t n = countArg(args[2], "time_steps");
  std::vector<Value> out;
  for (std::int64_t i = 0; i < n; i++)
    out.push_back(Value::time(start + step * (double)i));
  *result = Value::list(std::move(out));
  return true;
}

SYNTH_EXTERNAL(jitter) {
  double seed = args[0].asScalar();
  double spread = args[1].asTime();
  if (spread < 0) throw std::runtime_error("jitter: negative spread");
  std::vector<Value> out;
  std::int64_t i = 0;
  for (auto& x : args[2].asList()) {
    double t = x.asTime();
    // Math.hash applied in the time domain: same seed, same index, same
    // bits (see util.hpp's hashUnit).
    double unit = synth::core::hashUnit(seed, i);
    double t2 = t + (unit * 2.0 - 1.0) * spread;
    out.push_back(Value::time(std::max(0.0, t2)));
    i++;
  }
  *result = Value::list(std::move(out));
  return true;
}

// Iterate an effectful function over a list. This lives in C++ rather
// than lib.synth because it is the one List function that cannot be
// written in the language: `unit` has no literal, so a synth-side
// iterator would have nothing to return in its Nil arm. C++ can mint
// the unit value; that is the whole implementation.
SYNTH_EXTERNAL(iter) {
  for (auto& x : args[1].asList()) ctx.apply(args[0], {x});
  *result = Value::unit();
  return true;
}
