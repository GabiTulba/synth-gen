#include "util.hpp"

// Math over the numeric kinds ('a in the signatures is checked here:
// anything non-numeric is a build-time error), Scalar -> Timestamp
// conversions, and Boolean negation.

using synth::ext::Value;
using namespace synth;

namespace {

// Elementwise math over Scalars, Vectors, and Signals.
Value math1(const Value& v, SigUnaryOp op, const char* prim) {
  auto f = [op](double x) {
    return op == SigUnaryOp::Exp    ? std::exp(x)
           : op == SigUnaryOp::Sqrt ? std::sqrt(x)
                                    : std::log(x);
  };
  switch (v.kind) {
    case Value::Kind::Scalar: return Value::scalar(f(v.num));
    case Value::Kind::Vector: {
      Value out = v;
      for (auto& c : out.items) c.num = f(c.num);
      return out;
    }
    case Value::Kind::Signal: return Value::signal(makeUnaryOp(op, v.sig));
    case Value::Kind::Int:
      throw std::runtime_error(std::string(prim) +
                               ": expected a Scalar, Vector, or Signal "
                               "(got an Int; convert with to_scalar)");
    default:
      throw std::runtime_error(std::string(prim) +
                               ": expected a Scalar, Vector, or Signal");
  }
}

// Scalar -> Int with an explicit fraction policy; NaNs and values
// outside the Int range have no honest answer and are build errors.
Value scalarToInt(double x, double rounded, const char* prim) {
  if (!(rounded >= -9.2e18 && rounded <= 9.2e18))
    throw std::runtime_error(std::string(prim) + ": " + std::to_string(x) +
                             " has no Int value");
  return Value::integer((std::int64_t)rounded);
}

}  // namespace

SYNTH_EXTERNAL(exp) {
  *result = math1(args[0], SigUnaryOp::Exp, "exp");
  return true;
}
SYNTH_EXTERNAL(sqrt) {
  *result = math1(args[0], SigUnaryOp::Sqrt, "sqrt");
  return true;
}
SYNTH_EXTERNAL(log) {
  *result = math1(args[0], SigUnaryOp::Log, "log");
  return true;
}

SYNTH_EXTERNAL(pow) {
  // y is Scalar-typed, but under `signal ~f` symbolic substitution
  // the time signal can flow into either side.
  const Value& x = args[0];
  const Value& y = args[1];
  bool xSig = x.kind == Value::Kind::Signal;
  bool ySig = y.kind == Value::Kind::Signal;
  if (xSig || ySig) {
    *result = Value::signal(
        makeBinOp(SigBinOp::Pow, x.toSignal(), y.toSignal()));
    return true;
  }
  if (x.kind == Value::Kind::Vector) {
    Value out = x;
    for (auto& c : out.items) c.num = std::pow(c.num, y.asScalar());
    *result = std::move(out);
    return true;
  }
  if (x.kind == Value::Kind::Scalar) {
    *result = Value::scalar(std::pow(x.num, y.asScalar()));
    return true;
  }
  if (x.kind == Value::Kind::Int)
    throw std::runtime_error("pow: expected a Scalar, Vector, or Signal "
                             "(got an Int; convert with to_scalar)");
  throw std::runtime_error("pow: expected a Scalar, Vector, or Signal");
}

SYNTH_EXTERNAL(to_scalar) {
  *result = Value::scalar((double)args[0].asInt());
  return true;
}

SYNTH_EXTERNAL(round) {
  double x = args[0].asScalar();
  *result = scalarToInt(x, std::round(x), "round");
  return true;
}
SYNTH_EXTERNAL(floor) {
  double x = args[0].asScalar();
  *result = scalarToInt(x, std::floor(x), "floor");
  return true;
}
SYNTH_EXTERNAL(ceil) {
  double x = args[0].asScalar();
  *result = scalarToInt(x, std::ceil(x), "ceil");
  return true;
}

SYNTH_EXTERNAL(to_sec) {
  *result = Value::time(args[0].asScalar());
  return true;
}
SYNTH_EXTERNAL(to_ms) {
  *result = Value::time(args[0].asScalar() * 1e-3);
  return true;
}
SYNTH_EXTERNAL(to_min) {
  *result = Value::time(args[0].asScalar() * 60.0);
  return true;
}

SYNTH_EXTERNAL(not) {
  *result = Value::boolean(!args[0].asBool());
  return true;
}
