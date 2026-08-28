#include "metadata.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "build.hpp"
#include "json.hpp"
#include "library.hpp"

namespace fs = std::filesystem;

namespace synth::devapp {

MetadataLoadResult loadProjectMetadata(const std::string& path) {
  MetadataLoadResult r;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    r.error = "cannot read '" + path + "'";
    return r;
  }
  std::ostringstream ss;
  ss << in.rdbuf();

  json::Value root;
  std::string jsonErr;
  if (!json::parse(ss.str(), root, jsonErr)) {
    r.error = "invalid metadata JSON: " + jsonErr;
    return r;
  }
  if (root.kind != json::Value::Kind::Object) {
    r.error = "invalid metadata: top level is not an object";
    return r;
  }

  r.meta.project = root.getString("project");
  r.meta.status = root.getString("status", "error");

  if (const json::Value* diags = root.get("diagnostics");
      diags && diags->kind == json::Value::Kind::Array) {
    for (auto& d : diags->array) {
      DiagnosticMeta m;
      m.severity = d.getString("severity", "error");
      m.file = d.getString("file");
      m.message = d.getString("message");
      m.rendered = d.getString("rendered");
      r.meta.diagnostics.push_back(std::move(m));
    }
  }
  if (const json::Value* targets = root.get("targets");
      targets && targets->kind == json::Value::Kind::Array) {
    for (auto& t : targets->array) {
      TargetMeta m;
      m.name = t.getString("name");
      m.kind = t.getString("kind", "audio");
      m.status = t.getString("status", "error");
      m.artifact = t.getString("artifact");
      m.error = t.getString("error");
      m.rate = t.getNumber("rate");
      m.durationSeconds = t.getNumber("duration_seconds");
      m.channels = (int)t.getNumber("channels");
      m.frames = (int64_t)t.getNumber("frames");
      r.meta.targets.push_back(std::move(m));
    }
  }
  if (const json::Value* controls = root.get("controls");
      controls && controls->kind == json::Value::Kind::Array) {
    for (auto& c : controls->array) {
      ControlMeta m;
      m.name = c.getString("name");
      m.kind = c.getString("kind", "slider");
      m.min = c.getNumber("min");
      m.max = c.getNumber("max", 1);
      m.def = c.getNumber("default");
      m.value = c.getNumber("value", m.def);
      if (const json::Value* opts = c.get("options");
          opts && opts->kind == json::Value::Kind::Array)
        for (auto& o : opts->array)
          if (o.kind == json::Value::Kind::String) m.options.push_back(o.string);
      m.group = c.getString("group");
      m.groupIndex = (int)c.getNumber("group_index", -1);
      m.sumMin = c.getNumber("sum_min");
      m.sumMax = c.getNumber("sum_max");
      if (!m.name.empty()) r.meta.controls.push_back(std::move(m));
    }
  }
  if (const json::Value* panels = root.get("panels");
      panels && panels->kind == json::Value::Kind::Array) {
    // Defensive like the arrays above: a malformed entry is dropped, not
    // fatal. A panel is presentation, and losing one must never stop the
    // app showing the build.
    auto names = [](const json::Value& p, const char* key) {
      std::vector<std::string> out;
      const json::Value* v = p.get(key);
      if (!v || v->kind != json::Value::Kind::Array) return out;
      for (auto& n : v->array)
        if (n.kind == json::Value::Kind::String && !n.string.empty())
          out.push_back(n.string);
      return out;
    };
    for (auto& p : panels->array) {
      PanelMeta m;
      m.name = p.getString("name");
      if (m.name.empty()) continue;
      // A control member is {"name", "depth"}; a bare string is the
      // pre-controller spelling and reads as depth 0.
      if (const json::Value* cs = p.get("controls");
          cs && cs->kind == json::Value::Kind::Array)
        for (auto& c : cs->array) {
          if (c.kind == json::Value::Kind::String && !c.string.empty())
            m.controls.push_back(PanelMember{c.string, 0, {}});
          else if (c.kind == json::Value::Kind::Object) {
            std::string n = c.getString("name");
            if (!n.empty())
              m.controls.push_back(PanelMember{std::move(n),
                                               (int)c.getNumber("depth", 0),
                                               c.getString("key")});
          }
        }
      m.targets = names(p, "targets");
      r.meta.panels.push_back(std::move(m));
    }
  }
  r.ok = true;
  return r;
}

std::string unitKey(const MetadataUnit& u) {
  return u.label.empty() ? std::string(".") : u.label;
}

std::vector<PanelMeta> resolvePanels(const ProjectMeta& meta) {
  std::vector<PanelMeta> out = meta.panels;
  std::set<std::string> claimedControls, claimedTargets;
  for (auto& p : meta.panels) {
    for (auto& m : p.controls) claimedControls.insert(m.name);
    claimedTargets.insert(p.targets.begin(), p.targets.end());
  }
  PanelMeta rest;
  std::set<std::string> seenGroups;
  for (auto& c : meta.controls) {
    // A group is claimed under its group name, which is how a panel
    // names it; its lanes are never listed individually there.
    const std::string& key = c.group.empty() ? c.name : c.group;
    if (claimedControls.count(key) || claimedControls.count(c.name)) continue;
    if (!c.group.empty() && !seenGroups.insert(c.group).second) continue;
    rest.controls.push_back(PanelMember{key, 0, {}});
  }
  for (auto& t : meta.targets)
    if (!claimedTargets.count(t.name)) rest.targets.push_back(t.name);
  if (!rest.controls.empty() || !rest.targets.empty()) {
    // With nothing declared this panel *is* the project, so it says so;
    // alongside declared panels it is the leftovers.
    rest.name = meta.panels.empty()
                    ? (meta.project.empty() ? "all" : meta.project)
                    : "ungrouped";
    out.push_back(std::move(rest));
  }
  return out;
}

