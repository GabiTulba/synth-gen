#include "lsp.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <istream>
#include <optional>
#include <ostream>
#include <set>
#include <tuple>

#include "build.hpp"
#include "json.hpp"
#include "lexer.hpp"
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
  bool labeled = false;  // a `~name:Type` parameter
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
      for (auto& p : e.params) binders.push_back({p.name, p.span, p.labeled});
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
      for (auto& p : d.params)
        hit.binders.push_back({p.name, p.span, p.labeled});
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

// --- References & rename ---------------------------------------------------

// Identity of the value symbol under the cursor: either a local binder
// (parameter, lambda parameter, `let ... in`) inside one top-level
// definition, or a top-level definition identified by its defining
// module (canonical id + source key, so same-named modules in different
// directories never conflate) and its stored, possibly dotted, name.
struct SymbolTarget {
  bool local = false;
  const TopDef* def = nullptr;  // local: the enclosing top-level let
  Binder binder;                // local: the binding site
  std::string hostId;           // top-level: canonical module id
  std::string hostKey;          // top-level: canonicalSourceKey of its file
  std::string stored;           // top-level: stored (dotted) name
};

// The local binder whose *name occurrence* (declaration site) is under
// `off`, innermost match winning. Complements findIdent, which only
// sees uses.
struct BinderDecl {
  Binder binder;
  bool found = false;
};

void findBinderDecl(const std::string& src, const Expr& e, uint32_t off,
                    BinderDecl& out) {
  if (!covers(e.span, off)) return;
  switch (e.kind) {
    case Expr::Kind::Let: {
      Binder b{e.name, e.span};
      Span ns = binderNameSpan(src, b);
      if (e.name != "_" && ns.hi > ns.lo && covers(ns, off)) out = {b, true};
      findBinderDecl(src, *e.items[0], off, out);
      findBinderDecl(src, *e.items[1], off, out);
      return;
    }
    case Expr::Kind::Lambda:
      for (auto& p : e.params)
        if (p.name != "_" && covers(p.span, off))
          out = {{p.name, p.span, p.labeled}, true};
      findBinderDecl(src, *e.items[0], off, out);
      return;
    default:
      for (auto& it : e.items) findBinderDecl(src, *it, off, out);
      return;
  }
}

// Visits every Ident in `e` with the local binders in scope at it
// (innermost last), mirroring findIdent's scoping.
template <typename Fn>
void forEachIdent(const Expr& e, std::vector<Binder>& binders, const Fn& fn) {
  switch (e.kind) {
    case Expr::Kind::Ident:
      fn(e, binders);
      return;
    case Expr::Kind::Let:
      forEachIdent(*e.items[0], binders, fn);
      binders.push_back({e.name, e.span});
      forEachIdent(*e.items[1], binders, fn);
      binders.pop_back();
      return;
    case Expr::Kind::Lambda:
      for (auto& p : e.params) binders.push_back({p.name, p.span, p.labeled});
      forEachIdent(*e.items[0], binders, fn);
      binders.resize(binders.size() - e.params.size());
      return;
    default:
      for (auto& it : e.items) forEachIdent(*it, binders, fn);
      return;
  }
}

// The leaf-name subrange of a reference: for a qualified `Keys.strike`
// only `strike` is the symbol's own text (and the part a rename edits).
Span identLeafSpan(const std::string& src, const Expr& e) {
  uint32_t lo = e.span.lo;
  std::string surface = src.substr(e.span.lo, e.span.hi - e.span.lo);
  if (size_t dot = surface.rfind('.'); dot != std::string::npos)
    lo = e.span.lo + (uint32_t)dot + 1;
  return {lo, e.span.hi};
}

struct RefLoc {
  const CheckedModule* mod = nullptr;
  Span span{};
  bool isDecl = false;
};

