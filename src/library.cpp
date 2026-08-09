#include "library.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#include "build.hpp"
#include "parser.hpp"

namespace fs = std::filesystem;

namespace synth {

namespace {

bool readFileText(const fs::path& p, std::string& out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

bool isHiddenName(const std::string& name) {
  return !name.empty() && name[0] == '.';
}

// A library's members are the `.synth` files sitting next to its
// manifest, minus the interface file - the same "everything in this
// directory" rule standalone files already follow, so siblings can import
// each other without any manifest bookkeeping.
void collectLibraryFiles(const fs::path& dir, LibraryInfo& info) {
  std::error_code ec;
  for (auto& entry : fs::directory_iterator(
           dir, fs::directory_options::skip_permission_denied, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    std::string name = entry.path().filename().string();
    if (isHiddenName(name) || entry.path().extension() != ".synth") continue;
    if (name == kLibraryInterfaceFile) {
      info.hasInterface = true;
      continue;
    }
    info.files.push_back(name);
  }
  std::sort(info.files.begin(), info.files.end());
}

}  // namespace

std::string LibraryInfo::fileForModule(const std::string& moduleName) const {
  for (auto& f : files)
    if (moduleNameForPath(f) == moduleName) return f;
  return {};
}

LibraryRegistry discoverLibraries(const std::string& rootDir,
                                  DiagnosticBag& diags) {
  LibraryRegistry reg;
  std::map<std::string, std::string> dirByName;  // for collision messages

  // Depth-first scan; skip output dirs ("_build", plus legacy/cmake
  // "build") and hidden entries.
  std::function<void(const fs::path&)> scan = [&](const fs::path& dir) {
    std::error_code ec;
    fs::path manifestPath = dir / kManifestFileName;
    if (fs::exists(manifestPath, ec)) {
      std::string text;
      if (readFileText(manifestPath, text)) {
        Manifest m;
        DiagnosticBag local;
        bool ok = parseManifest(text, manifestPath.string(), m, local);
        if (m.isLibrary() || text.find("\"library\"") != std::string::npos) {
          if (!ok) {
            // A broken library manifest breaks name resolution for
            // everyone; surface its diagnostics now.
            for (auto& d : local.items) diags.items.push_back(d);
          } else if (m.isLibrary()) {
            auto prev = dirByName.find(m.libraryName);
            if (prev != dirByName.end()) {
              diags.projectError("duplicate library name '" + m.libraryName +
                                 "' declared in '" + prev->second +
                                 "' and '" + dir.string() + "'");
            } else {
              LibraryInfo info;
              info.name = m.libraryName;
              info.dir = dir.string();
              info.deps = m.deps;
              collectLibraryFiles(dir, info);
              if (!info.hasInterface)
                diags.projectError(
                    "library '" + m.libraryName + "' in '" + dir.string() +
                    "' has no '" + kLibraryInterfaceFile +
                    "': a library's public surface is declared there (e.g. "
                    "'module Keys = Keys ;;')");
              dirByName[m.libraryName] = dir.string();
              reg.byName.emplace(info.name, std::move(info));
            }
          }
        }
        // Non-library manifests (projects, roots) are not registry
        // entries; their own builds diagnose any problems.
      }
    }
    for (auto& entry : fs::directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec)) {
      if (!entry.is_directory(ec)) continue;
      std::string name = entry.path().filename().string();
      if (name == "_build" || name == "build" || isHiddenName(name)) continue;
      scan(entry.path());
    }
  };
  std::error_code ec;
  if (fs::is_directory(rootDir, ec)) scan(fs::path(rootDir));

  // Validate dependencies: every dep must name a discovered library, and
  // the library dependency graph must be acyclic.
  for (auto& [name, lib] : reg.byName)
    for (auto& d : lib.deps)
      if (!reg.find(d))
        diags.projectError("library '" + name + "' depends on unknown "
                           "library '" + d + "'");
  std::map<std::string, int> state;  // 0 new, 1 visiting, 2 done
  std::function<void(const std::string&)> visit = [&](const std::string& n) {
    const LibraryInfo* lib = reg.find(n);
    if (!lib) return;
    int& st = state[n];
    if (st == 2) return;
    if (st == 1) {
      diags.projectError("library dependency cycle involving '" + n + "'");
      return;
    }
    st = 1;
    for (auto& d : lib->deps) visit(d);
    st = 2;
  };
  for (auto& [name, lib] : reg.byName) visit(name);
  return reg;
}

std::string findEnclosingRoot(const std::string& dir) {
  std::error_code ec;
  fs::path p = fs::absolute(fs::path(dir), ec);
  if (ec) p = fs::path(dir);
  for (; !p.empty(); p = p.parent_path()) {
    fs::path manifestPath = p / kManifestFileName;
    if (fs::exists(manifestPath, ec)) {
      std::string text;
      if (readFileText(manifestPath, text)) {
        Manifest m;
        DiagnosticBag local;
        parseManifest(text, manifestPath.string(), m, local);
        if (m.isRoot()) return p.string();
      }
    }
    if (p == p.root_path()) break;
  }
  return {};
}

}  // namespace synth
