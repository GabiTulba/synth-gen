#include "lsp.hpp"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "checker.hpp"
#include "json.hpp"
#include "library.hpp"
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

// A request carrying only the textDocument (documentSymbol, formatting).
std::string docRequest(int id, const std::string& method,
                       const std::string& uri) {
  json::Value td = json::makeObject();
  td.set("uri", json::makeString(uri));
  json::Value params = json::makeObject();
  params.set("textDocument", std::move(td));
  json::Value msg = json::makeObject();
  msg.set("jsonrpc", json::makeString("2.0"));
  msg.set("id", json::makeNumber(id));
  msg.set("method", json::makeString(method));
  msg.set("params", std::move(params));
  return json::serialize(msg);
}

std::string referencesRequest(int id, const std::string& uri, LC lc,
                              bool includeDecl) {
  json::Value td = json::makeObject();
  td.set("uri", json::makeString(uri));
  json::Value pos = json::makeObject();
  pos.set("line", json::makeNumber(lc.line));
  pos.set("character", json::makeNumber(lc.ch));
  json::Value ctx = json::makeObject();
  ctx.set("includeDeclaration", json::makeBool(includeDecl));
  json::Value params = json::makeObject();
  params.set("textDocument", std::move(td));
  params.set("position", std::move(pos));
  params.set("context", std::move(ctx));
  json::Value msg = json::makeObject();
  msg.set("jsonrpc", json::makeString("2.0"));
  msg.set("id", json::makeNumber(id));
  msg.set("method", json::makeString("textDocument/references"));
  msg.set("params", std::move(params));
  return json::serialize(msg);
}

