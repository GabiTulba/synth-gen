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

const TopDef* findDef(const CheckedModule& mod, const std::string& name) {
  for (auto& d : mod.parsed.defs)
    if (d.kind == TopDef::Kind::Let && d.name == name) return &d;
  return nullptr;
}

// Collects the top-level definitions referenced by an expression:
// unqualified names that aren't parameters resolve to this module's defs
// (primitives fall through and are ignored); qualified names resolve
// through the program. Scoping is simple by design: parameters are the
// only local bindings in v1. The map is keyed by "Module.name" so the
// hash-combination order is stable across processes and rebuilds.
using DepMap =
    std::map<std::string, std::pair<const CheckedModule*, const TopDef*>>;

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
  for (auto& child : e.items) collectDeps(*child, params, mod, prog, out);
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

    if (def.body) {
      std::set<std::string> params;
      for (auto& p : def.params) params.insert(p.name);
      DepMap deps;
      collectDeps(*def.body, params, mod, prog, deps);
      for (auto& [key, dep] : deps)
        if (dep.second != &def) h = fnvCombine(h, hashDef(*dep.first, *dep.second));
    }
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

}  // namespace synth
