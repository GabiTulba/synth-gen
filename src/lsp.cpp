#include "lsp.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <istream>
#include <optional>
#include <ostream>

#include "build.hpp"
#include "json.hpp"
#include "library.hpp"
#include "types.hpp"

namespace fs = std::filesystem;

namespace synth {

namespace {

using json::Value;

// --- URIs ------------------------------------------------------------------

bool isHex(char c) { return std::isxdigit((unsigned char)c); }

std::string uriToPath(const std::string& uri) {
  const std::string scheme = "file://";
  if (uri.rfind(scheme, 0) != 0) return {};
  std::string rest = uri.substr(scheme.size());
  std::string out;
  for (size_t i = 0; i < rest.size(); i++) {
    if (rest[i] == '%' && i + 2 < rest.size() && isHex(rest[i + 1]) &&
        isHex(rest[i + 2])) {
      out += (char)std::strtol(rest.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += rest[i];
    }
  }
  return out;
}

std::string pathToUri(const std::string& path) {
  static const char* hex = "0123456789ABCDEF";
  std::string out = "file://";
  for (unsigned char c : path) {
    if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
        c == '/') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0xF];
    }
  }
  return out;
}

// --- Positions -------------------------------------------------------------
// LSP positions are 0-based (line, UTF-16 code unit); spans are byte
// offsets into UTF-8 source. The conversions walk the line's UTF-8.

int utf8Bytes(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;  // invalid byte: count it as one unit and move on
}

struct Pos {
  int line = 0;
  int character = 0;
};

Pos positionAt(const std::string& text, uint32_t offset) {
  offset = std::min<uint32_t>(offset, (uint32_t)text.size());
  Pos p;
  uint32_t lineStart = 0;
  for (uint32_t i = 0; i < offset; i++)
    if (text[i] == '\n') {
      p.line++;
      lineStart = i + 1;
    }
  for (uint32_t i = lineStart; i < offset;) {
    int bytes = utf8Bytes((unsigned char)text[i]);
    p.character += bytes == 4 ? 2 : 1;
    i += (uint32_t)bytes;
  }
  return p;
}

uint32_t offsetAt(const std::string& text, int line, int character) {
  uint32_t i = 0;
  for (int l = 0; l < line && i < text.size();)
    if (text[i++] == '\n') l++;
  int units = 0;
  while (i < text.size() && text[i] != '\n' && units < character) {
    int bytes = utf8Bytes((unsigned char)text[i]);
    units += bytes == 4 ? 2 : 1;
    i += (uint32_t)bytes;
  }
  return i;
}

Value positionValue(Pos p) {
  Value v = json::makeObject();
  v.set("line", json::makeNumber(p.line));
  v.set("character", json::makeNumber(p.character));
  return v;
}

Value rangeValue(const std::string& text, Span span) {
  Value v = json::makeObject();
  v.set("start", positionValue(positionAt(text, span.lo)));
  v.set("end", positionValue(positionAt(text, span.hi)));
  return v;
}

Value locationValue(const std::string& path, const std::string& text,
                    Span span) {
  Value v = json::makeObject();
  v.set("uri", json::makeString(pathToUri(canonicalSourceKey(path))));
  v.set("range", rangeValue(text, span));
  return v;
}

// --- AST lookup ------------------------------------------------------------

bool covers(Span s, uint32_t off) { return s.lo <= off && off <= s.hi; }

bool isIdentCont(char c) {
  return std::isalnum((unsigned char)c) || c == '_' || c == '\'';
}

// A local binding site (parameter, lambda parameter, or `let ... in`)
// enclosing the position of interest.
struct Binder {
  std::string name;
  Span span;
};

// Innermost Ident whose span covers `off`, recording the local binders
// in scope at it (innermost last).
const Expr* findIdent(const Expr& e, uint32_t off,
                      std::vector<Binder>& binders) {
  if (!covers(e.span, off)) return nullptr;
  switch (e.kind) {
    case Expr::Kind::Ident:
      return &e;
    case Expr::Kind::Let: {
      if (const Expr* r = findIdent(*e.items[0], off, binders)) return r;
      binders.push_back({e.name, e.span});
      if (const Expr* r = findIdent(*e.items[1], off, binders)) return r;
      binders.pop_back();
      return nullptr;
    }
    case Expr::Kind::Lambda: {
      for (auto& p : e.params) binders.push_back({p.name, p.span});
      if (const Expr* r = findIdent(*e.items[0], off, binders)) return r;
      binders.resize(binders.size() - e.params.size());
      return nullptr;
    }
    default:
      for (auto& it : e.items)
        if (const Expr* r = findIdent(*it, off, binders)) return r;
      return nullptr;
  }
}

