#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace synth {

// Byte range [lo, hi) into a single source file.
struct Span {
  uint32_t lo = 0;
  uint32_t hi = 0;
};

enum class Severity { Error, Warning };

struct Diagnostic {
  Severity severity = Severity::Error;
  std::string file;   // path as given to the front-end; empty for project-level
  Span span{};        // meaningless when file is empty
  std::string message;
};

// Line/column (1-based) resolution and human-readable rendering of
// diagnostics against the file contents they refer to.
struct LineCol {
  uint32_t line = 1;
  uint32_t col = 1;
};

LineCol lineColAt(const std::string& source, uint32_t offset);

// Renders "file:line:col: error: message" plus the offending source line
// with a caret marker. `source` may be empty (project-level diagnostics).
std::string renderDiagnostic(const Diagnostic& d, const std::string& source);

struct DiagnosticBag {
  std::vector<Diagnostic> items;

  void error(std::string file, Span span, std::string msg) {
    items.push_back({Severity::Error, std::move(file), span, std::move(msg)});
  }
  void projectError(std::string msg) {
    items.push_back({Severity::Error, {}, {}, std::move(msg)});
  }
  bool hasErrors() const {
    for (auto& d : items)
      if (d.severity == Severity::Error) return true;
    return false;
  }
};

}  // namespace synth