std::string renameRequest(int id, const std::string& uri, LC lc,
                          const std::string& newName) {
  json::Value td = json::makeObject();
  td.set("uri", json::makeString(uri));
  json::Value pos = json::makeObject();
  pos.set("line", json::makeNumber(lc.line));
  pos.set("character", json::makeNumber(lc.ch));
  json::Value params = json::makeObject();
  params.set("textDocument", std::move(td));
  params.set("position", std::move(pos));
  params.set("newName", json::makeString(newName));
  json::Value msg = json::makeObject();
  msg.set("jsonrpc", json::makeString("2.0"));
  msg.set("id", json::makeNumber(id));
  msg.set("method", json::makeString("textDocument/rename"));
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

// The "error" of the single response returned for a request.
json::Value errorOf(LspServer& server, const std::string& request) {
  json::Value v = parseOne(server.onMessage(request));
  const json::Value* error = v.get("error");
  CHECK(error != nullptr);
  return *error;
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
    "  (sine base_freq) *. gain\n"
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
  CHECK(caps->get("referencesProvider") != nullptr);
  CHECK(caps->get("renameProvider") != nullptr);
  CHECK(caps->get("documentSymbolProvider") != nullptr);
  CHECK(caps->get("documentFormattingProvider") != nullptr);
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

TEST(lsp_document_outline) {
  TempDir tmp;
  fs::path p = tmp.write("song.synth", "");
  std::string uri = uriFor(p);
  std::string text =
      "module Voices = struct\n"
      "  let strike freq:Scalar : Scalar = freq ;;\n"
      "end ;;\n"
      "\n"
      "let tempo : Scalar = 120.0 ;;\n";
  LspServer server;
  server.onMessage(didOpen(uri, text));

  json::Value syms = resultOf(
      server, docRequest(2, "textDocument/documentSymbol", uri));
  CHECK(syms.array.size() == 2);

  const json::Value& mod = syms.array[0];
  CHECK(mod.getString("name") == "Voices");
  CHECK((int)mod.getNumber("kind") == 2);  // Module
  const json::Value* children = mod.get("children");
  CHECK(children != nullptr);
  CHECK(children->array.size() == 1);
  const json::Value& strike = children->array[0];
  CHECK(strike.getString("name") == "strike");
  CHECK((int)strike.getNumber("kind") == 12);  // Function
  CHECK(strike.getString("detail").find("->") != std::string::npos);
  // The selection range is the name itself, inside the full range.
  const json::Value* sel = strike.get("selectionRange");
  CHECK(sel != nullptr);
  CHECK((int)sel->get("start")->getNumber("line") == 1);
  CHECK((int)sel->get("start")->getNumber("character") ==
        lcOf(text, "strike", 0).ch);

  const json::Value& tempo = syms.array[1];
  CHECK(tempo.getString("name") == "tempo");
  CHECK((int)tempo.getNumber("kind") == 14);  // Constant
  CHECK(tempo.getString("detail") == "Scalar");
}

TEST(lsp_references_top_level_def) {
  TempDir tmp;
  fs::path p = tmp.write("song.synth", "");
  std::string uri = uriFor(p);
  std::string text = kSource;
  LspServer server;
  server.onMessage(didOpen(uri, text));

  // From the use inside `voice`: declaration + use.
  json::Value refs = resultOf(
      server, referencesRequest(2, uri, lcOf(text, "base_freq", 1), true));
  CHECK(refs.array.size() == 2);
  CHECK(rangeStartLine(refs.array[0]) == lcOf(text, "base_freq", 0).line);
  CHECK(rangeStartLine(refs.array[1]) == lcOf(text, "base_freq", 1).line);

  // Without the declaration only the use remains, and asking from the
  // declaration itself finds the same set.
  json::Value uses = resultOf(
      server, referencesRequest(3, uri, lcOf(text, "base_freq", 0), false));
  CHECK(uses.array.size() == 1);
  CHECK(rangeStartLine(uses.array[0]) == lcOf(text, "base_freq", 1).line);
}

TEST(lsp_references_across_files) {
  TempDir tmp;
  fs::path keys =
      tmp.write("keys.synth", "let strike : Scalar = 440.0 ;;\n");
  fs::path song = tmp.write("song.synth", "");
  std::string songUri = uriFor(song);
  std::string text =
      "import Keys\n"
      "let x : Scalar = Keys.strike ;;\n"
      "let y : Scalar = Keys.strike *. 2.0 ;;\n";
  LspServer server;
  server.onMessage(didOpen(songUri, text));

  json::Value refs = resultOf(
      server,
      referencesRequest(2, songUri, lcOf(text, "strike", 0), true));
  CHECK(refs.array.size() == 3);
  CHECK(refs.array[0].getString("uri") == uriFor(keys));  // the declaration
  CHECK(refs.array[1].getString("uri") == songUri);
  CHECK(refs.array[2].getString("uri") == songUri);
  // Qualified references cover only the leaf name.
  CHECK(rangeStartChar(refs.array[1]) == lcOf(text, "strike", 0).ch);
}

TEST(lsp_references_local_binders_respect_shadowing) {
  TempDir tmp;
  fs::path p = tmp.write("song.synth", "");
  std::string uri = uriFor(p);
  std::string text =
      "let x : Scalar = 1.0 ;;\n"
      "let f : Scalar = let x : Scalar = 2.0 in x ;;\n"
      "let g : Scalar = x ;;\n";
  LspServer server;
  server.onMessage(didOpen(uri, text));

  // The top-level `x`: its declaration and the use in `g`, but not the
  // shadowed use inside `f`.
  json::Value top = resultOf(
      server, referencesRequest(2, uri, lcOf(text, "x", 0), true));
  CHECK(top.array.size() == 2);
  CHECK(rangeStartLine(top.array[0]) == 0);
  CHECK(rangeStartLine(top.array[1]) == 2);

  // The local `x` inside `f`: its binder and the body use only.
  json::Value local = resultOf(
      server,
      referencesRequest(3, uri, lcOf(text, "let x : Scalar = 2.0", 0),
                        true));
  // Position on the local binder's name.
  LC localX = lcOf(text, "let x : Scalar = 2.0", 0);
  localX.ch += 4;
  local = resultOf(server, referencesRequest(4, uri, localX, true));
  CHECK(local.array.size() == 2);
  CHECK(rangeStartLine(local.array[0]) == 1);
  CHECK(rangeStartLine(local.array[1]) == 1);
  CHECK(rangeStartChar(local.array[1]) > rangeStartChar(local.array[0]));

  // A parameter: declaration + use, confined to its definition.
  std::string ptext = kSource;
  fs::path p2 = tmp.write("voice.synth", "");
  std::string uri2 = uriFor(p2);
  server.onMessage(didOpen(uri2, ptext));
  json::Value param = resultOf(
      server, referencesRequest(5, uri2, lcOf(ptext, "gain", 1), true));
  CHECK(param.array.size() == 2);
  CHECK(rangeStartLine(param.array[0]) == lcOf(ptext, "gain", 0).line);
  CHECK(rangeStartLine(param.array[1]) == lcOf(ptext, "gain", 1).line);
}

TEST(lsp_rename_across_files) {
  TempDir tmp;
  fs::path keys =
      tmp.write("keys.synth", "let strike : Scalar = 440.0 ;;\n");
  fs::path song = tmp.write("song.synth", "");
  std::string songUri = uriFor(song);
  std::string text =
      "import Keys\n"
      "let x : Scalar = Keys.strike ;;\n";
  LspServer server;
  server.onMessage(didOpen(songUri, text));

  json::Value edit = resultOf(
      server, renameRequest(2, songUri, lcOf(text, "strike", 0), "pluck"));
  const json::Value* changes = edit.get("changes");
  CHECK(changes != nullptr);
  const json::Value* keysEdits = changes->get(uriFor(keys));
  CHECK(keysEdits != nullptr);
  CHECK(keysEdits->array.size() == 1);
  CHECK(keysEdits->array[0].getString("newText") == "pluck");
  CHECK(rangeStartChar(keysEdits->array[0]) == 4);  // `let strike`
  const json::Value* songEdits = changes->get(songUri);
  CHECK(songEdits != nullptr);
  CHECK(songEdits->array.size() == 1);
  // Only the leaf of `Keys.strike` is edited.
  CHECK(rangeStartChar(songEdits->array[0]) == lcOf(text, "strike", 0).ch);
}

TEST(lsp_rename_local_binder) {
  TempDir tmp;
  fs::path p = tmp.write("song.synth", "");
  std::string uri = uriFor(p);
  std::string text = kSource;
  LspServer server;
  server.onMessage(didOpen(uri, text));

  // Rename the `let g ... in` binder from its use site.
  LC gUse = lcOf(text, "gain:g", 0);
  gUse.ch += 5;
  json::Value edit =
      resultOf(server, renameRequest(2, uri, gUse, "gg"));
  const json::Value* changes = edit.get("changes");
  CHECK(changes != nullptr);
  CHECK(changes->object.size() == 1);
  const json::Value* edits = changes->get(uri);
  CHECK(edits != nullptr);
  CHECK(edits->array.size() == 2);
  CHECK(rangeStartLine(edits->array[0]) == lcOf(text, "let g :", 0).line);
  CHECK(rangeStartLine(edits->array[1]) == gUse.line);
}

TEST(lsp_rename_rejections) {
  TempDir tmp;
  fs::path p = tmp.write("song.synth", "");
  std::string uri = uriFor(p);
  std::string text = kSource;
  LspServer server;
  server.onMessage(didOpen(uri, text));

  // A labeled parameter's name is call-site syntax.
  json::Value labeled = errorOf(
      server, renameRequest(2, uri, lcOf(text, "gain", 0), "amount"));
  CHECK(labeled.getString("message").find("labeled") != std::string::npos);

  // New names must be value identifiers.
  json::Value bad = errorOf(
      server,
      renameRequest(3, uri, lcOf(text, "base_freq", 1), "NotLower"));
  CHECK(bad.getString("message").find("valid") != std::string::npos);

  // Core definitions are read-only.
  json::Value core = errorOf(
      server, renameRequest(4, uri, lcOf(text, "sine", 0), "sine2"));
  CHECK(core.getString("message").find("standard library") !=
        std::string::npos);
}

TEST(lsp_formatting_normalizes_whitespace) {
  TempDir tmp;
  fs::path p = tmp.write("song.synth", "");
  std::string uri = uriFor(p);
  std::string text =
      "let x : Scalar =  1.0+.2.0 ;;   \n"
      "\n"
      "\n"
      "\tlet y:Scalar = x*.3.0 ;;\n"
      "let z : Scalar = -1.5 ;;  (* keep   me *)\n";
  LspServer server;
  server.onMessage(didOpen(uri, text));

  json::Value edits =
      resultOf(server, docRequest(2, "textDocument/formatting", uri));
  CHECK(edits.array.size() == 1);
  std::string formatted = edits.array[0].getString("newText");
  CHECK(formatted ==
        "let x : Scalar = 1.0 +. 2.0 ;;\n"  // operators spaced, no trailing
        "\n"                               // blank run collapsed
        "  let y:Scalar = x *. 3.0 ;;\n"    // tab -> spaces, tight `:` kept
        "let z : Scalar = -1.5 ;;  (* keep   me *)\n");

  // Formatting is idempotent: a clean document needs no edits.
  json::Value note = parseOne(server.onMessage(
      R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{)"
      R"("textDocument":{"uri":")" + uri + R"("},)"
      R"("contentChanges":[{"text":)" + json::serialize([&] {
        return json::makeString(formatted);
      }()) + R"(}]}})"));
  json::Value clean =
      resultOf(server, docRequest(3, "textDocument/formatting", uri));
  CHECK(clean.array.empty());
}

TEST(lsp_formatting_refuses_unlexable_source) {
  TempDir tmp;
  fs::path p = tmp.write("bad.synth", "");
  std::string uri = uriFor(p);
  LspServer server;
  server.onMessage(didOpen(uri, "let x : Scalar = 1.0 (* nope\n"));
  json::Value r =
      resultOf(server, docRequest(2, "textDocument/formatting", uri));
  CHECK(r.kind == json::Value::Kind::Null);
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

TEST(lsp_formatting_record_and_type_syntax) {
  TempDir tmp;
  fs::path p = tmp.write("song.synth", "");
  std::string uri = uriFor(p);
  std::string text =
      "type Env = {attack : Timestamp;release : Timestamp} ;;\n"
      "type Wave = |Sine|Pulse of Scalar ;;\n"
      "let e : Env = { attack=5ms;   release =100ms } ;;\n"
      "let q : Env = {e with attack = 1ms} ;;\n"
      "let a : Timestamp = e . attack ;;\n";
  LspServer server;
  server.onMessage(didOpen(uri, text));
  json::Value edits =
      resultOf(server, docRequest(2, "textDocument/formatting", uri));
  CHECK(edits.array.size() == 1);
  std::string formatted = edits.array[0].getString("newText");
  CHECK(formatted ==
        "type Env = { attack : Timestamp; release : Timestamp } ;;\n"
        "type Wave = | Sine | Pulse of Scalar ;;\n"
        "let e : Env = { attack = 5ms; release = 100ms } ;;\n"
        "let q : Env = { e with attack = 1ms } ;;\n"
        "let a : Timestamp = e.attack ;;\n");
  // The result is a fixed point.
  json::Value note = parseOne(server.onMessage(
      R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{)"
      R"("textDocument":{"uri":")" + uri + R"("},)"
      R"("contentChanges":[{"text":)" + json::serialize([&] {
        return json::makeString(formatted);
      }()) + R"(}]}})"));
  json::Value clean =
      resultOf(server, docRequest(3, "textDocument/formatting", uri));
  CHECK(clean.array.empty());
}

TEST(lsp_formatting_stdlib_and_examples_are_a_fixed_point) {
  // docs/tooling.md: the shipped stdlib and the examples tree are
  // already formatted - the formatter must propose no edits for them.
  std::vector<fs::path> files;
  files.push_back(fs::path(bundledStdlibDir()) / "core" /
                  kLibraryInterfaceFile);
  fs::path examples = fs::path(bundledStdlibDir()) / ".." / "examples";
  std::error_code ec;
  if (fs::exists(examples, ec))
    for (auto& e : fs::recursive_directory_iterator(examples, ec))
      if (e.path().extension() == ".synth" &&
          e.path().string().find("_build") == std::string::npos)
        files.push_back(e.path());
  CHECK(files.size() >= 1);
  int reqId = 2;
  for (auto& f : files) {
    std::ifstream in(f);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    CHECK(!text.empty());
    std::string uri = uriFor(f);
    LspServer server;
    server.onMessage(didOpen(uri, text));
    json::Value edits =
        resultOf(server, docRequest(reqId++, "textDocument/formatting", uri));
    if (!edits.array.empty())
      std::cerr << "formatter is not a fixed point on " << f << "\n";
    CHECK(edits.array.empty());
  }
}