// The definition (and, inside a Let, the identifier + binders) at `off`.
struct DefHit {
  const TopDef* def = nullptr;
  std::string prefix;  // inline-module prefix of the definition
  const Expr* ident = nullptr;
  std::vector<Binder> binders;  // params + enclosing locals, innermost last
};

bool findAt(const std::vector<TopDef>& defs, uint32_t off,
            const std::string& prefix, DefHit& hit) {
  for (auto& d : defs) {
    if (!covers(d.span, off)) continue;
    if (d.kind == TopDef::Kind::ModuleDef) {
      if (findAt(d.defs, off, prefix + d.name + ".", hit)) return true;
      hit.def = &d;
      hit.prefix = prefix;
      return true;  // on the module header itself
    }
    hit.def = &d;
    hit.prefix = prefix;
    if (d.kind == TopDef::Kind::Let) {
      for (auto& p : d.params) hit.binders.push_back({p.name, p.span});
      if (d.body) hit.ident = findIdent(*d.body, off, hit.binders);
    }
    return true;
  }
  return false;
}

// Top-level (possibly dotted) definition lookup by stored name.
const TopDef* findDef(const std::vector<TopDef>& defs,
                      const std::string& stored,
                      const std::string& prefix = {}) {
  for (auto& d : defs) {
    if (d.kind == TopDef::Kind::Let && prefix + d.name == stored) return &d;
    if (d.kind == TopDef::Kind::ModuleDef) {
      std::string p = prefix + d.name + ".";
      if (stored.rfind(p, 0) == 0)
        if (const TopDef* r = findDef(d.defs, stored, p)) return r;
    }
  }
  return nullptr;
}

// Inline-module definition lookup by dotted path.
const TopDef* findModuleDef(const std::vector<TopDef>& defs,
                            const std::string& path,
                            const std::string& prefix = {}) {
  for (auto& d : defs) {
    if (d.kind != TopDef::Kind::ModuleDef) continue;
    std::string full = prefix + d.name;
    if (full == path) return &d;
    if (path.rfind(full + ".", 0) == 0)
      if (const TopDef* r = findModuleDef(d.defs, path, full + "."))
        return r;
  }
  return nullptr;
}

// The span of the name being bound: the first whole-word occurrence of
// the (leaf) name inside the definition's span, so go-to-definition
// lands on `pluck` in `let pluck freq:Scalar ...` rather than on `let`.
Span nameSpan(const std::string& src, const TopDef& d) {
  std::string leaf = d.name;
  if (size_t dot = leaf.rfind('.'); dot != std::string::npos)
    leaf = leaf.substr(dot + 1);
  uint32_t hi = std::min<uint32_t>(d.span.hi, (uint32_t)src.size());
  for (uint32_t i = d.span.lo; i + leaf.size() <= hi; i++) {
    if (src.compare(i, leaf.size(), leaf) != 0) continue;
    bool leftOk = i == 0 || !isIdentCont(src[i - 1]);
    uint32_t end = i + (uint32_t)leaf.size();
    bool rightOk = end >= src.size() || !isIdentCont(src[end]);
    if (leftOk && rightOk) return {i, end};
  }
  return {d.span.lo, d.span.lo};
}

Span binderNameSpan(const std::string& src, const Binder& b) {
  // Parameters carry a tight span already; `let x : T = ... in` spans the
  // whole expression, so narrow to the first occurrence of the name.
  uint32_t hi = std::min<uint32_t>(b.span.hi, (uint32_t)src.size());
  for (uint32_t i = b.span.lo; i + b.name.size() <= hi; i++) {
    if (src.compare(i, b.name.size(), b.name) != 0) continue;
    bool leftOk = i == 0 || !isIdentCont(src[i - 1]);
    uint32_t end = i + (uint32_t)b.name.size();
    bool rightOk = end >= src.size() || !isIdentCont(src[end]);
    if (leftOk && rightOk) return {i, end};
  }
  return {b.span.lo, b.span.lo};
}

// --- Module scope ----------------------------------------------------------

