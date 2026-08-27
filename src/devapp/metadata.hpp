#pragma once
#include <cstdint>
#include <map>
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

// A live control the build declared (Core.Control.slider/knob): the app
// shows it as a slider or knob and writes overrides for the daemon.
struct ControlMeta {
  std::string name;
  std::string kind;  // "slider" | "knob"
  double min = 0, max = 1;
  double def = 0;    // the declaration's default
  double value = 0;  // the value the last build used
};

struct ProjectMeta {
  std::string project;
  std::string status;  // "ok" | "error"
  std::vector<DiagnosticMeta> diagnostics;
  std::vector<TargetMeta> targets;
  std::vector<ControlMeta> controls;
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

// The unit's control-overrides file, next to its metadata.json. The app
// writes it, the build reads it, and - because the build records it as an
// input - an attached `synthc watch` instance rebuilds on every write.
std::string controlsPathFor(const std::string& metadataPath);

// Atomically (write temp + rename, so a mid-write daemon poll never sees
// a torn file) writes {"overrides": {"name": value, ...}}. An empty map
// still writes the file: that is how "back to defaults" reaches the
// daemon. Returns false with `error` set when the write fails.
bool writeControlOverrides(const std::string& path,
                           const std::map<std::string, double>& overrides,
                           std::string& error);

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
