#pragma once
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "checker.hpp"

namespace synth {

// A Language Server Protocol server over the compiler front-end - the
// editor-integration counterpart of `synthc lint`. Speaks JSON-RPC; the
// stdio framing lives in runLspServer so tests can drive the server with
// plain message bodies.
//
// Supported: diagnostics (published on open/change), go-to-definition,
// completion (unqualified names in scope and `Module.` members), and
// hover (the checked type of the identifier under the cursor). Documents
// use full-text sync; unsaved buffers are layered over the on-disk
// project via ModuleLoadContext::overlay, so imports and libraries
// resolve exactly as a build would see them after a save.
class LspServer {
 public:
  // Handles one JSON-RPC message body (no framing). Returns the message
  // bodies to send back, in order: the response to a request and/or
  // server-initiated notifications (publishDiagnostics).
  std::vector<std::string> onMessage(const std::string& body);

  bool exitRequested() const { return exit_; }

 private:
  struct Document {
    std::string path;  // filesystem path from the uri
    std::string key;   // canonicalSourceKey(path)
    std::string text;
  };
  struct Analysis {
    Program program;
    DiagnosticBag diags;
  };

  std::map<std::string, Document> docs_;   // by uri
  std::map<std::string, Analysis> cache_;  // by uri; cleared on any edit
  bool shutdown_ = false;
  bool exit_ = false;

  Analysis& analysisFor(const std::string& uri);
  std::string publishDiagnostics(const std::string& uri);
};

// Blocking Content-Length-framed JSON-RPC loop over the given streams
// (stdin/stdout for `synthc lsp`). Returns the process exit code.
int runLspServer(std::istream& in, std::ostream& out);

}  // namespace synth
