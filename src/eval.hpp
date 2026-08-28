#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "checker.hpp"
#include "signal.hpp"

namespace synth {

// A build-time evaluation error: thrown by the interpreter and by
// external implementations (native and user-compiled alike), caught at
// the top-level definition being evaluated and turned into a diagnostic.
struct EvalError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Runtime (build-time) values. The evaluator reduces every top-level
// definition to one of these; signals stay lazy (a DAG of SigNodes) until a
// render target discretizes them.
struct Value;

struct UnitV {};
struct ScalarV { double v = 0; };
struct IntV { int64_t v = 0; };
struct TimeV { double seconds = 0; };
struct BoolV { bool v = false; };
struct StringV { std::string s; };
struct VectorV { std::vector<double> v; };
// A sample as the render pipeline consumes it: a signal plus the window
// to cut. At the language level a Sample is an ordinary Core RECORD
// value ({ sig; from; to }); this plain struct survives only inside
// RenderTarget, after evaluation.
struct SampleV {
  SigPtr sig;
  double from = 0, to = 0;
};
// A user function, possibly with some labeled arguments already bound
// (label-driven partial application).
struct FunV {
  const TopDef* def = nullptr;
  const CheckedModule* mod = nullptr;
  std::shared_ptr<std::map<std::string, Value>> bound;  // by param name
};
// An anonymous function: the Lambda expression, its defining module, a
// by-value snapshot of the local environment at creation (top-level defs
// resolve through the module's globals, not the snapshot), and any
// already-bound arguments (partial application).
struct LambdaV {
  const Expr* lam = nullptr;  // Expr::Kind::Lambda
  const CheckedModule* mod = nullptr;
  std::shared_ptr<const std::map<std::string, Value>> captured;
  std::shared_ptr<std::map<std::string, Value>> bound;  // by param name
};
struct TupleV { std::vector<Value> items; };
// A record value: fields in DECLARATION order (projection is by index
// into the declaration's field list, whatever order a literal wrote).
struct RecordV {
  const TypeDecl* decl = nullptr;
  std::vector<Value> fields;
};
// A variant value: which constructor (an index into the declaration's
// constructor list) and its payload, if the constructor takes one.
struct VariantV {
  const TypeDecl* decl = nullptr;
  int ctor = 0;
  std::shared_ptr<Value> payload;  // null for a payload-less constructor
};

struct Value {
  std::variant<UnitV, ScalarV, IntV, TimeV, BoolV, StringV, VectorV, SigPtr,
               TupleV, FunV, LambdaV, RecordV, VariantV>
      v;
};

// Core's list declaration and its constructor indexes: lists are
// ordinary Cons/Nil variant values, and the external boundary flattens
// them to (and rebuilds them from) ext-level lists with this.
struct CoreListInfo {
  const TypeDecl* decl = nullptr;
  int nilIndex = 0;
  int consIndex = 1;
  explicit operator bool() const { return decl != nullptr; }
};

// Core's Option declaration and its constructor indexes: optional
// parameters wrap and unwrap Some/None values at application time, and
// an unfilled non-defaulted optional parameter arrives in the body as
// None.
struct CoreOptionInfo {
  const TypeDecl* decl = nullptr;
  int noneIndex = 0;
  int someIndex = 1;
  explicit operator bool() const { return decl != nullptr; }
};

// Core's Sample declaration and its field indexes: samples are ordinary
// record values ({ sig; from; to }), and the external boundary converts
// them to (and from) the engine-level ext::Sample with this.
struct CoreSampleInfo {
  const TypeDecl* decl = nullptr;
  int sigField = 0;
  int fromField = 1;
  int toField = 2;
  explicit operator bool() const { return decl != nullptr; }
};

// A build target collected from a `render` call (§5.2): the name is the
// stable artifact identifier, unique project-wide.
struct RenderTarget {
  enum class Kind { Audio, Visual, VisualStems };
  Kind kind = Kind::Audio;
  std::string name;
  double rate = 0;
  SampleV sample;  // Audio / Visual
  // VisualStems: labeled samples drawn as stacked lanes in one image.
  std::vector<std::pair<std::string, SampleV>> stems;
  std::string file;  // source file that declared it (for diagnostics)
  Span span{};
  // The top-level definition whose evaluation declared this target -
  // the root of its dependency closure for incremental rebuilds (Epic 8).
  const CheckedModule* declModule = nullptr;
  const TopDef* declDef = nullptr;
};

// A live control collected from a `slider`/`knob` call (Core.Control): a
// named build-time Scalar parameter with a range and a default, which the
// dev app can override between rebuilds through the unit's controls.json.
// `value` is what this build used: the active override, clamped to
// [min, max], or `def`.
struct ControlDecl {
  enum class Kind { Slider, Knob, MultiSlider };
  Kind kind = Kind::Slider;
  std::string name;  // stable identifier, unique project-wide
  double min = 0, max = 1;
  double def = 0;
  double value = 0;
  // MultiSlider lanes only: the group this lane belongs to, its position
  // in the group, and the bounds the whole group's sum satisfies. `name`
  // is "<group>.<lane>", so lanes are ordinary controls downstream.
  std::string group;
  int groupIndex = -1;
  double sumMin = 0, sumMax = 0;
  std::string file;  // source file that declared it (for diagnostics)
  Span span{};
};

// A `multi_slider` call (Core.Control): several named Scalar lanes, each
// with its own range and default, whose values additionally sum into
// [sumMin, sumMax]. Declaring it yields one value per lane; each lane
// becomes a ControlDecl named "<group>.<lane>".
struct ControlGroupDecl {
  struct Lane {
    std::string name;
    double min = 0, max = 1, def = 0;
  };
  std::string name;  // group identifier, unique project-wide
  std::vector<Lane> lanes;
  double sumMin = 0, sumMax = 1;
  std::string file;
  Span span{};
};

// A `panel` call (Core.Ui): a named grouping that pairs some of the
// project's controls with some of its render targets so the dev app can
// show them together. Members are recorded as written - a control member
// may name a whole multi_slider group rather than its lanes - and are
// resolved against the declared controls and targets once evaluation has
// seen all of them. Presentation only: a panel never reaches the engine
// and never affects a rendered artifact.
struct PanelDecl {
  std::string name;  // panel identifier and title, unique project-wide
  std::vector<std::string> controls;
  std::vector<std::string> targets;
  std::string file;  // source file that declared it (for diagnostics)
  Span span{};
};

// Evaluates every module of a checked program in dependency order and
// collects all render targets. Runtime errors (bad file, channel mismatch,
// invalid windows) become diagnostics attached to the declaring top-level
// definition. Returns false if any evaluation error occurred.
// `loadedFiles`, when non-null, receives the resolved paths of every audio
// file read by load_mono/load_multi and every user external C++ file —
// they are build inputs the daemon watches (§8.3).
// `externalCacheDir` is where user `external "file.cpp"` implementations
// are compiled and cached (a build's `_build/externals`); when empty a
// per-user temp directory is used.
// `controlOverrides`, when non-null, maps control names to the values an
// attached dev tool set; `controls`, when non-null, receives every
// control the program declared, in declaration order; `panels`, when
// non-null, receives every dev-app panel declared, likewise in
// declaration order.
bool evaluateProgram(const Program& prog, std::vector<RenderTarget>& targets,
                     DiagnosticBag& diags,
                     std::vector<std::string>* loadedFiles = nullptr,
                     const std::string& externalCacheDir = {},
                     const std::map<std::string, double>* controlOverrides =
                         nullptr,
                     std::vector<ControlDecl>* controls = nullptr,
                     std::vector<PanelDecl>* panels = nullptr);

}  // namespace synth
