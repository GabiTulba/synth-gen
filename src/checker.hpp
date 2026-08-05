#pragma once
#include <map>
#include <string>
#include <vector>

#include "ast.hpp"
#include "diagnostics.hpp"

namespace synth {

struct CheckedModule {
  ParsedModule parsed;
  // Types of top-level lets (constants have their value type; functions a
  // Fun type). Only named bindings appear here; `let _` targets do not.
  std::map<std::string, TypePtr> defTypes;
  std::vector<std::string> imports;  // module names, resolved
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

// Loads, parses and type-checks `rootFiles` plus everything they import
// (same-directory rule). Paths in diagnostics are as given / derived.
// Returns the (possibly partially) checked program; check diags.hasErrors().
Program checkProject(const std::vector<std::string>& rootFiles,
                     DiagnosticBag& diags);

}  // namespace synth
