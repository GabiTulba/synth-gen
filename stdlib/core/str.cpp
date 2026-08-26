#include "util.hpp"

// The minimal string vocabulary: concatenation and Int rendering. This
// is deliberately not a string library - it is exactly what computed
// render-target names need ("section-" ^ index), and nothing more until
// a real need shows up.

using synth::ext::Value;

SYNTH_EXTERNAL(cat) {
  *result = Value::string(args[0].asString() + args[1].asString());
  return true;
}

SYNTH_EXTERNAL(of_int) {
  *result = Value::string(std::to_string(args[0].asInt()));
  return true;
}
