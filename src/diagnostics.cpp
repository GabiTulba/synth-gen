#include "diagnostics.hpp"

#include <sstream>

namespace synth {

LineCol lineColAt(const std::string& source, uint32_t offset) {
  LineCol lc;
  uint32_t n = std::min<uint32_t>(offset, (uint32_t)source.size());
  for (uint32_t i = 0; i < n; i++) {
    if (source[i] == '\n') {
      lc.line++;
      lc.col = 1;
    } else {
      lc.col++;
    }
  }
  return lc;
}

std::string renderDiagnostic(const Diagnostic& d, const std::string& source) {
  std::ostringstream out;
  const char* sev = d.severity == Severity::Error ? "error" : "warning";
  if (d.file.empty()) {
    out << sev << ": " << d.message << "\n";
    return out.str();
  }
  LineCol lc = lineColAt(source, d.span.lo);
  out << d.file << ":" << lc.line << ":" << lc.col << ": " << sev << ": "
      << d.message << "\n";
  // Extract the offending line.
  uint32_t lineStart = d.span.lo > source.size() ? (uint32_t)source.size()
                                                 : d.span.lo;
  while (lineStart > 0 && source[lineStart - 1] != '\n') lineStart--;
  uint32_t lineEnd = lineStart;
  while (lineEnd < source.size() && source[lineEnd] != '\n') lineEnd++;
  if (lineEnd > lineStart) {
    out << "  " << source.substr(lineStart, lineEnd - lineStart) << "\n  ";
    for (uint32_t i = lineStart; i < d.span.lo && i < lineEnd; i++)
      out << (source[i] == '\t' ? '\t' : ' ');
    uint32_t caretLen = d.span.hi > d.span.lo ? d.span.hi - d.span.lo : 1;
    uint32_t maxLen = lineEnd > d.span.lo ? lineEnd - d.span.lo : 1;
    caretLen = std::min(caretLen, maxLen);
    for (uint32_t i = 0; i < std::max<uint32_t>(caretLen, 1); i++) out << '^';
    out << "\n";
  }
  return out.str();
}

}  // namespace synth
