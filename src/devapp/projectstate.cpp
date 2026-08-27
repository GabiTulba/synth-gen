#include "projectstate.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "json.hpp"
#include "metadata.hpp"

namespace synth::devapp {

namespace {

bool jsonBool(const json::Value& v, const std::string& key, bool fallback) {
  const json::Value* f = v.get(key);
  if (!f || f->kind != json::Value::Kind::Bool) return fallback;
  return f->boolean;
}

// The shortest decimal that reads back as the same double. json.cpp's
// serializer prints %.17g, which is exact but turns a knob at 0.42 into
// "0.41999999999999998" - fine for a file under _build/, not for one a
// person is expected to open.
std::string numberLiteral(double n) {
  if (!std::isfinite(n)) return "0";
  if (n == (double)(long long)n && std::fabs(n) < 1e15)
    return std::to_string((long long)n);
  char buf[40];
  for (int prec = 3; prec < 17; prec++) {
    std::snprintf(buf, sizeof buf, "%.*g", prec, n);
    if (std::strtod(buf, nullptr) == n) return buf;
  }
  std::snprintf(buf, sizeof buf, "%.17g", n);
  return buf;
}

// Indented JSON: one member per line, scalars inline. Leaves string
// escaping and the overall grammar to json::serialize by handing it one
// scalar at a time.
void writeIndented(const json::Value& v, int depth, std::string& out) {
  std::string pad(2 * (depth + 1), ' '), close(2 * depth, ' ');
  switch (v.kind) {
    case json::Value::Kind::Number:
      out += numberLiteral(v.number);
      return;
    case json::Value::Kind::Object:
      if (v.object.empty()) {
        out += "{}";
        return;
      }
      out += "{\n";
      for (size_t i = 0; i < v.object.size(); i++) {
        out += pad + json::serialize(json::makeString(v.object[i].first)) + ": ";
        writeIndented(v.object[i].second, depth + 1, out);
        out += i + 1 < v.object.size() ? ",\n" : "\n";
      }
      out += close + "}";
      return;
    case json::Value::Kind::Array:
      if (v.array.empty()) {
        out += "[]";
        return;
      }
      out += "[\n";
      for (size_t i = 0; i < v.array.size(); i++) {
        out += pad;
        writeIndented(v.array[i], depth + 1, out);
        out += i + 1 < v.array.size() ? ",\n" : "\n";
      }
      out += close + "]";
      return;
    default:
      out += json::serialize(v);
      return;
  }
}

}  // namespace

ProjectStateLoad loadProjectState(const std::string& path) {
  ProjectStateLoad r;
  std::ifstream in(path, std::ios::binary);
  if (!in) return r;
  std::ostringstream ss;
  ss << in.rdbuf();

  json::Value root;
  std::string err;
  // A corrupt file must never keep the app from starting - and must not
  // be mistaken for "this project has no settings", which would let the
  // app overwrite a perfectly good controls.json.
  if (!json::parse(ss.str(), root, err) ||
      root.kind != json::Value::Kind::Object)
    return r;
  r.found = true;

  if (const json::Value* units = root.get("controls");
      units && units->kind == json::Value::Kind::Object) {
    for (auto& [unit, values] : units->object) {
      if (values.kind != json::Value::Kind::Object) continue;
      std::map<std::string, double> m;
      for (auto& [name, value] : values.object)
        if (value.kind == json::Value::Kind::Number) m[name] = value.number;
      r.state.controls[unit] = std::move(m);
    }
  }

  const json::Value* ui = root.get("ui");
  if (!ui || ui->kind != json::Value::Kind::Object) return r;

  if (const json::Value* w = ui->get("window");
      w && w->kind == json::Value::Kind::Object) {
    WindowGeometry g;
    g.x = (int)w->getNumber("x");
    g.y = (int)w->getNumber("y");
    g.w = (int)w->getNumber("w");
    g.h = (int)w->getNumber("h");
    // A zero-size window would open invisible; treat it as "never saved".
    if (g.w > 0 && g.h > 0) {
      g.valid = true;
      r.state.ui.window = g;
    }
  }

  r.state.ui.imguiIni = ui->getString("imgui");

  if (const json::Value* waves = ui->get("waves");
      waves && waves->kind == json::Value::Kind::Array) {
    for (const json::Value& w : waves->array) {
      if (w.kind != json::Value::Kind::Object) continue;
      WavePanelState p;
      p.artifact = w.getString("artifact");
      if (p.artifact.empty()) continue;
      p.target = w.getString("target");
      p.viewStart = w.getNumber("viewStart");
      p.viewEnd = w.getNumber("viewEnd");
      p.selStart = w.getNumber("selStart", -1);
      p.selEnd = w.getNumber("selEnd", -1);
      p.loop = jsonBool(w, "loop", false);
      r.state.ui.waves.push_back(std::move(p));
    }
  }

  if (const json::Value* secs = ui->get("sections");
      secs && secs->kind == json::Value::Kind::Object)
    for (auto& [key, value] : secs->object)
      if (value.kind == json::Value::Kind::Bool)
        r.state.ui.sections[key] = value.boolean;

  auto readFlags = [&ui](const char* key, std::map<std::string, bool>& out) {
    const json::Value* v = ui->get(key);
    if (!v || v->kind != json::Value::Kind::Object) return;
    for (auto& [name, value] : v->object)
      if (value.kind == json::Value::Kind::Bool) out[name] = value.boolean;
  };
  readFlags("panels", r.state.ui.panels);

  return r;
}

bool saveProjectState(const std::string& path, const ProjectState& state,
                      std::string& error) {
  json::Value root = json::makeObject();
  root.set("version", json::makeNumber(1));

  json::Value controls = json::makeObject();
  for (auto& [unit, values] : state.controls) {
    if (values.empty()) continue;  // no overrides is the absence of an entry
    json::Value v = json::makeObject();
    for (auto& [name, value] : values) v.set(name, json::makeNumber(value));
    controls.set(unit, std::move(v));
  }
  root.set("controls", std::move(controls));

  json::Value ui = json::makeObject();
  if (state.ui.window.valid) {
    json::Value w = json::makeObject();
    w.set("x", json::makeNumber(state.ui.window.x));
    w.set("y", json::makeNumber(state.ui.window.y));
    w.set("w", json::makeNumber(state.ui.window.w));
    w.set("h", json::makeNumber(state.ui.window.h));
    ui.set("window", std::move(w));
  }
  ui.set("imgui", json::makeString(state.ui.imguiIni));

  json::Value waves = json::makeArray();
  for (const WavePanelState& p : state.ui.waves) {
    json::Value w = json::makeObject();
    w.set("artifact", json::makeString(p.artifact));
    w.set("target", json::makeString(p.target));
    w.set("viewStart", json::makeNumber(p.viewStart));
    w.set("viewEnd", json::makeNumber(p.viewEnd));
    w.set("selStart", json::makeNumber(p.selStart));
    w.set("selEnd", json::makeNumber(p.selEnd));
    w.set("loop", json::makeBool(p.loop));
    waves.array.push_back(std::move(w));
  }
  ui.set("waves", std::move(waves));

  json::Value secs = json::makeObject();
  for (auto& [key, open] : state.ui.sections)
    secs.set(key, json::makeBool(open));
  ui.set("sections", std::move(secs));

  auto writeFlags = [&ui](const char* key,
                          const std::map<std::string, bool>& flags) {
    json::Value v = json::makeObject();
    for (auto& [name, on] : flags) v.set(name, json::makeBool(on));
    ui.set(key, std::move(v));
  };
  writeFlags("panels", state.ui.panels);
  root.set("ui", std::move(ui));

  std::string text;
  writeIndented(root, 0, text);
  text += "\n";
  return writeFileAtomically(path, text, /*createParents=*/false, error);
}

}  // namespace synth::devapp