// References to a local binder: its declaration plus every use in the
// enclosing definition's body that resolves to it (shadowing respected).
void collectLocalRefs(const CheckedModule& cm, const SymbolTarget& t,
                      std::vector<RefLoc>& out) {
  const std::string& src = cm.parsed.source;
  Span decl = binderNameSpan(src, t.binder);
  if (decl.hi > decl.lo) out.push_back({&cm, decl, true});
  const TopDef& d = *t.def;
  if (!d.body) return;
  std::vector<Binder> binders;
  for (auto& p : d.params) binders.push_back({p.name, p.span, p.labeled});
  forEachIdent(*d.body, binders,
               [&](const Expr& e, const std::vector<Binder>& bs) {
    if (!e.moduleName.empty() || e.name != t.binder.name) return;
    for (auto it = bs.rbegin(); it != bs.rend(); ++it)
      if (it->name == e.name) {
        if (it->span.lo == t.binder.span.lo &&
            it->span.hi == t.binder.span.hi)
          out.push_back({&cm, identLeafSpan(src, e), false});
        return;  // some other binder shadows ours (or is ours)
      }
  });
}

// References to a top-level definition within one checked program: the
// declaration in the defining module plus every use. The checker rewrote
// resolved references to canonical form (moduleName = defining module's
// id, "" for the module's own globals; name = stored dotted key), so
// matching is a straight comparison once local binders are ruled out.
void collectProgramRefs(const Program& prog, const SymbolTarget& t,
                        std::vector<RefLoc>& out) {
  const CheckedModule* host = prog.find(t.hostId);
  if (!host || canonicalSourceKey(host->parsed.path) != t.hostKey)
    return;  // this program's module of that id is a different file
  if (const TopDef* d = findDef(host->parsed.defs, t.stored)) {
    Span ns = nameSpan(host->parsed.source, *d);
    if (ns.hi > ns.lo) out.push_back({host, ns, true});
  }
  for (auto& m : prog.modules) {
    std::function<void(const std::vector<TopDef>&)> walk =
        [&](const std::vector<TopDef>& defs) {
      for (auto& d : defs) {
        if (d.kind == TopDef::Kind::ModuleDef) {
          walk(d.defs);
          continue;
        }
        if (d.kind != TopDef::Kind::Let || !d.body) continue;
        std::vector<Binder> binders;
        for (auto& p : d.params)
          binders.push_back({p.name, p.span, p.labeled});
        forEachIdent(*d.body, binders,
                     [&](const Expr& e, const std::vector<Binder>& bs) {
          if (e.moduleName.empty() &&
              e.name.find('.') == std::string::npos)
            for (auto it = bs.rbegin(); it != bs.rend(); ++it)
              if (it->name == e.name) return;  // a local wins
          std::string hostId =
              e.moduleName.empty() ? m.parsed.name : e.moduleName;
          if (hostId != t.hostId || e.name != t.stored) return;
          out.push_back({&m, identLeafSpan(m.parsed.source, e), false});
        });
      }
    };
    walk(m.parsed.defs);
  }
}

