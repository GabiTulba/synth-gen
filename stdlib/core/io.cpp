#include "util.hpp"

// Audio file import. Files are build inputs (§5.3): read and validated
// at build time, resolved relative to the source file that names them
// (the host's loadAudio service owns resolution and caching).

using synth::ext::Value;
using namespace synth;

SYNTH_EXTERNAL(load_mono) {
  SigPtr sig = ctx.loadAudio(args[0].asString());
  if (signalChannels(sig) != 1)
    throw std::runtime_error("load_mono: '" + args[0].asString() + "' has " +
                             std::to_string(signalChannels(sig)) +
                             " channels (expected 1); use load_multi");
  *result = Value::signal(std::move(sig));
  return true;
}

SYNTH_EXTERNAL(load_multi) {
  *result = Value::signal(ctx.loadAudio(args[0].asString()));
  return true;
}
