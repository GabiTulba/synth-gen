#include "incremental.hpp"

#include <map>
#include <set>

namespace synth {

uint64_t fnv1a(const void* data, size_t len, uint64_t seed) {
  uint64_t h = seed;
  const unsigned char* p = (const unsigned char*)data;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= kFnvPrime;
  }
  return h;
}

namespace {

uint64_t hashString(const std::string& s, uint64_t seed = kFnvOffset) {
  return fnv1a(s.data(), s.size(), seed);
}

// `name` is a stored (canonical) definition name: bare for top-level lets,
// dotted for members of inline modules ("A.x" reaches into
// `module A = struct ... end`).
const TopDef* findDefIn(const std::vector<TopDef>& defs,
                        const std::string& name,
                        TopDef::Kind kind = TopDef::Kind::Let) {
  for (auto& d : defs) {
    if (d.kind == kind && d.name == name) return &d;
    if (d.kind == TopDef::Kind::ModuleDef &&
        name.size() > d.name.size() + 1 &&
        name.compare(0, d.name.size(), d.name) == 0 &&
        name[d.name.size()] == '.') {
      if (const TopDef* r =
              findDefIn(d.defs, name.substr(d.name.size() + 1), kind))
        return r;
    }
  }
  return nullptr;
}

const TopDef* findDef(const CheckedModule& mod, const std::string& name) {
  return findDefIn(mod.parsed.defs, name);
}

// Every Let at any depth (top level and inside inline modules).
template <typename Fn>
void forEachLet(const std::vector<TopDef>& defs, Fn&& fn) {
  for (auto& d : defs) {
    if (d.kind == TopDef::Kind::Let) fn(d);
    else if (d.kind == TopDef::Kind::ModuleDef) forEachLet(d.defs, fn);
  }
}

// Collects the top-level definitions referenced by an expression:
// unqualified names that aren't parameters resolve to this module's defs
// (primitives fall through and are ignored); qualified names resolve
// through the program. Local binders are def params, `let ... in` names,
// and lambda params. The map is keyed by "Module.name" so the
// hash-combination order is stable across processes and rebuilds.
using DepMap =
    std::map<std::string, std::pair<const CheckedModule*, const TopDef*>>;

void collectPatternNames(const Pattern& p, std::set<std::string>& into) {
  if (p.kind == Pattern::Kind::Bind) into.insert(p.name);
  for (auto& s : p.items) collectPatternNames(s, into);
}

void collectDeps(const Expr& e, const std::set<std::string>& params,
                 const CheckedModule& mod, const Program& prog, DepMap& out) {
  if (e.kind == Expr::Kind::Ident) {
    if (e.moduleName.empty()) {
      if (params.count(e.name)) return;
      if (const TopDef* d = findDef(mod, e.name))
        out.emplace(mod.parsed.name + "." + e.name, std::make_pair(&mod, d));
    } else if (const CheckedModule* m = prog.find(e.moduleName)) {
      if (const TopDef* d = findDef(*m, e.name))
        out.emplace(e.moduleName + "." + e.name, std::make_pair(m, d));
    }
    return;
  }
  if (e.kind == Expr::Kind::Let) {
    // The bound expression sees the outer scope; the body additionally
    // sees (and may be shadowed by) the local name.
    collectDeps(*e.items[0], params, mod, prog, out);
    std::set<std::string> scoped = params;
    scoped.insert(e.name);
    collectDeps(*e.items[1], scoped, mod, prog, out);
    return;
  }
  if (e.kind == Expr::Kind::Lambda) {
    // Lambda params shadow same-named defs inside the body; without this
    // the shadowed def would be recorded as a spurious dependency and
    // needlessly invalidate cached artifacts.
    std::set<std::string> scoped = params;
    for (auto& p : e.params) scoped.insert(p.name);
    collectDeps(*e.items[0], scoped, mod, prog, out);
    return;
  }
  if (e.kind == Expr::Kind::Match) {
    // Pattern-bound names shadow defs inside their own arm's body only.
    collectDeps(*e.items[0], params, mod, prog, out);
    for (size_t a = 0; a + 1 < e.items.size(); a++) {
      std::set<std::string> scoped = params;
      if (a < e.patterns.size()) collectPatternNames(e.patterns[a], scoped);
      collectDeps(*e.items[a + 1], scoped, mod, prog, out);
    }
    return;
  }
  for (auto& child : e.items) collectDeps(*child, params, mod, prog, out);
}

// --- Type-declaration dependencies -----------------------------------------
//
// A definition depends on every type declaration its checked types
// mention: change a record's fields and everything typed by it must
// rebuild. The checker leaves resolved types on every expression
// (Expr::type), parameter and return annotation; walking those Named
// nodes catches record literals, projections and annotations alike.

void collectNamedDecls(const TypePtr& t, const Program& prog, DepMap& out) {
  if (!t) return;
  switch (t->kind) {
    case Type::Kind::Named: {
      if (const CheckedModule* m = prog.find(t->decl->moduleId))
        if (const TopDef* d = findDefIn(m->parsed.defs, t->decl->name,
                                        TopDef::Kind::TypeDecl))
          // The "type:" prefix keeps type keys clear of value keys in
          // the stable combination order.
          out.emplace("type:" + t->decl->moduleId + "." + t->decl->name,
                      std::make_pair(m, d));
      for (auto& x : t->items) collectNamedDecls(x, prog, out);
      return;
    }
    case Type::Kind::Signal:
    case Type::Kind::Sample:
    case Type::Kind::List:
      collectNamedDecls(t->elem, prog, out);
      return;
    case Type::Kind::Tuple:
    case Type::Kind::Fun:
      for (auto& x : t->items) collectNamedDecls(x, prog, out);
      collectNamedDecls(t->ret, prog, out);
      return;
    default:
      return;
  }
}

void collectExprTypeDeps(const Expr& e, const Program& prog, DepMap& out) {
  collectNamedDecls(e.type, prog, out);
  collectNamedDecls(e.declType, prog, out);
  for (auto& p : e.params) collectNamedDecls(p.type, prog, out);
  for (auto& child : e.items) collectExprTypeDeps(*child, prog, out);
}

void collectTypeDeps(const TopDef& def, const Program& prog, DepMap& out) {
  if (def.kind == TopDef::Kind::TypeDecl) {
    // A declaration depends on the declarations its members mention.
    for (auto& f : def.fields)
      if (f.type) collectNamedDecls(f.type->resolved, prog, out);
    for (auto& c : def.ctors)
      if (c.type) collectNamedDecls(c.type->resolved, prog, out);
    return;
  }
  for (auto& p : def.params) collectNamedDecls(p.type, prog, out);
  collectNamedDecls(def.retType, prog, out);
  if (def.body) collectExprTypeDeps(*def.body, prog, out);
}

struct Hasher {
  const Program& prog;
  std::map<const TopDef*, uint64_t> memo;

