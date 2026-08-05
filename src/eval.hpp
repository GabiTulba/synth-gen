#pragma once
#include <string>
#include <variant>
#include <vector>

#include "checker.hpp"
#include "signal.hpp"

namespace synth {

// Runtime (build-time) values. The evaluator reduces every top-level
// definition to one of these; signals stay lazy (a DAG of SigNodes) until a
// render target discretizes them.
struct Value;

struct UnitV {};
struct ScalarV { double v = 0; };
struct TimeV { double seconds = 0; };
struct StringV { std::string s; };
struct VectorV { std::vector<double> v; };
struct SampleV {
  SigPtr sig;
  double from = 0, to = 0;
};
struct FunV {
  const TopDef* def = nullptr;
  const CheckedModule* mod = nullptr;
};
struct ListV { std::vector<Value> items; };
struct TupleV { std::vector<Value> items; };

struct Value {
  std::variant<UnitV, ScalarV, TimeV, StringV, VectorV, SigPtr, SampleV,
               ListV, TupleV, FunV>
      v;
};

// A build target collected from a `render` call (§5.2): the name is the
// stable artifact identifier, unique project-wide.
struct RenderTarget {
  std::string name;
  double rate = 0;
  SampleV sample;
  std::string file;  // source file that declared it (for diagnostics)
  Span span{};
};

// Evaluates every module of a checked program in dependency order and
// collects all render targets. Runtime errors (bad file, channel mismatch,
// invalid windows) become diagnostics attached to the declaring top-level
// definition. Returns false if any evaluation error occurred.
// `loadedFiles`, when non-null, receives the resolved paths of every audio
// file read by load_mono/load_multi — they are build inputs the daemon
// watches (§8.3).
bool evaluateProgram(const Program& prog, std::vector<RenderTarget>& targets,
                     DiagnosticBag& diags,
                     std::vector<std::string>* loadedFiles = nullptr);

}  // namespace synth
