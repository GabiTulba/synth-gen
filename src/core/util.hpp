#pragma once
#include <cmath>
#include <string>

#include "native.hpp"

// Shared argument accessors and conversions for the built-in external
// implementations. Types were verified by the checker; the throws here
// guard the paths the type system cannot see (symbolic substitution
// under `signal ~f`, and count ranges).
namespace synth::native {

inline double scalarArg(const Value& v) { return std::get<ScalarV>(v.v).v; }
inline int64_t intArg(const Value& v) { return std::get<IntV>(v.v).v; }
inline double timeArg(const Value& v) { return std::get<TimeV>(v.v).seconds; }
inline const std::string& strArg(const Value& v) {
  return std::get<StringV>(v.v).s;
}
inline SigPtr signalArg(const Value& v) { return std::get<SigPtr>(v.v); }

// Counts are Ints, so wholeness is the type system's problem now; what
// remains here is the range the engine is willing to iterate.
inline int64_t countArg(const Value& v, const char* prim) {
  int64_t n = intArg(v);
  if (n < 0)
    throw EvalError(std::string(prim) + ": count must be non-negative (got " +
                    std::to_string(n) + ")");
  if (n > 1000000)
    throw EvalError(std::string(prim) + ": count " + std::to_string(n) +
                    " is unreasonably large");
  return n;
}

inline SigPtr asSignal(const Value& v) {
  if (auto* s = std::get_if<SigPtr>(&v.v)) return *s;
  if (auto* sc = std::get_if<ScalarV>(&v.v)) return makeConst(sc->v);
  throw EvalError("internal error: operand is not a signal");
}

// `signal ~f`: apply the Scalar -> Scalar function symbolically to the
// time ramp. Arithmetic and the math externals lift pointwise, so the
// whole body becomes one signal graph; a constant-valued function
// degenerates to a constant signal via asSignal.
inline SigPtr fnOfTime(Ctx& ctx, const Value& f, const char* prim) {
  Value r = ctx.apply(f, {Value{makeTime()}});
  if (std::holds_alternative<SigPtr>(r.v) ||
      std::holds_alternative<ScalarV>(r.v))
    return asSignal(r);
  throw EvalError(std::string(prim) +
                  ": the function must produce a Scalar from the time "
                  "argument");
}

}  // namespace synth::native
