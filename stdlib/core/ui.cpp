#include "util.hpp"

// Dev-app panels: a named grouping that pairs some of the project's live
// controls with some of its render targets, so `synth-dev` can show a
// knob next to the waveform it moves instead of listing every control
// and every target flat. Purely presentational - the declaration never
// reaches the engine and never changes a rendered artifact.
//
// Controls reach here as (name, depth) pairs and targets as plain names.
// Core.Ui.panel takes a tree of `Controller`s, but a variant cannot cross
// this boundary with its structure intact, so the public entry point
// flattens the tree first - the same move multi_slider makes with its
// lane records. Depth 0 is a control the panel names directly; deeper
// members are the parts of a composite. A render declaration evaluates to
// unit, so a target's name stays its only handle. The host checks that
// every name resolves.

using synth::ext::PanelDecl;
using synth::ext::Value;

static std::vector<std::string> names(const Value& v, const char* what) {
  std::vector<std::string> out;
  for (const Value& item : v.asList()) {
    const std::string& n = item.asString();
    if (n.empty())
      throw std::runtime_error(std::string("panel: empty ") + what + " name");
    out.push_back(n);
  }
  return out;
}

static std::vector<PanelDecl::Member> members(const Value& v) {
  std::vector<PanelDecl::Member> out;
  for (const Value& item : v.asList()) {
    const std::vector<Value>& pair = item.asTuple();
    if (pair.size() != 2)
      throw std::runtime_error("panel: malformed control member");
    const std::string& n = pair[0].asString();
    if (n.empty()) throw std::runtime_error("panel: empty control name");
    out.push_back(PanelDecl::Member{n, (int)pair[1].asInt()});
  }
  return out;
}

SYNTH_EXTERNAL(panel_members) {
  PanelDecl p;
  p.name = args[0].asString();
  if (p.name.empty()) throw std::runtime_error("panel: empty panel name");
  p.controls = members(args[1]);
  p.targets = names(args[2], "target");
  ctx.panel(std::move(p));
  *result = Value::unit();
  return true;
}