// A module reference: a whole checked module, or an inline module (the
// dotted `prefix`) inside one.
struct ModEntry {
  const CheckedModule* host = nullptr;
  std::string prefix;
};

// What is visible at one position: values (with their checked types) and
// module names. A static mirror of the checker's scope frames, built by
// replaying the file's binders up to the cursor.
struct Scope {
  std::map<std::string, TypePtr> values;
  std::map<std::string, ModEntry> modules;
};

std::vector<std::string> splitPath(const std::string& s) {
  std::vector<std::string> segs;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); i++)
    if (i == s.size() || s[i] == '.') {
      segs.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  return segs;
}

// Resolve a surface module path the way the checker would at this
// position: the first segment through the local scope (opens, aliases,
// inline modules seen so far), then the file's load-time module scope;
// later segments descend through exported bindings and inline modules.
std::optional<ModEntry> resolveSurfacePath(const Program& prog,
                                           const CheckedModule& cm,
                                           const Scope* sc,
                                           const std::string& surface) {
  if (auto it = cm.moduleScope.find(surface); it != cm.moduleScope.end())
    if (const CheckedModule* m = prog.find(it->second)) return ModEntry{m, ""};
  std::vector<std::string> segs = splitPath(surface);
  if (segs.empty() || segs[0].empty()) return std::nullopt;
  ModEntry cur;
  if (sc) {
    auto it = sc->modules.find(segs[0]);
    if (it != sc->modules.end()) cur = it->second;
  }
  if (!cur.host) {
    if (auto it = cm.moduleScope.find(segs[0]); it != cm.moduleScope.end())
      cur = {prog.find(it->second), ""};
    else if (cm.inlineModules.count(segs[0]))
      cur = {&cm, segs[0]};
    else if (const CheckedModule* m = prog.find(segs[0]))
      cur = {m, ""};
  }
  if (!cur.host) return std::nullopt;
  for (size_t i = 1; i < segs.size(); i++) {
    const std::string& s = segs[i];
    if (cur.prefix.empty()) {
      auto ex = cur.host->exportedModules.find(s);
      if (ex != cur.host->exportedModules.end()) {
        const CheckedModule* m = prog.find(ex->second);
        if (!m) return std::nullopt;
        cur = {m, ""};
        continue;
      }
      if (cur.host->inlineModules.count(s)) {
        cur.prefix = s;
        continue;
      }
      return std::nullopt;
    }
    std::string next = cur.prefix + "." + s;
    if (!cur.host->inlineModules.count(next)) return std::nullopt;
    cur.prefix = next;
  }
  return cur;
}

// Members brought into scope by `open`ing (host, prefix): immediate
// values and sub-modules, plus (whole modules only) re-exported bindings.
void addOpenedMembers(const Program& prog, const ModEntry& m, Scope& sc) {
  if (m.prefix.empty()) {
    for (auto& [name, type] : m.host->defTypes)
      if (name.find('.') == std::string::npos) sc.values[name] = type;
    for (auto& p : m.host->inlineModules)
      if (p.find('.') == std::string::npos) sc.modules[p] = {m.host, p};
    for (auto& [name, target] : m.host->exportedModules)
      if (const CheckedModule* t = prog.find(target))
        sc.modules[name] = {t, ""};
    return;
  }
  std::string pre = m.prefix + ".";
  for (auto& [name, type] : m.host->defTypes)
    if (name.rfind(pre, 0) == 0 &&
        name.find('.', pre.size()) == std::string::npos)
      sc.values[name.substr(pre.size())] = type;
  for (auto& p : m.host->inlineModules)
    if (p.rfind(pre, 0) == 0 && p.find('.', pre.size()) == std::string::npos)
      sc.modules[p.substr(pre.size())] = {m.host, p};
}

// Replay the file's position-ordered binders down to `off`, mirroring
// ModuleChecker::checkDefs: imports, opens, aliases, inline modules and
// definitions before the cursor bind; inside the definition that covers
// the cursor, parameters and enclosing locals bind too.
void collectExprScope(const Expr& e, uint32_t off, Scope& sc) {
  if (!covers(e.span, off)) return;
  switch (e.kind) {
    case Expr::Kind::Let:
      collectExprScope(*e.items[0], off, sc);
      if (covers(e.items[1]->span, off)) {
        sc.values[e.name] = e.declType;
        collectExprScope(*e.items[1], off, sc);
      }
      return;
    case Expr::Kind::Lambda:
      if (covers(e.items[0]->span, off)) {
        for (auto& p : e.params) sc.values[p.name] = p.type;
        collectExprScope(*e.items[0], off, sc);
      }
      return;
    default:
      for (auto& it : e.items) collectExprScope(*it, off, sc);
      return;
  }
}

