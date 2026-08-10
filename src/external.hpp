#pragma once
#include <functional>
#include <string>
#include <vector>

#include "eval.hpp"

namespace synth {

// User `external "file.cpp"` implementations: the referenced C++ file is
// compiled once into a cached shared object (keyed by its content), the
// object is loaded, and the exported `synth_external_<name>` entry point
// is wrapped as a callable over build-time values.
//
// Only *data* crosses the boundary - Scalar, Timestamp, Bool, String,
// Vector, unit, and lists/tuples of those. Signals stay inside the
// engine; the checker enforces this on external signatures outside the
// bundled Core library.
using ExternalFn = std::function<Value(const std::vector<Value>&)>;

// Compile (or reuse) `cppPath` and bind `name`. Throws EvalError with a
// full explanation (including compiler output) on failure.
ExternalFn loadUserExternal(const std::string& cppPath,
                            const std::string& name,
                            const std::string& cacheDir);

// The self-contained API header user externals include as
// <synth/external.hpp>; written into the cache's include directory.
const char* externalApiHeader();

}  // namespace synth
