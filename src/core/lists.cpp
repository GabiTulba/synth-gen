#include <cstring>

#include "util.hpp"

// The Core.List combinators and builders, plus the Timestamp sequence
// helpers that drive rhythm patterns.
namespace synth::native {

Value mapImpl(Ctx& ctx, std::vector<Value>& args) {
  ListV out;
  const Value& f = args[0];
  for (auto& x : std::get<ListV>(args[1].v).items)
    out.items.push_back(ctx.apply(f, {x}));
  return Value{std::move(out)};
}

Value foldImpl(Ctx& ctx, std::vector<Value>& args) {
  const Value& f = args[0];
  Value acc = args[1];
  for (auto& x : std::get<ListV>(args[2].v).items)
    acc = ctx.apply(f, {acc, x});
  return acc;
}

Value listInitImpl(Ctx& ctx, std::vector<Value>& args) {
  int64_t n = countArg(args[0], "List.init");
  ListV out;
  for (int64_t i = 0; i < n; i++)
    out.items.push_back(ctx.apply(args[1], {Value{IntV{i}}}));
  return Value{std::move(out)};
}

Value repeatImpl(Ctx&, std::vector<Value>& args) {
  int64_t n = countArg(args[0], "List.repeat");
  ListV out;
  for (int64_t i = 0; i < n; i++) out.items.push_back(args[1]);
  return Value{std::move(out)};
}

Value timeStepsImpl(Ctx&, std::vector<Value>& args) {
  double start = timeArg(args[0]);
  double step = timeArg(args[1]);
  int64_t n = countArg(args[2], "time_steps");
  ListV out;
  for (int64_t i = 0; i < n; i++)
    out.items.push_back(Value{TimeV{start + step * (double)i}});
  return Value{std::move(out)};
}

Value jitterImpl(Ctx&, std::vector<Value>& args) {
  double seed = scalarArg(args[0]);
  double spread = timeArg(args[1]);
  if (spread < 0) throw EvalError("jitter: negative spread");
  ListV out;
  int64_t i = 0;
  for (auto& x : std::get<ListV>(args[2].v).items) {
    double t = std::get<TimeV>(x.v).seconds;
    // splitmix64 over (seed bits, index): integer-only, so the
    // deltas are bit-identical on every platform.
    uint64_t h;
    std::memcpy(&h, &seed, sizeof h);
    h ^= (uint64_t)i * 0x9E3779B97F4A7C15ull;
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27; h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    double unit = (double)(h >> 11) / 9007199254740992.0;  // [0,1)
    double t2 = t + (unit * 2.0 - 1.0) * spread;
    out.items.push_back(Value{TimeV{std::max(0.0, t2)}});
    i++;
  }
  return Value{std::move(out)};
}

}  // namespace synth::native
