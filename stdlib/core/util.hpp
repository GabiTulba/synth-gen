#pragma once
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <synth/external.hpp>

// Shared argument helpers for the Core implementations. Types were
// verified by the checker; the throws here guard the paths the type
// system cannot see (symbolic substitution under `signal ~f`, and count
// ranges).
namespace synth::core {

// Counts are Ints, so wholeness is the type system's problem now; what
// remains here is the range the engine is willing to iterate.
inline std::int64_t countArg(const ext::Value& v, const char* prim) {
  std::int64_t n = v.asInt();
  if (n < 0)
    throw std::runtime_error(std::string(prim) +
                             ": count must be non-negative (got " +
                             std::to_string(n) + ")");
  if (n > 1000000)
    throw std::runtime_error(std::string(prim) + ": count " +
                             std::to_string(n) + " is unreasonably large");
  return n;
}

// `signal ~f`: apply the Scalar -> Scalar function symbolically to the
// time ramp. Arithmetic and the math externals lift pointwise, so the
// whole body becomes one signal graph; a constant-valued function
// degenerates to a constant signal via toSignal.
inline SigPtr fnOfTime(ext::Ctx& ctx, const ext::Value& f,
                       const char* prim) {
  ext::Value r = ctx.apply(f, {ext::Value::signal(makeTime())});
  if (r.kind == ext::Value::Kind::Signal ||
      r.kind == ext::Value::Kind::Scalar)
    return r.toSignal();
  throw std::runtime_error(std::string(prim) +
                           ": the function must produce a Scalar from the "
                           "time argument");
}

}  // namespace synth::core
