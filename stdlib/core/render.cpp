#include "util.hpp"

// The render effects: the language's only side effect. Each call declares
// a build target; artifacts share one project-wide name space. The host
// attributes each declaration to the synth definition being evaluated.

using synth::ext::RenderDecl;
using synth::ext::Value;
using namespace synth;

SYNTH_EXTERNAL(render) {
  RenderDecl t;
  t.kind = RenderDecl::Kind::Audio;
  t.name = args[0].asString();
  t.rate = args[1].asScalar();
  t.sample = args[2].asSample();
  if (t.name.empty()) throw std::runtime_error("render: empty artifact name");
  if (t.rate <= 0)
    throw std::runtime_error("render: sample rate must be positive");
  ctx.render(std::move(t));
  *result = Value::unit();
  return true;
}

SYNTH_EXTERNAL(render_vis) {
  RenderDecl t;
  t.kind = RenderDecl::Kind::Visual;
  t.name = args[0].asString();
  t.rate = args[1].asScalar();
  t.sample = args[2].asSample();
  if (t.name.empty()) throw std::runtime_error("render: empty artifact name");
  if (t.rate <= 0)
    throw std::runtime_error("render: sample rate must be positive");
  ctx.render(std::move(t));
  *result = Value::unit();
  return true;
}

SYNTH_EXTERNAL(render_stems) {
  const std::string& base = args[0].asString();
  double rate = args[1].asScalar();
  if (base.empty()) throw std::runtime_error("render_stems: empty base name");
  if (rate <= 0)
    throw std::runtime_error("render_stems: sample rate must be positive");
  for (auto& item : args[2].asList()) {
    auto& stem = item.asTuple();
    const std::string& label = stem[0].asString();
    if (label.empty())
      throw std::runtime_error("render_stems: empty stem label");
    RenderDecl t;
    t.kind = RenderDecl::Kind::Audio;
    t.name = base + "-" + label;
    t.rate = rate;
    t.sample = stem[1].asSample();
    ctx.render(std::move(t));
  }
  *result = Value::unit();
  return true;
}

SYNTH_EXTERNAL(render_vis_stems) {
  RenderDecl t;
  t.kind = RenderDecl::Kind::VisualStems;
  t.name = args[0].asString();
  t.rate = args[1].asScalar();
  if (t.name.empty())
    throw std::runtime_error("render_vis_stems: empty artifact name");
  if (t.rate <= 0)
    throw std::runtime_error("render_vis_stems: sample rate must be positive");
  for (auto& item : args[2].asList()) {
    auto& stem = item.asTuple();
    const std::string& label = stem[0].asString();
    if (label.empty())
      throw std::runtime_error("render_vis_stems: empty lane label");
    t.stems.emplace_back(label, stem[1].asSample());
  }
  ctx.render(std::move(t));
  *result = Value::unit();
  return true;
}
