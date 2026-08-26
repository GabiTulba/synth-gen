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
    switch (op) {
      case SigUnaryOp::Exp: return std::exp(x);
      case SigUnaryOp::Sqrt: return std::sqrt(x);
      case SigUnaryOp::Sin: return std::sin(x);
      case SigUnaryOp::Cos: return std::cos(x);
      case SigUnaryOp::Tan: return std::tan(x);
      case SigUnaryOp::Atan: return std::atan(x);
      case SigUnaryOp::Abs: return std::fabs(x);
      default: return std::log(x);
    }
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
SYNTH_EXTERNAL(sin) {
  *result = math1(args[0], SigUnaryOp::Sin, "sin");
  return true;
}
SYNTH_EXTERNAL(cos) {
  *result = math1(args[0], SigUnaryOp::Cos, "cos");
  return true;
}
SYNTH_EXTERNAL(tan) {
  *result = math1(args[0], SigUnaryOp::Tan, "tan");
  return true;
}
SYNTH_EXTERNAL(atan) {
  *result = math1(args[0], SigUnaryOp::Atan, "atan");
  return true;
}
SYNTH_EXTERNAL(abs) {
  *result = math1(args[0], SigUnaryOp::Abs, "abs");
  return true;
}

// Value-level randomness: jitter's splitmix64 exposed as a value - a
// pure, platform-stable function of (seed, index) in [0, 1). Purity by
// construction: same arguments, same bits, so renders stay cacheable.
SYNTH_EXTERNAL(hash) {
  *result =
      Value::scalar(synth::core::hashUnit(args[0].asScalar(), args[1].asInt()));
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
  // Under `signal ~f` a condition is carried as a 0/1 signal rather than
  // one Bool (there is no single value to negate), so negation there is
  // the complement 1 - b.
  if (args[0].kind == Value::Kind::Signal) {
    *result = Value::signal(
        makeBinOp(SigBinOp::Sub, makeConst(1.0), args[0].asSignal()));
    return true;
  }
  *result = Value::boolean(!args[0].asBool());
  return true;
}

// The missing quotient of two durations: `div` answers "how many of
// these fit" (a dimensionless count, so an Int) and `rem` "what is
// left" (still a duration). No unit ever decays - this is deliberately
// not Timestamp/Timestamp -> Scalar. Floor convention, matching
// wrap_div: the pair satisfies num == den * div + rem with 0 <= rem < den.
SYNTH_EXTERNAL(div) {
  double num = args[0].asTime();
  double den = args[1].asTime();
  if (den <= 0) throw std::runtime_error("Time.div: division by 0s");
  *result = scalarToInt(num / den, std::floor(num / den), "Time.div");
  return true;
}

SYNTH_EXTERNAL(rem) {
  double num = args[0].asTime();
  double den = args[1].asTime();
  if (den <= 0) throw std::runtime_error("Time.rem: division by 0s");
  double r = num - den * std::floor(num / den);
  if (r < 0) r = 0;  // floating fuzz; a Timestamp is never negative
  *result = Value::time(r);
  return true;
}