// The symbol at `off`, if it is one references/rename can work with:
// a value binding or a reference to one. Module names, imports, opens
// and labels are not targets.
std::optional<SymbolTarget> targetAt(const Program& prog,
                                     const CheckedModule& cm,
                                     const std::string& text, uint32_t off) {
  DefHit hit;
  findAt(cm.parsed.defs, off, "", hit);
  if (hit.ident) {
    const Expr& e = *hit.ident;
    std::string surface = text.substr(e.span.lo, e.span.hi - e.span.lo);
    size_t lastDot = surface.rfind('.');
    if (lastDot != std::string::npos &&
        off < e.span.lo + (uint32_t)lastDot + 1)
      return std::nullopt;  // on the module part of a qualified name
    if (e.moduleName.empty() && e.name.find('.') == std::string::npos)
      for (auto it = hit.binders.rbegin(); it != hit.binders.rend(); ++it)
        if (it->name == e.name) {
          SymbolTarget t;
          t.local = true;
          t.def = hit.def;
          t.binder = *it;
          return t;
        }
    const CheckedModule* host =
        e.moduleName.empty() ? &cm : prog.find(e.moduleName);
    if (!host || !findDef(host->parsed.defs, e.name)) return std::nullopt;
    SymbolTarget t;
    t.hostId = host->parsed.name;
    t.hostKey = canonicalSourceKey(host->parsed.path);
    t.stored = e.name;
    return t;
  }
  if (hit.def && hit.def->kind == TopDef::Kind::Let) {
    Span ns = nameSpan(text, *hit.def);
    if (hit.def->name != "_" && ns.hi > ns.lo && covers(ns, off)) {
      SymbolTarget t;
      t.hostId = cm.parsed.name;
      t.hostKey = canonicalSourceKey(cm.parsed.path);
      t.stored = hit.prefix + hit.def->name;
      return t;
    }
    for (auto& p : hit.def->params)
      if (p.name != "_" && covers(p.span, off)) {
        SymbolTarget t;
        t.local = true;
        t.def = hit.def;
        t.binder = {p.name, p.span, p.labeled};
        return t;
      }
    if (hit.def->body) {
      BinderDecl bd;
      findBinderDecl(text, *hit.def->body, off, bd);
      if (bd.found) {
        SymbolTarget t;
        t.local = true;
        t.def = hit.def;
        t.binder = bd.binder;
        return t;
      }
    }
  }
  return std::nullopt;
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
        // The annotation can be unresolved (null) when the enclosing
        // definition's own signature failed to check; skip it rather
        // than surface a typeless entry.
        if (e.declType) sc.values[e.name] = e.declType;
        collectExprScope(*e.items[1], off, sc);
      }
      return;
    case Expr::Kind::Lambda:
      if (covers(e.items[0]->span, off)) {
        for (auto& p : e.params)
          if (p.type) sc.values[p.name] = p.type;
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
          for (auto& p : d.params)
            if (p.type) sc.values[p.name] = p.type;
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

// --- Document symbols ------------------------------------------------------

// LSP SymbolKind constants (the few we use).
constexpr int kSymModule = 2;
constexpr int kSymFunction = 12;
constexpr int kSymConstant = 14;

// Hierarchical DocumentSymbols: inline modules nest, lets carry their
// checked type as detail. `let _` render effects have no name to list.
Value documentSymbols(const CheckedModule& cm, const std::string& text,
                      const std::vector<TopDef>& defs,
                      const std::string& prefix) {
  std::vector<Value> items;
  for (auto& d : defs) {
    if (d.kind == TopDef::Kind::ModuleDef) {
      Value v = json::makeObject();
      v.set("name", json::makeString(d.name));
      v.set("kind", json::makeNumber(kSymModule));
      v.set("range", rangeValue(text, d.span));
      v.set("selectionRange", rangeValue(text, nameSpan(text, d)));
      v.set("children",
            documentSymbols(cm, text, d.defs, prefix + d.name + "."));
      items.push_back(std::move(v));
    } else if (d.kind == TopDef::Kind::Let && d.name != "_") {
      auto it = cm.defTypes.find(prefix + d.name);
      TypePtr ty = it != cm.defTypes.end() ? it->second : nullptr;
      bool isFun =
          !d.params.empty() || (ty && ty->kind == Type::Kind::Fun);
      Value v = json::makeObject();
      v.set("name", json::makeString(d.name));
      v.set("kind", json::makeNumber(isFun ? kSymFunction : kSymConstant));
      if (ty) v.set("detail", json::makeString(typeName(ty)));
      v.set("range", rangeValue(text, d.span));
      v.set("selectionRange", rangeValue(text, nameSpan(text, d)));
      items.push_back(std::move(v));
    }
  }
  return json::makeArray(std::move(items));
}

// --- Formatting ------------------------------------------------------------
// Conservative and purely lexical: the author's line breaks, indentation
// and comments are kept; only horizontal whitespace between tokens is
// normalized (plus trailing whitespace, runs of blank lines, and the
// final newline). Token text is copied from the source verbatim, so
// formatting can never change what the file means.

bool endsValue(Tok k) {
  switch (k) {
    case Tok::Ident:
    case Tok::UpIdent:
    case Tok::TypeVar:
    case Tok::Number:
    case Tok::IntNum:
    case Tok::Time:
    case Tok::Bool:
    case Tok::String:
    case Tok::RParen:
    case Tok::RBracket:
      return true;
    default:
      return false;
  }
}

// Spaces between two tokens on one line (no comment between them).
// `gap` is the original inter-token text: annotation colons keep the
// author's choice (spaced `let x : Scalar`, tight `~gain:Scalar`),
// normalized to at most one space.
int spacesBetween(Tok prev, Tok cur, const std::string& gap,
                  bool prevUnaryMinus) {
  if (prevUnaryMinus) return 0;
  if (prev == Tok::Tilde || prev == Tok::Dot || prev == Tok::LParen ||
      prev == Tok::LBracket)
    return 0;
  if (cur == Tok::Dot || cur == Tok::Comma || cur == Tok::Semi ||
      cur == Tok::RParen || cur == Tok::RBracket)
    return 0;
  if (prev == Tok::Colon || cur == Tok::Colon) return gap.empty() ? 0 : 1;
  return 1;
}

// Trailing spaces/tabs stripped before every newline; tabs in what
// remains become two spaces (the language's indent unit).
std::string stripTrailingWs(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\n')
      while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    out += c;
  }
  return out;
}

