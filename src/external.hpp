#pragma once
#include <functional>
#include <string>
#include <vector>

#include "eval.hpp"

namespace synth {

// `external "file.cpp"` implementations - the bundled Core library's and
// user libraries' alike: the referenced C++ file is compiled once into a
// cached shared object (keyed by its content and the API headers), the
// object is loaded, and the exported `synth_external_<name>` entry point
// is wrapped as a callable over build-time values.
//
// Every value kind crosses the boundary: data transparently, signals and
// samples as engine graph handles (the implementation links against the
// host's engine constructors), everything else - functions included - as
// opaque boxes usable through the call services.

// The interpreter-side services one external call may use; the loader
// bridges them to the public synth::ext::Ctx.
struct ExtServices {
  // Core's list declaration: host-side Cons/Nil chains flatten to
  // ext-level lists at the boundary (and rebuild coming back), so list
  // implementations keep seeing plain vectors.
  CoreListInfo coreList;
  // Core's Sample declaration: host-side { sig; from; to } records
  // convert to (and from) the engine-level ext::Sample.
  CoreSampleInfo coreSample;
  // Apply a synth function value to positional arguments.
  std::function<Value(const Value& fn, std::vector<Value> args)> apply;
  // Resolve a path relative to the calling module, load (and cache) the
  // audio file, and record it as a build input.
  std::function<SigPtr(const std::string& path)> loadAudio;
  // Record a render target; the caller fills declaration attribution
  // (file, span, declaring module/def) before pushing.
  std::function<void(RenderTarget)> declareTarget;
  // Record a live control and return its value for this build (the
  // active override or the default); the caller fills declaration
  // attribution before registering.
  std::function<double(ControlDecl)> declareControl;
  // Record a sum-constrained group of live controls and return this
  // build's value for every lane, in lane order; the caller fills
  // declaration attribution before registering.
  std::function<std::vector<double>(ControlGroupDecl)> declareControlGroup;
  // Record a dev-app panel; the caller fills declaration attribution
  // before registering.
  std::function<void(PanelDecl)> declarePanel;
};

using ExternalFn =
    std::function<Value(const ExtServices&, const std::vector<Value>&)>;

// Compile (or reuse) `cppPath` and bind `name`. Throws EvalError with a
// full explanation (including compiler output) on failure.
ExternalFn loadUserExternal(const std::string& cppPath,
                            const std::string& name,
                            const std::string& cacheDir);

// The directory holding the public API headers (<synth/external.hpp>,
// <synth/engine.hpp>) external implementations compile against:
// $SYNTH_EXT_INCLUDE_DIR when set, the source-tree default otherwise.
std::string externalIncludeDir();

}  // namespace synth
