// synth external API v3.
//
// Implement a synth `external "this_file.cpp"` binding by defining, for a
// declaration `let name ... = external "this_file.cpp"`:
//
//   #include <synth/external.hpp>
//   SYNTH_EXTERNAL(succ) {
//     *result = synth::ext::Value::scalar(args[0].asScalar() + 1.0);
//     return true;
//   }
//
// `args`/`argc` are the fully-applied arguments in declaration order;
// `ctx` offers the host services (applying synth function values, loading
// audio files, declaring render targets). Return true on success; on
// failure either fill *error and return false, or throw std::exception.
//
// Every synth value can cross this boundary. Data (scalars, ints,
// timestamps, bools, strings, vectors, lists, tuples) arrives as
// transparent Values; signals and samples arrive as engine graph handles
// you can combine with the <synth/engine.hpp> constructors; functions
// (and any future value kind) arrive as opaque handles usable through
// ctx.apply or passable back unchanged.
//
// The implementation is compiled by the host's $CXX into the host
// process, so the full C++ standard library is usable across the
// boundary. One .cpp may implement several externals.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "engine.hpp"

namespace synth::ext {

// A sample: a signal with the window [from, to) cut out of it, not yet
// placed anywhere on the timeline.
struct Sample {
  SigPtr signal;
  double from = 0, to = 0;
};

struct Value {
  enum class Kind { Unit, Scalar, Time, Bool, String, List, Tuple, Vector,
                    Int, Signal, Sample, Opaque };
  Kind kind = Kind::Unit;
  double num = 0.0;              // Scalar / Time (seconds) / Bool (0 or 1)
  std::int64_t inum = 0;         // Int
  std::string str;               // String
  std::vector<Value> items;      // List / Tuple / Vector (scalar items)
  SigPtr sig;                    // Signal
  Sample samp;                   // Sample
  // Any host value with no transparent representation here (a function,
  // for instance). Pass it to ctx.apply or return it unchanged; its
  // meaning lives on the host side.
  std::shared_ptr<void> opaque;

  static Value unit() { return {}; }
  static Value scalar(double v) { Value x; x.kind = Kind::Scalar; x.num = v; return x; }
  static Value integer(std::int64_t v) { Value x; x.kind = Kind::Int; x.inum = v; return x; }
  static Value time(double seconds) { Value x; x.kind = Kind::Time; x.num = seconds; return x; }
  static Value boolean(bool v) { Value x; x.kind = Kind::Bool; x.num = v ? 1.0 : 0.0; return x; }
  static Value string(std::string s) { Value x; x.kind = Kind::String; x.str = std::move(s); return x; }
  static Value list(std::vector<Value> xs) { Value x; x.kind = Kind::List; x.items = std::move(xs); return x; }
  static Value tuple(std::vector<Value> xs) { Value x; x.kind = Kind::Tuple; x.items = std::move(xs); return x; }
  static Value signal(SigPtr s) { Value x; x.kind = Kind::Signal; x.sig = std::move(s); return x; }
  static Value sample(Sample s) { Value x; x.kind = Kind::Sample; x.samp = std::move(s); return x; }

  double asScalar() const { require(Kind::Scalar, "Scalar"); return num; }
  std::int64_t asInt() const { require(Kind::Int, "Int"); return inum; }
  double asTime() const { require(Kind::Time, "Timestamp"); return num; }
  bool asBool() const { require(Kind::Bool, "Bool"); return num != 0.0; }
  const std::string& asString() const { require(Kind::String, "String"); return str; }
  const std::vector<Value>& asList() const { require(Kind::List, "list"); return items; }
  const std::vector<Value>& asTuple() const { require(Kind::Tuple, "tuple"); return items; }
  const SigPtr& asSignal() const { require(Kind::Signal, "Signal"); return sig; }
  const Sample& asSample() const { require(Kind::Sample, "Sample"); return samp; }

  // A Signal, or a Scalar broadcast to one - the coercion the engine's
  // pointwise semantics give scalars in signal position.
  SigPtr toSignal() const {
    if (kind == Kind::Signal) return sig;
    if (kind == Kind::Scalar) return makeConst(num);
    throw std::runtime_error("external: value is not a Signal or Scalar");
  }

 private:
  void require(Kind k, const char* what) const {
    if (kind != k)
      throw std::runtime_error(std::string("external: argument is not a ") + what);
  }
};

// A render target being declared: the language's only side effect. The
// host attributes it to the synth definition whose evaluation made the
// call and adds it to the build.
struct RenderDecl {
  enum class Kind { Audio, Visual, VisualStems };
  Kind kind = Kind::Audio;
  std::string name;  // stable artifact identifier, unique project-wide
  double rate = 0;
  Sample sample;                                      // Audio / Visual
  std::vector<std::pair<std::string, Sample>> stems;  // VisualStems lanes
};

// Host services available during one call.
struct Ctx {
  // Apply a synth function value (an Opaque argument) to positional
  // arguments.
  std::function<Value(const Value& fn, std::vector<Value> args)> apply;
  // Resolve `path` relative to the calling module's source file, read
  // (and cache) the audio file, and record it as a build input.
  std::function<SigPtr(const std::string& path)> loadAudio;
  // Declare a render target.
  std::function<void(RenderDecl)> render;
};

}  // namespace synth::ext

// Entry-point contract. `name` must match the synth definition's name.
#define SYNTH_EXTERNAL(name)                                              \
  extern "C" bool synth_external_##name(                                  \
      ::synth::ext::Ctx& ctx, const ::synth::ext::Value* args, int argc,  \
      ::synth::ext::Value* result, ::std::string* error)
