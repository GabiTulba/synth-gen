#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "diagnostics.hpp"

namespace synth {

// A library discovered from a `library` manifest: its declared name, the
// directory holding the .build, its member files, the exposed (public)
// subset, and its declared library dependencies. Member files are module
// files: `keys.synth` in library `Basic` is module `Basic.Keys`.
struct LibraryInfo {
  std::string name;
  std::string dir;                 // directory containing the .build
  std::vector<std::string> files;  // relative paths (manifest order)
  std::set<std::string> exposedFiles;  // relative paths, subset of files
  std::vector<std::string> deps;       // library names

  // Relative path of the member file whose module name (capitalized stem)
  // is `moduleName`, or empty if the library has no such member.
  std::string fileForModule(const std::string& moduleName) const;
  bool isExposedModule(const std::string& moduleName) const;
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

// Recursively scan `rootDir` for `.build` files - skipping directories
// named "build" (output dirs) and hidden directories - and collect every
// `library` manifest into a registry. Malformed manifests that declare a
// library are diagnosed; other manifests are left for their own builds to
// report. Duplicate library names, unknown `dep` names and library
// dependency cycles are diagnosed here.
LibraryRegistry discoverLibraries(const std::string& rootDir,
                                  DiagnosticBag& diags);

// Walk upward from `dir` looking for a root manifest (a .build with
// 'build' rules). Returns the directory containing it, or "" if none.
std::string findEnclosingRoot(const std::string& dir);

}  // namespace synth
