#include "lsp.hpp"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "checker.hpp"
#include "json.hpp"
#include "test_framework.hpp"

using namespace synth;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path dir;
  TempDir() {
    static int counter = 0;
    dir = fs::temp_directory_path() /
          ("synthgraph-lsp-test-" + std::to_string(::getpid()) + "-" +
           std::to_string(counter++));
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  fs::path write(const std::string& name, const std::string& text) {
    fs::path p = dir / name;
    std::ofstream out(p);
    out << text;
    return p;
  }
};

std::string uriFor(const fs::path& p) {
  // Test paths contain no characters that need percent-encoding.
  return "file://" + canonicalSourceKey(p.string());
}

// 0-based line/character of the `occurrence`-th appearance of `needle`
// (ASCII sources, so characters == bytes).
struct LC {
  int line = 0;
  int ch = 0;
};

LC lcOf(const std::string& text, const std::string& needle,
        int occurrence = 0) {
  size_t pos = 0;
  for (int i = 0;; i++) {
    pos = text.find(needle, pos);
    CHECK(pos != std::string::npos);
    if (i == occurrence) break;
    pos++;
  }
  LC lc;
  size_t lineStart = 0;
  for (size_t j = 0; j < pos; j++)
    if (text[j] == '\n') {
      lc.line++;
      lineStart = j + 1;
    }
  lc.ch = (int)(pos - lineStart);
  return lc;
}

std::string didOpen(const std::string& uri, const std::string& text) {
  json::Value td = json::makeObject();
  td.set("uri", json::makeString(uri));
  td.set("languageId", json::makeString("synth"));
  td.set("version", json::makeNumber(1));
  td.set("text", json::makeString(text));
  json::Value params = json::makeObject();
  params.set("textDocument", std::move(td));
  json::Value msg = json::makeObject();
  msg.set("jsonrpc", json::makeString("2.0"));
  msg.set("method", json::makeString("textDocument/didOpen"));
  msg.set("params", std::move(params));
  return json::serialize(msg);
}

std::string positionRequest(int id, const std::string& method,
                            const std::string& uri, LC lc) {
  json::Value td = json::makeObject();
  td.set("uri", json::makeString(uri));
  json::Value pos = json::makeObject();
  pos.set("line", json::makeNumber(lc.line));
  pos.set("character", json::makeNumber(lc.ch));
  json::Value params = json::makeObject();
  params.set("textDocument", std::move(td));
  params.set("position", std::move(pos));
  json::Value msg = json::makeObject();
  msg.set("jsonrpc", json::makeString("2.0"));
  msg.set("id", json::makeNumber(id));
  msg.set("method", json::makeString(method));
  msg.set("params", std::move(params));
  return json::serialize(msg);
}

json::Value parseOne(const std::vector<std::string>& msgs) {
  CHECK(msgs.size() == 1);
  json::Value v;
  std::string err;
  CHECK(json::parse(msgs[0], v, err));
  return v;
}

// The "result" of the single response returned for a request.
json::Value resultOf(LspServer& server, const std::string& request) {
  json::Value v = parseOne(server.onMessage(request));
  const json::Value* result = v.get("result");
  CHECK(result != nullptr);
  return *result;
}

bool hasCompletion(const json::Value& items, const std::string& label) {
  for (auto& item : items.array)
    if (item.getString("label") == label) return true;
  return false;
}

int rangeStartLine(const json::Value& loc) {
  const json::Value* range = loc.get("range");
  CHECK(range != nullptr);
  const json::Value* start = range->get("start");
  CHECK(start != nullptr);
  return (int)start->getNumber("line", -1);
}

int rangeStartChar(const json::Value& loc) {
  const json::Value* range = loc.get("range");
  const json::Value* start = range->get("start");
  return (int)start->getNumber("character", -1);
}

const char* kSource =
    "open Core\n"
    "open Core.Osc\n"
    "\n"
    "let base_freq : Scalar = 220.0 ;;\n"
    "\n"
    "let voice ~gain:Scalar : Scalar Signal =\n"
    "  (sine base_freq) * gain\n"
    ";;\n"
    "\n"
    "let doubled : Scalar Signal =\n"
    "  let g : Scalar = 2.0 in\n"
    "  voice ~gain:g\n"
    ";;\n";

}  // namespace

