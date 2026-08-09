#pragma once
#include <string>
#include <vector>

// Builders for build.json manifest text used across the test suites, so
// individual tests stay one-liners and don't hand-quote JSON.
namespace {

inline std::string jsonStringArray(const std::vector<std::string>& items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) out += ", ";
    out += "\"" + items[i] + "\"";
  }
  return out + "]";
}

inline std::string projectManifest(const std::string& name,
                                   const std::vector<std::string>& sources,
                                   const std::vector<std::string>& deps = {}) {
  std::string out = "{ \"project\": \"" + name + "\"";
  if (!deps.empty()) out += ",\n  \"dependencies\": " + jsonStringArray(deps);
  out += ",\n  \"sources\": " + jsonStringArray(sources);
  return out + " }\n";
}

// A library manifest names the library and its dependencies; its members
// are the .synth files in the directory and its public surface is
// declared in lib.synth (see libraryInterface).
inline std::string libraryManifest(const std::string& name,
                                   const std::vector<std::string>& deps = {}) {
  std::string out = "{ \"library\": \"" + name + "\"";
  if (!deps.empty()) out += ",\n  \"dependencies\": " + jsonStringArray(deps);
  return out + " }\n";
}

// A lib.synth exposing the named modules verbatim (`module X = X ;;`).
inline std::string libraryInterface(const std::vector<std::string>& modules) {
  std::string out;
  for (auto& m : modules) out += "module " + m + " = " + m + " ;;\n";
  return out;
}

inline std::string rootManifest(const std::string& name,
                                const std::vector<std::string>& rules) {
  return "{ \"project\": \"" + name +
         "\",\n  \"build\": " + jsonStringArray(rules) + " }\n";
}

}  // namespace
