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

inline std::string libraryManifest(const std::string& name,
                                   const std::vector<std::string>& expose,
                                   const std::vector<std::string>& sources = {},
                                   const std::vector<std::string>& deps = {}) {
  std::string out = "{ \"library\": \"" + name + "\"";
  if (!expose.empty()) out += ",\n  \"expose\": " + jsonStringArray(expose);
  if (!sources.empty()) out += ",\n  \"sources\": " + jsonStringArray(sources);
  if (!deps.empty()) out += ",\n  \"dependencies\": " + jsonStringArray(deps);
  return out + " }\n";
}

inline std::string rootManifest(const std::string& name,
                                const std::vector<std::string>& rules) {
  return "{ \"project\": \"" + name +
         "\",\n  \"build\": " + jsonStringArray(rules) + " }\n";
}

}  // namespace
