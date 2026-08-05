#pragma once
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
  std::string artifact;  // path relative to project dir; empty on failure
  double rate = 0;
  int channelCount = 0;
  int64_t frames = 0;
  double durationSeconds = 0;
  bool ok = false;
  std::string error;  // per-target failure, if any
};

struct BuildResult {
  bool ok = false;  // front-end + validation succeeded and all targets wrote
  Manifest manifest;
  std::vector<TargetInfo> targets;
  DiagnosticBag diags;
  std::string metadataPath;
};

// One-shot build of the project in `projectDir` (§8.2): parse & check all
// sources, validate project rules, evaluate render targets, write artifacts
// to <projectDir>/build/artifacts/<name>.wav and metadata to
// <projectDir>/build/metadata.json. Metadata is written even for failed
// builds (§6.3-style error surfacing).
BuildResult buildProject(const std::string& projectDir);

// Front-end only (lint): parse + type-check the given files and their
// imports; no evaluation, no artifacts.
DiagnosticBag lintFiles(const std::vector<std::string>& files);

}  // namespace synth
