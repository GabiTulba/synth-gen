#pragma once
#include <map>
#include <string>
#include <vector>

#include "diagnostics.hpp"

namespace synth {

// The interface file every library must have: its module *is* the
// library (library `Basic` -> module `Basic`), and what it binds is the
// library's public surface.
inline constexpr const char* kLibraryInterfaceFile = "lib.synth";

// A library discovered from a `library` manifest: its declared name, the
// directory holding the manifest, its member files and its declared
// library dependencies. Members are every `.synth` file in that
// directory except the interface file: `keys.synth` in library `Basic`
// is module `Basic.Keys`. Members are all mutually importable by short
// name; the public surface is whatever `lib.synth` binds (see
// CheckedModule::exportedModules).
struct LibraryInfo {
  std::string name;
  std::string dir;                 // directory containing the build.json
  std::vector<std::string> files;  // member file names (sorted), no lib.synth
  bool hasInterface = false;       // a readable lib.synth exists
  std::vector<std::string> deps;   // library names

  // Relative path of the member file whose module name (capitalized stem)
  // is `moduleName`, or empty if the library has no such member.
  std::string fileForModule(const std::string& moduleName) const;
};

// The set of libraries discovered under a project root, keyed by declared
// name. Populated by discoverLibraries; duplicate names, unknown deps and
// dependency cycles are diagnosed there.
struct LibraryRegistry {
  std::map<std::string, LibraryInfo> byName;
  const LibraryInfo* find(const std::string& name) const {
    auto it = byName.find(name);
    return it == byName.end() ? nullptr : &it->second;
  }
  bool empty() const { return byName.empty(); }
};

// Recursively scan `rootDir` for `build.json` files - skipping output
// directories ("_build", legacy "build") and hidden directories - and
// collect every `library` manifest into a registry, listing each
// library's directory to find its members. Malformed manifests that
// declare a library are diagnosed; other manifests are left for their own
// builds to report. Duplicate library names, a missing `lib.synth`,
// unknown dependency names and library dependency cycles are diagnosed
// here.
LibraryRegistry discoverLibraries(const std::string& rootDir,
                                  DiagnosticBag& diags);

// Walk upward from `dir` looking for a root manifest (a build.json with
// 'build' rules). Returns the directory containing it, or "" if none.
std::string findEnclosingRoot(const std::string& dir);

// Directory holding the bundled standard library sources (the Core
// library lives at <stdlib>/core). Resolution: $SYNTH_STDLIB_DIR, then
// the build-time source location.
std::string bundledStdlibDir();

}  // namespace synth
