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
  // `typeVarSeq` allocates rigid type-variable ids program-wide: one
  // module's 'a and another's (or a Core declaration's parameter) must
  // never share an id, or substitutions built across modules - a
  // declaration's parameters against a use site's arguments - would
  // conflate them.
  ModuleChecker(CheckedModule& mod, const Program& prog,
                DiagnosticBag& diags, int& typeVarSeq,
                bool warnUnusedOpens = false)
      : mod_(mod), prog_(prog), diags_(diags), typeVarSeq_(typeVarSeq),
        warnUnusedOpens_(warnUnusedOpens) {}

  void run() {
    frames_.emplace_back();  // the file-level scope
    // The prelude: Core's `list`, `Signal` and `Sample` declarations
    // (and list's Nil/Cons constructors) are ambient, like the built-in
    // type names - annotations and list literals need no import. Core
    // itself declares them at the top of its interface, so nothing to
    // seed there.
    for (const char* name : {"list", "Signal", "Sample"}) {
      if (const TypeDecl* d = coreDecl(name)) {
        frames_.back().types[name] = {d, nullptr};
        bindDeclCtors(frames_.back(), d);
      }
    }
    checkDefs(mod_.parsed.defs, "");
    // The lint aid against the cargo-culted preamble: an `open` none of
    // whose bindings (values, types, constructors, or the module name
    // itself) resolved anything in this file is noise for the reader.
    if (warnUnusedOpens_)
      for (const TopDef* open : allOpens_)
        if (!usedOpens_.count(open))
          diags_.warning(mod_.parsed.path, open->span,
                         "unused open: nothing from '" + open->moduleName +
                             "' is used by this file");
  }

 private:
  CheckedModule& mod_;
  const Program& prog_;
  DiagnosticBag& diags_;
  int& typeVarSeq_;
  bool warnUnusedOpens_ = false;
  // Every successfully resolved `open`, in order, and the ones some
  // lookup actually went through. Lookups are const, so the used-set is
  // mutable.
  std::vector<const TopDef*> allOpens_;
  mutable std::set<const TopDef*> usedOpens_;

  void markOpenUsed(const TopDef* origin) const {
    if (origin) usedOpens_.insert(origin);
  }

  // One lexical scope layer: the file's top level, or one `struct` body.
  // Lookup runs innermost frame outward; within a frame, later writes
  // overwrite earlier ones, which is exactly position-ordered shadowing.
  // Entries carry the `open` that injected them (null for every other
  // binder) so lint can tell which opens earned their keep.
  struct ScopeVal {
    TypePtr type;
    std::string moduleId;    // canonical id of the module storing it;
                             // "" = this module
    std::string storedName;  // its key there: dotted for inline-module
                             // members ("A.x"), bare otherwise
    const TopDef* fromOpen = nullptr;
  };
  // A module name in scope: a whole file module/library (prefix empty),
  // or an inline module - the dotted path `prefix` inside `moduleId`
  // ("" = this module).
  struct ModRef {
    std::string moduleId;
    std::string prefix;
    const TopDef* fromOpen = nullptr;
  };
  struct TypeRef {
    const TypeDecl* decl = nullptr;
    const TopDef* fromOpen = nullptr;
  };
  struct CtorRef {
    const TypeDecl* decl = nullptr;
    int index = 0;
    const TopDef* fromOpen = nullptr;
  };
  struct Frame {
    std::map<std::string, ScopeVal> values;
    std::map<std::string, ModRef> modules;
    // Type declarations in scope, by surface name. Same position-ordered
    // shadowing as values.
    std::map<std::string, TypeRef> types;
    // Variant constructors in scope: name -> (declaration, index).
    std::map<std::string, CtorRef> ctors;
  };
  std::vector<Frame> frames_;

  const TypeDecl* lookupTypeDecl(const std::string& name) const {
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
      auto v = it->types.find(name);
      if (v != it->types.end()) {
        markOpenUsed(v->second.fromOpen);
        return v->second.decl;
      }
    }
    return nullptr;
  }

  // A Core-declared type the compiler itself knows - through the
  // program for ordinary modules, from our own declarations while Core
  // itself is checking.
  const TypeDecl* coreDecl(const std::string& name) const {
    if (const TypeDecl* d = prog_.coreTypeDecl(name)) return d;
    if (mod_.parsed.name != "Core") return nullptr;
    auto it = mod_.typeDecls.find(name);
    return it != mod_.typeDecls.end() ? it->second.get() : nullptr;
  }
  const TypeDecl* coreListDecl() const { return coreDecl("list"); }

  // Operator lifting recognizes signals nominally, by Core's
  // declaration identity.
  bool isSignalType(const TypePtr& t) const {
    return t->kind == Type::Kind::Named && t->decl == coreDecl("Signal");
  }

  const CtorRef* lookupCtor(const std::string& name) const {
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
      auto v = it->ctors.find(name);
      if (v != it->ctors.end()) {
        markOpenUsed(v->second.fromOpen);
        return &v->second;
      }
    }
    return nullptr;
  }

  static void bindDeclCtors(Frame& frame, const TypeDecl* decl,
                            const TopDef* fromOpen = nullptr) {
    for (size_t i = 0; i < decl->ctors.size(); i++)
      frame.ctors[decl->ctors[i].name] = {decl, (int)i, fromOpen};
  }

  const ScopeVal* lookupValue(const std::string& name) const {
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
      auto v = it->values.find(name);
      if (v != it->values.end()) {
        markOpenUsed(v->second.fromOpen);
        return &v->second;
      }
    }
    return nullptr;
  }

  // Walk definitions in order: opens and module aliases are
  // position-ordered (they affect only later definitions, and later
  // binders shadow earlier ones); imports are file-scoped and were wired
  // into moduleScope at load time. An inline module's body checks under a
  // nested frame, so its opens and sibling bindings end at its `end`.
  void checkDefs(std::vector<TopDef>& defs, const std::string& prefix) {
    for (auto& def : defs) {
      switch (def.kind) {
        case TopDef::Kind::Import: {
          // Imports are position-ordered binders like everything else:
          // re-binding the imported root name at this spot lets
          // `import Fx` shadow an earlier open's same-named inline
          // module (say Core.Fx), and later binders shadow the import.
          std::string first =
              def.moduleName.substr(0, def.moduleName.find('.'));
          auto ms = mod_.moduleScope.find(first);
          if (ms != mod_.moduleScope.end())
            frames_.back().modules[first] = {ms->second, ""};
          break;
        }
        case TopDef::Kind::Open:
          applyOpen(def);
          break;
        case TopDef::Kind::ModuleAlias:
          applyAlias(def);
          break;
        case TopDef::Kind::ModuleDef: {
          std::string full = prefix + def.name;
          if (!mod_.inlineModules.insert(full).second) {
            diags_.error(mod_.parsed.path, def.span,
                         "duplicate definition of module '" + full + "'");
            break;
          }
          // The name is visible in the scope that declares the module
          // (from here on); the body gets its own frame and prefix.
          frames_.back().modules[def.name] = {"", full};
          frames_.emplace_back();
          checkDefs(def.defs, full + ".");
          frames_.pop_back();
          break;
        }
        case TopDef::Kind::Let: {
          checkDef(def, prefix);
          // A definition shadows any previously opened name.
          auto dt = mod_.defTypes.find(prefix + def.name);
          if (dt != mod_.defTypes.end())
            frames_.back().values[def.name] = {dt->second, "", dt->first};
          break;
        }
        case TopDef::Kind::TypeDecl:
          checkTypeDecl(def, prefix);
          break;
      }
    }
  }

  // type [params] Name [= { fields } | = ctors] ;;
  // The declaration's name is in scope while its own fields resolve, so
  // a recursive type can mention itself. Field and constructor types are
  // written over the declaration's own rigid parameter variables; use
  // sites substitute their arguments for them.
  void checkTypeDecl(TopDef& def, const std::string& prefix) {
    const std::string stored = prefix + def.name;
    if (mod_.typeDecls.count(stored)) {
      diags_.error(mod_.parsed.path, def.span,
                   "duplicate declaration of type '" + stored + "'");
      return;
    }
    auto decl = std::make_shared<TypeDecl>();
    decl->flavor = def.typeFlavor == TopDef::TypeFlavor::Record
                       ? TypeDecl::Flavor::Record
                   : def.typeFlavor == TopDef::TypeFlavor::Variant
                       ? TypeDecl::Flavor::Variant
                       : TypeDecl::Flavor::Abstract;
    decl->moduleId = mod_.parsed.name;
    decl->name = stored;
    defTypeVars_.clear();
    typeResolveFailed_ = false;
    for (auto& p : def.typeParams) {
      TypePtr v = tRigidVar(typeVarSeq_++, p);
      defTypeVars_.emplace(p, v);
      decl->params.push_back(p);
      decl->paramVars.push_back(v);
    }
    // Register the name before resolving members: the declaration may
    // refer to itself. Registration is what makes it visible to later
    // definitions, too (shadowing any opened name, like a let).
    mod_.typeDecls[stored] = decl;
    frames_.back().types[def.name] = {decl.get(), nullptr};
    // Members may only use the declared parameters - a stray 'c in a
    // field has no meaning at any use site.
    declTypeVarsOnly_ = true;
    for (auto& f : def.fields) {
      if (decl->findField(f.name))
        diags_.error(mod_.parsed.path, f.span,
                     "duplicate field '" + f.name + "' in type '" + stored +
                         "'");
      decl->fields.push_back({f.name, resolveType(*f.type)});
    }
    for (auto& c : def.ctors) {
      if (decl->findCtor(c.name))
        diags_.error(mod_.parsed.path, c.span,
                     "duplicate constructor '" + c.name + "' in type '" +
                         stored + "'");
      decl->ctors.push_back(
          {c.name, c.type ? resolveType(*c.type) : nullptr});
    }
    declTypeVarsOnly_ = false;
    // Constructor names enter scope once the list is complete (their
    // scope entries carry indexes into it).
    bindDeclCtors(frames_.back(), decl.get());
  }
  // Signature variables ('a) are rigid ids (negative); every use site
  // instantiates a stored signature with fresh non-negative ids so two
  // polymorphic calls - or a partial application fed into another
  // polymorphic call - can never capture each other's variables.
  int nextFreshVar_ = 0;

  struct Abort {};  // stop checking the current definition

  [[noreturn]] void fail(Span span, std::string msg) {
    diags_.error(mod_.parsed.path, span, std::move(msg));
    throw Abort{};
  }

  // --- Type resolution ----------------------------------------------------
  //
  // Annotations arrive from the parser as surface TypeExprs; they resolve
  // here, where scope is known. Type variables are scoped to one
  // top-level definition: every 'a in its parameters, return type,
  // lambda parameters and local annotations is the same rigid variable,
  // and the next definition starts over. Ids stay unique module-wide
  // (typeVarSeq_ never resets, and is shared program-wide) so a stored
  // signature can never be
  // confused with another definition's.
  std::map<std::string, TypePtr> defTypeVars_;
  // Set when any resolution in the current definition failed. Resolution
  // errors do not abort on the spot: every annotation still gets a
  // (poison) type so the LSP and the fallback signature stay usable.
  bool typeResolveFailed_ = false;
  // Inside a type declaration's members, type variables must be declared
  // parameters; a fresh one is an error rather than a new variable.
  bool declTypeVarsOnly_ = false;

  // Records the diagnostic and yields a placeholder that behaves like an
  // opaque type variable, so one bad name doesn't cascade.
  TypePtr typeError(Span span, std::string msg, const std::string& name) {
    diags_.error(mod_.parsed.path, span, std::move(msg));
    typeResolveFailed_ = true;
    return tRigidVar(typeVarSeq_++, name);
  }

  TypePtr resolveType(TypeExpr& te) {
    if (te.resolved) return te.resolved;
    return te.resolved = resolveTypeUncached(te);
  }

  TypePtr resolveTypeUncached(TypeExpr& te) {
    switch (te.kind) {
      case TypeExpr::Kind::Var: {
        auto it = defTypeVars_.find(te.name);
        if (it != defTypeVars_.end()) return it->second;
        if (declTypeVarsOnly_)
          return typeError(te.span,
                           "unbound type variable '" + te.name +
                               " (a type declaration's members may only "
                               "use its declared parameters)",
                           te.name);
        TypePtr t = tRigidVar(typeVarSeq_++, te.name);
        defTypeVars_.emplace(te.name, t);
        return t;
      }
      case TypeExpr::Kind::Tuple: {
        std::vector<TypePtr> items;
        for (auto& i : te.items) items.push_back(resolveType(*i));
        return tTuple(std::move(items));
      }
      case TypeExpr::Kind::Fun: {
        std::vector<TypePtr> ps;
        for (auto& i : te.items) ps.push_back(resolveType(*i));
        return tFun(std::move(ps), te.labels, resolveType(*te.ret));
      }
      case TypeExpr::Kind::Name:
        return resolveTypeName(te);
    }
    return typeError(te.span, "internal error: unknown type syntax", "?");
  }

  TypePtr resolveTypeName(TypeExpr& te) {
    const std::string& n = te.name;
    std::string shown = te.moduleName.empty() ? n : te.moduleName + "." + n;
    if (te.moduleName.empty()) {
      // The built-in roster. Atoms take no parameter; Signal/Sample/list
      // take exactly one, written postfix.
      bool atom = n == "Scalar" || n == "Int" || n == "Vector" ||
                  n == "Timestamp" || n == "String" || n == "Bool" ||
                  n == "unit";
      // `list`, `Signal` and `Sample` are not here: they are ordinary
      // (Core-declared, ambient) declarations and resolve through the
      // scope below.
      if (atom) {
        if (!te.args.empty())
          return typeError(te.span,
                           "type '" + n + "' does not take a parameter", n);
        if (n == "Scalar") return tScalar();
        if (n == "Int") return tInt();
        if (n == "Vector") return tVector();
        if (n == "Timestamp") return tTimestamp();
        if (n == "String") return tString();
        if (n == "Bool") return tBool();
        return tUnit();
      }
      if (const TypeDecl* decl = lookupTypeDecl(n)) {
        // Rewrite to canonical form (declaring module + stored dotted
        // name) so the incremental hasher sees one stable identity.
        te.moduleName = decl->moduleId == mod_.parsed.name ? "" : decl->moduleId;
        te.name = decl->name;
        return applyTypeArgs(te, decl);
      }
      return typeError(te.span, "unknown type '" + shown + "'", n);
    }
    // Qualified: resolve the module path, then look the name up in the
    // host module's declarations (dotted for inline-module members).
    auto r = tryResolveModulePath(te.moduleName, te.span);
    if (!r)
      return typeError(te.span, "module '" + te.moduleName +
                                    "' is not imported by this module", n);
    const CheckedModule* host = &mod_;
    if (!r->moduleId.empty()) {
      host = prog_.find(r->moduleId);
      if (!host)
        return typeError(te.span,
                         "module '" + r->moduleId + "' was not checked", n);
    }
    std::string stored = r->prefix.empty() ? n : r->prefix + "." + n;
    auto it = host->typeDecls.find(stored);
    if (it == host->typeDecls.end())
      return typeError(te.span, "module '" + te.moduleName +
                                    "' has no type named '" + n + "'", n);
    te.moduleName = host == &mod_ ? "" : r->moduleId;
    te.name = stored;
    return applyTypeArgs(te, it->second.get());
  }

  // Resolve `te`'s arguments against the declaration's arity. A single
  // parenthesized tuple spreads across a multi-parameter declaration
  // ((Scalar, String) Pair); for a one-parameter one the tuple IS the
  // argument ((String, 'a Sample) list).
  TypePtr applyTypeArgs(TypeExpr& te, const TypeDecl* decl) {
    size_t arity = decl->params.size();
    std::vector<TypePtr> args;
    if (arity > 1 && te.args.size() == 1 &&
        te.args[0]->kind == TypeExpr::Kind::Tuple &&
        te.args[0]->items.size() == arity) {
      for (auto& a : te.args[0]->items) args.push_back(resolveType(*a));
    } else {
      for (auto& a : te.args) args.push_back(resolveType(*a));
    }
    if (args.size() != arity)
      return typeError(
          te.span,
          "type '" + decl->name + "' expects " + std::to_string(arity) +
              " parameter(s), got " + std::to_string(args.size()),
          decl->name);
    return tNamed(decl, std::move(args));
  }

  // Resolves a definition's declared signature (parameters and return
  // type). Runs before anything else looks at the definition so the
  // resolved types are in place even when checking later aborts.
  void resolveDefSignature(TopDef& def) {
    defTypeVars_.clear();
    typeResolveFailed_ = false;
    for (auto& p : def.params)
      if (p.typeExpr) p.type = resolveType(*p.typeExpr);
    if (def.retTypeExpr) def.retType = resolveType(*def.retTypeExpr);
    if (typeResolveFailed_) throw Abort{};
  }

  void checkDef(TopDef& def, const std::string& prefix) {
    // Inside `module A = struct`, `x` is stored under "A.x": one flat
    // per-file map, with the dotted path as the canonical name.
    const std::string stored = prefix + def.name;
    try {
      resolveDefSignature(def);
      std::map<std::string, TypePtr> env;
      for (auto& p : def.params) {
        if (env.count(p.name))
          fail(p.span, "duplicate parameter '" + p.name + "'");
        env[p.name] = p.type;
      }
      if (def.isRec) {
        // The name is in scope in its own body, at its full annotated
        // signature. Like a parameter, it is NOT instantiated: inside
        // the body its 'a stays rigid (no polymorphic recursion).
        if (!def.retType)
          fail(def.span, "missing return type annotation ('let rec' must "
                         "declare its return type; the recursive name is "
                         "in scope in its own body at that type)");
        if (env.count(def.name))
          fail(def.span, "a parameter of '" + def.name +
                             "' shadows the recursive name itself");
        env[def.name] = defFunType(def);
      }
      if (def.retType) checkResultVarsAreBound(def);
      if (def.body->kind == Expr::Kind::External) {
        // No body to check: the annotation IS the type.
        checkExternal(def);
        if (mod_.defTypes.count(stored))
          fail(def.span, "duplicate definition of '" + stored + "'");
        mod_.defTypes[stored] =
            def.params.empty() ? def.retType : defFunType(def);
        return;
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
      if (!def.retType) {
        // Local inference: an unannotated binding takes its body's
        // synthesized type. A leftover *free* variable means nothing in
        // the body determined it (an un-pinned partial application, an
        // empty list) - the annotation is then genuinely needed.
        if (containsFreeVar(bodyType))
          fail(def.body->span,
               "cannot infer the type of '" + def.name +
                   "' (the body leaves a type undetermined); annotate "
                   "the return type");
        def.retType = bodyType;
      } else {
        // A partial application of a polymorphic primitive leaves free
        // type variables in the body type, which the declared annotation
        // resolves; a body built from this definition's own 'a carries
        // rigid ones, which unify only against the same variable in the
        // annotation.
        bool matches;
        if (containsAnyVar(bodyType)) {
          Subst subst;
          matches = unify(bodyType, def.retType, subst);
        } else {
          matches = typeEquals(bodyType, def.retType);
        }
        if (!matches)
          fail(def.body->span, "body has type " + typeName(bodyType) +
                                   " but the signature declares " +
                                   typeName(def.retType));
      }
      if (mod_.defTypes.count(stored))
        fail(def.span, "duplicate definition of '" + stored + "'");
      mod_.defTypes[stored] = def.params.empty() ? def.retType
                                                 : defFunType(def);
    } catch (const Abort&) {
      // Diagnostic already recorded; give the binding its declared type so
      // later definitions don't cascade "unknown name" errors.
      if (def.name != "_" && def.retType && !mod_.defTypes.count(stored)) {
        mod_.defTypes[stored] =
            def.params.empty() ? def.retType : defFunType(def);
      }
    }
  }

  // let name ... = external "file.cpp": C++ compiled at build time, for
  // the bundled Core library and user code alike. Every type crosses the
  // boundary - data transparently, signals and samples as engine graph
  // handles, functions and type variables as opaque values - so the only
  // signature demand left is that the name can be a C++ symbol.
  void checkExternal(TopDef& def) {
    if (def.name == "_")
      fail(def.span, "'let _' cannot be external (an external binds a "
                     "named value to an implementation)");
    if (!def.retType) fail(def.span, "missing return type annotation");
    def.body->type = def.retType;
    for (char c : def.name)
      if (!(std::isalnum((unsigned char)c) || c == '_'))
        fail(def.span, "external '" + def.name +
                           "': the name must form a valid C++ symbol "
                           "(letters, digits and '_' only)");
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

  // The substitution that maps a declaration's parameter variables to
  // the arguments of one of its instances.
  static Subst declSubst(const TypeDecl* decl,
                         const std::vector<TypePtr>& args) {
    Subst s;
    for (size_t i = 0; i < decl->paramVars.size() && i < args.size(); i++)
      s[decl->paramVars[i]->var] = args[i];
    return s;
  }

  // A record literal names no type; it resolves to the innermost visible
  // record declaration with exactly its field set. Two matches in the
  // same frame are a genuine ambiguity and an error; an inner match
  // shadows outer ones like any other binder.
  const TypeDecl* findRecordDeclByFields(const Expr& e) {
    std::set<std::string> want(e.argLabels.begin(), e.argLabels.end());
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
      const TypeDecl* found = nullptr;
      for (auto& [name, tref] : it->types) {
        const TypeDecl* decl = tref.decl;
        if (decl->flavor != TypeDecl::Flavor::Record) continue;
        if (decl->fields.size() != want.size()) continue;
        bool all = true;
        for (auto& f : decl->fields)
          if (!want.count(f.name)) {
            all = false;
            break;
          }
        if (!all) continue;
        if (found && found != decl)
          fail(e.span, "this record literal matches two types in scope, '" +
                           found->name + "' and '" + decl->name +
                           "'; annotate the binding or qualify a field "
                           "through an update ({ base with ... })");
        found = decl;
        markOpenUsed(tref.fromOpen);
      }
      if (found) return found;
    }
    std::string fs;
    for (auto& l : e.argLabels) fs += (fs.empty() ? "" : "; ") + l;
    fail(e.span,
         "no record type in scope has exactly the fields { " + fs + " }");
  }

  // Resolve a (possibly qualified) constructor reference to its
  // declaration and index. Unqualified names resolve through the scope
  // frames; qualified ones search the named module's variant
  // declarations.
  std::pair<const TypeDecl*, int> resolveCtor(const std::string& moduleName,
                                              const std::string& name,
                                              Span span) {
    if (moduleName.empty()) {
      if (const auto* c = lookupCtor(name)) return {c->decl, c->index};
      fail(span, "unknown constructor '" + name + "'");
    }
    ModRef r = resolveModulePath(moduleName, span);
    const CheckedModule* host = &mod_;
    if (!r.moduleId.empty()) {
      host = prog_.find(r.moduleId);
      if (!host) fail(span, "module '" + r.moduleId + "' was not checked");
    }
    const TypeDecl* found = nullptr;
    int foundIdx = -1;
    std::string pre = r.prefix.empty() ? "" : r.prefix + ".";
    for (auto& [dn, decl] : host->typeDecls) {
      if (!pre.empty() && (dn.rfind(pre, 0) != 0 ||
                           dn.find('.', pre.size()) != std::string::npos))
        continue;
      if (pre.empty() && dn.find('.') != std::string::npos) continue;
      for (size_t i = 0; i < decl->ctors.size(); i++) {
        if (decl->ctors[i].name != name) continue;
        if (found)
          fail(span, "constructor '" + name + "' is ambiguous in module '" +
                         moduleName + "' ('" + found->name + "' and '" +
                         decl->name + "' both declare it)");
        found = decl.get();
        foundIdx = (int)i;
      }
    }
    if (!found)
      fail(span, "module '" + moduleName + "' has no constructor named '" +
                     name + "'");
    return {found, foundIdx};
  }

  // Constructor application through the ordinary App node: exactly one
  // positional argument (tuples carry more). Constructors are not
  // first-class - a bare payload-carrying constructor is an error, so
  // this is the only path an applied one takes.
  TypePtr checkCtorApp(Expr& e, std::map<std::string, TypePtr>& env) {
    Expr& callee = *e.items[0];
    auto [decl, idx] = resolveCtor(callee.moduleName, callee.name,
                                   callee.span);
    callee.inum = idx;
    const TypeDecl::Ctor& c = decl->ctors[idx];
    if (!c.payload)
      fail(e.span, "constructor '" + callee.name + "' takes no argument");
    if (e.items.size() != 2 ||
        (!e.argLabels.empty() && !e.argLabels[0].empty()))
      fail(e.span, "constructor '" + callee.name +
                       "' takes exactly one (positional) argument; bundle "
                       "several as a tuple");
    TypePtr argT = check(*e.items[1], env);
    Subst freshening;
    for (auto& pv : decl->paramVars)
      freshening[pv->var] = tVar(nextFreshVar_++, pv->varName);
    TypePtr sig = applySubst(c.payload, freshening);
    Subst subst;
    if (!unify(sig, argT, subst))
      fail(e.items[1]->span,
           "constructor '" + callee.name + "' expects " +
               typeName(applySubst(sig, subst)) + ", got " + typeName(argT));
    std::vector<TypePtr> args;
    for (auto& pv : decl->paramVars)
      args.push_back(applySubst(freshening.at(pv->var), subst));
    TypePtr result = tNamed(decl, std::move(args));
    callee.type = result;  // the evaluator reads decl and index from here
    return result;
  }

  // Bind a pattern against the type it matches, collecting the names it
  // introduces. Patterns destructure - every type here comes from the
  // scrutinee, no inference happens.
  void bindPattern(Pattern& p, const TypePtr& t,
                   std::map<std::string, TypePtr>& binds) {
    p.type = t;
    switch (p.kind) {
      case Pattern::Kind::Wildcard:
        return;
      case Pattern::Kind::Bind:
        if (binds.count(p.name))
          fail(p.span, "duplicate name '" + p.name + "' in pattern");
        binds[p.name] = t;
        return;
      case Pattern::Kind::Tuple: {
        if (t->kind != Type::Kind::Tuple)
          fail(p.span, "this pattern destructures a tuple, but the value "
                       "has type " + typeName(t));
        if (t->items.size() != p.items.size())
          fail(p.span, "tuple pattern has " +
                           std::to_string(p.items.size()) +
                           " element(s) but the value has " +
                           std::to_string(t->items.size()));
        for (size_t i = 0; i < p.items.size(); i++)
          bindPattern(p.items[i], t->items[i], binds);
        return;
      }
      case Pattern::Kind::Record: {
        if (t->kind != Type::Kind::Named ||
            t->decl->flavor != TypeDecl::Flavor::Record)
          fail(p.span, "this pattern destructures a record, but the value "
                       "has type " + typeName(t));
        Subst args = declSubst(t->decl, t->items);
        for (size_t i = 0; i < p.fieldNames.size(); i++) {
          for (size_t j = 0; j < i; j++)
            if (p.fieldNames[i] == p.fieldNames[j])
              fail(p.items[i].span,
                   "duplicate field '" + p.fieldNames[i] + "' in pattern");
          const TypeDecl::Field* f = t->decl->findField(p.fieldNames[i]);
          if (!f)
            fail(p.items[i].span, "type '" + t->decl->name +
                                      "' has no field '" + p.fieldNames[i] +
                                      "'");
          bindPattern(p.items[i], applySubst(f->type, args), binds);
        }
        return;
      }
      case Pattern::Kind::Ctor: {
        if (t->kind != Type::Kind::Named ||
            t->decl->flavor != TypeDecl::Flavor::Variant) {
          if (t->kind == Type::Kind::Named &&
              t->decl->flavor == TypeDecl::Flavor::Abstract)
            fail(p.span, "type '" + t->decl->name +
                             "' is abstract - it has no constructors to "
                             "match");
          fail(p.span, "this pattern matches a constructor, but the value "
                       "has type " + typeName(t));
        }
        const TypeDecl::Ctor* c = t->decl->findCtor(p.name);
        if (!c)
          fail(p.span, "type '" + t->decl->name +
                           "' has no constructor '" + p.name + "'");
        for (size_t i = 0; i < t->decl->ctors.size(); i++)
          if (&t->decl->ctors[i] == c) p.ctorIndex = (int)i;
        if (!c->payload && !p.items.empty())
          fail(p.span, "constructor '" + p.name + "' takes no argument");
        if (c->payload && p.items.empty())
          fail(p.span, "constructor '" + p.name + "' carries a value; "
                       "match it too ('" + p.name + " _' at least)");
        if (c->payload)
          bindPattern(p.items[0],
                      applySubst(c->payload, declSubst(t->decl, t->items)),
                      binds);
        return;
      }
    }
  }

  // --- Match usefulness (exhaustiveness + redundancy) ---------------------
  //
  // Classic pattern-matrix usefulness (Maranget), simplified by the
  // pattern language: no literal or or-patterns, so every column is
  // either an opaque type (wildcards only), a tuple/record (one
  // constructor), or a variant (a small closed constructor set). The
  // witness built alongside makes "not exhaustive" errors concrete.

  // The single-constructor sub-columns of `t` (empty for opaque types).
  struct ColCtor {
    int index = 0;                 // variant ctor index (0 otherwise)
    std::vector<TypePtr> subTypes; // its argument columns
    std::string display;           // witness rendering, %s per sub
  };

  static std::vector<ColCtor> columnCtors(const TypePtr& t) {
    std::vector<ColCtor> out;
    if (t->kind == Type::Kind::Tuple) {
      ColCtor c;
      c.subTypes = t->items;
      out.push_back(std::move(c));
      return out;
    }
    if (t->kind != Type::Kind::Named) return out;
    if (t->decl->flavor == TypeDecl::Flavor::Record) {
      ColCtor c;
      Subst args = declSubst(t->decl, t->items);
      for (auto& f : t->decl->fields)
        c.subTypes.push_back(applySubst(f.type, args));
      out.push_back(std::move(c));
      return out;
    }
    if (t->decl->flavor == TypeDecl::Flavor::Variant) {
      Subst args = declSubst(t->decl, t->items);
      for (size_t i = 0; i < t->decl->ctors.size(); i++) {
        ColCtor c;
        c.index = (int)i;
        if (t->decl->ctors[i].payload)
          c.subTypes.push_back(applySubst(t->decl->ctors[i].payload, args));
        out.push_back(std::move(c));
      }
      return out;
    }
    return out;
  }

  // Does row-pattern `p` cover constructor `k` of its column? Wildcards
  // and binds cover everything; a ctor pattern covers its own index;
  // tuple/record patterns cover their single constructor.
  static bool coversCtor(const Pattern& p, int k) {
    switch (p.kind) {
      case Pattern::Kind::Wildcard:
      case Pattern::Kind::Bind:
        return true;
      case Pattern::Kind::Ctor:
        return p.ctorIndex == k;
      case Pattern::Kind::Tuple:
      case Pattern::Kind::Record:
        return true;
    }
    return false;
  }

  // The sub-patterns `p` contributes for constructor `k` (arity n).
  // Record patterns list a subset of fields in any order; expand them to
  // the declaration's full field list.
  static void subPatterns(const Pattern& p, const TypePtr& t, size_t n,
                          std::vector<const Pattern*>& out) {
    static const Pattern kWild{};
    switch (p.kind) {
      case Pattern::Kind::Wildcard:
      case Pattern::Kind::Bind:
        out.insert(out.end(), n, &kWild);
        return;
      case Pattern::Kind::Ctor:
        for (auto& s : p.items) out.push_back(&s);
        out.insert(out.end(), n - p.items.size(), &kWild);
        return;
      case Pattern::Kind::Tuple:
        for (auto& s : p.items) out.push_back(&s);
        return;
      case Pattern::Kind::Record: {
        for (auto& f : t->decl->fields) {
          const Pattern* sub = &kWild;
          for (size_t i = 0; i < p.fieldNames.size(); i++)
            if (p.fieldNames[i] == f.name) sub = &p.items[i];
          out.push_back(sub);
        }
        return;
      }
    }
  }

  // Is pattern-vector `q` useful (matches something no row matches)?
  // On success `witness` (when asked for) receives one example value
  // per column that `q` matches and the rows miss.
  bool useful(const std::vector<std::vector<const Pattern*>>& rows,
              const std::vector<TypePtr>& types,
              const std::vector<const Pattern*>& q,
              std::vector<std::string>* witness) {
    if (types.empty()) return rows.empty();
    static const Pattern kWild{};
    const Pattern& q0 = *q[0];
    const TypePtr& t = types[0];
    std::vector<ColCtor> ctors = columnCtors(t);

    auto specialized = [&](const ColCtor& c, const Pattern* head)
        -> std::optional<bool> {
      // Build the specialized problem for constructor `c`, with `head`
      // as q's first pattern (a wildcard expands to sub-wildcards).
      std::vector<std::vector<const Pattern*>> subRows;
      for (auto& row : rows) {
        if (!coversCtor(*row[0], c.index)) continue;
        std::vector<const Pattern*> r;
        subPatterns(*row[0], t, c.subTypes.size(), r);
        r.insert(r.end(), row.begin() + 1, row.end());
        subRows.push_back(std::move(r));
      }
      std::vector<TypePtr> subTypes = c.subTypes;
      subTypes.insert(subTypes.end(), types.begin() + 1, types.end());
      std::vector<const Pattern*> subQ;
      subPatterns(*head, t, c.subTypes.size(), subQ);
      subQ.insert(subQ.end(), q.begin() + 1, q.end());
      std::vector<std::string> subWitness;
      if (!useful(subRows, subTypes, subQ,
                  witness ? &subWitness : nullptr))
        return std::nullopt;
      if (witness) {
        // Render this column's example from the constructor + its subs.
        std::string s = renderCtorWitness(
            t, c,
            std::vector<std::string>(subWitness.begin(),
                                     subWitness.begin() +
                                         (long)c.subTypes.size()));
        witness->clear();
        witness->push_back(std::move(s));
        witness->insert(witness->end(),
                        subWitness.begin() + (long)c.subTypes.size(),
                        subWitness.end());
      }
      return true;
    };

    if (q0.kind == Pattern::Kind::Wildcard ||
        q0.kind == Pattern::Kind::Bind) {
      // Which root constructors do the rows pin down? Only a COMPLETE
      // signature forces specializing on every constructor; otherwise
      // the default matrix decides - which is also what guarantees
      // termination against recursive declarations (wildcards never
      // unfold a constructor no row mentions).
      std::set<int> used;
      for (auto& row : rows)
        if (row[0]->kind == Pattern::Kind::Ctor)
          used.insert(row[0]->ctorIndex);
        else if (row[0]->kind == Pattern::Kind::Tuple ||
                 row[0]->kind == Pattern::Kind::Record)
          used.insert(0);
      if (!ctors.empty() && used.size() == ctors.size()) {
        for (auto& c : ctors)
          if (specialized(c, &kWild)) return true;
        return false;
      }
      // Incomplete (or opaque) column: drop it from the wildcard rows.
      std::vector<std::vector<const Pattern*>> defRows;
      for (auto& row : rows) {
        if (row[0]->kind != Pattern::Kind::Wildcard &&
            row[0]->kind != Pattern::Kind::Bind)
          continue;
        std::vector<const Pattern*> r(row.begin() + 1, row.end());
        defRows.push_back(std::move(r));
      }
      std::vector<TypePtr> rest(types.begin() + 1, types.end());
      std::vector<const Pattern*> restQ(q.begin() + 1, q.end());
      std::vector<std::string> subWitness;
      if (!useful(defRows, rest, restQ, witness ? &subWitness : nullptr))
        return false;
      if (witness) {
        std::string ex = "_";
        for (auto& c : ctors) {
          if (used.count(c.index)) continue;
          ex = renderCtorWitness(
              t, c, std::vector<std::string>(c.subTypes.size(), "_"));
          break;
        }
        witness->clear();
        witness->push_back(std::move(ex));
        witness->insert(witness->end(), subWitness.begin(),
                        subWitness.end());
      }
      return true;
    }
    // q rooted at one constructor: only its specialization matters.
    for (auto& c : ctors)
      if (coversCtor(q0, c.index)) return specialized(c, &q0).has_value();
    return false;
  }

  static std::string renderCtorWitness(const TypePtr& t, const ColCtor& c,
                                       const std::vector<std::string>& subs) {
    if (t->kind == Type::Kind::Tuple) {
      std::string s = "(";
      for (size_t i = 0; i < subs.size(); i++)
        s += (i ? ", " : "") + subs[i];
      return s + ")";
    }
    if (t->kind == Type::Kind::Named &&
        t->decl->flavor == TypeDecl::Flavor::Record) {
      std::string s = "{ ";
      for (size_t i = 0; i < subs.size(); i++)
        s += (i ? "; " : "") + t->decl->fields[i].name + " = " + subs[i];
      return s + " }";
    }
    const std::string& name = t->decl->ctors[(size_t)c.index].name;
    if (subs.empty()) return name;
    std::string payload = subs[0];
    if (payload.find(' ') != std::string::npos && payload[0] != '(' &&
        payload[0] != '{')
      payload = "(" + payload + ")";
    return name + " " + payload;
  }

  // Exhaustiveness and redundancy for a checked match.
  void checkMatchCoverage(Expr& e, const TypePtr& scrT) {
    static const Pattern kWild{};
    std::vector<std::vector<const Pattern*>> rows;
    std::vector<TypePtr> types{scrT};
    for (size_t a = 0; a < e.patterns.size(); a++) {
      std::vector<const Pattern*> q{&e.patterns[a]};
      if (!useful(rows, types, q, nullptr)) {
        if (!e.declType)  // pure duplication, not a destructuring let
          fail(e.patterns[a].span,
               "this match case is unreachable (the cases above already "
               "cover it)");
      }
      rows.push_back(std::move(q));
    }
    std::vector<std::string> witness;
    std::vector<const Pattern*> q{&kWild};
    if (useful(rows, types, q, &witness)) {
      std::string ex = witness.empty() ? "_" : witness[0];
      if (e.declType)
        fail(e.patterns[0].span,
             "this pattern must match every value of " + typeName(scrT) +
                 ", but " + ex + " is not covered (use 'match' for "
                 "refutable patterns)");
      fail(e.span, "this match is not exhaustive: for example, " + ex +
                       " is not covered");
    }
  }

  TypePtr checkInner(Expr& e, std::map<std::string, TypePtr>& env) {
    switch (e.kind) {
      case Expr::Kind::NumLit: return tScalar();
      case Expr::Kind::IntLit: return tInt();
      case Expr::Kind::TimeLit: return tTimestamp();
      case Expr::Kind::BoolLit: return tBool();
      case Expr::Kind::StrLit: return tString();
      case Expr::Kind::Ident: return checkIdentIn(e, env);
      case Expr::Kind::External:
        // The parser only produces External as a whole definition body,
        // which checkDef intercepts before reaching here.
        fail(e.span, "'external' can only be the entire body of a "
                     "top-level definition");
      case Expr::Kind::App: return checkApp(e, env);
      case Expr::Kind::BinOp: return checkBinOp(e, env);
      case Expr::Kind::Neg: {
        // Negation keeps the operand's numeric kind; the same kinds the
        // arithmetic operators accept (a rigid 'a stays out for the same
        // reason it does there: the caller never promised a number).
        TypePtr t = check(*e.items[0], env);
        switch (t->kind) {
          case Type::Kind::Int:
          case Type::Kind::Scalar:
          case Type::Kind::Vector:
            return t;
          default:
            if (isSignalType(t)) return t;
            fail(e.span, "unary '-' is not defined for " + typeName(t));
        }
      }
      case Expr::Kind::If: {
        TypePtr condT = check(*e.items[0], env);
        // The condition is a build-time Bool. A rigid 'a is rejected too:
        // it stays whatever the caller picks, and the caller never
        // promised a Bool.
        if (condT->kind != Type::Kind::Bool)
          fail(e.items[0]->span,
               "the condition of 'if' must be a Bool, got " +
                   typeName(condT));
        TypePtr thenT = check(*e.items[1], env);
        TypePtr elseT = check(*e.items[2], env);
        // Same variable rule as annotations: a var-carrying branch (a
        // partial application of a polymorphic callee) unifies against
        // the other branch.
        if (containsAnyVar(thenT) || containsAnyVar(elseT)) {
          Subst subst;
          if (unify(thenT, elseT, subst)) return applySubst(thenT, subst);
        } else if (typeEquals(thenT, elseT)) {
          return thenT;
        }
        fail(e.span, "the branches of 'if' have different types: " +
                         typeName(thenT) + " vs " + typeName(elseT));
      }
      case Expr::Kind::ListLit: {
        // [a; b; c] is sugar for a Cons chain of Core's list variant.
        // An empty [] leaves the element as a free variable that the
        // annotation (or the surrounding call) resolves - the same rule
        // as a partial application.
        const TypeDecl* list = coreListDecl();
        if (!list)
          fail(e.span, "internal error: Core's list type is not loaded");
        TypePtr elemVar = tVar(nextFreshVar_++);
        Subst subst;
        for (auto& item : e.items) {
          TypePtr t = check(*item, env);
          if (!unify(elemVar, t, subst))
            fail(item->span,
                 "list elements must all have the same type: expected " +
                     typeName(applySubst(elemVar, subst)) + ", got " +
                     typeName(t));
        }
        return tNamed(list, {applySubst(elemVar, subst)});
      }
      case Expr::Kind::TupleLit: {
        std::vector<TypePtr> items;
        for (auto& x : e.items) items.push_back(check(*x, env));
        return tTuple(std::move(items));
      }
      case Expr::Kind::RecordLit: {
        for (size_t i = 0; i < e.argLabels.size(); i++)
          for (size_t j = 0; j < i; j++)
            if (e.argLabels[i] == e.argLabels[j])
              fail(e.items[i]->span,
                   "duplicate field '" + e.argLabels[i] + "'");
        const TypeDecl* decl = findRecordDeclByFields(e);
        // Instantiate the declaration's parameters fresh, then let the
        // field values pin them down - the same shape as a call.
        Subst freshening;
        for (auto& pv : decl->paramVars)
          freshening[pv->var] = tVar(nextFreshVar_++, pv->varName);
        Subst subst;
        for (size_t i = 0; i < e.items.size(); i++) {
          const TypeDecl::Field* f = decl->findField(e.argLabels[i]);
          TypePtr sig = applySubst(f->type, freshening);
          TypePtr got = check(*e.items[i], env);
          if (!unify(sig, got, subst))
            fail(e.items[i]->span,
                 "field '" + f->name + "' of type '" + decl->name +
                     "' expects " + typeName(applySubst(sig, subst)) +
                     ", got " + typeName(got));
        }
        std::vector<TypePtr> args;
        for (auto& pv : decl->paramVars)
          args.push_back(applySubst(freshening.at(pv->var), subst));
        // A leftover free variable here means no field mentioned the
        // parameter; the annotation this literal checks against can
        // still resolve it, exactly like a partial application.
        return tNamed(decl, std::move(args));
      }
      case Expr::Kind::RecordUpdate: {
        TypePtr baseT = check(*e.items[0], env);
        if (baseT->kind != Type::Kind::Named ||
            baseT->decl->flavor != TypeDecl::Flavor::Record)
          fail(e.items[0]->span,
               "record update needs a record, got " + typeName(baseT));
        const TypeDecl* decl = baseT->decl;
        Subst args = declSubst(decl, baseT->items);
        for (size_t i = 0; i < e.argLabels.size(); i++) {
          for (size_t j = 0; j < i; j++)
            if (e.argLabels[i] == e.argLabels[j])
              fail(e.items[i + 1]->span,
                   "duplicate field '" + e.argLabels[i] + "'");
          const TypeDecl::Field* f = decl->findField(e.argLabels[i]);
          if (!f)
            fail(e.items[i + 1]->span, "type '" + decl->name +
                                           "' has no field '" +
                                           e.argLabels[i] + "'");
          TypePtr sig = applySubst(f->type, args);
          TypePtr got = check(*e.items[i + 1], env);
          bool ok;
          if (containsAnyVar(sig) || containsAnyVar(got)) {
            Subst subst;
            ok = unify(sig, got, subst);
          } else {
            ok = typeEquals(sig, got);
          }
          if (!ok)
            fail(e.items[i + 1]->span,
                 "field '" + f->name + "' of type '" + decl->name +
                     "' expects " + typeName(sig) + ", got " +
                     typeName(got));
        }
        return baseT;
      }
      case Expr::Kind::Ctor: {
        auto [decl, idx] = resolveCtor(e.moduleName, e.name, e.span);
        e.inum = idx;
        const TypeDecl::Ctor& c = decl->ctors[(size_t)idx];
        if (c.payload)
          fail(e.span, "constructor '" + e.name + "' takes an argument "
                       "(constructors are not functions; write '" + e.name +
                       " <value>')");
        Subst freshening;
        for (auto& pv : decl->paramVars)
          freshening[pv->var] = tVar(nextFreshVar_++, pv->varName);
        std::vector<TypePtr> args;
        for (auto& pv : decl->paramVars)
          args.push_back(freshening.at(pv->var));
        // Leftover free arguments resolve against the annotation or the
        // surrounding call, exactly like a partial application.
        return tNamed(decl, std::move(args));
      }
      case Expr::Kind::Match: {
        if (!e.declType && e.declTypeExpr)
          e.declType = resolveType(*e.declTypeExpr);
        TypePtr scrT = check(*e.items[0], env);
        if (e.declType) {
          // A destructuring let: the annotation pins the scrutinee, the
          // same way an ordinary local binding's annotation does.
          bool ok;
          if (containsAnyVar(scrT)) {
            Subst subst;
            ok = unify(scrT, e.declType, subst);
          } else {
            ok = typeEquals(scrT, e.declType);
          }
          if (!ok)
            fail(e.items[0]->span,
                 "this binding has type " + typeName(scrT) +
                     " but is annotated as " + typeName(e.declType));
          scrT = e.declType;
        }
        if (scrT->kind == Type::Kind::Var)
          fail(e.items[0]->span,
               "cannot match on a value whose type is not determined "
               "here (" + typeName(scrT) + ")");
        TypePtr result;
        for (size_t a = 0; a < e.patterns.size(); a++) {
          std::map<std::string, TypePtr> binds;
          bindPattern(e.patterns[a], scrT, binds);
          // Bind (shadowing) for this arm's body only, then restore.
          std::vector<std::pair<std::string, std::optional<TypePtr>>> saved;
          for (auto& [n, t] : binds) {
            auto prev = env.find(n);
            saved.emplace_back(n, prev != env.end()
                                      ? std::optional<TypePtr>(prev->second)
                                      : std::nullopt);
            env[n] = t;
          }
          TypePtr armT = check(*e.items[a + 1], env);
          for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            if (it->second) env[it->first] = *it->second;
            else env.erase(it->first);
          }
          if (!result) {
            result = armT;
            continue;
          }
          // Arms agree like `if` branches: unify when either side still
          // carries variables, structural equality otherwise.
          if (containsAnyVar(result) || containsAnyVar(armT)) {
            Subst subst;
            if (!unify(result, armT, subst))
              fail(e.items[a + 1]->span,
                   "match cases have different types: " + typeName(result) +
                       " vs " + typeName(armT));
            result = applySubst(result, subst);
          } else if (!typeEquals(result, armT)) {
            fail(e.items[a + 1]->span,
                 "match cases have different types: " + typeName(result) +
                     " vs " + typeName(armT));
          }
        }
        checkMatchCoverage(e, scrT);
        return result;
      }
      case Expr::Kind::Project: {
        TypePtr t = check(*e.items[0], env);
        if (t->kind != Type::Kind::Named ||
            t->decl->flavor != TypeDecl::Flavor::Record) {
          std::string msg = "cannot access field '" + e.name + "' of " +
                            typeName(t);
          if (t->kind == Type::Kind::Named)
            msg += " (not a record)";
          else if (t->kind == Type::Kind::Var)
            msg += " (its type is not known here)";
          fail(e.items[0]->span, msg);
        }
        const TypeDecl::Field* f = t->decl->findField(e.name);
        if (!f)
          fail(e.span, "type '" + t->decl->name + "' has no field '" +
                           e.name + "'");
        return applySubst(f->type, declSubst(t->decl, t->items));
      }
      case Expr::Kind::Let: {
        if (!e.declType && e.declTypeExpr)
          e.declType = resolveType(*e.declTypeExpr);
        // `let rec` puts the name in scope for its own bound expression
        // (at the annotated type); a plain let does not.
        std::optional<TypePtr> savedRec;
        bool hadRec = false;
        if (e.isRec) {
          auto prev = env.find(e.name);
          if (prev != env.end()) {
            hadRec = true;
            savedRec = prev->second;
          }
          env[e.name] = e.declType;
        }
        TypePtr boundT = check(*e.items[0], env);
        if (e.isRec) {
          if (hadRec) env[e.name] = *savedRec;
          else env.erase(e.name);
        }
        if (!e.declType) {
          // Local inference: an unannotated `let ... in` takes the bound
          // expression's synthesized type. A leftover free variable
          // means nothing determined it, and the annotation is needed.
          if (containsFreeVar(boundT))
            fail(e.items[0]->span,
                 "cannot infer the type of '" + e.name +
                     "' (the bound expression leaves a type "
                     "undetermined); annotate the binding");
          e.declType = boundT;
        } else {
          // Same rule as top-level bindings: a var-carrying partial
          // application resolves against the annotation.
          bool ok;
          if (containsAnyVar(boundT)) {
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
        }
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
        for (auto& p : e.params)
          if (!p.type && p.typeExpr) p.type = resolveType(*p.typeExpr);
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

  // Resolution order for unqualified names: parameters, then the scope
  // frames (own/sibling definitions and opened names). Everything but a
  // parameter or local is a stored signature, so it is instantiated here.
  TypePtr checkIdentIn(Expr& e, std::map<std::string, TypePtr>& env) {
    if (e.moduleName.empty()) {
      auto it = env.find(e.name);
      if (it != env.end()) return it->second;
      if (const ScopeVal* v = lookupValue(e.name)) {
        // Resolutions rewrite to canonical form - the module that stores
        // the definition plus its stored (dotted) name - so the evaluator
        // and dependency hasher see one stable identity regardless of
        // surface spelling (open, sibling shorthand inside a struct, ...).
        if (!v->moduleId.empty()) e.moduleName = v->moduleId;
        e.name = v->storedName;
        return instantiate(v->type);
      }
      // Not in scope. If the bundled Core library has it (in some
      // submodule), say exactly how to reach it. The Dsp prelude is a
      // view over the other submodules, so the hint prefers the
      // canonical home.
      if (const CheckedModule* core = prog_.find("Core")) {
        std::string suffix = "." + e.name;
        std::string found;
        for (auto& [stored, type] : core->defTypes) {
          if (stored.size() <= suffix.size() ||
              stored.compare(stored.size() - suffix.size(), suffix.size(),
                             suffix) != 0)
            continue;
          if (found.empty() || found.rfind("Dsp.", 0) == 0) found = stored;
        }
        if (!found.empty()) {
          std::string sub = found.substr(0, found.size() - suffix.size());
          fail(e.span, "unknown name '" + e.name + "' (a Core primitive: "
                           "'open Core." + sub + "', or 'import Core' and "
                           "write Core." + found + ")");
        }
      }
      fail(e.span, "unknown name '" + e.name + "'");
    }
    ModRef r = resolveModulePath(e.moduleName, e.span);
    if (!r.prefix.empty()) {
      // A member of an inline module: stored under its dotted name in
      // whichever file module hosts it (possibly this one).
      const CheckedModule* host = &mod_;
      if (!r.moduleId.empty()) {
        host = prog_.find(r.moduleId);
        if (!host)
          fail(e.span, "module '" + r.moduleId + "' was not checked");
      }
      std::string stored = r.prefix + "." + e.name;
      auto dt = host->defTypes.find(stored);
      if (dt == host->defTypes.end())
        fail(e.span, "module '" + e.moduleName +
                         "' has no definition named '" + e.name + "'");
      e.moduleName = r.moduleId;  // "" = this module's own globals
      e.name = stored;
      return instantiate(dt->second);
    }
    std::string canonical = r.moduleId;
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
    return instantiate(it->second);
  }

  // open M: bring M's definitions into the unqualified value scope of the
  // *current frame* (shadowing earlier same-named binders, ending with the
  // enclosing struct) and its public module bindings into module scope.
  // For a library that is exactly the right thing: `open Basic` opens
  // Basic's lib.synth, so its re-exported values are bare and
  // `Keys.strike` works for every module it binds. Opening an inline
  // module brings its immediate members and sub-modules into scope.
  void applyOpen(const TopDef& def) {
    try {
      ModRef r =
          resolveModulePath(def.moduleName, def.span, /*mentionTarget=*/true);
      // A resolved open is a lint candidate: report it if nothing it
      // binds is ever looked up through it.
      allOpens_.push_back(&def);
      // Opening also (re)binds the opened module's own name at this
      // position: after `open Fx`, `Fx.x` means the module just opened,
      // whatever earlier binders said.
      std::vector<std::string> segs = splitPath(def.moduleName);
      r.fromOpen = &def;
      frames_.back().modules[segs.back()] = r;
      const CheckedModule* host = &mod_;
      if (!r.moduleId.empty()) {
        host = prog_.find(r.moduleId);
        if (!host)
          fail(def.span, "module '" + r.moduleId + "' was not checked");
      }
      Frame& frame = frames_.back();
      if (!r.prefix.empty()) {
        // An inline module: its immediate values become bare names, its
        // immediate sub-modules become bare module names, and its
        // immediate type declarations become bare type names.
        std::string pre = r.prefix + ".";
        for (auto& [name, type] : host->defTypes)
          if (name.rfind(pre, 0) == 0 &&
              name.find('.', pre.size()) == std::string::npos)
            frame.values[name.substr(pre.size())] =
                {type, r.moduleId, name, &def};
        for (auto& p : host->inlineModules)
          if (p.rfind(pre, 0) == 0 &&
              p.find('.', pre.size()) == std::string::npos)
            frame.modules[p.substr(pre.size())] = {r.moduleId, p, &def};
        for (auto& [name, decl] : host->typeDecls)
          if (name.rfind(pre, 0) == 0 &&
              name.find('.', pre.size()) == std::string::npos) {
            frame.types[name.substr(pre.size())] = {decl.get(), &def};
            bindDeclCtors(frame, decl.get(), &def);
          }
        return;
      }
      for (auto& [name, type] : host->defTypes) {
        // Dotted names belong to the module's inline modules; those come
        // along as module names below, not as bare values.
        if (name.find('.') != std::string::npos) continue;
        frame.values[name] = {type, r.moduleId, name, &def};
      }
      for (auto& p : host->inlineModules)
        if (p.find('.') == std::string::npos)
          frame.modules[p] = {r.moduleId, p, &def};
      for (auto& [name, decl] : host->typeDecls)
        if (name.find('.') == std::string::npos) {
          frame.types[name] = {decl.get(), &def};
          bindDeclCtors(frame, decl.get(), &def);
        }
      for (auto& [name, target] : host->exportedModules)
        frame.modules[name] = {target, "", &def};
    } catch (const Abort&) {
      // Diagnostic recorded; later defs check against the scope so far.
    }
  }

  // module Alias = Path: bind (or override) a module name. The target may
  // be a file module, a library, an earlier alias, or an inline module
  // (`module L = C.List`). A whole-module binding is also this module's
  // public surface: in a library's lib.synth, `module Keys = Keys` is
  // what exposes Basic.Keys. An inline-module alias stays scope-local -
  // libraries publish whole modules, and inline members travel with them
  // under their dotted names.
  void applyAlias(const TopDef& def) {
    try {
      ModRef r =
          resolveModulePath(def.moduleName, def.span, /*mentionTarget=*/true);
      frames_.back().modules[def.name] = r;
      if (r.prefix.empty()) {
        mod_.moduleScope[def.name] = r.moduleId;
        mod_.exportedModules[def.name] = r.moduleId;
      }
    } catch (const Abort&) {
      // Diagnostic recorded.
    }
  }

  static std::vector<std::string> splitPath(const std::string& s) {
    std::vector<std::string> segs;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++)
      if (i == s.size() || s[i] == '.') {
        segs.push_back(s.substr(start, i - start));
        start = i + 1;
      }
    return segs;
  }

  // Resolve a surface module path ("Keys", "Basic.Keys", "A.B", an
  // alias, "Core", ...) to what it names: a whole file module/library
  // (prefix empty) or an inline module inside one. The first segment
  // resolves lexically - scope frames innermost-out, then load-time
  // moduleScope, then the built-in Core - and each further segment
  // descends through exported module bindings and inline modules.
  // Returns nullopt when the first segment names nothing; hard-fails
  // (with the most specific message) when a later segment does not
  // resolve, since by then the user clearly meant this path.
  std::optional<ModRef> tryResolveModulePath(const std::string& surface,
                                             Span span,
                                             bool mentionTarget = false) {
    std::vector<std::string> segs = splitPath(surface);
    ModRef cur;
    bool found = false;
    // The first segment resolves lexically: scope frames innermost-out
    // (position-ordered binders - opens, aliases, inline modules,
    // imports), then the load-time module scope. There is no fallback
    // for "Core": the bundled library must be brought into scope like
    // any other (`import Core` / `open Core` / `open Core.X`).
    //
    // Exception: when resolving an open/alias *target* (mentionTarget),
    // the load-time binding this very mention created takes effect at
    // the mention's own position, so it wins over earlier frame binders
    // - `open Core ;; open Fx` must open the Fx *library*, not Core's
    // inline Fx module brought in one line earlier.
    if (mentionTarget) {
      auto ms = mod_.moduleScope.find(surface);
      if (ms != mod_.moduleScope.end()) return ModRef{ms->second, ""};
      auto s0 = mod_.moduleScope.find(segs[0]);
      if (s0 != mod_.moduleScope.end()) {
        cur = {s0->second, ""};
        found = true;
      }
    }
    for (auto it = frames_.rbegin(); !found && it != frames_.rend(); ++it) {
      auto m = it->modules.find(segs[0]);
      if (m != it->modules.end()) {
        markOpenUsed(m->second.fromOpen);
        cur = m->second;
        found = true;
      }
    }
    if (!found) {
      // Dotted *surface* keys bound at load time ("Basic.Keys") match
      // as a whole, as before inline modules existed.
      auto ms = mod_.moduleScope.find(surface);
      if (ms != mod_.moduleScope.end()) return ModRef{ms->second, ""};
      auto s0 = mod_.moduleScope.find(segs[0]);
      if (s0 != mod_.moduleScope.end()) {
        cur = {s0->second, ""};
        found = true;
      }
    }
    if (!found) return std::nullopt;

    for (size_t i = 1; i < segs.size(); i++) {
      const std::string& s = segs[i];
      if (cur.prefix.empty()) {
        const CheckedModule* m = prog_.find(cur.moduleId);
        if (!m) return std::nullopt;
        auto ex = m->exportedModules.find(s);
        if (ex != m->exportedModules.end()) {
          cur.moduleId = ex->second;
          continue;
        }
        if (m->inlineModules.count(s)) {
          cur.prefix = s;
          continue;
        }
        // A library's interface module carries the library's own name;
        // a missing binding there is an exposure problem, not a typo.
        if (m->libName == cur.moduleId)
          fail(span, "module '" + cur.moduleId + "." + s +
                         "' is not exposed by library '" + cur.moduleId +
                         "' (add 'module " + s + " = " + s +
                         " ;;' to its " + kLibraryInterfaceFile + ")");
        fail(span, "module '" + cur.moduleId + "' has no module named '" +
                       s + "'");
      }
      // Inside an inline module only nested inline modules remain.
      const CheckedModule* host =
          cur.moduleId.empty() ? &mod_ : prog_.find(cur.moduleId);
      if (!host) return std::nullopt;
      std::string next = cur.prefix + "." + s;
      if (!host->inlineModules.count(next))
        fail(span, "module '" + cur.prefix + "' has no module named '" + s +
                       "'");
      cur.prefix = next;
    }
    return cur;
  }

  // As tryResolveModulePath, but an unresolved first segment fails with a
  // diagnostic.
  ModRef resolveModulePath(const std::string& surface, Span span,
                           bool mentionTarget = false) {
    auto r = tryResolveModulePath(surface, span, mentionTarget);
    if (r) return *r;
    std::string msg =
        "module '" + surface + "' is not imported by this module";
    // The standard library is not ambient; say how to get it.
    if (splitPath(surface)[0] == "Core")
      msg += " (Core must be brought into scope: 'import Core', "
             "'open Core', or 'open Core.<Module>')";
    fail(span, msg);
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
    if (callee.kind == Expr::Kind::Ctor) return checkCtorApp(e, env);

    // Check arguments first. Most are concrete; the ones that are not - a
    // nested partial application of a polymorphic callee, or a value of
    // this definition's own 'a - carry variables that unify below.
    std::vector<TypePtr> argTypes;
    for (size_t i = 1; i < e.items.size(); i++)
      argTypes.push_back(check(*e.items[i], env));
    auto labelOf = [&](size_t argIdx) {
      return argIdx < e.argLabels.size() ? e.argLabels[argIdx]
                                         : std::string{};
    };

    // Resolve the callee to parameter types + labels + return type. Any
    // Fun-typed expression can be applied: a name (including Core
    // externals, which resolve like every other stored signature), a
    // nested partial application, a function-typed parameter, a
    // lambda, ...
    std::vector<TypePtr> paramTypes;
    std::vector<std::string> paramLabels;
    TypePtr retType;
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
        std::string msg =
            label + " of " + calleeDesc(callee) + " expects " +
            typeName(applySubst(paramTypes[i], subst)) + ", got " +
            typeName(argTypes[j]);
        // The most common way to hold an Int where a Scalar is wanted is
        // a whole-number literal; say how to spell the other one.
        TypePtr want = applySubst(paramTypes[i], subst);
        if (want->kind == Type::Kind::Scalar &&
            argTypes[j]->kind == Type::Kind::Int)
          msg += " (a literal like 440 is an Int; write 440.0 for a "
                 "Scalar, or convert with to_scalar)";
        fail(e.items[j + 1]->span, msg);
      }
    }

    // Fully applied?
    std::vector<size_t> unfilled;
    for (size_t i = 0; i < paramTypes.size(); i++)
      if (argForParam[i] < 0) unfilled.push_back(i);
    if (unfilled.empty()) {
      TypePtr ret = applySubst(retType, subst);
      // A rigid variable left in the result is fine - it is this
      // definition's own 'a, still standing for whatever the caller picks.
      // A *free* one is a genuine gap: nothing in the call determined it.
      if (containsFreeVar(ret))
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

  // A type variable in the result has to be pinned down by an argument -
  // that is what lets the caller choose it. One that appears only in the
  // result could never be produced (the body would have to conjure a value
  // of a type it is not given), so the signature is rejected up front
  // rather than as a confusing mismatch against whatever the body returns.
  void checkResultVarsAreBound(const TopDef& def) {
    if (!containsRigidVar(def.retType)) return;
    std::set<int> bound;
    for (auto& p : def.params) collectVarIds(p.type, bound);
    // A binding with no declared parameters can still be a function by
    // annotation (`let damp : 'a Signal -> 'a Signal = lowpass ~cutoff:...`);
    // those arrow parameters bind variables just as declared ones do, so
    // peel them off before looking at what is left over.
    TypePtr result = def.retType;
    while (result->kind == Type::Kind::Fun) {
      for (auto& p : result->items) collectVarIds(p, bound);
      result = result->ret;
    }
    if (!containsRigidVar(result)) return;
    std::set<int> used;
    collectVarIds(result, used);
    for (int id : used) {
      if (id >= 0 || bound.count(id)) continue;
      // Recover the name for the message from the annotation itself.
      fail(def.span, "type variable " + varNameById(def.retType, id) +
                         " appears in the result type of '" + def.name +
                         "' but in no parameter, so nothing can determine "
                         "it at a call site");
    }
  }

  static void collectVarIds(const TypePtr& t, std::set<int>& out) {
    switch (t->kind) {
      case Type::Kind::Var: out.insert(t->var); break;
      case Type::Kind::Tuple:
      case Type::Kind::Fun:
      case Type::Kind::Named:
        for (auto& x : t->items) collectVarIds(x, out);
        if (t->ret) collectVarIds(t->ret, out);
        break;
      default:
        break;
    }
  }

  static std::string varNameById(const TypePtr& t, int id) {
    switch (t->kind) {
      case Type::Kind::Var:
        return t->var == id ? typeName(t) : std::string{};
      case Type::Kind::Tuple:
      case Type::Kind::Fun:
      case Type::Kind::Named: {
        for (auto& x : t->items)
          if (std::string s = varNameById(x, id); !s.empty()) return s;
        return t->ret ? varNameById(t->ret, id) : std::string{};
      }
      default:
        return {};
    }
  }

  // Maps every var id in `t` to a fresh free id, accumulating into
  // `renaming` so one signature's variables stay linked across its
  // parameters and result.
  void collectFreshening(const TypePtr& t, Subst& renaming) {
    switch (t->kind) {
      case Type::Kind::Var:
        if (!renaming.count(t->var))
          renaming[t->var] = tVar(nextFreshVar_++, t->varName);
        break;
      case Type::Kind::Tuple:
      case Type::Kind::Fun:
      case Type::Kind::Named: {
        for (auto& x : t->items) collectFreshening(x, renaming);
        if (t->ret) collectFreshening(t->ret, renaming);
        break;
      }
      default:
        break;
    }
  }

  // Instantiate a *stored* signature at a use site. Both a Core
  // external's 'a and a user definition's own rigid 'a become fresh free
  // variables, so
  // each reference is solved on its own: `List.map dampen sigs` and
  // `dampen (sine 440.0)` in the same file pick different element types.
  // Parameters and locals are deliberately NOT run through this - inside
  // the body that declares it, 'a is one fixed type, not a fresh unknown.
  TypePtr instantiate(const TypePtr& t) {
    if (!containsAnyVar(t)) return t;
    Subst renaming;
    collectFreshening(t, renaming);
    return applySubst(t, renaming);
  }


  // Arithmetic: pointwise lifting with Scalar broadcasting (design doc
  // §4.4). Comparisons and Bool combinators are build-time only: signals
  // are lazy per-sample streams, and a sample-wise select would be a
  // different (signal-producing) operation, deliberately absent in v1.
  TypePtr checkBinOp(Expr& e, std::map<std::string, TypePtr>& env) {
    TypePtr l = check(*e.items[0], env);
    TypePtr r = check(*e.items[1], env);
    auto is = [](const TypePtr& t, Type::Kind k) { return t->kind == k; };
    using K = Type::Kind;
    switch (e.op) {
      case BinOpKind::Lt:
      case BinOpKind::Le:
      case BinOpKind::Gt:
      case BinOpKind::Ge:
      case BinOpKind::Eq:
      case BinOpKind::Ne:
        if ((is(l, K::Scalar) && is(r, K::Scalar)) ||
            (is(l, K::Int) && is(r, K::Int)) ||
            (is(l, K::Timestamp) && is(r, K::Timestamp)))
          return tBool();
        fail(e.span, "comparison is not defined for " + typeName(l) +
                         " and " + typeName(r) +
                         " (compare two Ints, two Scalars, or two "
                         "Timestamps)");
      case BinOpKind::And:
      case BinOpKind::Or:
        if (is(l, K::Bool) && is(r, K::Bool)) return tBool();
        fail(e.span, "'&&' and '||' need Bool operands, got " +
                         typeName(l) + " and " + typeName(r));
      default:
        break;
    }
    if (is(l, K::Scalar) && is(r, K::Scalar)) return tScalar();
    // Ints stay Ints: whole-number arithmetic for counts and indices
    // (`/` divides towards zero). They never mix with the continuous
    // kinds implicitly - conversion is explicit (to_scalar, round, ...).
    if (is(l, K::Int) && is(r, K::Int)) return tInt();
    if (is(l, K::Int) || is(r, K::Int))
      fail(e.span, "operator is not defined for " + typeName(l) + " and " +
                       typeName(r) +
                       " (an Int does not mix with other numeric types "
                       "implicitly; convert with to_scalar)");
    // Timestamps: durations add and subtract, and scale by a Scalar
    // (either order for `*`, left operand only for `/`). The table is
    // deliberately partial - it adds a dimensional rule rather than
    // relaxing one. Timestamp*Timestamp is not a duration, and
    // Timestamp/Timestamp would hand back the bare Scalar the unit
    // discipline exists to prevent: there is no way back from a
    // Timestamp on purpose (§6, `to_sec`/`to_ms`/`to_min`).
    if (is(l, K::Timestamp) || is(r, K::Timestamp)) {
      bool both = is(l, K::Timestamp) && is(r, K::Timestamp);
      bool add = e.op == BinOpKind::Add || e.op == BinOpKind::Sub;
      if (both && add) return tTimestamp();
      if (!both && e.op == BinOpKind::Mul &&
          (is(l, K::Scalar) || is(r, K::Scalar)))
        return tTimestamp();
      if (!both && e.op == BinOpKind::Div && is(l, K::Timestamp) &&
          is(r, K::Scalar))
        return tTimestamp();
      if (both && e.op == BinOpKind::Div)
        fail(e.span,
             "two Timestamps do not divide: the ratio would be a bare "
             "Scalar, and a Timestamp deliberately has no way back to one "
             "(there is no inverse of to_sec/to_ms/to_min). Carry the "
             "Scalar you divided by instead, or keep the result a "
             "Timestamp with 'beat / 2.0'");
      if (both)
        fail(e.span, "two Timestamps do not multiply (the result would not "
                     "be a duration); scale a Timestamp by a Scalar "
                     "instead, as in 'beat * 1.5'");
      if (add)
        fail(e.span, "operator is not defined for " + typeName(l) + " and " +
                         typeName(r) +
                         " (a Timestamp only adds to another Timestamp; "
                         "convert with to_sec/to_ms/to_min first)");
      fail(e.span, "operator is not defined for " + typeName(l) + " and " +
                       typeName(r) +
                       " (scale a Timestamp by a Scalar: 'beat * 1.5' or "
                       "'1.5 * beat', and 'beat / 2.0')");
    }
    if (is(l, K::Vector) && is(r, K::Vector)) return tVector();
    if ((is(l, K::Vector) && is(r, K::Scalar)) ||
        (is(l, K::Scalar) && is(r, K::Vector)))
      return tVector();
    if (isSignalType(l) && isSignalType(r)) {
      if (typeEquals(l->items[0], r->items[0])) return l;
      // The mono broadcast row: a Scalar Signal lifts across the other
      // side's channels, mirroring the Scalar broadcast row and am's
      // mono-modulator rule - `bus * envelope` works on any bus. The
      // Scalar side never decides the element type, so a rigid 'a Signal
      // stays 'a Signal.
      if (r->items[0]->kind == K::Scalar) return l;
      if (l->items[0]->kind == K::Scalar) return r;
      fail(e.span, "cannot combine " + typeName(l) + " with " + typeName(r) +
                       " (element types differ)");
    }
    if (isSignalType(l) && is(r, K::Scalar)) return l;
    if (is(l, K::Scalar) && isSignalType(r)) return r;
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

std::string canonicalSourceKey(const std::string& path) {
  std::error_code ec;
  fs::path abs = fs::absolute(path, ec);
  if (ec) abs = path;
  fs::path canon = fs::weakly_canonical(abs, ec);
  if (ec) canon = abs;
  return canon.lexically_normal().string();
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
    // Names of inline modules defined anywhere in this file: mentions
    // whose first segment matches resolve inside the checker, never on
    // disk.
    std::set<std::string> inlineNames;
  };
  std::map<std::string, Loaded> byName;  // canonical module id -> module
  std::vector<std::string> queue;

  // The bundled Core library: stdlib declarations whose `external`
  // bodies bind to implementations compiled into synthc. Always
  // discoverable (shadowing any user library named Core), always an
  // allowed dependency, and loaded eagerly below so `Core.sine` and
  // `open Core` work with no import and no manifest entry.
  LibraryInfo coreLib;
  coreLib.name = "Core";
  coreLib.dir = (fs::path(bundledStdlibDir()) / "core").string();
  coreLib.hasInterface = true;

  auto libByName = [&](const std::string& n) -> const LibraryInfo* {
    if (n == "Core") return &coreLib;
    if (currentLib && currentLib->name == n) return currentLib;
    return registry ? registry->find(n) : nullptr;
  };

  // May module `fromLib` (or the unit under build, when empty) use library
  // `libName`? The unit's declared deps come from the context; a library's
  // files consult that library's own manifest deps.
  auto depAllowed = [&](const std::string& libName,
                        const std::string& fromLib) -> bool {
    if (libName == "Core") return true;  // the bundled library is free
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

  // Read a source file, honoring the context's in-memory overlay (the
  // LSP server's unsaved editor buffers) before touching disk.
  auto readSource = [&](const fs::path& p, std::string& out) -> bool {
    if (ctx && ctx->overlay) {
      auto it = ctx->overlay->find(canonicalSourceKey(p.string()));
      if (it != ctx->overlay->end()) {
        out = it->second;
        return true;
      }
    }
    return readFile(p, out);
  };

  // Load one module file under its canonical id. Returns false only when
  // the file cannot be read (diagnosed).
  auto loadFile = [&](const fs::path& path, const std::string& canonical,
                      const std::string& libName, bool external, Span errSpan,
                      const std::string& errFile) -> bool {
    if (byName.count(canonical)) return true;
    std::string source;
    if (!readSource(path, source)) {
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
    // Core is not ambient, but it is always a permitted dependency and
    // its unknown-name hints need it checked; this edge puts the
    // bundled library first in the topological order.
    if (libName != "Core") l.loadDeps.push_back("Core");
    std::vector<Token> toks = lex(l.parsed.source, l.parsed.path, diags);
    l.parsed.defs = parse(toks, l.parsed.path, diags);
    // Mentions live at any depth: an `open` inside a struct body still
    // has to load its target file before checking starts.
    std::function<void(const std::vector<TopDef>&)> collect =
        [&](const std::vector<TopDef>& ds) {
          for (auto& d : ds) {
            if (d.kind == TopDef::Kind::ModuleDef) {
              l.inlineNames.insert(d.name);
              collect(d.defs);
            } else if (d.kind != TopDef::Kind::Let &&
                       d.kind != TopDef::Kind::TypeDecl) {
              l.mentions.push_back({d.kind, d.moduleName, d.span});
            }
          }
        };
    collect(l.parsed.defs);
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
    fs::path fp(f);
    if (canonicalSourceKey(fp.parent_path().string()) ==
        canonicalSourceKey(coreLib.dir)) {
      // The bundled stdlib source itself (opened in an editor or linted
      // directly while developing synthc): check it as the Core library
      // so its externals resolve beside lib.synth and the module is
      // named Core, not by its path.
      libName = coreLib.name;
      canonical = canonicalInLibrary(libName, fp);
    } else if (currentLib) {
      libName = currentLib->name;
      canonical = canonicalInLibrary(libName, fp);
    } else {
      canonical = moduleNameForPath(f);
    }
    loadFile(fp, canonical, libName, false, {}, {});
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

  // Core loads unconditionally: its declarations back every hint and
  // every `Core.`-qualified reference, import or no import.
  loadLibraryInterface(&coreLib, /*external=*/true, {}, {}, /*quiet=*/false);

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
      // A mention whose first segment names an inline module of this same
      // file (`open A` after `module A = struct ... end`) resolves inside
      // the checker, never on disk.
      if (byName.at(name).inlineNames.count(
              surface.substr(0, surface.find('.'))))
        continue;
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
      // Dotted: Lib.File - a module the library's lib.synth binds. Deeper
      // segments ("Lib.File.A") name inline modules inside that file;
      // loading needs only the two-segment prefix, the checker resolves
      // the rest.
      std::string libName = surface.substr(0, dot);
      std::string modName = surface.substr(dot + 1);
      std::string bindSurface = surface;
      size_t dot2 = modName.find('.');
      if (dot2 != std::string::npos) {
        modName = modName.substr(0, dot2);
        bindSurface = libName + "." + modName;
      }
      const LibraryInfo* lib = libByName(libName);
      if (!lib) {
        // Not a library: the first segment can still be a file module -
        // already loaded, a same-directory sibling, or a library member
        // next to this file - whose inline module the rest of the path
        // reaches. Load and bind the file; the checker walks into it.
        if (byName.count(libName)) {
          bind(libName, libName);
          continue;
        }
        if (fromLib.empty()) {
          fs::path target =
              fs::path(file).parent_path() / (lowercase(libName) + ".synth");
          std::error_code ec;
          if (fs::exists(target, ec)) {
            if (loadFile(target, libName, "", fromExternal, span, file))
              bind(libName, libName);
            continue;
          }
        } else {
          const LibraryInfo* own = libByName(fromLib);
          std::string rel = own ? own->fileForModule(libName) : std::string{};
          if (!rel.empty()) {
            std::string canonical = fromLib + "." + libName;
            if (loadFile(fs::path(own->dir) / rel, canonical, fromLib,
                         fromExternal, span, file))
              bind(libName, canonical);
            continue;
          }
        }
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
      // A dotted path may reach an *inline* module of the interface
      // (`open Core.Osc`, `open Core.List`): binding the library is
      // enough - the checker walks the rest of the path.
      {
        auto iface = byName.find(libName);
        bool inlineTarget = false;
        if (iface != byName.end())
          for (auto& d : iface->second.parsed.defs)
            if (d.kind == TopDef::Kind::ModuleDef && d.name == modName)
              inlineTarget = true;
        if (inlineTarget) {
          bind(libName, libName);
          continue;
        }
      }
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
      bind(bindSurface, canonical);
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

  // Rigid type-variable ids are unique across the whole program (see
  // ModuleChecker's constructor comment).
  int typeVarSeq = 0;
  // Unused-open warnings apply to the files the caller named, never to
  // what they import.
  std::set<std::string> warnKeys;
  if (ctx && ctx->warnUnusedOpens)
    for (auto& f : rootFiles) warnKeys.insert(canonicalSourceKey(f));
  for (auto& name : order) {
    Loaded& l = byName.at(name);
    CheckedModule cm;
    cm.parsed = std::move(l.parsed);
    cm.imports = l.loadDeps;
    cm.moduleScope = std::move(l.moduleScope);
    cm.libName = l.libName;
    cm.external = l.external;
    prog.modules.push_back(std::move(cm));
    bool warn =
        !warnKeys.empty() &&
        warnKeys.count(canonicalSourceKey(prog.modules.back().parsed.path));
    ModuleChecker(prog.modules.back(), prog, diags, typeVarSeq, warn).run();
  }
  return prog;
}

}  // namespace synth
