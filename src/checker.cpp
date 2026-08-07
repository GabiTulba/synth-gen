#include "checker.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <sstream>

#include "lexer.hpp"
#include "parser.hpp"
#include "primitives.hpp"

namespace fs = std::filesystem;

namespace synth {

namespace {

std::string lowercase(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool readFile(const fs::path& p, std::string& out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

// ---------------------------------------------------------------------------
// Per-module type checking
// ---------------------------------------------------------------------------

class ModuleChecker {
 public:
  ModuleChecker(CheckedModule& mod, const Program& prog, DiagnosticBag& diags,
                const ModuleLoadContext* ctx = nullptr)
      : mod_(mod), prog_(prog), diags_(diags), ctx_(ctx) {}

  void run() {
    // Walk definitions in order: opens and module aliases are
    // position-ordered (they affect only later definitions, and later
    // binders shadow earlier ones); imports are file-scoped and were
    // wired into moduleScope at load time.
    for (auto& def : mod_.parsed.defs) {
      switch (def.kind) {
        case TopDef::Kind::Import:
          break;
        case TopDef::Kind::Open:
          applyOpen(def);
          break;
        case TopDef::Kind::ModuleAlias:
          applyAlias(def);
          break;
        case TopDef::Kind::Let: {
          checkDef(def);
          // A definition shadows any previously opened name.
          auto dt = mod_.defTypes.find(def.name);
          if (dt != mod_.defTypes.end())
            topScope_[def.name] = {dt->second, ""};
          break;
        }
      }
    }
  }

 private:
  CheckedModule& mod_;
  const Program& prog_;
  DiagnosticBag& diags_;
  const ModuleLoadContext* ctx_ = nullptr;
  // The position-ordered top-level value scope: own definitions and names
  // brought in by `open File`, later binders shadowing earlier ones. The
  // string is the canonical module id of an opened name ("" = own def).
  std::map<std::string, std::pair<TypePtr, std::string>> topScope_;
  // Primitive signatures share global var objects (ids 0 and 1); every call
  // site instantiates them with fresh ids so two polymorphic calls - or a
  // partial application fed into another polymorphic call - can never
  // capture each other's variables.
  int nextFreshVar_ = 2;

  struct Abort {};  // stop checking the current definition

  [[noreturn]] void fail(Span span, std::string msg) {
    diags_.error(mod_.parsed.path, span, std::move(msg));
    throw Abort{};
  }

  void checkDef(TopDef& def) {
    try {
      std::map<std::string, TypePtr> env;
      for (auto& p : def.params) {
        if (env.count(p.name))
          fail(p.span, "duplicate parameter '" + p.name + "'");
        env[p.name] = p.type;
      }
      TypePtr bodyType = check(*def.body, env);
      if (def.name == "_") {
        if (def.retType && def.retType->kind != Type::Kind::Unit)
          fail(def.span, "'let _' bindings must have type unit");
        if (bodyType->kind != Type::Kind::Unit)
          fail(def.body->span,
               "'let _' body must have type unit (got " +
                   typeName(bodyType) + "); only render produces unit");
        return;
      }
      if (!def.retType) fail(def.span, "missing return type annotation");
      // A partial application of a polymorphic primitive leaves type
      // variables in the body type; the declared annotation resolves them.
      bool matches;
      if (containsVar(bodyType)) {
        Subst subst;
        matches = unify(bodyType, def.retType, subst);
      } else {
        matches = typeEquals(bodyType, def.retType);
      }
      if (!matches)
        fail(def.body->span, "body has type " + typeName(bodyType) +
                                 " but the signature declares " +
                                 typeName(def.retType));
      if (mod_.defTypes.count(def.name))
        fail(def.span, "duplicate definition of '" + def.name + "'");
      mod_.defTypes[def.name] = def.params.empty() ? def.retType
                                                   : defFunType(def);
    } catch (const Abort&) {
      // Diagnostic already recorded; give the binding its declared type so
      // later definitions don't cascade "unknown name" errors.
      if (def.name != "_" && def.retType && !mod_.defTypes.count(def.name)) {
        mod_.defTypes[def.name] =
            def.params.empty() ? def.retType : defFunType(def);
      }
    }
  }

  static TypePtr defFunType(const TopDef& def) {
    std::vector<TypePtr> ps;
    std::vector<std::string> labels;
    for (auto& p : def.params) {
      ps.push_back(p.type);
      labels.push_back(p.labeled ? p.name : "");
    }
    return tFun(std::move(ps), std::move(labels), def.retType);
  }

  TypePtr check(Expr& e, std::map<std::string, TypePtr>& env) {
    TypePtr t = checkInner(e, env);
    e.type = t;
    return t;
  }

  TypePtr checkInner(Expr& e, std::map<std::string, TypePtr>& env) {
    switch (e.kind) {
      case Expr::Kind::NumLit: return tScalar();
      case Expr::Kind::TimeLit: return tTimestamp();
      case Expr::Kind::StrLit: return tString();
      case Expr::Kind::Ident: return checkIdentIn(e, env);
      case Expr::Kind::App: return checkApp(e, env);
      case Expr::Kind::BinOp: return checkBinOp(e, env);
      case Expr::Kind::ListLit: {
        if (e.items.empty())
          fail(e.span, "cannot determine the element type of an empty list "
                       "(not supported in v1)");
        TypePtr elem = check(*e.items[0], env);
        for (size_t i = 1; i < e.items.size(); i++) {
          TypePtr t = check(*e.items[i], env);
          if (!typeEquals(t, elem))
            fail(e.items[i]->span,
                 "list elements must all have the same type: expected " +
                     typeName(elem) + ", got " + typeName(t));
        }
        return tList(elem);
      }
      case Expr::Kind::TupleLit: {
        std::vector<TypePtr> items;
        for (auto& x : e.items) items.push_back(check(*x, env));
        return tTuple(std::move(items));
      }
      case Expr::Kind::Let: {
        TypePtr boundT = check(*e.items[0], env);
        // Same rule as top-level bindings: a var-carrying partial
        // application resolves against the annotation.
        bool ok;
        if (containsVar(boundT)) {
          Subst subst;
          ok = unify(boundT, e.declType, subst);
        } else {
          ok = typeEquals(boundT, e.declType);
        }
        if (!ok)
          fail(e.items[0]->span,
               "local binding '" + e.name + "' has type " +
                   typeName(boundT) + " but is annotated as " +
                   typeName(e.declType));
        // Bind (shadowing whatever was visible), check the body, restore.
        auto prev = env.find(e.name);
        std::optional<TypePtr> saved;
        if (prev != env.end()) saved = prev->second;
        env[e.name] = e.declType;
        TypePtr bodyT = check(*e.items[1], env);
        if (saved) env[e.name] = *saved;
        else env.erase(e.name);
        return bodyT;
      }
      case Expr::Kind::Lambda: {
        // Params bind (shadowing) for the body only; the result type is
        // the function of the annotated params to the synthesized body
        // type, labels included - the same shape defFunType builds.
        for (size_t i = 0; i < e.params.size(); i++)
          for (size_t j = 0; j < i; j++)
            if (e.params[i].name == e.params[j].name)
              fail(e.params[i].span,
                   "duplicate parameter '" + e.params[i].name + "'");
        std::vector<std::pair<std::string, std::optional<TypePtr>>> saved;
        for (auto& p : e.params) {
          auto prev = env.find(p.name);
          saved.emplace_back(p.name, prev != env.end()
                                         ? std::optional<TypePtr>(prev->second)
                                         : std::nullopt);
          env[p.name] = p.type;
        }
        TypePtr bodyT = check(*e.items[0], env);
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
          if (it->second) env[it->first] = *it->second;
          else env.erase(it->first);
        }
        std::vector<TypePtr> ps;
        std::vector<std::string> labels;
        for (auto& p : e.params) {
          ps.push_back(p.type);
          labels.push_back(p.labeled ? p.name : "");
        }
        return tFun(std::move(ps), std::move(labels), bodyT);
      }
    }
    fail(e.span, "internal error: unknown expression kind");
  }

  // Resolution order for unqualified names: parameters, then earlier
  // definitions in this module, then primitives.
  TypePtr checkIdentIn(Expr& e, std::map<std::string, TypePtr>& env) {
    if (e.moduleName.empty()) {
      auto it = env.find(e.name);
      if (it != env.end()) return it->second;
      auto ts = topScope_.find(e.name);
      if (ts != topScope_.end()) {
        // An opened name resolves cross-module: rewrite to its canonical
        // module id so the evaluator and dependency hasher see a
        // qualified reference. Own definitions stay unqualified.
        if (!ts->second.second.empty()) e.moduleName = ts->second.second;
        return ts->second.first;
      }
      if (const PrimSig* p = findPrimitive(e.name))
        return tFun(p->paramTypes, p->paramNames, p->retType);
      fail(e.span, "unknown name '" + e.name + "'");
    }
    std::string canonical = resolveModuleRef(e.moduleName, e.span);
    // Rewrite to the canonical id so the evaluator and the incremental
    // dependency hasher see one stable module identity regardless of the
    // surface spelling (short name, library path, open, alias).
    e.moduleName = canonical;
    const CheckedModule* m = prog_.find(canonical);
    if (!m) fail(e.span, "module '" + canonical + "' was not checked");
    auto it = m->defTypes.find(e.name);
    if (it == m->defTypes.end())
      fail(e.span, "module '" + canonical + "' has no definition named '" +
                       e.name + "'");
    return it->second;
  }

  const LibraryInfo* libByName(const std::string& n) const {
    if (!ctx_) return nullptr;
    if (ctx_->currentLib && ctx_->currentLib->name == n)
      return ctx_->currentLib;
    return ctx_->registry ? ctx_->registry->find(n) : nullptr;
  }

  // open Lib: bring the library's exposed file modules into module scope
  // (so `File.name` works). open File / open Lib.File: bring that file's
  // definitions into the unqualified value scope, shadowing earlier
  // same-named binders.
  void applyOpen(const TopDef& def) {
    try {
      const std::string& surface = def.moduleName;
      // A whole-library open: inject its exposed file modules.
      if (surface.find('.') == std::string::npos) {
        auto il = mod_.importedLibs.find(surface);
        if (il != mod_.importedLibs.end()) {
          const LibraryInfo* lib = libByName(il->second);
          if (lib) {
            for (auto& f : lib->exposedFiles) {
              std::string m = moduleNameForPath(f);
              mod_.moduleScope[m] = il->second + "." + m;
            }
          }
          return;
        }
      }
      // A file-module open: inject its definitions as value names.
      std::string canonical = resolveModuleRef(surface, def.span);
      const CheckedModule* m = prog_.find(canonical);
      if (!m) fail(def.span, "module '" + canonical + "' was not checked");
      for (auto& [name, type] : m->defTypes)
        topScope_[name] = {type, canonical};
    } catch (const Abort&) {
      // Diagnostic recorded; later defs check against the scope so far.
    }
  }

  // module Alias = Path: bind (or override) a module name. The target may
  // be a file module (by any spelling in scope) or a whole library.
  void applyAlias(const TopDef& def) {
    try {
      const std::string& target = def.moduleName;
      if (target.find('.') == std::string::npos) {
        auto il = mod_.importedLibs.find(target);
        if (il != mod_.importedLibs.end()) {
          mod_.importedLibs[def.name] = il->second;
          return;
        }
      }
      std::string canonical = resolveModuleRef(target, def.span);
      mod_.moduleScope[def.name] = canonical;
    } catch (const Abort&) {
      // Diagnostic recorded.
    }
  }

  // Resolve a surface module qualifier ("Keys", "Basic.Keys", an alias...)
  // to a canonical module id, or fail with a diagnostic.
  std::string resolveModuleRef(const std::string& surface, Span span) {
    auto ms = mod_.moduleScope.find(surface);
    if (ms != mod_.moduleScope.end()) return ms->second;
    size_t dot = surface.find('.');
    if (dot != std::string::npos) {
      // "Lib.File" under a whole-library import (possibly via an alias
      // of the library name).
      std::string first = surface.substr(0, dot);
      std::string rest = surface.substr(dot + 1);
      auto il = mod_.importedLibs.find(first);
      if (il != mod_.importedLibs.end() &&
          rest.find('.') == std::string::npos) {
        const LibraryInfo* lib = libByName(il->second);
        if (lib && lib->fileForModule(rest).empty())
          fail(span, "library '" + il->second + "' has no module named '" +
                         rest + "'");
        if (lib && mod_.libName != lib->name && !lib->isExposedModule(rest))
          fail(span, "module '" + il->second + "." + rest +
                         "' is not exposed by library '" + il->second + "'");
        return il->second + "." + rest;
      }
    }
    fail(span,
         "module '" + surface + "' is not imported by this module");
  }

  // Application with labels. Positional arguments fill the leftmost
  // unfilled parameters in order; labeled arguments (~x:v) fill their
  // parameter by name, in any order. If every parameter is filled the
  // call evaluates; otherwise the call is a partial application whose
  // value is the curried function of the remaining parameters, in
  // declaration order, keeping their labels.
  // How to refer to the callee in diagnostics: by name when it is one,
  // generically when it is a computed expression.
  static std::string calleeDesc(const Expr& callee) {
    return callee.kind == Expr::Kind::Ident ? "'" + callee.name + "'"
                                            : "this function";
  }

  TypePtr checkApp(Expr& e, std::map<std::string, TypePtr>& env) {
    Expr& callee = *e.items[0];

    // Check arguments first; user code is monomorphic so they are concrete
    // (a nested partial application of a polymorphic primitive is the one
    // exception and surfaces as a leftover-variable error below).
    std::vector<TypePtr> argTypes;
    for (size_t i = 1; i < e.items.size(); i++)
      argTypes.push_back(check(*e.items[i], env));
    auto labelOf = [&](size_t argIdx) {
      return argIdx < e.argLabels.size() ? e.argLabels[argIdx]
                                         : std::string{};
    };

    // Resolve the callee to parameter types + labels + return type.
    // Primitive parameters are all labeled with their signature names.
    std::vector<TypePtr> paramTypes;
    std::vector<std::string> paramLabels;
    TypePtr retType;
    const PrimSig* prim = nullptr;
    if (callee.kind == Expr::Kind::Ident && callee.moduleName.empty() &&
        !env.count(callee.name) && !mod_.defTypes.count(callee.name))
      prim = findPrimitive(callee.name);
    if (prim) {
      paramTypes = prim->paramTypes;
      paramLabels = prim->paramNames;
      retType = prim->retType;
      freshenVars(paramTypes, retType);
      callee.type = tFun(paramTypes, paramLabels, retType);
    } else {
      // Any Fun-typed expression can be applied: a name, a nested partial
      // application, a function-typed parameter, a lambda, ...
      TypePtr fnType = check(callee, env);
      if (fnType->kind != Type::Kind::Fun)
        fail(callee.span,
             (callee.kind == Expr::Kind::Ident ? "'" + callee.name + "'"
                                               : "this expression") +
                 " has type " + typeName(fnType) + " and cannot be applied");
      paramTypes = fnType->items;
      for (size_t i = 0; i < paramTypes.size(); i++)
        paramLabels.push_back(fnType->labelAt(i));
      retType = fnType->ret;
    }

    // Match arguments to parameters.
    std::vector<int> argForParam(paramTypes.size(), -1);
    for (size_t j = 0; j < argTypes.size(); j++) {
      std::string label = labelOf(j);
      size_t target = paramTypes.size();
      if (!label.empty()) {
        for (size_t i = 0; i < paramTypes.size(); i++)
          if (paramLabels[i] == label && argForParam[i] < 0) {
            target = i;
            break;
          }
        if (target == paramTypes.size())
          fail(e.items[j + 1]->span,
               calleeDesc(callee) + " has no unfilled argument labeled '~" +
                   label + "'");
      } else {
        for (size_t i = 0; i < paramTypes.size(); i++)
          if (argForParam[i] < 0) {
            target = i;
            break;
          }
        if (target == paramTypes.size())
          fail(e.span, calleeDesc(callee) + " expects " +
                           std::to_string(paramTypes.size()) +
                           " argument(s), got " +
                           std::to_string(argTypes.size()));
      }
      argForParam[target] = (int)j;
    }

    // Type-check the provided arguments against their parameters.
    Subst subst;
    for (size_t i = 0; i < paramTypes.size(); i++) {
      if (argForParam[i] < 0) continue;
      size_t j = (size_t)argForParam[i];
      // unify degenerates to typeEquals when both sides are var-free (all
      // user-code types), and handles vars on either side: freshened
      // primitive parameters, or an argument that is itself a polymorphic
      // partial application.
      bool ok = unify(paramTypes[i], argTypes[j], subst);
      if (!ok) {
        std::string label = paramLabels[i].empty()
                                ? "argument " + std::to_string(i + 1)
                                : "argument '" + paramLabels[i] + "'";
        fail(e.items[j + 1]->span,
             label + " of " + calleeDesc(callee) + " expects " +
                 typeName(applySubst(paramTypes[i], subst)) + ", got " +
                 typeName(argTypes[j]));
      }
    }

    // Fully applied?
    std::vector<size_t> unfilled;
    for (size_t i = 0; i < paramTypes.size(); i++)
      if (argForParam[i] < 0) unfilled.push_back(i);
    if (unfilled.empty()) {
      TypePtr ret = applySubst(retType, subst);
      if (containsVar(ret))
        fail(e.span, "cannot determine the result type of this call to " +
                         calleeDesc(callee));
      return ret;
    }

    // Partial application: the result is the curried function of the
    // remaining parameters, in declaration order, keeping their labels.
    std::vector<TypePtr> remTypes;
    std::vector<std::string> remLabels;
    for (size_t i : unfilled) {
      remTypes.push_back(applySubst(paramTypes[i], subst));
      remLabels.push_back(paramLabels[i]);
    }
    return tFun(std::move(remTypes), std::move(remLabels),
                applySubst(retType, subst));
  }

  // Instantiate a primitive signature: rename every var id occurring in the
  // parameter/return types to a fresh id (see nextFreshVar_).
  void freshenVars(std::vector<TypePtr>& paramTypes, TypePtr& retType) {
    Subst renaming;
    std::function<void(const TypePtr&)> collect = [&](const TypePtr& t) {
      switch (t->kind) {
        case Type::Kind::Var:
          if (!renaming.count(t->var))
            renaming[t->var] = tVar(nextFreshVar_++);
          break;
        case Type::Kind::Signal:
        case Type::Kind::Sample:
        case Type::Kind::List:
          collect(t->elem);
          break;
        case Type::Kind::Tuple:
        case Type::Kind::Fun: {
          for (auto& x : t->items) collect(x);
          if (t->ret) collect(t->ret);
          break;
        }
        default:
          break;
      }
    };
    for (auto& p : paramTypes) collect(p);
    collect(retType);
    if (renaming.empty()) return;
    for (auto& p : paramTypes) p = applySubst(p, renaming);
    retType = applySubst(retType, renaming);
  }

  static bool containsVar(const TypePtr& t) {
    switch (t->kind) {
      case Type::Kind::Var: return true;
      case Type::Kind::Signal:
      case Type::Kind::Sample:
      case Type::Kind::List:
        return containsVar(t->elem);
      case Type::Kind::Tuple:
      case Type::Kind::Fun: {
        for (auto& x : t->items)
          if (containsVar(x)) return true;
        return t->ret && containsVar(t->ret);
      }
      default:
        return false;
    }
  }

  // Pointwise lifting with Scalar broadcasting (design doc §4.4).
  TypePtr checkBinOp(Expr& e, std::map<std::string, TypePtr>& env) {
    TypePtr l = check(*e.items[0], env);
    TypePtr r = check(*e.items[1], env);
    auto is = [](const TypePtr& t, Type::Kind k) { return t->kind == k; };
    using K = Type::Kind;
    if (is(l, K::Scalar) && is(r, K::Scalar)) return tScalar();
    if (is(l, K::Vector) && is(r, K::Vector)) return tVector();
    if ((is(l, K::Vector) && is(r, K::Scalar)) ||
        (is(l, K::Scalar) && is(r, K::Vector)))
      return tVector();
    if (is(l, K::Signal) && is(r, K::Signal)) {
      if (!typeEquals(l->elem, r->elem))
        fail(e.span, "cannot combine " + typeName(l) + " with " + typeName(r) +
                         " (element types differ)");
      return l;
    }
    if (is(l, K::Signal) && is(r, K::Scalar)) return l;
    if (is(l, K::Scalar) && is(r, K::Signal)) return r;
    fail(e.span, "operator is not defined for " + typeName(l) + " and " +
                     typeName(r));
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Project loading: parse roots + transitively imported modules, topological
// order, then check each module.
// ---------------------------------------------------------------------------

Program checkProject(const std::vector<std::string>& rootFiles,
                     DiagnosticBag& diags) {
  return checkProject(rootFiles, diags, nullptr);
}

Program checkProject(const std::vector<std::string>& rootFiles,
                     DiagnosticBag& diags, const ModuleLoadContext* ctx) {
  Program prog;
  const LibraryRegistry* registry = ctx ? ctx->registry : nullptr;
  const LibraryInfo* currentLib = ctx ? ctx->currentLib : nullptr;

  struct Loaded {
    ParsedModule parsed;
    std::vector<std::string> loadDeps;  // canonical ids (topo edges)
    std::map<std::string, std::string> moduleScope;
    std::map<std::string, std::string> importedLibs;
    std::string libName;
    bool external = false;
    // Raw module-path mentions to resolve: import / open / module alias.
    struct Mention {
      TopDef::Kind kind;
      std::string surface;
      Span span;
    };
    std::vector<Mention> mentions;
  };
  std::map<std::string, Loaded> byName;  // canonical module id -> module
  std::vector<std::string> queue;

  auto libByName = [&](const std::string& n) -> const LibraryInfo* {
    if (currentLib && currentLib->name == n) return currentLib;
    return registry ? registry->find(n) : nullptr;
  };

  // May module `fromLib` (or the unit under build, when empty) use library
  // `libName`? The unit's declared deps come from the context; a library's
  // files consult that library's own manifest deps.
  auto depAllowed = [&](const std::string& libName,
                        const std::string& fromLib) -> bool {
    if (fromLib.empty()) {
      if (currentLib && libName == currentLib->name) return true;
      if (!ctx) return false;
      return std::find(ctx->deps.begin(), ctx->deps.end(), libName) !=
             ctx->deps.end();
    }
    if (fromLib == libName) return true;
    const LibraryInfo* from = libByName(fromLib);
    if (!from) return false;
    return std::find(from->deps.begin(), from->deps.end(), libName) !=
           from->deps.end();
  };

  // Load one module file under its canonical id. Returns false only when
  // the file cannot be read (diagnosed).
  auto loadFile = [&](const fs::path& path, const std::string& canonical,
                      const std::string& libName, bool external, Span errSpan,
                      const std::string& errFile) -> bool {
    if (byName.count(canonical)) return true;
    std::string source;
    if (!readFile(path, source)) {
      if (errFile.empty())
        diags.projectError("cannot read source file '" + path.string() + "'");
      else
        diags.error(errFile, errSpan,
                    "unresolved import: cannot read '" + path.string() + "'");
      return false;
    }
    Loaded l;
    l.parsed.name = canonical;
    l.parsed.path = path.string();
    l.parsed.source = std::move(source);
    l.libName = libName;
    l.external = external;
    std::vector<Token> toks = lex(l.parsed.source, l.parsed.path, diags);
    l.parsed.defs = parse(toks, l.parsed.path, diags);
    for (auto& d : l.parsed.defs)
      if (d.kind != TopDef::Kind::Let)
        l.mentions.push_back({d.kind, d.moduleName, d.span});
    byName.emplace(canonical, std::move(l));
    queue.push_back(canonical);
    return true;
  };

  for (auto& f : rootFiles) {
    std::string canonical = moduleNameForPath(f);
    std::string libName;
    if (currentLib) {
      libName = currentLib->name;
      canonical = libName + "." + canonical;
    }
    loadFile(fs::path(f), canonical, libName, false, {}, {});
  }

  // Resolve module-path mentions transitively. Single names resolve to a
  // sibling file module first (the current library's listed files, or the
  // same-directory rule for standalone files), then to a discovered
  // library; dotted paths are Lib.File through the registry with `dep`
  // and `expose` enforcement.
  for (size_t qi = 0; qi < queue.size(); qi++) {
    std::string name = queue[qi];
    // Note: byName may rehash as new modules load; re-find each iteration.
    for (size_t mi = 0; mi < byName.at(name).mentions.size(); mi++) {
      auto mention = byName.at(name).mentions[mi];
      const std::string& surface = mention.surface;
      Span span = mention.span;
      Loaded& l = byName.at(name);
      const std::string file = l.parsed.path;
      // A `module X = Path` target may name an earlier alias, which only
      // the checker can resolve (aliases are position-ordered). The
      // loader resolves alias targets opportunistically - creating load
      // edges when the target is a real file/library - and stays quiet
      // otherwise; the checker diagnoses genuinely unresolved aliases.
      bool quiet = mention.kind == TopDef::Kind::ModuleAlias;
      size_t dot = surface.find('.');
      if (dot == std::string::npos) {
        // Sibling file module of the current library?
        if (!l.libName.empty()) {
          const LibraryInfo* lib = libByName(l.libName);
          std::string rel = lib ? lib->fileForModule(surface) : std::string{};
          if (!rel.empty()) {
            std::string canonical = l.libName + "." + surface;
            if (loadFile(fs::path(lib->dir) / rel, canonical, l.libName,
                         l.external, span, file)) {
              Loaded& l2 = byName.at(name);
              l2.moduleScope[surface] = canonical;
              l2.loadDeps.push_back(canonical);
            }
            continue;
          }
        } else {
          // Standalone: an already-loaded flat module of this name
          // satisfies the import (historical behavior), else the
          // same-directory rule.
          if (byName.count(surface)) {
            l.moduleScope[surface] = surface;
            l.loadDeps.push_back(surface);
            continue;
          }
          fs::path target =
              fs::path(file).parent_path() / (lowercase(surface) + ".synth");
          std::error_code ec;
          bool haveFile = fs::exists(target, ec);
          if (haveFile || (!quiet && !libByName(surface))) {
            if (loadFile(target, surface, "", l.external, span, file)) {
              Loaded& l2 = byName.at(name);
              l2.moduleScope[surface] = surface;
              l2.loadDeps.push_back(surface);
            }
            continue;
          }
        }
        // A whole library.
        const LibraryInfo* lib = libByName(surface);
        if (!lib) {
          if (quiet) continue;
          diags.error(file, span,
                      "unresolved import: no module or library named '" +
                          surface + "'");
          continue;
        }
        if (!depAllowed(surface, l.libName)) {
          if (quiet) continue;
          diags.error(file, span,
                      "library '" + surface +
                          "' is not declared as a dependency (add 'dep " +
                          surface + "' to the .build manifest)");
          continue;
        }
        bool external = !(currentLib && surface == currentLib->name);
        for (auto& f : lib->exposedFiles) {
          std::string canonical = surface + "." + moduleNameForPath(f);
          if (loadFile(fs::path(lib->dir) / f, canonical, surface, external,
                       span, file))
            byName.at(name).loadDeps.push_back(canonical);
        }
        byName.at(name).importedLibs[surface] = surface;
        continue;
      }
      // Dotted: Lib.File.
      std::string libName = surface.substr(0, dot);
      std::string modName = surface.substr(dot + 1);
      if (modName.find('.') != std::string::npos) {
        diags.error(file, span,
                    "module paths have at most two segments "
                    "(Library.File): '" + surface + "'");
        continue;
      }
      const LibraryInfo* lib = libByName(libName);
      if (!lib) {
        if (quiet) continue;
        diags.error(file, span, "unknown library '" + libName + "'");
        continue;
      }
      if (!depAllowed(libName, l.libName)) {
        if (quiet) continue;
        diags.error(file, span,
                    "library '" + libName +
                        "' is not declared as a dependency (add 'dep " +
                        libName + "' to the .build manifest)");
        continue;
      }
      std::string rel = lib->fileForModule(modName);
      if (rel.empty()) {
        if (quiet) continue;
        diags.error(file, span, "library '" + libName +
                                    "' has no module named '" + modName +
                                    "'");
        continue;
      }
      if (l.libName != libName && !lib->isExposedModule(modName)) {
        if (quiet) continue;
        diags.error(file, span, "module '" + surface +
                                    "' is not exposed by library '" +
                                    libName + "'");
        continue;
      }
      bool external = !(currentLib && libName == currentLib->name);
      std::string canonical = libName + "." + modName;
      if (loadFile(fs::path(lib->dir) / rel, canonical, libName, external,
                   span, file)) {
        Loaded& l2 = byName.at(name);
        l2.moduleScope[surface] = canonical;
        l2.loadDeps.push_back(canonical);
      }
    }
  }

  // Topological sort (DFS); cycles are project errors.
  std::vector<std::string> order;
  std::map<std::string, int> state;  // 0 = new, 1 = visiting, 2 = done
  std::function<void(const std::string&)> visit =
      [&](const std::string& name) {
        auto it = byName.find(name);
        if (it == byName.end()) return;  // unresolved; already diagnosed
        int& st = state[name];
        if (st == 2) return;
        if (st == 1) {
          diags.projectError("import cycle involving module '" + name + "'");
          return;
        }
        st = 1;
        for (auto& dep : it->second.loadDeps) visit(dep);
        st = 2;
        order.push_back(name);
      };
  for (auto& [name, l] : byName) visit(name);

  for (auto& name : order) {
    Loaded& l = byName.at(name);
    CheckedModule cm;
    cm.parsed = std::move(l.parsed);
    cm.imports = l.loadDeps;
    cm.moduleScope = std::move(l.moduleScope);
    cm.importedLibs = std::move(l.importedLibs);
    cm.libName = l.libName;
    cm.external = l.external;
    prog.modules.push_back(std::move(cm));
    ModuleChecker(prog.modules.back(), prog, diags, ctx).run();
  }
  return prog;
}

}  // namespace synth
