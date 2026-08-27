#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "diagnostics.hpp"
#include "library.hpp"

namespace synth {

struct SampleCache;  // signal.hpp; opaque here

// The build.json manifest (design doc §8.1). A single JSON object:
//
//   {
//     "project": "<name>",          project OR root (with "build" rules)
//     "library": "<Name>",          declares a library (capitalized name)
//     "description": "free text",   optional, ignored by the build
//     "sources": ["file.synth"],    project only: its member files
//     "dependencies": ["<Name>"],   library dependencies, by declared name
//     "build": ["path"]             root only: rules (.synth file or a
//                                   directory containing a build.json)
//   }
//
// Exactly one of "project"/"library" per manifest. A "project" manifest
// with "build" rules is a root (an orchestrator: no "sources"); a
// "project" manifest with "sources" is a standalone project. A "library"
// manifest lists no files: every `.synth` file in its directory is a
// member, and its `lib.synth` declares the public surface.
inline constexpr const char* kManifestFileName = "build.json";
struct Manifest {
  std::string projectName;
  std::string libraryName;
  std::vector<std::string> sources;  // relative; projects only
  std::vector<std::string> deps;     // library names
  std::vector<std::string> buildRules;  // root only: rule paths (relative)
  bool isLibrary() const { return !libraryName.empty(); }
  bool isRoot() const { return !buildRules.empty(); }
};

bool parseManifest(const std::string& text, const std::string& file,
                   Manifest& out, DiagnosticBag& diags);

struct TargetInfo {
  std::string name;
  std::string kind = "audio";  // "audio" (.wav) or "visual" (.svg)
  std::string artifact;  // path relative to the root dir; empty on failure
  double rate = 0;
  int channelCount = 0;
  int64_t frames = 0;
  double durationSeconds = 0;
  double renderMillis = 0;  // evaluate+discretize+write time (0 if cached)
  bool ok = false;
  bool cached = false;  // reused from a previous build (Epic 8)
  std::string error;    // per-target failure, if any
};

// A live control declared by the build (Core.Control.slider/knob): a
// named build-time Scalar parameter the dev app can override between
// rebuilds. Overrides live in the unit's controls.json (next to its
// metadata.json, format {"overrides": {"name": value, ...}}); the file
// is a build input, so a running daemon rebuilds when the dev app
// writes it.
struct ControlInfo {
  std::string name;
  std::string kind = "slider";  // "slider" | "knob" | "multi_slider"
  double min = 0, max = 1;
  double defaultValue = 0;
  double value = 0;  // what this build used: override (clamped) or default
  // "multi_slider" lanes only: the group this lane belongs to, its
  // position in it, and the bounds the group's sum satisfies. `name` is
  // "<group>.<lane>", so a lane overrides like any other control.
  std::string group;
  int groupIndex = -1;
  double sumMin = 0, sumMax = 0;
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
  std::vector<ControlInfo> controls;  // declaration order
  DiagnosticBag diags;
  std::string metadataPath;
  std::string controlsPath;  // where control overrides are read from
  // Every file this build depended on: the manifest, all source modules
  // (listed and imported), and all loaded audio files. This is what the
  // daemon watches (§8.3).
  std::vector<std::string> inputs;
};

// Build configuration. `log` is the streaming build log: phase timings,
// per-target worker/duration lines, and dependency statistics. It may be
// called from several render workers concurrently but calls are
// serialized internally; leave it null for a silent build.
struct BuildOptions {
  BuildCache* cache = nullptr;
  // Optional cross-build sample-window cache (see makeSampleCache). The
  // daemon passes one that survives rebuilds: structurally unchanged
  // samples then never re-render, even when their targets do. When
  // omitted, buildProject still shares a cache across the build's own
  // targets.
  SampleCache* sampleCache = nullptr;
  std::function<void(const std::string&)> log;
};

// One-shot build of the project in `projectDir` (§8.2): parse & check all
// sources, validate project rules, evaluate render targets, write artifacts
// to <root>/_build/<unit>/artifacts/<name>.wav and metadata to
// <root>/_build/<unit>/metadata.json, where <root> is the enclosing
// project root (or the project dir itself when standalone) and <unit>
// is the project's path relative to it. Metadata is written even for failed
// builds (§6.3-style error surfacing). Independent targets render in
// parallel across a thread pool (Epic 9). When a cache is given it is
// consulted and updated (Epic 8).
BuildResult buildProject(const std::string& projectDir,
                         const BuildOptions& options);
BuildResult buildProject(const std::string& projectDir,
                         BuildCache* cache = nullptr);

// A root build: the root manifest's `build` rules built in order, each
// into its own directory under the root's _build/, against the
// dynamically discovered library registry.
struct RootBuildResult {
  bool ok = false;  // every rule built (and the root itself was valid)
  Manifest manifest;       // the root manifest
  DiagnosticBag diags;     // root-level: manifest, discovery, registry
  LibraryRegistry registry;
  std::vector<std::pair<std::string, BuildResult>> rules;  // rule -> result
};

// Build the root at `rootDir`: parse the root manifest, discover all
// libraries under the root, then build every `build` rule (a directory
// with a build.json - project or library - or a single .synth file).
// Each rule's outputs land under <rootDir>/_build/, mirroring the rule
// path (file rules drop their extension). `ruleCaches`, when given,
// holds one incremental cache per rule (the root daemon owns it);
// otherwise rules build uncached.
RootBuildResult buildRoot(const std::string& rootDir,
                          const BuildOptions& options,
                          std::map<std::string, BuildCache>* ruleCaches =
                              nullptr);

// Front-end only (lint): parse + type-check the given files and their
// imports; no evaluation, no artifacts.
DiagnosticBag lintFiles(const std::vector<std::string>& files);

// The build daemon loop (§8.3): builds once, then watches the project's
// inputs (sources, build.json, imported audio files) and rebuilds on
// change.
// `onBuild` is called after every build. Polling-based (portable, no
// dependencies); a whole-project rebuild per change is the acceptable v1
// (§12 Epic 6). Runs until `keepRunning` returns false.
void watchProject(const std::string& projectDir,
                  const std::function<void(const BuildResult&)>& onBuild,
                  const std::function<bool()>& keepRunning,
                  int pollMillis = 300,
                  std::function<void(const std::string&)> log = {});

// The root daemon: builds all of the root's rules, then watches the whole
// tree - every rule's inputs, every directory (for created/removed files)
// and every build.json manifest (a change re-runs library discovery) - and
// rebuilds on change. Per-rule incremental caches and a shared sample
// cache persist across rebuilds.
void watchRoot(const std::string& rootDir,
               const std::function<void(const RootBuildResult&)>& onBuild,
               const std::function<bool()>& keepRunning,
               int pollMillis = 300,
               std::function<void(const std::string&)> log = {});

}  // namespace synth
