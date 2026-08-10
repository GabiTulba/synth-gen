#include "util.hpp"

// The render effects: the language's only side effect. Each call declares
// a build target; artifacts share one project-wide name space.
namespace synth::native {

namespace {
void fillDecl(RenderTarget& t, Ctx& ctx) {
  t.file = ctx.mod->parsed.path;
  t.span = ctx.currentDef ? ctx.currentDef->span : Span{};
  t.declModule = ctx.currentModule;
  t.declDef = ctx.currentDef;
}
}  // namespace

Value renderImpl(Ctx& ctx, std::vector<Value>& args) {
  RenderTarget t;
  t.kind = RenderTarget::Kind::Audio;
  t.name = strArg(args[0]);
  t.rate = scalarArg(args[1]);
  t.sample = std::get<SampleV>(args[2].v);
  fillDecl(t, ctx);
  if (t.name.empty()) throw EvalError("render: empty artifact name");
  if (t.rate <= 0) throw EvalError("render: sample rate must be positive");
  ctx.targets->push_back(std::move(t));
  return Value{UnitV{}};
}

Value renderVisImpl(Ctx& ctx, std::vector<Value>& args) {
  RenderTarget t;
  t.kind = RenderTarget::Kind::Visual;
  t.name = strArg(args[0]);
  t.rate = scalarArg(args[1]);
  t.sample = std::get<SampleV>(args[2].v);
  fillDecl(t, ctx);
  if (t.name.empty()) throw EvalError("render: empty artifact name");
  if (t.rate <= 0) throw EvalError("render: sample rate must be positive");
  ctx.targets->push_back(std::move(t));
  return Value{UnitV{}};
}

Value renderStemsImpl(Ctx& ctx, std::vector<Value>& args) {
  const std::string& base = strArg(args[0]);
  double rate = scalarArg(args[1]);
  if (base.empty()) throw EvalError("render_stems: empty base name");
  if (rate <= 0)
    throw EvalError("render_stems: sample rate must be positive");
  for (auto& item : std::get<ListV>(args[2].v).items) {
    const TupleV& stem = std::get<TupleV>(item.v);
    const std::string& label = std::get<StringV>(stem.items[0].v).s;
    if (label.empty()) throw EvalError("render_stems: empty stem label");
    RenderTarget t;
    t.kind = RenderTarget::Kind::Audio;
    t.name = base + "-" + label;
    t.rate = rate;
    t.sample = std::get<SampleV>(stem.items[1].v);
    fillDecl(t, ctx);
    ctx.targets->push_back(std::move(t));
  }
  return Value{UnitV{}};
}

Value renderVisStemsImpl(Ctx& ctx, std::vector<Value>& args) {
  const std::string& name = strArg(args[0]);
  double rate = scalarArg(args[1]);
  if (name.empty()) throw EvalError("render_vis_stems: empty artifact name");
  if (rate <= 0)
    throw EvalError("render_vis_stems: sample rate must be positive");
  RenderTarget t;
  t.kind = RenderTarget::Kind::VisualStems;
  t.name = name;
  t.rate = rate;
  for (auto& item : std::get<ListV>(args[2].v).items) {
    const TupleV& stem = std::get<TupleV>(item.v);
    const std::string& label = std::get<StringV>(stem.items[0].v).s;
    if (label.empty()) throw EvalError("render_vis_stems: empty lane label");
    t.stems.emplace_back(label, std::get<SampleV>(stem.items[1].v));
  }
  fillDecl(t, ctx);
  ctx.targets->push_back(std::move(t));
  return Value{UnitV{}};
}

}  // namespace synth::native
