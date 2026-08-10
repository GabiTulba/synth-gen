#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace synth::devapp {

// The dev app's model of a project: everything comes from the build
// metadata file (§9 — the app is a pure consumer of build outputs and has
// no language knowledge).

struct TargetMeta {
  std::string name;
  std::string kind;      // "audio" | "visual"
  std::string status;    // "ok" | "error"
  std::string artifact;  // path relative to the project directory
  std::string error;
  double rate = 0;
  double durationSeconds = 0;
  int channels = 0;
  int64_t frames = 0;
};

struct DiagnosticMeta {
  std::string severity;
  std::string file;
  std::string message;
  std::string rendered;
};

struct ProjectMeta {
  std::string project;
  std::string status;  // "ok" | "error"
  std::vector<DiagnosticMeta> diagnostics;
  std::vector<TargetMeta> targets;
};

struct MetadataLoadResult {
  bool ok = false;
  std::string error;  // set when !ok (missing file, bad JSON, ...)
  ProjectMeta meta;
};

// Reads and parses a build metadata.json file (under the root's _build/).
MetadataLoadResult loadProjectMetadata(const std::string& path);

// One buildable unit whose metadata the app shows. A standalone project
// (or a project inside a root) is a single unit; a root is one unit per
// `build` rule.
struct MetadataUnit {
  std::string label;         // the rule path for root units, else empty
  std::string metadataPath;  // where the build writes this unit's metadata
};

struct MetadataLayout {
  std::string rootDir;  // artifact paths in metadata are relative to this
  std::string manifestPath;  // re-resolve the layout when this changes
  std::vector<MetadataUnit> units;
};

// Maps a project directory to the metadata file(s) its builds write,
// mirroring the build system's output layout (build.hpp §8.2): a project
// builds to <root>/_build/<rel>/metadata.json; a root manifest builds one
// _build/<rule>/metadata.json per rule, file rules dropping their
// extension.
MetadataLayout resolveMetadataLayout(const std::string& projectDir);

// Change detection for live refresh (§9): the v1 mechanism is watching the
// metadata file, per the design doc's "simplest v1" note.
struct FileStamp {
  bool exists = false;
  int64_t size = 0;
  int64_t mtime = 0;
  bool operator==(const FileStamp&) const = default;
};
FileStamp stampFile(const std::string& path);

}  // namespace synth::devapp
