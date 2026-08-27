#include "util.hpp"

// Dev-app panels: a named grouping that pairs some of the project's live
// controls with some of its render targets, so `synth-dev` can show a
// knob next to the waveform it moves instead of listing every control
// and every target flat. Purely presentational - the declaration never
// reaches the engine and never changes a rendered artifact.
//
// Members are named rather than passed by value because a control
// declaration evaluates to a bare Scalar and a render declaration to
// unit: the name is the only handle either one leaves behind. The host
// checks that every name resolves.

using synth::ext::PanelDecl;
using synth::ext::Value;

// Strings arrive as a plain list, not the tuple list multi_slider needs:
// a panel member is one name, so there is nothing to flatten.
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

SYNTH_EXTERNAL(panel) {
  PanelDecl p;
  p.name = args[0].asString();
  if (p.name.empty()) throw std::runtime_error("panel: empty panel name");
  p.controls = names(args[1], "control");
  p.targets = names(args[2], "target");
  ctx.panel(std::move(p));
  *result = Value::unit();
  return true;
}