  uint64_t hashDef(const CheckedModule& mod, const TopDef& def) {
    auto it = memo.find(&def);
    if (it != memo.end()) return it->second;
    // Seed the memo to make accidental cycles terminate deterministically
    // (the checker rejects them; this is defense in depth).
    memo[&def] = kFnvOffset;

    // Own source text (covers name, params, types, literals, body).
    const std::string& src = mod.parsed.source;
    uint32_t lo = std::min<uint32_t>(def.span.lo, (uint32_t)src.size());
    uint32_t hi = std::min<uint32_t>(def.span.hi, (uint32_t)src.size());
    uint64_t h = hashString(src.substr(lo, hi - lo));
    h = fnvCombine(h, hashString(mod.parsed.name));

    DepMap deps;
    if (def.body) {
      std::set<std::string> params;
      for (auto& p : def.params) params.insert(p.name);
      collectDeps(*def.body, params, mod, prog, deps);
    }
    collectTypeDeps(def, prog, deps);
    for (auto& [key, dep] : deps)
      if (dep.second != &def)
        h = fnvCombine(h, hashDef(*dep.first, *dep.second));
    memo[&def] = h;
    return h;
  }
};

}  // namespace

uint64_t defClosureHash(const Program& prog, const CheckedModule& mod,
                        const TopDef& def) {
  Hasher hasher{prog, {}};
  return hasher.hashDef(mod, def);
}

std::map<const TopDef*, DefStats> defGraphStats(const Program& prog) {
  std::map<const TopDef*, DefStats> stats;
  std::map<const TopDef*, std::vector<const TopDef*>> edges;

  for (auto& mod : prog.modules) {
    // The stats present the *user's* definition graph: the bundled Core
    // library's declarations (and edges into them) stay out, exactly as
    // primitives always have. Content hashing (defClosureHash) still
    // covers them.
    if (mod.libName == "Core") continue;
    forEachLet(mod.parsed.defs, [&](const TopDef& def) {
      DefStats& st = stats[&def];
      st.mod = &mod;
      st.def = &def;
      if (!def.body) return;
      std::set<std::string> params;
      for (auto& p : def.params) params.insert(p.name);
      DepMap deps;
      collectDeps(*def.body, params, mod, prog, deps);
      for (auto& [key, dep] : deps) {
        if (dep.second == &def) continue;
        if (dep.first->libName == "Core") continue;
        st.directDeps++;
        edges[&def].push_back(dep.second);
      }
    });
  }
  for (auto& [def, outs] : edges)
    for (const TopDef* d : outs) {
      auto it = stats.find(d);
      if (it != stats.end()) it->second.dependents++;
    }
  for (auto& [defPtr, st] : stats) {
    std::set<const TopDef*> seen;
    std::vector<const TopDef*> stack{defPtr};
    while (!stack.empty()) {
      const TopDef* d = stack.back();
      stack.pop_back();
      if (!seen.insert(d).second) continue;
      auto it = edges.find(d);
      if (it != edges.end())
        for (const TopDef* next : it->second) stack.push_back(next);
    }
    st.closureSize = (int)seen.size();
  }
  return stats;
}

}  // namespace synth
