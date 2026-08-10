#include "util.hpp"

// Math over the numeric kinds ('a in the signatures is checked here:
// anything non-numeric is a build-time error), Scalar -> Timestamp
// conversions, and Boolean negation.
namespace synth::native {

namespace {
// Elementwise math over Scalars, Vectors, and Signals.
Value math1(const Value& v, SigUnaryOp op, const char* prim) {
  auto f = [op](double x) {
    return op == SigUnaryOp::Exp    ? std::exp(x)
           : op == SigUnaryOp::Sqrt ? std::sqrt(x)
                                    : std::log(x);
  };
  if (auto* sc = std::get_if<ScalarV>(&v.v)) return Value{ScalarV{f(sc->v)}};
  if (auto* vec = std::get_if<VectorV>(&v.v)) {
    VectorV out = *vec;
    for (auto& c : out.v) c = f(c);
    return Value{std::move(out)};
  }
  if (auto* s = std::get_if<SigPtr>(&v.v)) return Value{makeUnaryOp(op, *s)};
  if (std::holds_alternative<IntV>(v.v))
    throw EvalError(std::string(prim) +
                    ": expected a Scalar, Vector, or Signal (got an Int; "
                    "convert with to_scalar)");
  throw EvalError(std::string(prim) +
                  ": expected a Scalar, Vector, or Signal");
}
}  // namespace

Value expImpl(Ctx&, std::vector<Value>& args) {
  return math1(args[0], SigUnaryOp::Exp, "exp");
}
Value sqrtImpl(Ctx&, std::vector<Value>& args) {
  return math1(args[0], SigUnaryOp::Sqrt, "sqrt");
}
Value logImpl(Ctx&, std::vector<Value>& args) {
  return math1(args[0], SigUnaryOp::Log, "log");
}

Value powImpl(Ctx&, std::vector<Value>& args) {
  // y is Scalar-typed, but under `signal ~f` symbolic substitution
  // the time signal can flow into either side.
  const Value& x = args[0];
  const Value& y = args[1];
  bool xSig = std::holds_alternative<SigPtr>(x.v);
  bool ySig = std::holds_alternative<SigPtr>(y.v);
  if (xSig || ySig)
    return Value{makeBinOp(SigBinOp::Pow, asSignal(x), asSignal(y))};
  if (auto* vec = std::get_if<VectorV>(&x.v)) {
    VectorV out = *vec;
    for (auto& c : out.v) c = std::pow(c, scalarArg(y));
    return Value{std::move(out)};
  }
  if (std::holds_alternative<ScalarV>(x.v))
    return Value{ScalarV{std::pow(scalarArg(x), scalarArg(y))}};
  if (std::holds_alternative<IntV>(x.v))
    throw EvalError("pow: expected a Scalar, Vector, or Signal (got an "
                    "Int; convert with to_scalar)");
  throw EvalError("pow: expected a Scalar, Vector, or Signal");
}

Value toScalarImpl(Ctx&, std::vector<Value>& args) {
  return Value{ScalarV{(double)intArg(args[0])}};
}

namespace {
// Scalar -> Int with an explicit fraction policy; NaNs and values
// outside the Int range have no honest answer and are build errors.
Value scalarToInt(double x, double rounded, const char* prim) {
  if (!(rounded >= -9.2e18 && rounded <= 9.2e18))
    throw EvalError(std::string(prim) + ": " + std::to_string(x) +
                    " has no Int value");
  return Value{IntV{(int64_t)rounded}};
}
}  // namespace

Value roundImpl(Ctx&, std::vector<Value>& args) {
  double x = scalarArg(args[0]);
  return scalarToInt(x, std::round(x), "round");
}
Value floorImpl(Ctx&, std::vector<Value>& args) {
  double x = scalarArg(args[0]);
  return scalarToInt(x, std::floor(x), "floor");
}
Value ceilImpl(Ctx&, std::vector<Value>& args) {
  double x = scalarArg(args[0]);
  return scalarToInt(x, std::ceil(x), "ceil");
}

Value toSecImpl(Ctx&, std::vector<Value>& args) {
  return Value{TimeV{scalarArg(args[0])}};
}
Value toMsImpl(Ctx&, std::vector<Value>& args) {
  return Value{TimeV{scalarArg(args[0]) * 1e-3}};
}
Value toMinImpl(Ctx&, std::vector<Value>& args) {
  return Value{TimeV{scalarArg(args[0]) * 60.0}};
}

Value notImpl(Ctx&, std::vector<Value>& args) {
  const BoolV* b = std::get_if<BoolV>(&args[0].v);
  if (!b) throw EvalError("not: expected a Bool");
  return Value{BoolV{!b->v}};
}

}  // namespace synth::native