TEST(lsp_initialize_capabilities) {
  LspServer server;
  json::Value v = parseOne(server.onMessage(
      R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"));
  const json::Value* result = v.get("result");
  CHECK(result != nullptr);
  const json::Value* caps = result->get("capabilities");
  CHECK(caps != nullptr);
  CHECK(caps->get("completionProvider") != nullptr);
  CHECK(caps->get("definitionProvider") != nullptr);
  CHECK(caps->get("hoverProvider") != nullptr);
}

TEST(lsp_publishes_diagnostics_on_open_and_change) {
  TempDir tmp;
  fs::path p = tmp.write("bad.synth", "");
  std::string uri = uriFor(p);
  LspServer server;
  // A type error: the annotation says Scalar, the body is a Timestamp.
  json::Value note =
      parseOne(server.onMessage(didOpen(uri, "let x : Scalar = 1s ;;\n")));
  CHECK(note.getString("method") == "textDocument/publishDiagnostics");
  const json::Value* params = note.get("params");
  CHECK(params != nullptr);
  CHECK(params->getString("uri") == uri);
  const json::Value* diags = params->get("diagnostics");
  CHECK(diags != nullptr);
  CHECK(diags->array.size() == 1);
  CHECK(diags->array[0].getNumber("severity") == 1);
  CHECK(diags->array[0].getString("message").find("Scalar") !=
        std::string::npos);

  // Fixing the file clears the diagnostics.
  json::Value fixed = parseOne(server.onMessage(
      R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{)"
      R"("textDocument":{"uri":")" + uri + R"("},)"
      R"("contentChanges":[{"text":"let x : Scalar = 1.0 ;;\n"}]}})"));
  const json::Value* diags2 = fixed.get("params")->get("diagnostics");
  CHECK(diags2 != nullptr);
  CHECK(diags2->array.empty());
}

TEST(lsp_definition_of_sibling_and_local_names) {
  TempDir tmp;
  fs::path p = tmp.write("song.synth", "");
  std::string uri = uriFor(p);
  std::string text = kSource;
  LspServer server;
  server.onMessage(didOpen(uri, text));

  // `base_freq` used inside `voice` resolves to its top-level definition.
  json::Value loc = resultOf(
      server, positionRequest(2, "textDocument/definition", uri,
                              lcOf(text, "base_freq", 1)));
  CHECK(loc.getString("uri") == uri);
  CHECK(rangeStartLine(loc) == lcOf(text, "base_freq", 0).line);
  CHECK(rangeStartChar(loc) == lcOf(text, "base_freq", 0).ch);

  // `gain` in the body resolves to the parameter.
  json::Value pl = resultOf(server,
                            positionRequest(3, "textDocument/definition", uri,
                                            lcOf(text, "gain", 1)));
  CHECK(pl.getString("uri") == uri);
  CHECK(rangeStartLine(pl) == lcOf(text, "gain", 0).line);

  // `g` resolves to the `let g : Scalar = 2.0 in` binder.
  json::Value gl = resultOf(server,
                            positionRequest(4, "textDocument/definition", uri,
                                            lcOf(text, "gain:g", 0)));
  // Position the cursor on the `g` argument itself.
  LC gUse = lcOf(text, "gain:g", 0);
  gUse.ch += 5;
  gl = resultOf(server,
                positionRequest(5, "textDocument/definition", uri, gUse));
  CHECK(gl.getString("uri") == uri);
  CHECK(rangeStartLine(gl) == lcOf(text, "let g :", 0).line);

  // `voice` in `doubled` resolves to the `voice` definition.
  json::Value vl = resultOf(server,
                            positionRequest(6, "textDocument/definition", uri,
                                            lcOf(text, "voice ~gain:g", 0)));
  CHECK(vl.getString("uri") == uri);
  CHECK(rangeStartLine(vl) == lcOf(text, "let voice", 0).line);
}

TEST(lsp_definition_into_core) {
  TempDir tmp;
  fs::path p = tmp.write("song.synth", "");
  std::string uri = uriFor(p);
  std::string text = kSource;
  LspServer server;
  server.onMessage(didOpen(uri, text));

  // `sine` (brought in by `open Core.Osc`) resolves into the bundled
  // Core interface file.
  json::Value loc = resultOf(server,
                             positionRequest(2, "textDocument/definition",
                                             uri, lcOf(text, "sine", 0)));
  std::string target = loc.getString("uri");
  CHECK(target.find("lib.synth") != std::string::npos);
  CHECK(rangeStartLine(loc) > 0);
}

TEST(lsp_completion_qualified_and_unqualified) {
  TempDir tmp;
  fs::path p = tmp.write("song.synth", "");
  std::string uri = uriFor(p);
  // The cursor sits right after "Osc." on the last line.
  std::string text =
      "import Core\n"
      "open Core.Fx\n"
      "let mine : Scalar = 1.0 ;;\n"
      "let x : Scalar Signal = Core.Osc.\n";
  LspServer server;
  server.onMessage(didOpen(uri, text));

  LC afterDot = lcOf(text, "Core.Osc.", 0);
  afterDot.ch += (int)std::string("Core.Osc.").size();
  json::Value items = resultOf(
      server, positionRequest(2, "textDocument/completion", uri, afterDot));
  CHECK(hasCompletion(items, "sine"));
  CHECK(hasCompletion(items, "fm"));
  CHECK(!hasCompletion(items, "lowpass"));  // lives in Core.Fx

  // Unqualified: opened names, own definitions, modules and keywords.
  LC atEnd = lcOf(text, "= Core.Osc.", 0);
  json::Value bare = resultOf(
      server, positionRequest(3, "textDocument/completion", uri, atEnd));
  CHECK(hasCompletion(bare, "lowpass"));  // from `open Core.Fx`
  CHECK(hasCompletion(bare, "mine"));     // earlier definition
  CHECK(hasCompletion(bare, "Core"));     // imported module
  CHECK(hasCompletion(bare, "let"));      // keyword
  CHECK(!hasCompletion(bare, "sine"));    // Osc was never opened
}

TEST(lsp_completion_module_members_after_open_core) {
  TempDir tmp;
  fs::path p = tmp.write("song.synth", "");
  std::string uri = uriFor(p);
  std::string text =
      "open Core\n"
      "let x : Scalar Signal = Osc.\n";
  LspServer server;
  server.onMessage(didOpen(uri, text));
  LC afterDot = lcOf(text, "Osc.", 0);
  afterDot.ch += 4;
  json::Value items = resultOf(
      server, positionRequest(2, "textDocument/completion", uri, afterDot));
  CHECK(hasCompletion(items, "sine"));
  CHECK(hasCompletion(items, "noise"));
}

TEST(lsp_hover_shows_checked_type) {
  TempDir tmp;
  fs::path p = tmp.write("song.synth", "");
  std::string uri = uriFor(p);
  std::string text = kSource;
  LspServer server;
  server.onMessage(didOpen(uri, text));

  json::Value hover = resultOf(server,
                               positionRequest(2, "textDocument/hover", uri,
                                               lcOf(text, "base_freq", 1)));
  const json::Value* contents = hover.get("contents");
  CHECK(contents != nullptr);
  std::string value = contents->getString("value");
  CHECK(value.find("base_freq") != std::string::npos);
  CHECK(value.find("Scalar") != std::string::npos);

  // Hovering a function name shows its function type.
  json::Value fh = resultOf(server,
                            positionRequest(3, "textDocument/hover", uri,
                                            lcOf(text, "voice ~gain:g", 0)));
  std::string fnType = fh.get("contents")->getString("value");
  CHECK(fnType.find("->") != std::string::npos);
}

TEST(lsp_definition_across_files) {
  TempDir tmp;
  tmp.write("keys.synth", "let strike : Scalar = 440.0 ;;\n");
  fs::path p = tmp.write("song.synth", "");
  std::string uri = uriFor(p);
  std::string text =
      "import Keys\n"
      "let x : Scalar = Keys.strike ;;\n";
  LspServer server;
  server.onMessage(didOpen(uri, text));

  json::Value loc = resultOf(server,
                             positionRequest(2, "textDocument/definition",
                                             uri, lcOf(text, "strike", 0)));
  CHECK(loc.getString("uri") == uriFor(tmp.dir / "keys.synth"));
  CHECK(rangeStartLine(loc) == 0);
  CHECK(rangeStartChar(loc) == 4);  // `let strike`: the name, not the let
}

TEST(lsp_unsaved_buffer_overrides_disk) {
  TempDir tmp;
  // On disk `keys.synth` has no `strike`; the open buffer adds it. The
  // importer must see the buffer, exactly like a build after save would.
  fs::path keys = tmp.write("keys.synth", "let other : Scalar = 1.0 ;;\n");
  fs::path song = tmp.write("song.synth", "");
  std::string keysUri = uriFor(keys);
  std::string songUri = uriFor(song);
  LspServer server;
  server.onMessage(didOpen(
      keysUri,
      "let other : Scalar = 1.0 ;;\nlet strike : Scalar = 440.0 ;;\n"));
  json::Value note = parseOne(server.onMessage(didOpen(
      songUri, "import Keys\nlet x : Scalar = Keys.strike ;;\n")));
  const json::Value* diags = note.get("params")->get("diagnostics");
  CHECK(diags != nullptr);
  CHECK(diags->array.empty());
}
