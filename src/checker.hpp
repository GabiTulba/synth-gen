#pragma once
#include <map>
#include <set>
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
  // A definition inside an inline module (`module A = struct ... end`)
  // appears under its dotted path from the file root: "A.x", "A.B.y".
  std::map<std::string, TypePtr> defTypes;
  // Dotted paths (from the file root) of every inline module this file
  // defines: "A", "A.B". Lets qualified references and `open` reach into
  // them, here and from other modules.
  std::set<std::string> inlineModules;
  std::vector<std::string> imports;  // canonical ids of load dependencies
  // Surface module path -> canonical module id, built from this file's
  // imports (and, during checking, opens/aliases): "Keys" -> "Basic.Keys"
  // inside library Basic, "Basic.Keys" -> "Basic.Keys", ...
  std::map<std::string, std::string> moduleScope;
  // This module's *public* module bindings: every `module X = Path` at
  // top level, as surface name -> canonical module id. For a library's
  // `lib.synth` (whose module id is the library name) this is the
  // library's exposed module surface: `module Keys = Keys` in Basic's
  // lib.synth is what makes `Basic.Keys` reachable from outside. For any
  // other module it is what `open`ing it re-exports.
  std::map<std::string, std::string> exportedModules;
  // The library this module belongs to ("" for standalone/project files).
  // A library's interface module (lib.synth) carries it too: its own
  // canonical id *is* the library name.
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
// Short-name imports resolve by the same-directory rule - inside a
// library that is its own directory, so members freely import each other;
// library imports (`import Lib`, `import Lib.File`) resolve through the
// context's registry with `dep` enforcement, and `Lib.File` additionally
// has to be bound by `Lib`'s lib.synth. Paths in diagnostics are as given
// / derived. Returns the (possibly partially) checked program; check
// diags.hasErrors().
Program checkProject(const std::vector<std::string>& rootFiles,
                     DiagnosticBag& diags,
                     const ModuleLoadContext* ctx);
Program checkProject(const std::vector<std::string>& rootFiles,
                     DiagnosticBag& diags);

}  // namespace synth
