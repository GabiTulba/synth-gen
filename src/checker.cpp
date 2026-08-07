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
    for (auto& def : mod_.parsed.defs) {
      if (def.kind == TopDef::Kind::Import) continue;
      checkDef(def);
    }
  }

 private:
  CheckedModule& mod_;
  const Program& prog_;
  DiagnosticBag& diags_;
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
      auto dt = mod_.defTypes.find(e.name);
      if (dt != mod_.defTypes.end()) return dt->second;
      if (const PrimSig* p = findPrimitive(e.name))
        return tFun(p->paramTypes, p->paramNames, p->retType);
      fail(e.span, "unknown name '" + e.name + "'");
    }
    if (std::find(mod_.imports.begin(), mod_.imports.end(), e.moduleName) ==
        mod_.imports.end())
      fail(e.span, "module '" + e.moduleName +
                       "' is not imported by this module");
    const CheckedModule* m = prog_.find(e.moduleName);
    if (!m) fail(e.span, "module '" + e.moduleName + "' was not checked");
    auto it = m->defTypes.find(e.name);
    if (it == m->defTypes.end())
      fail(e.span, "module '" + e.moduleName + "' has no definition named '" +
                       e.name + "'");
    return it->second;
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
  Program prog;

  struct Loaded {
    ParsedModule parsed;
    std::vector<std::string> imports;
    std::vector<Span> importSpans;
  };
  std::map<std::string, Loaded> byName;  // module name -> parsed module
  std::vector<std::string> queue;

  auto loadFile = [&](const fs::path& path, Span errSpan,
                      const std::string& errFile) -> std::string {
    std::string modName = moduleNameForPath(path.string());
    if (byName.count(modName)) return modName;
    std::string source;
    if (!readFile(path, source)) {
      if (errFile.empty())
        diags.projectError("cannot read source file '" + path.string() + "'");
      else
        diags.error(errFile, errSpan,
                    "unresolved import: cannot read '" + path.string() + "'");
      return {};
    }
    Loaded l;
    l.parsed.name = modName;
    l.parsed.path = path.string();
    l.parsed.source = std::move(source);
    std::vector<Token> toks = lex(l.parsed.source, l.parsed.path, diags);
    l.parsed.defs = parse(toks, l.parsed.path, diags);
    for (auto& d : l.parsed.defs) {
      if (d.kind == TopDef::Kind::Import) {
        l.imports.push_back(d.moduleName);
        l.importSpans.push_back(d.span);
      }
    }
    byName.emplace(modName, std::move(l));
    queue.push_back(modName);
    return modName;
  };

  for (auto& f : rootFiles) loadFile(fs::path(f), {}, {});

  // Resolve imports transitively (same-directory rule: import A -> a.synth).
  for (size_t qi = 0; qi < queue.size(); qi++) {
    std::string name = queue[qi];
    Loaded& l = byName.at(name);
    fs::path dir = fs::path(l.parsed.path).parent_path();
    for (size_t i = 0; i < l.imports.size(); i++) {
      const std::string& imp = l.imports[i];
      if (byName.count(imp)) continue;
      fs::path target = dir / (lowercase(imp) + ".synth");
      loadFile(target, l.importSpans[i], l.parsed.path);
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
        for (auto& imp : it->second.imports) visit(imp);
        st = 2;
        order.push_back(name);
      };
  for (auto& [name, l] : byName) visit(name);

  for (auto& name : order) {
    Loaded& l = byName.at(name);
    CheckedModule cm;
    cm.parsed = std::move(l.parsed);
    cm.imports = l.imports;
    prog.modules.push_back(std::move(cm));
    ModuleChecker(prog.modules.back(), prog, diags).run();
  }
  return prog;
}

}  // namespace synth
