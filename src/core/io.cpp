#include "util.hpp"

// Audio file import. Files are build inputs (§5.3): read and validated
// at build time, resolved relative to the source file that names them
// (the interpreter's loadAudio callback owns resolution and caching).
namespace synth::native {

Value loadMonoImpl(Ctx& ctx, std::vector<Value>& args) {
  SigPtr sig = ctx.loadAudio(strArg(args[0]));
  if (sig->channels() != 1)
    throw EvalError("load_mono: '" + strArg(args[0]) + "' has " +
                    std::to_string(sig->channels()) +
                    " channels (expected 1); use load_multi");
  return Value{sig};
}

Value loadMultiImpl(Ctx& ctx, std::vector<Value>& args) {
  return Value{ctx.loadAudio(strArg(args[0]))};
}

}  // namespace synth::native
