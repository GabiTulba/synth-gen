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
  throw EvalError("pow: expected a Scalar, Vector, or Signal");
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