void collectScope(const Program& prog, const CheckedModule& cm,
                  const std::vector<TopDef>& defs, uint32_t off,
                  const std::string& prefix, Scope& sc) {
  for (auto& d : defs) {
    if (d.span.lo > off) break;  // definitions must precede use
    bool inside = covers(d.span, off);
    switch (d.kind) {
      case TopDef::Kind::Import: {
        std::string first = d.moduleName.substr(0, d.moduleName.find('.'));
        auto it = cm.moduleScope.find(first);
        if (it != cm.moduleScope.end())
          if (const CheckedModule* m = prog.find(it->second))
            sc.modules[first] = {m, ""};
        break;
      }
      case TopDef::Kind::Open: {
        auto r = resolveSurfacePath(prog, cm, &sc, d.moduleName);
        if (r) {
          sc.modules[splitPath(d.moduleName).back()] = *r;
          addOpenedMembers(prog, *r, sc);
        }
        break;
      }
      case TopDef::Kind::ModuleAlias: {
        auto r = resolveSurfacePath(prog, cm, &sc, d.moduleName);
        if (r) sc.modules[d.name] = *r;
        break;
      }
      case TopDef::Kind::ModuleDef: {
        sc.modules[d.name] = {&cm, prefix + d.name};
        if (inside) {
          collectScope(prog, cm, d.defs, off, prefix + d.name + ".", sc);
          return;
        }
        break;
      }
      case TopDef::Kind::Let: {
        auto it = cm.defTypes.find(prefix + d.name);
        if (it != cm.defTypes.end() && d.name != "_")
          sc.values[d.name] = it->second;
        if (inside) {
          for (auto& p : d.params) sc.values[p.name] = p.type;
          if (d.body) collectExprScope(*d.body, off, sc);
          return;
        }
        break;
      }
    }
  }
}

// --- Completion items ------------------------------------------------------

// LSP CompletionItemKind constants (the few we use).
constexpr int kKindFunction = 3;
constexpr int kKindVariable = 6;
constexpr int kKindModule = 9;
constexpr int kKindKeyword = 14;
constexpr int kKindValue = 12;

Value completionItem(const std::string& label, int kind,
                     const std::string& detail) {
  Value v = json::makeObject();
  v.set("label", json::makeString(label));
  v.set("kind", json::makeNumber(kind));
  if (!detail.empty()) v.set("detail", json::makeString(detail));
  return v;
}

int kindForType(const TypePtr& t) {
  if (t && t->kind == Type::Kind::Fun) return kKindFunction;
  return kKindValue;
}

// Members of a module (or inline module) for `Path.` completion.
void memberItems(const Program& prog, const ModEntry& m,
                 std::vector<Value>& out) {
  Scope sc;
  addOpenedMembers(prog, m, sc);
  for (auto& [name, type] : sc.values)
    out.push_back(completionItem(name, kindForType(type), typeName(type)));
  for (auto& [name, entry] : sc.modules)
    out.push_back(completionItem(name, kKindModule, "module"));
}

const char* kKeywords[] = {"let",  "in",     "fun",  "import", "open",
                           "module", "if",   "then", "else",   "struct",
                           "end",  "external", "true", "false"};
const char* kTypeWords[] = {"Scalar", "Int",    "Vector", "Timestamp",
                            "String", "Bool",   "Signal", "Sample",
                            "list",   "unit"};

// --- Diagnostics -----------------------------------------------------------

Value diagnosticValue(const std::string& text, const Diagnostic& d) {
  Value v = json::makeObject();
  Span span = d.file.empty() ? Span{0, 0} : d.span;
  v.set("range", rangeValue(text, span));
  v.set("severity",
        json::makeNumber(d.severity == Severity::Error ? 1 : 2));
  v.set("source", json::makeString("synthc"));
  v.set("message", json::makeString(d.message));
  return v;
}

}  // namespace

// --- The server ------------------------------------------------------------