ControlBand controlLaneBand(const ControlMeta& lane, double otherLanesSum) {
  ControlBand b;
  b.lo = std::max(lane.min, lane.sumMin - otherLanesSum);
  b.hi = std::min(lane.max, lane.sumMax - otherLanesSum);
  if (b.hi < b.lo) b.hi = b.lo;
  return b;
}

std::string controlsPathFor(const std::string& metadataPath) {
  return (fs::path(metadataPath).parent_path() / "controls.json").string();
}

bool writeFileAtomically(const std::string& path, const std::string& text,
                         bool createParents, std::string& error) {
  fs::path target(path);
  if (createParents && target.has_parent_path()) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
      error = "cannot create '" + target.parent_path().string() +
              "': " + ec.message();
      return false;
    }
  }
  fs::path tmp = target;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "cannot write '" + tmp.string() + "'";
      return false;
    }
    out << text;
    if (!out) {
      error = "write to '" + tmp.string() + "' failed";
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, target, ec);
  if (ec) {
    error = "cannot rename '" + tmp.string() + "': " + ec.message();
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

bool writeControlOverrides(const std::string& path,
                           const std::map<std::string, double>& overrides,
                           std::string& error) {
  json::Value ov = json::makeObject();
  for (auto& [name, value] : overrides) ov.set(name, json::makeNumber(value));
  json::Value root = json::makeObject();
  root.set("overrides", std::move(ov));
  return writeFileAtomically(path, json::serialize(root) + "\n",
                             /*createParents=*/false, error);
}

std::map<std::string, double> readControlOverrides(const std::string& path) {
  std::map<std::string, double> out;
  std::ifstream in(path, std::ios::binary);
  if (!in) return out;
  std::ostringstream ss;
  ss << in.rdbuf();
  json::Value root;
  std::string err;
  if (!json::parse(ss.str(), root, err)) return out;
  const json::Value* ov = root.get("overrides");
  if (!ov || ov->kind != json::Value::Kind::Object) return out;
  for (auto& [name, value] : ov->object)
    if (value.kind == json::Value::Kind::Number) out[name] = value.number;
  return out;
}

MetadataLayout resolveMetadataLayout(const std::string& projectDir) {
  MetadataLayout layout;
  // Outputs live under the enclosing root's _build/, mirroring the source
  // tree; a project with no enclosing root is its own root.
  std::string root = findEnclosingRoot(projectDir);
  fs::path base = root.empty() ? fs::path(projectDir) : fs::path(root);
  fs::path rel;
  if (!root.empty()) {
    std::error_code ec;
    rel = fs::relative(fs::absolute(projectDir), base, ec).lexically_normal();
    if (ec || rel == ".") rel = "";
  }
  layout.rootDir = base.string();
  layout.manifestPath = (base / kManifestFileName).string();
  // Beside the build.json the app was pointed at, not beside the root's:
  // pointed at a subproject, its settings belong to that subproject.
  layout.projectStatePath =
      (fs::path(projectDir) / "project.json").lexically_normal().string();

  // The root dir itself: one unit per build rule, mirroring buildRoot's
  // output layout.
  if (!root.empty() && rel.empty()) {
    Manifest m;
    DiagnosticBag diags;
    std::ifstream in(layout.manifestPath, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    if (parseManifest(ss.str(), layout.manifestPath, m, diags) &&
        m.isRoot()) {
      for (auto& rule : m.buildRules) {
        fs::path rp(rule);
        std::error_code ec;
        if (!fs::is_directory(base / rp, ec) && rp.extension() == ".synth")
          rp = rp.parent_path() / rp.stem();
        MetadataUnit u;
        u.label = rule;
        u.metadataPath = (base / "_build" / rp.lexically_normal() /
                          "metadata.json")
                             .lexically_normal()
                             .string();
        layout.units.push_back(std::move(u));
      }
      return layout;
    }
  }

  MetadataUnit u;
  u.metadataPath =
      (base / "_build" / rel / "metadata.json").lexically_normal().string();
  layout.units.push_back(std::move(u));
  return layout;
}

FileStamp stampFile(const std::string& path) {
  FileStamp s;
  std::error_code ec;
  auto status = fs::status(path, ec);
  if (ec || !fs::exists(status)) return s;
  s.exists = true;
  if (fs::is_regular_file(status)) s.size = (int64_t)fs::file_size(path, ec);
  auto t = fs::last_write_time(path, ec);
  if (!ec) s.mtime = t.time_since_epoch().count();
  return s;
}

}  // namespace synth::devapp