std::string expandTabs(const std::string& s) {
  std::string out;
  for (char c : s)
    if (c == '\t')
      out += "  ";
    else
      out += c;
  return out;
}

std::string formatTokens(const std::string& src,
                         const std::vector<Token>& toks) {
  std::string out;
  uint32_t prevEnd = 0;
  Tok prev = Tok::Eof;
  bool first = true;
  bool prevUnaryMinus = false;
  for (auto& t : toks) {
    uint32_t lo = t.kind == Tok::Eof ? (uint32_t)src.size() : t.span.lo;
    std::string gap = src.substr(prevEnd, lo - prevEnd);
    bool hasComment = gap.find("(*") != std::string::npos;
    if (t.kind == Tok::Eof) {
      if (hasComment) {
        std::string g = stripTrailingWs(gap);
        if (first) g.erase(0, g.find_first_not_of('\n'));
        while (!g.empty() && g.back() == '\n') g.pop_back();
        out += g;
      }
      if (!out.empty()) out += '\n';
      break;
    }
    if (hasComment) {
      // Comments (and any alignment around them) pass through verbatim.
      std::string g = stripTrailingWs(gap);
      if (first) g.erase(0, g.find_first_not_of('\n'));
      out += g;
    } else if (first) {
      // Drop blank space before the first token.
    } else if (size_t nl = (size_t)std::count(gap.begin(), gap.end(), '\n');
               nl > 0) {
      // Keep the line break (collapsing 2+ blank lines to one) and the
      // author's indentation of the new line.
      out.append(std::min<size_t>(nl, 2), '\n');
      out += expandTabs(gap.substr(gap.rfind('\n') + 1));
    } else {
      size_t n = (size_t)spacesBetween(prev, t.kind, gap, prevUnaryMinus);
      // A run of two or more spaces before `=` is a hand-aligned column
      // (library interfaces align their module bindings): layout, not
      // noise, so it is kept.
      if (t.kind == Tok::Equals && gap.size() >= 2 &&
          gap.find_first_not_of(' ') == std::string::npos)
        n = gap.size();
      out.append(n, ' ');
    }
    out.append(src, t.span.lo, t.span.hi - t.span.lo);
    prevUnaryMinus = t.kind == Tok::Minus && (first || !endsValue(prev));
    prev = t.kind;
    prevEnd = t.span.hi;
    first = false;
  }
  return out;
}

// --- Rename ----------------------------------------------------------------

// Value names are lowercase-initial identifiers (types and modules are
// uppercase-initial and not renameable here).
bool validValueName(const std::string& s) {
  if (s.empty() || s == "_") return false;
  if (!std::islower((unsigned char)s[0]) && s[0] != '_') return false;
  for (char c : s)
    if (!isIdentCont(c)) return false;
  for (const char* k : kKeywords)
    if (s == k) return false;
  return true;
}

}  // namespace