LspServer::Analysis& LspServer::analysisFor(const std::string& uri) {
  auto cached = cache_.find(uri);
  if (cached != cache_.end()) return cached->second;

  Analysis a;
  const Document& doc = docs_.at(uri);
  std::map<std::string, std::string> overlay;
  for (auto& [u, d] : docs_) overlay[d.key] = d.text;

  ModuleLoadContext ctx;
  ctx.overlay = &overlay;
  LibraryRegistry reg;
  std::string root =
      findEnclosingRoot(fs::path(doc.path).parent_path().string());
  if (!root.empty()) {
    reg = discoverLibraries(root, a.diags);
    ctx.registry = &reg;
    // Like lint: be lenient about deps - every discovered library is in
    // scope, so editing never demands manifest bookkeeping first.
    for (auto& [name, li] : reg.byName) ctx.deps.push_back(name);
    std::error_code ec;
    fs::path fp = fs::absolute(doc.path, ec);
    for (auto& [name, li] : reg.byName) {
      fs::path rel = fs::relative(fp, fs::absolute(li.dir, ec), ec);
      if (!rel.empty() && rel.string().rfind("..", 0) != 0)
        ctx.currentLib = reg.find(name);
    }
  }
  a.program = checkProject({doc.path}, a.diags, &ctx);
  return cache_.emplace(uri, std::move(a)).first->second;
}

std::string LspServer::publishDiagnostics(const std::string& uri) {
  const Document& doc = docs_.at(uri);
  Analysis& a = analysisFor(uri);
  std::vector<Value> items;
  for (auto& d : a.diags.items) {
    if (!d.file.empty() && canonicalSourceKey(d.file) != doc.key) continue;
    items.push_back(diagnosticValue(doc.text, d));
  }
  Value params = json::makeObject();
  params.set("uri", json::makeString(uri));
  params.set("diagnostics", json::makeArray(std::move(items)));
  Value msg = json::makeObject();
  msg.set("jsonrpc", json::makeString("2.0"));
  msg.set("method", json::makeString("textDocument/publishDiagnostics"));
  msg.set("params", std::move(params));
  return json::serialize(msg);
}

