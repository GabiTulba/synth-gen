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
  ModuleChecker(CheckedModule& mod, const Program& prog, DiagnosticBag& diags)
      : mod_(mod), prog_(prog), diags_(diags) {}

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
  // The position-ordered top-level value scope: own definitions and names
  // brought in by `open File`, later binders shadowing earlier ones. The
  // string is the canonical module id of an opened name ("" = own def).
  std::map<std::string, std::pair<TypePtr, std::string>> topScope_;
  // Whether `open Core` / `open Core.List` appeared (position-ordered):
  // primitives resolve unqualified only under these; qualified Core.name
  // and Core.List.name always work.
  bool coreOpen_ = false;
  bool coreListOpen_ = false;
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
      // Primitives live in the built-in Core namespace: reachable
      // unqualified only under `open Core` / `open Core.List`.
      if (coreOpen_) {
        if (const PrimSig* p = findCorePrim(e.name))
          return p->paramTypes.empty()
                     ? p->retType
                     : tFun(p->paramTypes, p->paramNames, p->retType);
      }
      if (coreListOpen_) {
        if (const PrimSig* p = findCoreListPrim(e.name))
          return p->paramTypes.empty()
                     ? p->retType
                     : tFun(p->paramTypes, p->paramNames, p->retType);
      }
      if (findCorePrim(e.name))
        fail(e.span, "unknown name '" + e.name +
                         "' (a Core primitive: add 'open Core' or write "
                         "Core." + e.name + ")");
      if (findCoreListPrim(e.name) || findPrimitive(e.name))
        fail(e.span, "unknown name '" + e.name +
                         "' (a Core.List function: 'open Core' + List." +
                         (e.name == "list_init" ? "init" : e.name) +
                         ", or 'open Core.List')");
      fail(e.span, "unknown name '" + e.name + "'");
    }
    std::string canonical = resolveModuleRef(e.moduleName, e.span);
    // Rewrite to the canonical id so the evaluator and the incremental
    // dependency hasher see one stable module identity regardless of the
    // surface spelling (short name, library path, open, alias).
    e.moduleName = canonical;
    if (canonical == "Core") {
      const PrimSig* p = findCorePrim(e.name);
      if (!p)
        fail(e.span, "'Core' has no primitive named '" + e.name + "'");
      return p->paramTypes.empty()
                     ? p->retType
                     : tFun(p->paramTypes, p->paramNames, p->retType);
    }
    if (canonical == "Core.List") {
      const PrimSig* p = findCoreListPrim(e.name);
      if (!p)
        fail(e.span, "'Core.List' has no function named '" + e.name + "'");
      return p->paramTypes.empty()
                     ? p->retType
                     : tFun(p->paramTypes, p->paramNames, p->retType);
    }
    const CheckedModule* m = prog_.find(canonical);
    if (!m) fail(e.span, "module '" + canonical + "' was not checked");
    auto it = m->defTypes.find(e.name);
    if (it == m->defTypes.end())
      fail(e.span, "module '" + canonical + "' has no definition named '" +
                       e.name + "'");
    return it->second;
  }

  // open M: bring M's definitions into the unqualified value scope
  // (shadowing earlier same-named binders) and its public module bindings
  // into module scope. For a library that is exactly the right thing:
  // `open Basic` opens Basic's lib.synth, so its re-exported values are
  // bare and `Keys.strike` works for every module it binds.
  void applyOpen(const TopDef& def) {
    try {
      const std::string& surface = def.moduleName;
      // The built-in Core namespace: `open Core` makes primitives
      // callable bare and its List submodule reachable as `List`;
      // `open Core.List` makes the list functions callable bare.
      std::string builtin = tryResolveModuleRef(surface, def.span);
      if (builtin == "Core") {
        coreOpen_ = true;
        mod_.moduleScope["List"] = "Core.List";
        return;
      }
      if (builtin == "Core.List") {
        coreListOpen_ = true;
        return;
      }
      std::string canonical = resolveModuleRef(surface, def.span);
      const CheckedModule* m = prog_.find(canonical);
      if (!m) fail(def.span, "module '" + canonical + "' was not checked");
      for (auto& [name, type] : m->defTypes)
        topScope_[name] = {type, canonical};
      for (auto& [name, target] : m->exportedModules)
        mod_.moduleScope[name] = target;
    } catch (const Abort&) {
      // Diagnostic recorded; later defs check against the scope so far.
    }
  }

  // module Alias = Path: bind (or override) a module name. The target may
  // be a file module, a library, or an earlier alias - anything spelled in
  // module scope. The binding is also this module's public surface: in a
  // library's lib.synth, `module Keys = Keys` is what exposes Basic.Keys.
  void applyAlias(const TopDef& def) {
    try {
      std::string canonical = resolveModuleRef(def.moduleName, def.span);
      mod_.moduleScope[def.name] = canonical;
      mod_.exportedModules[def.name] = canonical;
    } catch (const Abort&) {
      // Diagnostic recorded.
    }
  }

  // Resolve a surface module qualifier ("Keys", "Basic.Keys", "Core",
  // an alias...) to a canonical module id, or "" when nothing matches.
  std::string tryResolveModuleRef(const std::string& surface, Span span) {
    auto ms = mod_.moduleScope.find(surface);
    if (ms != mod_.moduleScope.end()) return ms->second;
    // The built-in Core namespace is always in scope (unless shadowed by
    // an alias above).
    if (surface == "Core") return "Core";
    if (surface == "Core.List") return "Core.List";
    size_t dot = surface.find('.');
    if (dot != std::string::npos) {
      std::string first = surface.substr(0, dot);
      std::string rest = surface.substr(dot + 1);
      if (rest.find('.') != std::string::npos) return {};
      // An alias of Core composes with its List submodule (C.List).
      auto cs = mod_.moduleScope.find(first);
      if (cs != mod_.moduleScope.end() && cs->second == "Core" &&
          rest == "List")
        return "Core.List";
      // "Lib.File": the qualifier names a module in scope (a library, via
      // its lib.synth, or an alias of one) and `rest` must be one of the
      // module bindings it publishes.
      if (cs != mod_.moduleScope.end()) {
        const std::string& qualifier = cs->second;
        if (const CheckedModule* m = prog_.find(qualifier)) {
          auto ex = m->exportedModules.find(rest);
          if (ex != m->exportedModules.end()) return ex->second;
          // A library's interface module carries the library's own name;
          // a missing binding there is an exposure problem, not a typo.
          if (m->libName == qualifier)
            fail(span, "module '" + qualifier + "." + rest +
                           "' is not exposed by library '" + qualifier +
                           "' (add 'module " + rest + " = " + rest +
                           " ;;' to its " + kLibraryInterfaceFile + ")");
          fail(span, "module '" + qualifier + "' has no module named '" +
                         rest + "'");
        }
      }
    }
    return {};
  }

  // As tryResolveModuleRef, but unresolved references fail with a
  // diagnostic.
  std::string resolveModuleRef(const std::string& surface, Span span) {
    std::string canonical = tryResolveModuleRef(surface, span);
    if (!canonical.empty()) return canonical;
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
    if (callee.kind == Expr::Kind::Ident) {
      if (callee.moduleName.empty()) {
        // Bare primitive names resolve only under open Core /
        // open Core.List, and any explicit binder shadows them.
        if (!env.count(callee.name) && !topScope_.count(callee.name)) {
          if (coreOpen_) prim = findCorePrim(callee.name);
          if (!prim && coreListOpen_) prim = findCoreListPrim(callee.name);
        }
      } else {
        std::string canonical =
            tryResolveModuleRef(callee.moduleName, callee.span);
        if (canonical == "Core") {
          prim = findCorePrim(callee.name);
        } else if (canonical == "Core.List") {
          prim = findCoreListPrim(callee.name);
        }
        // Rewrite so the evaluator's qualified lookup hits the Core
        // fast path regardless of alias spelling.
        if (prim) callee.moduleName = canonical;
      }
    }
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

  // A library's interface file is the library itself: `lib.synth` of
  // library Basic is module `Basic`, not `Basic.Lib`.
  auto canonicalInLibrary = [](const std::string& libName,
                               const fs::path& path) {
    return path.filename().string() == kLibraryInterfaceFile
               ? libName
               : libName + "." + moduleNameForPath(path.string());
  };

  for (auto& f : rootFiles) {
    std::string libName;
    std::string canonical;
    if (currentLib) {
      libName = currentLib->name;
      canonical = canonicalInLibrary(libName, fs::path(f));
    } else {
      canonical = moduleNameForPath(f);
    }
    loadFile(fs::path(f), canonical, libName, false, {}, {});
  }

  // Load a library's interface module (its lib.synth) under the library's
  // own name, diagnosing a library that has none. Returns false when the
  // interface is unavailable.
  auto loadLibraryInterface = [&](const LibraryInfo* lib, bool external,
                                  Span span, const std::string& fromFile,
                                  bool quiet) -> bool {
    if (byName.count(lib->name)) return true;
    if (!lib->hasInterface) {
      if (!quiet)
        diags.error(fromFile, span,
                    "library '" + lib->name + "' has no '" +
                        kLibraryInterfaceFile +
                        "': its public surface is declared there");
      return false;
    }
    return loadFile(fs::path(lib->dir) / kLibraryInterfaceFile, lib->name,
                    lib->name, external, span, fromFile);
  };

  // What a library's lib.synth publishes, read straight off its parse
  // tree: `module X = Path` -> the surface path X names. Resolving a
  // `Lib.File` reference needs this before anything is type-checked, so
  // it works on the syntax rather than on CheckedModule::exportedModules
  // (which the checker fills later from the same defs).
  auto aliasTargetIn = [&](const std::string& moduleId,
                           const std::string& aliasName) -> std::string {
    auto it = byName.find(moduleId);
    if (it == byName.end()) return {};
    for (auto& d : it->second.parsed.defs)
      if (d.kind == TopDef::Kind::ModuleAlias && d.name == aliasName)
        return d.moduleName;
    return {};
  };

  // Resolve module-path mentions transitively, on demand: resolving
  // `Lib.File` has to look inside `Lib`'s already-resolved interface, so
  // a module's mentions may be needed before the queue reaches it.
  // `resolving` doubles as the re-entry guard (a self-referential
  // interface resolves to whatever it has so far; the topological sort
  // reports the cycle).
  std::set<std::string> resolving;
  // By value: `queue` grows as modules load, so a reference into it (or
  // into byName) would dangle across a nested load.
  std::function<void(std::string)> resolveMentions =
      [&](std::string name) {
    if (!byName.count(name)) return;
    if (!resolving.insert(name).second) return;
    // Note: byName may rehash as new modules load; re-find each iteration.
    for (size_t mi = 0; mi < byName.at(name).mentions.size(); mi++) {
      auto mention = byName.at(name).mentions[mi];
      const std::string& surface = mention.surface;
      Span span = mention.span;
      const std::string file = byName.at(name).parsed.path;
      const std::string fromLib = byName.at(name).libName;
      const bool fromExternal = byName.at(name).external;
      // The built-in Core namespace never loads from disk; the checker
      // resolves it.
      if (surface == "Core" || surface.rfind("Core.", 0) == 0) continue;
      // A `module X = Path` target may name an earlier alias, which only
      // the checker can resolve (aliases are position-ordered). The
      // loader resolves alias targets opportunistically - creating load
      // edges when the target is a real file/library - and stays quiet
      // otherwise; the checker diagnoses genuinely unresolved aliases.
      bool quiet = mention.kind == TopDef::Kind::ModuleAlias;
      auto bind = [&](const std::string& key, const std::string& canonical) {
        Loaded& l2 = byName.at(name);
        l2.moduleScope[key] = canonical;
        l2.loadDeps.push_back(canonical);
      };
      size_t dot = surface.find('.');
      if (dot == std::string::npos) {
        // A sibling module: any `.synth` file next to this one. Inside a
        // library that is the library's member set, so members import
        // each other by short name with no manifest bookkeeping; the
        // interface file is not a member and is deliberately not
        // reachable this way (it would import its own importers).
        if (!fromLib.empty()) {
          const LibraryInfo* lib = libByName(fromLib);
          if (surface == fromLib) {
            if (!quiet)
              diags.error(file, span,
                          "'" + fromLib +
                              "' is this library's own interface module; "
                              "refer to sibling modules by their short name");
            continue;
          }
          std::string rel = lib ? lib->fileForModule(surface) : std::string{};
          if (!rel.empty()) {
            std::string canonical = fromLib + "." + surface;
            if (loadFile(fs::path(lib->dir) / rel, canonical, fromLib,
                         fromExternal, span, file))
              bind(surface, canonical);
            continue;
          }
        } else {
          // Standalone: an already-loaded flat module of this name
          // satisfies the import (historical behavior), else the
          // same-directory rule.
          if (byName.count(surface)) {
            bind(surface, surface);
            continue;
          }
          fs::path target =
              fs::path(file).parent_path() / (lowercase(surface) + ".synth");
          std::error_code ec;
          bool haveFile = fs::exists(target, ec);
          if (haveFile || (!quiet && !libByName(surface))) {
            if (loadFile(target, surface, "", fromExternal, span, file))
              bind(surface, surface);
            continue;
          }
        }
        // A whole library: its interface module, under the library name.
        const LibraryInfo* lib = libByName(surface);
        if (!lib) {
          if (quiet) continue;
          diags.error(file, span,
                      "unresolved import: no module or library named '" +
                          surface + "'");
          continue;
        }
        if (!depAllowed(surface, fromLib)) {
          if (quiet) continue;
          diags.error(file, span,
                      "library '" + surface +
                          "' is not declared as a dependency (add \"" +
                          surface + "\" to \"dependencies\" in build.json)");
          continue;
        }
        bool external = !(currentLib && surface == currentLib->name);
        if (loadLibraryInterface(lib, external, span, file, quiet))
          bind(surface, surface);
        continue;
      }
      // Dotted: Lib.File - a module the library's lib.synth binds.
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
      if (libName == fromLib) {
        if (quiet) continue;
        diags.error(file, span,
                    "inside library '" + libName +
                        "' refer to sibling module '" + modName +
                        "' by its short name");
        continue;
      }
      if (!depAllowed(libName, fromLib)) {
        if (quiet) continue;
        diags.error(file, span,
                    "library '" + libName +
                        "' is not declared as a dependency (add \"" +
                        libName + "\" to \"dependencies\" in build.json)");
        continue;
      }
      bool external = !(currentLib && libName == currentLib->name);
      if (!loadLibraryInterface(lib, external, span, file, quiet)) continue;
      resolveMentions(libName);
      // The interface binds the exposed name; follow it (through an alias
      // chain, if the interface renames in steps) to a canonical id.
      std::string target = aliasTargetIn(libName, modName);
      std::string canonical;
      for (int hop = 0; hop < 8 && !target.empty(); hop++) {
        auto& iface = byName.at(libName).moduleScope;
        auto it = iface.find(target);
        if (it != iface.end()) {
          canonical = it->second;
          break;
        }
        target = aliasTargetIn(libName, target);
      }
      if (canonical.empty()) {
        if (quiet) continue;
        if (aliasTargetIn(libName, modName).empty())
          diags.error(file, span,
                      "module '" + surface + "' is not exposed by library '" +
                          libName + "' (add 'module " + modName + " = " +
                          modName + " ;;' to " +
                          (fs::path(lib->dir) / kLibraryInterfaceFile)
                              .generic_string() +
                          ")");
        else
          diags.error(file, span, "library '" + libName + "' exposes '" +
                                      modName +
                                      "' but its target does not resolve");
        continue;
      }
      bind(surface, canonical);
      // Reaching `Lib.File` goes through `Lib`'s interface, so the
      // library module is in scope (and checked first) as well.
      bind(libName, libName);
    }
  };
  for (size_t qi = 0; qi < queue.size(); qi++) resolveMentions(queue[qi]);

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
    cm.libName = l.libName;
    cm.external = l.external;
    prog.modules.push_back(std::move(cm));
    ModuleChecker(prog.modules.back(), prog, diags).run();
  }
  return prog;
}

}  // namespace synth