// --- The server ------------------------------------------------------------

LspServer::Analysis& LspServer::analysisFor(const std::string& uri) {
  return analysisForPath(docs_.at(uri).path);
}

LspServer::Analysis& LspServer::analysisForPath(const std::string& path) {
  std::string key = canonicalSourceKey(path);
  auto cached = cache_.find(key);
  if (cached != cache_.end()) return cached->second;

  Analysis a;
  std::map<std::string, std::string> overlay;
  for (auto& [u, d] : docs_) overlay[d.key] = d.text;

  ModuleLoadContext ctx;
  ctx.overlay = &overlay;
  LibraryRegistry reg;
  std::string root = findEnclosingRoot(fs::path(path).parent_path().string());
  if (!root.empty()) {
    reg = discoverLibraries(root, a.diags);
    ctx.registry = &reg;
    // Like lint: be lenient about deps - every discovered library is in
    // scope, so editing never demands manifest bookkeeping first.
    for (auto& [name, li] : reg.byName) ctx.deps.push_back(name);
    std::error_code ec;
    fs::path fp = fs::absolute(path, ec);
    for (auto& [name, li] : reg.byName) {
      fs::path rel = fs::relative(fp, fs::absolute(li.dir, ec), ec);
      if (!rel.empty() && rel.string().rfind("..", 0) != 0)
        ctx.currentLib = reg.find(name);
    }
  }
  a.program = checkProject({path}, a.diags, &ctx);
  return cache_.emplace(key, std::move(a)).first->second;
}