std::vector<std::string> LspServer::onMessage(const std::string& body) {
  std::vector<std::string> out;
  Value msg;
  std::string parseError;
  if (!json::parse(body, msg, parseError)) return out;

  const std::string method = msg.getString("method");
  const Value* id = msg.get("id");
  const Value* params = msg.get("params");

  auto respond = [&](Value result) {
    if (!id) return;
    Value r = json::makeObject();
    r.set("jsonrpc", json::makeString("2.0"));
    r.set("id", *id);
    r.set("result", std::move(result));
    out.push_back(json::serialize(r));
  };
  auto respondError = [&](int code, const std::string& message) {
    if (!id) return;
    Value e = json::makeObject();
    e.set("code", json::makeNumber(code));
    e.set("message", json::makeString(message));
    Value r = json::makeObject();
    r.set("jsonrpc", json::makeString("2.0"));
    r.set("id", *id);
    r.set("error", std::move(e));
    out.push_back(json::serialize(r));
  };

  // Position-carrying requests share their parameter shape.
  auto textDocumentUri = [&]() -> std::string {
    if (!params) return {};
    const Value* td = params->get("textDocument");
    return td ? td->getString("uri") : std::string{};
  };
  auto requestOffset = [&](const Document& doc) -> uint32_t {
    const Value* pos = params ? params->get("position") : nullptr;
    if (!pos) return 0;
    return offsetAt(doc.text, (int)pos->getNumber("line"),
                    (int)pos->getNumber("character"));
  };

  if (method == "initialize") {
    Value sync = json::makeObject();
    sync.set("openClose", json::makeBool(true));
    sync.set("change", json::makeNumber(1));  // full-text sync
    Value completion = json::makeObject();
    completion.set("triggerCharacters",
                   json::makeArray({json::makeString(".")}));
    Value caps = json::makeObject();
    caps.set("textDocumentSync", std::move(sync));
    caps.set("completionProvider", std::move(completion));
    caps.set("definitionProvider", json::makeBool(true));
    caps.set("hoverProvider", json::makeBool(true));
    Value info = json::makeObject();
    info.set("name", json::makeString("synthc"));
    Value result = json::makeObject();
    result.set("capabilities", std::move(caps));
    result.set("serverInfo", std::move(info));
    respond(std::move(result));
    return out;
  }
  if (method == "shutdown") {
    shutdown_ = true;
    respond(json::makeNull());
    return out;
  }
  if (method == "exit") {
    exit_ = true;
    return out;
  }

  if (method == "textDocument/didOpen") {
    const Value* td = params ? params->get("textDocument") : nullptr;
    if (!td) return out;
    std::string uri = td->getString("uri");
    std::string path = uriToPath(uri);
    if (path.empty()) return out;
    docs_[uri] = {path, canonicalSourceKey(path), td->getString("text")};
    cache_.clear();
    out.push_back(publishDiagnostics(uri));
    return out;
  }
  if (method == "textDocument/didChange") {
    std::string uri = textDocumentUri();
    auto doc = docs_.find(uri);
    const Value* changes = params ? params->get("contentChanges") : nullptr;
    if (doc == docs_.end() || !changes || changes->array.empty()) return out;
    // Full-text sync: the last change carries the whole document.
    doc->second.text = changes->array.back().getString("text");
    cache_.clear();
    out.push_back(publishDiagnostics(uri));
    return out;
  }
  if (method == "textDocument/didClose") {
    std::string uri = textDocumentUri();
    docs_.erase(uri);
    cache_.clear();
    // Clear the document's diagnostics.
    Value paramsOut = json::makeObject();
    paramsOut.set("uri", json::makeString(uri));
    paramsOut.set("diagnostics", json::makeArray());
    Value note = json::makeObject();
    note.set("jsonrpc", json::makeString("2.0"));
    note.set("method", json::makeString("textDocument/publishDiagnostics"));
    note.set("params", std::move(paramsOut));
    out.push_back(json::serialize(note));
    return out;
  }
  if (method == "textDocument/didSave") return out;

  if (method == "textDocument/definition" ||
      method == "textDocument/completion" ||
      method == "textDocument/hover") {
    std::string uri = textDocumentUri();
    auto docIt = docs_.find(uri);
    if (docIt == docs_.end()) {
      respond(json::makeNull());
      return out;
    }
    const Document& doc = docIt->second;
    Analysis& a = analysisFor(uri);
    const CheckedModule* cm = nullptr;
    for (auto& m : a.program.modules)
      if (canonicalSourceKey(m.parsed.path) == doc.key) cm = &m;
    if (!cm) {
      respond(method == "textDocument/completion" ? json::makeArray()
                                                  : json::makeNull());
      return out;
    }
    uint32_t off = requestOffset(doc);

    if (method == "textDocument/definition") {
      DefHit hit;
      findAt(cm->parsed.defs, off, "", hit);
      if (hit.ident) {
        const Expr& e = *hit.ident;
        // Which segment of the (possibly qualified) reference is the
        // cursor on? The leaf name starts after the last '.' in the
        // surface text.
        std::string surface =
            doc.text.substr(e.span.lo, e.span.hi - e.span.lo);
        size_t lastDot = surface.rfind('.');
        bool onModulePart = lastDot != std::string::npos &&
                            off < e.span.lo + (uint32_t)lastDot + 1;
        // The checker rewrote resolved references to canonical form:
        // moduleName is the canonical module id ("" = this module) and
        // name its stored, possibly dotted, key there.
        const CheckedModule* host = cm;
        if (!e.moduleName.empty()) {
          host = a.program.find(e.moduleName);
          if (!host) {
            // The reference did not check; try the surface spelling.
            auto ms = cm->moduleScope.find(
                e.moduleName.substr(0, e.moduleName.find('.')));
            if (ms != cm->moduleScope.end()) host = a.program.find(ms->second);
          }
        }
        if (onModulePart && host) {
          // Jump to the inline module when the path names one, else to
          // the top of the module's file.
          if (size_t dot = e.name.rfind('.'); dot != std::string::npos) {
            if (const TopDef* md =
                    findModuleDef(host->parsed.defs, e.name.substr(0, dot))) {
              respond(locationValue(host->parsed.path, host->parsed.source,
                                    nameSpan(host->parsed.source, *md)));
              return out;
            }
          }
          respond(locationValue(host->parsed.path, host->parsed.source,
                                Span{0, 0}));
          return out;
        }
        if (e.moduleName.empty() && e.name.find('.') == std::string::npos) {
          // A parameter or local wins over module-level definitions.
          for (auto it = hit.binders.rbegin(); it != hit.binders.rend(); ++it)
            if (it->name == e.name) {
              respond(locationValue(doc.path, doc.text,
                                    binderNameSpan(doc.text, *it)));
              return out;
            }
        }
        if (host)
          if (const TopDef* d = findDef(host->parsed.defs, e.name)) {
            respond(locationValue(host->parsed.path, host->parsed.source,
                                  nameSpan(host->parsed.source, *d)));
            return out;
          }
        respond(json::makeNull());
        return out;
      }
      if (hit.def && hit.def->kind != TopDef::Kind::Let) {
        // import / open / module alias / module header: jump to the module.
        Scope sc;
        collectScope(a.program, *cm, cm->parsed.defs, off, "", sc);
        auto r = resolveSurfacePath(a.program, *cm, &sc,
                                    hit.def->moduleName.empty()
                                        ? hit.prefix + hit.def->name
                                        : hit.def->moduleName);
        if (r) {
          if (!r->prefix.empty()) {
            if (const TopDef* md =
                    findModuleDef(r->host->parsed.defs, r->prefix)) {
              respond(locationValue(r->host->parsed.path,
                                    r->host->parsed.source,
                                    nameSpan(r->host->parsed.source, *md)));
              return out;
            }
          }
          respond(locationValue(r->host->parsed.path, r->host->parsed.source,
                                Span{0, 0}));
          return out;
        }
      }
      respond(json::makeNull());
      return out;
    }

    if (method == "textDocument/completion") {
      // The word being typed and any dotted path before it.
      uint32_t wordStart = off;
      while (wordStart > 0 && isIdentCont(doc.text[wordStart - 1]))
        wordStart--;
      std::string path;
      uint32_t q = wordStart;
      while (q > 0 && doc.text[q - 1] == '.') {
        uint32_t end = q - 1;
        uint32_t st = end;
        while (st > 0 && isIdentCont(doc.text[st - 1])) st--;
        if (st == end) break;
        std::string seg = doc.text.substr(st, end - st);
        path = path.empty() ? seg : seg + "." + path;
        q = st;
      }
      Scope sc;
      collectScope(a.program, *cm, cm->parsed.defs, off, "", sc);
      std::vector<Value> items;
      if (!path.empty()) {
        if (auto r = resolveSurfacePath(a.program, *cm, &sc, path))
          memberItems(a.program, *r, items);
      } else {
        for (auto& [name, type] : sc.values)
          if (name != "_")
            items.push_back(
                completionItem(name, kindForType(type), typeName(type)));
        for (auto& [name, entry] : sc.modules)
          items.push_back(completionItem(name, kKindModule, "module"));
        for (const char* k : kKeywords)
          items.push_back(completionItem(k, kKindKeyword, ""));
        for (const char* t : kTypeWords)
          items.push_back(completionItem(t, kKindKeyword, "type"));
      }
      respond(json::makeArray(std::move(items)));
      return out;
    }

    // hover
    DefHit hit;
    findAt(cm->parsed.defs, off, "", hit);
    if (hit.ident && hit.ident->type) {
      const Expr& e = *hit.ident;
      std::string surface = doc.text.substr(e.span.lo, e.span.hi - e.span.lo);
      Value contents = json::makeObject();
      contents.set("kind", json::makeString("markdown"));
      contents.set("value",
                   json::makeString("```synth\n" + surface + " : " +
                                    typeName(e.type) + "\n```"));
      Value result = json::makeObject();
      result.set("contents", std::move(contents));
      result.set("range", rangeValue(doc.text, e.span));
      respond(std::move(result));
      return out;
    }
    respond(json::makeNull());
    return out;
  }

  if (id) respondError(-32601, "method not found: " + method);
  return out;
}

int runLspServer(std::istream& in, std::ostream& out) {
  LspServer server;
  std::string line;
  while (!server.exitRequested()) {
    size_t contentLength = 0;
    bool sawHeader = false;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) break;
      sawHeader = true;
      const std::string cl = "Content-Length:";
      if (line.compare(0, cl.size(), cl) == 0)
        contentLength = std::strtoul(line.c_str() + cl.size(), nullptr, 10);
    }
    if (!in) break;
    if (!sawHeader || contentLength == 0) continue;
    std::string body(contentLength, '\0');
    in.read(body.data(), (std::streamsize)contentLength);
    if (!in) break;
    for (auto& msg : server.onMessage(body)) {
      out << "Content-Length: " << msg.size() << "\r\n\r\n" << msg;
      out.flush();
    }
  }
  return 0;
}

}  // namespace synth
