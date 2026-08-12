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
    // splitmix64 over (seed bits, index): integer-only, so the
    // deltas are bit-identical on every platform.
    std::uint64_t h;
    std::memcpy(&h, &seed, sizeof h);
    h ^= (std::uint64_t)i * 0x9E3779B97F4A7C15ull;
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27; h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    double unit = (double)(h >> 11) / 9007199254740992.0;  // [0,1)
    double t2 = t + (unit * 2.0 - 1.0) * spread;
    out.push_back(Value::time(std::max(0.0, t2)));
    i++;
  }
  *result = Value::list(std::move(out));
  return true;
}
