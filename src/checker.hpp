#pragma once
#include <map>
#include <string>
#include <vector>

#include "ast.hpp"
#include "diagnostics.hpp"
#include "library.hpp"

namespace synth {

struct CheckedModule {
  ParsedModule parsed;  // parsed.name is the CANONICAL module id:
                        // "Lib.File" for library members, "File" otherwise
  // Types of top-level lets (constants have their value type; functions a
  // Fun type). Only named bindings appear here; `let _` targets do not.
  std::map<std::string, TypePtr> defTypes;
  std::vector<std::string> imports;  // canonical ids of load dependencies
  // Surface module path -> canonical module id, built from this file's
  // imports (and, during checking, opens/aliases): "Keys" -> "Basic.Keys"
  // inside library Basic, "Basic.Keys" -> "Basic.Keys", ...
  std::map<std::string, std::string> moduleScope;
  // Whole-library imports: surface library name -> declared library name.
  std::map<std::string, std::string> importedLibs;
  // The library this module belongs to ("" for standalone/project files).
  std::string libName;
  // True for modules pulled in from dependency libraries: their `let _`
  // render effects belong to the library's own build, not this one.
  bool external = false;
};

struct Program {
  // Modules in dependency (topological) order: imports come before importers.
  std::vector<CheckedModule> modules;

  const CheckedModule* find(const std::string& name) const {
    for (auto& m : modules)
      if (m.parsed.name == name) return &m;
    return nullptr;
  }
};

// Library context for a check: the discovered registry, the library whose
// own files are being checked (null for project builds), and the declared
// library dependencies of the unit being built.
struct ModuleLoadContext {
  const LibraryRegistry* registry = nullptr;
  const LibraryInfo* currentLib = nullptr;
  std::vector<std::string> deps;
};

// Loads, parses and type-checks `rootFiles` plus everything they import.
// Short-name imports resolve within the current library's file set, or by
// today's same-directory rule for standalone files; library imports
// (`import Lib`, `import Lib.File`) resolve through the context's registry
// with `dep` and `expose` enforcement. Paths in diagnostics are as given /
// derived. Returns the (possibly partially) checked program; check
// diags.hasErrors().
Program checkProject(const std::vector<std::string>& rootFiles,
                     DiagnosticBag& diags,
                     const ModuleLoadContext* ctx);
Program checkProject(const std::vector<std::string>& rootFiles,
                     DiagnosticBag& diags);

}  // namespace synth
