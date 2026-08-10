#include "metadata.hpp"

#include <filesystem>
#include <fstream>
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
  r.ok = true;
  return r;
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