// The files a reference search covers: every open document plus every
// .synth file under the enclosing project root (skipping build outputs
// and hidden directories). Each is analyzed as its own root, so a
// definition's uses are found in files that import it even when those
// files are not open.
std::vector<std::string> LspServer::workspaceSourceFiles(
    const std::string& nearPath) {
  std::vector<std::string> files;
  std::set<std::string> seen;
  auto add = [&](const std::string& p) {
    if (seen.insert(canonicalSourceKey(p)).second) files.push_back(p);
  };
  for (auto& [u, d] : docs_) add(d.path);
  std::string root =
      findEnclosingRoot(fs::path(nearPath).parent_path().string());
  if (!root.empty()) {
    std::error_code ec;
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      std::string base = it->path().filename().string();
      std::error_code typeEc;
      if (it->is_directory(typeEc)) {
        if (base == "_build" || base == "build" ||
            (!base.empty() && base[0] == '.'))
          it.disable_recursion_pending();
        continue;
      }
      if (it->path().extension() == ".synth") add(it->path().string());
    }
  }
  return files;
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
    caps.set("referencesProvider", json::makeBool(true));
    caps.set("renameProvider", json::makeBool(true));
    caps.set("documentSymbolProvider", json::makeBool(true));
    caps.set("documentFormattingProvider", json::makeBool(true));
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

  if (method == "textDocument/formatting") {
    std::string uri = textDocumentUri();
    auto docIt = docs_.find(uri);
    if (docIt == docs_.end()) {
      respond(json::makeNull());
      return out;
    }
    const Document& doc = docIt->second;
    DiagnosticBag lexDiags;
    std::vector<Token> toks = lex(doc.text, doc.path, lexDiags);
    if (lexDiags.hasErrors()) {
      // Formatting a file the lexer cannot read would risk mangling it.
      respond(json::makeNull());
      return out;
    }
    std::string formatted = formatTokens(doc.text, toks);
    if (formatted == doc.text) {
      respond(json::makeArray());
      return out;
    }
    Value edit = json::makeObject();
    edit.set("range",
             rangeValue(doc.text, Span{0, (uint32_t)doc.text.size()}));
    edit.set("newText", json::makeString(formatted));
    std::vector<Value> edits;
    edits.push_back(std::move(edit));
    respond(json::makeArray(std::move(edits)));
    return out;
  }

  if (method == "textDocument/definition" ||
      method == "textDocument/completion" ||
      method == "textDocument/hover" ||
      method == "textDocument/references" ||
      method == "textDocument/rename" ||
      method == "textDocument/documentSymbol") {
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
      respond(method == "textDocument/completion" ||
                      method == "textDocument/documentSymbol"
                  ? json::makeArray()
                  : json::makeNull());
      return out;
    }
    uint32_t off = requestOffset(doc);

    if (method == "textDocument/documentSymbol") {
      respond(documentSymbols(*cm, doc.text, cm->parsed.defs, ""));
      return out;
    }

    if (method == "textDocument/references" ||
        method == "textDocument/rename") {
      auto target = targetAt(a.program, *cm, doc.text, off);
      if (!target) {
        if (method == "textDocument/rename")
          respondError(-32602, "nothing renameable at this position");
        else
          respond(json::makeNull());
        return out;
      }
      if (method == "textDocument/rename") {
        std::string newName =
            params ? params->getString("newName") : std::string{};
        if (!validValueName(newName)) {
          respondError(-32602, "'" + newName +
                                   "' is not a valid value name "
                                   "(lowercase-initial identifier)");
          return out;
        }
        if (target->local && target->binder.labeled) {
          respondError(-32602,
                       "cannot rename labeled parameter '~" +
                           target->binder.name +
                           "': the label is part of every call site");
          return out;
        }
        if (!target->local) {
          std::string stdlibKey = canonicalSourceKey(bundledStdlibDir());
          if (!stdlibKey.empty() &&
              target->hostKey.rfind(stdlibKey, 0) == 0) {
            respondError(-32602,
                         "cannot rename a definition in the bundled "
                         "standard library");
            return out;
          }
        }
      }
      std::vector<RefLoc> refs;
      if (target->local) {
        collectLocalRefs(*cm, *target, refs);
      } else {
        for (auto& f : workspaceSourceFiles(doc.path))
          collectProgramRefs(analysisForPath(f).program, *target, refs);
      }
      std::stable_sort(refs.begin(), refs.end(),
                       [](const RefLoc& x, const RefLoc& y) {
        if (x.mod->parsed.path != y.mod->parsed.path)
          return x.mod->parsed.path < y.mod->parsed.path;
        return x.span.lo < y.span.lo;
      });
      std::set<std::tuple<std::string, uint32_t, uint32_t>> seen;

      if (method == "textDocument/references") {
        bool includeDecl = true;
        if (const Value* c = params ? params->get("context") : nullptr)
          if (const Value* v = c->get("includeDeclaration"))
            includeDecl = v->boolean;
        std::vector<Value> items;
        for (auto& r : refs) {
          if (!includeDecl && r.isDecl) continue;
          if (!seen.insert({canonicalSourceKey(r.mod->parsed.path),
                            r.span.lo, r.span.hi})
                   .second)
            continue;
          items.push_back(locationValue(r.mod->parsed.path,
                                        r.mod->parsed.source, r.span));
        }
        respond(json::makeArray(std::move(items)));
        return out;
      }

      // rename
      std::string newName = params->getString("newName");
      std::vector<std::pair<std::string, std::vector<Value>>> byUri;
      for (auto& r : refs) {
        std::string key = canonicalSourceKey(r.mod->parsed.path);
        if (!seen.insert({key, r.span.lo, r.span.hi}).second) continue;
        Value edit = json::makeObject();
        edit.set("range", rangeValue(r.mod->parsed.source, r.span));
        edit.set("newText", json::makeString(newName));
        std::string u = pathToUri(key);
        auto grp = std::find_if(
            byUri.begin(), byUri.end(),
            [&](const auto& g) { return g.first == u; });
        if (grp == byUri.end()) {
          byUri.push_back({u, {}});
          grp = byUri.end() - 1;
        }
        grp->second.push_back(std::move(edit));
      }
      Value changes = json::makeObject();
      for (auto& [u, edits] : byUri)
        changes.set(u, json::makeArray(std::move(edits)));
      Value result = json::makeObject();
      result.set("changes", std::move(changes));
      respond(std::move(result));
      return out;
    }

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
