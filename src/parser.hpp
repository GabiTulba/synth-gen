#pragma once
#include "ast.hpp"
#include "lexer.hpp"

namespace synth {

// Parses one source file into top-level definitions. Errors go to `diags`;
// the parser recovers at the next `;;` and keeps going.
std::vector<TopDef> parse(const std::vector<Token>& tokens,
                          const std::string& file, DiagnosticBag& diags);

// Derives a module name from a file path: "pluck.synth" -> "Pluck".
std::string moduleNameForPath(const std::string& path);

}  // namespace synth
