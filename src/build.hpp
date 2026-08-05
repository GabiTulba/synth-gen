#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "diagnostics.hpp"

namespace synth {

// The `.build` project manifest (design doc §8.1). v1 format, line-based:
//
//   # comment
//   project <name>
//   source <file.synth>
//   source <other.synth>
//
struct Manifest {
  std::string projectName;
  std::vector<std::string> sources;  // relative to the project directory
};

bool parseManifest(const std::string& text, const std::string& file,
                   Manifest& out, DiagnosticBag& diags);

struct TargetInfo {
  std::string name;
  std::string kind = "audio";  // "audio" (.wav) or "visual" (.svg)
  std::string artifact;  // path relative to project dir; empty on failure
  double rate = 0;
  int channelCount = 0;
  int64_t frames = 0;
  double durationSeconds = 0;
  bool ok = false;
  bool cached = false;  // reused from a previous build (Epic 8)
  std::string error;    // per-target failure, if any
};

// Cross-build cache (Epic 8). Purely automatic: keys are content hashes of
// each target's dependency closure (plus audio-input stamps and an engine
// version salt); a target whose key is unchanged and whose artifact still
// exists is not re-rendered. Bounded by construction - one entry per
// current render target; entries for removed targets are pruned each
// build. The daemon owns one of these across rebuilds.
struct BuildCache {
  struct Entry {
    uint64_t key = 0;
    TargetInfo info;
  };
  std::map<std::string, Entry> targets;  // by render name
};

struct BuildResult {
  bool ok = false;  // front-end + validation succeeded and all targets wrote
  Manifest manifest;
  std::vector<TargetInfo> targets;
  DiagnosticBag diags;
  std::string metadataPath;
  // Every file this build depended on: the manifest, all source modules
  // (listed and imported), and all loaded audio files. This is what the
  // daemon watches (§8.3).
  std::vector<std::string> inputs;
};

// One-shot build of the project in `projectDir` (§8.2): parse & check all
// sources, validate project rules, evaluate render targets, write artifacts
// to <projectDir>/build/artifacts/<name>.wav and metadata to
// <projectDir>/build/metadata.json. Metadata is written even for failed
// builds (§6.3-style error surfacing). Independent targets render in
// parallel across a thread pool (Epic 9). When `cache` is given it is
// consulted and updated (Epic 8); pass nullptr for a full rebuild.
BuildResult buildProject(const std::string& projectDir,
                         BuildCache* cache = nullptr);

// Front-end only (lint): parse + type-check the given files and their
// imports; no evaluation, no artifacts.
DiagnosticBag lintFiles(const std::vector<std::string>& files);

// The build daemon loop (§8.3): builds once, then watches the project's
// inputs (sources, .build, imported audio files) and rebuilds on change.
// `onBuild` is called after every build. Polling-based (portable, no
// dependencies); a whole-project rebuild per change is the acceptable v1
// (§12 Epic 6). Runs until `keepRunning` returns false.
void watchProject(const std::string& projectDir,
                  const std::function<void(const BuildResult&)>& onBuild,
                  const std::function<bool()>& keepRunning,
                  int pollMillis = 300);

}  // namespace synth
