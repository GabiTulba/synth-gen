#pragma once
#include <cmath>
#include <string>

#include "native.hpp"

// Shared argument accessors and conversions for the built-in external
// implementations. Types were verified by the checker; the throws here
// guard the paths the type system cannot see (symbolic substitution
// under `signal ~f`, and counts that must be whole).
namespace synth::native {

inline double scalarArg(const Value& v) { return std::get<ScalarV>(v.v).v; }
inline double timeArg(const Value& v) { return std::get<TimeV>(v.v).seconds; }
inline const std::string& strArg(const Value& v) {
  return std::get<StringV>(v.v).s;
}
inline SigPtr signalArg(const Value& v) { return std::get<SigPtr>(v.v); }

// Counts are Scalars in the language; here they must be whole,
// non-negative, and sane (build-time validation).
inline int64_t wholeCount(double v, const char* prim) {
  double rounded = std::round(v);
  if (std::fabs(v - rounded) > 1e-9 || rounded < 0)
    throw EvalError(std::string(prim) +
                    ": count must be a whole non-negative number (got " +
                    std::to_string(v) + ")");
  if (rounded > 1e6)
    throw EvalError(std::string(prim) + ": count " + std::to_string(v) +
                    " is unreasonably large");
  return (int64_t)rounded;
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
