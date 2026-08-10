#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../eval.hpp"

namespace synth::native {

// The context a built-in external implementation runs in: callbacks into
// the interpreter (values are opaque to the engine, so applying a synth
// function or loading an audio file has to go back through it) plus the
// location info render targets and diagnostics need.
struct Ctx {
  // Apply a synth function value (user function, lambda, or another
  // external) to positional arguments.
  std::function<Value(const Value& fn, std::vector<Value> args)> apply;
  // Resolve `path` relative to the calling module, read (and cache) the
  // audio file, and record it as a build input.
  std::function<SigPtr(const std::string& path)> loadAudio;
  std::vector<RenderTarget>* targets = nullptr;
  const CheckedModule* mod = nullptr;          // module being evaluated
  const CheckedModule* currentModule = nullptr;
  const TopDef* currentDef = nullptr;
};

using Fn = Value (*)(Ctx&, std::vector<Value>&);

// One built-in implementation: the C++ file that holds it (the string a
// stdlib `external "file.cpp"` declaration names) and the function name.
struct Impl {
  const char* file;
  const char* name;
  Fn fn;
};

// Look up the implementation a Core-library `external "file"` declaration
// of `name` binds to. Returns nullptr when no built-in matches.
const Impl* find(const std::string& file, const std::string& name);

}  // namespace synth::native
