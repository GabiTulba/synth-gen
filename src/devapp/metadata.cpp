#include "metadata.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.hpp"

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
