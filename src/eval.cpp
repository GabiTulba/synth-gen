#include "eval.hpp"

#include <cstring>
#include <pthread.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

#include "external.hpp"
#include "library.hpp"
#include "wav.hpp"

namespace fs = std::filesystem;

namespace synth {

namespace {

class Interp {
 public:
  Interp(const Program& prog, std::vector<RenderTarget>& targets,
         DiagnosticBag& diags, std::vector<std::string>* loadedFiles,
         std::string extCacheDir,
         const std::map<std::string, double>* controlOverrides,
         std::vector<ControlDecl>* controls, std::vector<PanelDecl>* panels)
      : prog_(prog), targets_(targets), diags_(diags),
        loadedFiles_(loadedFiles), extCacheDir_(std::move(extCacheDir)),
        overrides_(controlOverrides), controls_(controls), panels_(panels) {}

  bool run() {
    bool ok = true;
    for (auto& mod : prog_.modules) {
      currentModule_ = &mod;
      if (!evalDefs(mod, mod.parsed.defs, "")) ok = false;
    }
    return ok;
  }

 private:
  using Env = std::map<std::string, Value>;

  const Program& prog_;
  std::vector<RenderTarget>& targets_;
  DiagnosticBag& diags_;
  const CheckedModule* currentModule_ = nullptr;
  const TopDef* currentDef_ = nullptr;
  std::vector<std::string>* loadedFiles_ = nullptr;
  std::string extCacheDir_;                        // user external .so cache
  const std::map<std::string, double>* overrides_ = nullptr;
  std::vector<ControlDecl>* controls_ = nullptr;
  std::map<std::string, ControlDecl> controlsByName_;
  std::map<std::string, ControlGroupDecl> groupsByName_;
  std::vector<PanelDecl>* panels_ = nullptr;
  std::map<std::string, PanelDecl> panelsByName_;
  std::map<std::string, Env> globals_;             // module -> name -> value
  std::map<std::string, SigPtr> fileCache_;        // absolute path -> signal
  mutable CoreListInfo coreList_;                  // resolved on first use
  mutable CoreSampleInfo coreSample_;              // resolved on first use

  const CoreListInfo& coreList() const {
    if (!coreList_) {
      const TypeDecl* d = prog_.coreTypeDecl("list");
      if (!d)
        throw EvalError("internal error: Core's list type is not loaded");
      coreList_.decl = d;
      for (size_t i = 0; i < d->ctors.size(); i++) {
        if (d->ctors[i].name == "Nil") coreList_.nilIndex = (int)i;
        if (d->ctors[i].name == "Cons") coreList_.consIndex = (int)i;
      }
    }
    return coreList_;
  }

  const CoreSampleInfo& coreSample() const {
    if (!coreSample_) {
      const TypeDecl* d = prog_.coreTypeDecl("Sample");
      if (!d)
        throw EvalError("internal error: Core's Sample type is not loaded");
      coreSample_.decl = d;
      for (size_t i = 0; i < d->fields.size(); i++) {
        if (d->fields[i].name == "sig") coreSample_.sigField = (int)i;
        if (d->fields[i].name == "from") coreSample_.fromField = (int)i;
        if (d->fields[i].name == "to") coreSample_.toField = (int)i;
      }
    }
    return coreSample_;
  }
  // Resolved user externals, one compile+load per definition per build.
  std::map<const TopDef*, ExternalFn> userExternals_;

  // Definitions inside `module A = struct ... end` live in the enclosing
  // file module's globals under their dotted names ("A.x"); the checker
  // rewrote every reference to that form, so lookup needs no new cases.
  bool evalDefs(const CheckedModule& mod, const std::vector<TopDef>& defs,
                const std::string& prefix) {
    bool ok = true;
    for (auto& def : defs) {
      if (def.kind == TopDef::Kind::ModuleDef) {
        if (!evalDefs(mod, def.defs, prefix + def.name + ".")) ok = false;
        continue;
      }
      if (def.kind != TopDef::Kind::Let) continue;
      // Modules pulled in from dependency libraries evaluate for their
      // values only: their `let _` render effects belong to the
      // library's own build, not to every consumer's.
      if (mod.external && def.name == "_") continue;
      currentDef_ = &def;
      try {
        if (!def.params.empty()) {
          // Functions are values; the body runs at call time. An
          // external-bodied function is a FunV like any other - full
          // application dispatches to its implementation.
          globals_[mod.parsed.name][prefix + def.name] =
              Value{FunV{&def, &mod, nullptr}};
        } else if (def.body->kind == Expr::Kind::External) {
          // A zero-parameter external (Core's `time`) IS its value.
          globals_[mod.parsed.name][prefix + def.name] =
              callExternal(def, mod, mod, {});
        } else {
          Env empty;
          Value v = eval(*def.body, empty, mod);
          if (def.name != "_")
            globals_[mod.parsed.name][prefix + def.name] = v;
        }
      } catch (const EvalError& e) {
        diags_.error(mod.parsed.path, def.span, e.what());
        ok = false;
      } catch (const EngineError& e) {
        diags_.error(mod.parsed.path, def.span, e.what());
        ok = false;
      }
    }
    return ok;
  }

  Value eval(const Expr& e, Env& env, const CheckedModule& mod) {
    switch (e.kind) {
      case Expr::Kind::NumLit: return Value{ScalarV{e.num}};
      case Expr::Kind::IntLit: return Value{IntV{e.inum}};
      case Expr::Kind::TimeLit: return Value{TimeV{e.num}};
      case Expr::Kind::BoolLit: return Value{BoolV{e.num != 0.0}};
      case Expr::Kind::StrLit: return Value{StringV{e.str}};
      case Expr::Kind::Ident: return lookup(e, env, mod);
      case Expr::Kind::If: {
        // Only the taken branch evaluates: the other branch's errors and
        // render effects never fire.
        Value cv = eval(*e.items[0], env, mod);
        if (const BoolV* c = std::get_if<BoolV>(&cv.v))
          return eval(*e.items[c->v ? 1 : 2], env, mod);
        // A condition signal (under `signal ~f`) has no single answer, so
        // the choice moves into the graph. This is the one case where
        // both branches evaluate - there is no per-sample control flow to
        // skip one with.
        if (auto* g = std::get_if<SigPtr>(&cv.v)) {
          SigPtr a = branchSignal(eval(*e.items[1], env, mod));
          SigPtr b = branchSignal(eval(*e.items[2], env, mod));
          return Value{makeSelect(*g, 0.5, a, b)};
        }
        throw EvalError("'if' condition is not a Bool at build time");
      }
      case Expr::Kind::App: {
        // A constructor application builds its variant directly - the
        // constructor is not a function value.
        if (e.items[0]->kind == Expr::Kind::Ctor) {
          const Expr& c = *e.items[0];
          const TypeDecl* decl = c.type ? c.type->decl : nullptr;
          if (!decl)
            throw EvalError("internal error: unresolved constructor");
          return Value{VariantV{
              decl, (int)c.inum,
              std::make_shared<Value>(eval(*e.items[1], env, mod))}};
        }
        std::vector<std::pair<std::string, Value>> args;  // label, value
        for (size_t i = 1; i < e.items.size(); i++) {
          std::string label =
              i - 1 < e.argLabels.size() ? e.argLabels[i - 1] : "";
          args.emplace_back(std::move(label), eval(*e.items[i], env, mod));
        }
        // The callee may be any Fun-typed expression (a name, a nested
        // partial application, a lambda, ...); Ident dispatches to lookup.
        Value fn = eval(*e.items[0], env, mod);
        return applyValue(fn, std::move(args), mod);
      }
      case Expr::Kind::BinOp: {
        // `&&` and `||` short-circuit: only the deciding operand runs.
        if (e.op == BinOpKind::And || e.op == BinOpKind::Or) {
          bool isAnd = e.op == BinOpKind::And;
          Value lv = eval(*e.items[0], env, mod);
          if (const BoolV* lb = std::get_if<BoolV>(&lv.v)) {
            if (isAnd && !lb->v) return Value{BoolV{false}};
            if (!isAnd && lb->v) return Value{BoolV{true}};
            // The left operand decided nothing, so the result is the
            // right one - as a Bool, or as a condition signal when the
            // right side is a sample-wise comparison.
            Value rv = eval(*e.items[1], env, mod);
            if (const BoolV* rb = std::get_if<BoolV>(&rv.v))
              return Value{BoolV{rb->v}};
            return Value{condSignal(rv)};
          }
          // A condition signal on the left: nothing can be decided up
          // front, so both sides run and combine sample-wise. On 0/1
          // signals `and` is a product and `or` is a + b - a*b.
          SigPtr a = condSignal(lv);
          SigPtr b = condSignal(eval(*e.items[1], env, mod));
          if (isAnd) return Value{makeBinOp(SigBinOp::Mul, a, b)};
          return Value{makeBinOp(SigBinOp::Sub,
                                 makeBinOp(SigBinOp::Add, a, b),
                                 makeBinOp(SigBinOp::Mul, a, b))};
        }
        Value l = eval(*e.items[0], env, mod);
        Value r = eval(*e.items[1], env, mod);
        return binop(e.op, std::move(l), std::move(r));
      }
      case Expr::Kind::Neg: {
        Value v = eval(*e.items[0], env, mod);
        if (auto* iv = std::get_if<IntV>(&v.v)) return Value{IntV{-iv->v}};
        if (auto* s = std::get_if<ScalarV>(&v.v))
          return Value{ScalarV{-s->v}};
        if (auto* vec = std::get_if<VectorV>(&v.v)) {
          VectorV out = *vec;
          for (double& x : out.v) x = -x;
          return Value{std::move(out)};
        }
        if (auto* sig = std::get_if<SigPtr>(&v.v))
          return Value{makeBinOp(SigBinOp::Sub, makeConst(0.0), *sig)};
        throw EvalError("unary '-' applied to a non-numeric value");
      }
      case Expr::Kind::ListLit: {
        // Sugar for a Cons chain. Elements evaluate left to right; the
        // chain builds back to front.
        const CoreListInfo& list = coreList();
        std::vector<Value> items;
        for (auto& x : e.items) items.push_back(eval(*x, env, mod));
        Value out{VariantV{list.decl, list.nilIndex, nullptr}};
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
          TupleV cell;
          cell.items.push_back(std::move(*it));
          cell.items.push_back(std::move(out));
          out = Value{VariantV{list.decl, list.consIndex,
                               std::make_shared<Value>(
                                   Value{std::move(cell)})}};
        }
        return out;
      }
      case Expr::Kind::TupleLit: {
        TupleV out;
        for (auto& x : e.items) out.items.push_back(eval(*x, env, mod));
        return Value{std::move(out)};
      }
      case Expr::Kind::Let: {
        Value bound = eval(*e.items[0], env, mod);
        auto prev = env.find(e.name);
        std::optional<Value> saved;
        if (prev != env.end()) saved = prev->second;
        env[e.name] = std::move(bound);
        Value out = eval(*e.items[1], env, mod);
        if (saved) env[e.name] = std::move(*saved);
        else env.erase(e.name);
        return out;
      }
      case Expr::Kind::Lambda:
        // Capture the local environment by value; the AST is owned by the
        // Program for the whole build, so the Expr pointer stays valid.
        return Value{LambdaV{&e, &mod, std::make_shared<Env>(env), nullptr}};
      case Expr::Kind::RecordLit: {
        // The checker stamped the resolved record type on the node;
        // fields evaluate in source order but store in declaration order,
        // so projection is a plain index.
        const TypeDecl* decl = e.type ? e.type->decl : nullptr;
        if (!decl)
          throw EvalError("internal error: unresolved record literal");
        RecordV out;
        out.decl = decl;
        out.fields.resize(decl->fields.size());
        for (size_t i = 0; i < e.items.size(); i++) {
          Value v = eval(*e.items[i], env, mod);
          for (size_t k = 0; k < decl->fields.size(); k++)
            if (decl->fields[k].name == e.argLabels[i]) {
              out.fields[k] = std::move(v);
              break;
            }
        }
        return Value{std::move(out)};
      }
      case Expr::Kind::RecordUpdate: {
        Value base = eval(*e.items[0], env, mod);
        auto* rec = std::get_if<RecordV>(&base.v);
        if (!rec)
          throw EvalError("record update applied to a non-record value");
        RecordV out = *rec;  // copy, then overwrite the named fields
        for (size_t i = 0; i + 1 < e.items.size(); i++) {
          Value v = eval(*e.items[i + 1], env, mod);
          for (size_t k = 0; k < out.decl->fields.size(); k++)
            if (out.decl->fields[k].name == e.argLabels[i]) {
              out.fields[k] = std::move(v);
              break;
            }
        }
        return Value{std::move(out)};
      }
      case Expr::Kind::Project: {
        Value base = eval(*e.items[0], env, mod);
        auto* rec = std::get_if<RecordV>(&base.v);
        if (!rec)
          throw EvalError("field access applied to a non-record value");
        for (size_t k = 0; k < rec->decl->fields.size(); k++)
          if (rec->decl->fields[k].name == e.name) return rec->fields[k];
        throw EvalError("record has no field '" + e.name + "'");
      }
      case Expr::Kind::Ctor: {
        const TypeDecl* decl = e.type ? e.type->decl : nullptr;
        if (!decl)
          throw EvalError("internal error: unresolved constructor");
        return Value{VariantV{decl, (int)e.inum, nullptr}};
      }
      case Expr::Kind::Match: {
        // The scrutinee evaluates once; only the taken arm's body runs
        // (same rule as `if`: untaken arms' render effects never fire).
        Value scr = eval(*e.items[0], env, mod);
        for (size_t a = 0; a < e.patterns.size(); a++) {
          Env binds;
          if (!matchPattern(e.patterns[a], scr, binds)) continue;
          std::vector<std::pair<std::string, std::optional<Value>>> saved;
          for (auto& [n, v] : binds) {
            auto prev = env.find(n);
            saved.emplace_back(n, prev != env.end()
                                      ? std::optional<Value>(prev->second)
                                      : std::nullopt);
            env[n] = v;
          }
          Value out = eval(*e.items[a + 1], env, mod);
          for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            if (it->second) env[it->first] = std::move(*it->second);
            else env.erase(it->first);
          }
          return out;
        }
        // The checker proved exhaustiveness; this is defense in depth.
        throw EvalError("no pattern matched the value at build time");
      }
      case Expr::Kind::External:
        // evalDefs and applyValue dispatch external bodies before eval.
        throw EvalError("internal error: external body evaluated directly");
    }
    throw EvalError("internal error: unknown expression kind");
  }

  // Structural pattern match; collects the bound names on success. The
  // checker resolved constructor indexes and field names already.
  bool matchPattern(const Pattern& p, const Value& v, Env& binds) {
    switch (p.kind) {
      case Pattern::Kind::Wildcard:
        return true;
      case Pattern::Kind::Bind:
        binds[p.name] = v;
        return true;
      case Pattern::Kind::Tuple: {
        const TupleV* t = std::get_if<TupleV>(&v.v);
        if (!t || t->items.size() != p.items.size()) return false;
        for (size_t i = 0; i < p.items.size(); i++)
          if (!matchPattern(p.items[i], t->items[i], binds)) return false;
        return true;
      }
      case Pattern::Kind::Record: {
        const RecordV* r = std::get_if<RecordV>(&v.v);
        if (!r) return false;
        for (size_t i = 0; i < p.fieldNames.size(); i++) {
          const Value* field = nullptr;
          for (size_t k = 0; k < r->decl->fields.size(); k++)
            if (r->decl->fields[k].name == p.fieldNames[i])
              field = &r->fields[k];
          if (!field || !matchPattern(p.items[i], *field, binds))
            return false;
        }
        return true;
      }
      case Pattern::Kind::Ctor: {
        const VariantV* var = std::get_if<VariantV>(&v.v);
        if (!var || var->ctor != p.ctorIndex) return false;
        if (p.items.empty()) return true;
        if (!var->payload) return false;
        return matchPattern(p.items[0], *var->payload, binds);
      }
    }
    return false;
  }

  Value lookup(const Expr& ident, Env& env, const CheckedModule& mod) {
    if (ident.moduleName.empty()) {
      auto it = env.find(ident.name);
      if (it != env.end()) return it->second;
      auto& g = globals_[mod.parsed.name];
      auto gt = g.find(ident.name);
      if (gt != g.end()) return gt->second;
      throw EvalError("unbound name '" + ident.name + "' at build time");
    }
    auto mt = globals_.find(ident.moduleName);
    if (mt == globals_.end())
      throw EvalError("module '" + ident.moduleName + "' was not evaluated");
    auto it = mt->second.find(ident.name);
    if (it == mt->second.end())
      throw EvalError("module '" + ident.moduleName +
                      "' has no value named '" + ident.name + "'");
    return it->second;
  }

  // Positional-only application (map/fold and internal call sites).
  Value apply(const Value& fn, std::vector<Value> args,
              const CheckedModule& mod) {
    std::vector<std::pair<std::string, Value>> labeled;
    for (auto& a : args) labeled.emplace_back("", std::move(a));
    return applyValue(fn, std::move(labeled), mod);
  }

  // A tail call in flight: evalTail found the frame's result is itself
  // an application, and hands the callee and evaluated arguments back to
  // applyValue's loop instead of recursing.
  struct TailNext {
    Value fn;
    std::vector<std::pair<std::string, Value>> args;
  };

  // Label-aware application over user functions, lambdas, and
  // external-bodied definitions. Positional values fill the leftmost
  // unbound parameters; labeled ones bind by name. Incomplete
  // applications yield a closure carrying the bindings; complete ones
  // evaluate. `mod` is the module the application happens in: bodies
  // evaluate in their own defining module, but an external implementation
  // resolves relative paths (and attributes render targets) against the
  // *calling* module - `load_mono "kick.wav"` means next to the file
  // that wrote the string, not next to Core's lib.synth.
  //
  // Tail calls are eliminated: a body whose result IS another call (in
  // tail position through let bodies, if branches and match arms) loops
  // here in the same C++ frame instead of recursing, so `List.fold` and
  // every accumulator-shaped combinator run at constant depth however
  // long the list. The depth guard is entered once per *chain*, so only
  // genuinely nested (non-tail) recursion counts against the limit.
  Value applyValue(Value fn,
                   std::vector<std::pair<std::string, Value>> args,
                   const CheckedModule& mod) {
    std::optional<ApplyDepthGuard> guard;
    // The tail loop's own runaway brake: depth no longer grows with a
    // tail chain, so a chain that never terminates needs its own
    // diagnostic. The limit is far above any musical fold (one
    // iteration per list element).
    static constexpr int64_t kMaxTailCalls = 10'000'000;
    int64_t tailCalls = 0;
    for (;;) {
      if (++tailCalls > kMaxTailCalls) {
        const FunV* ff = std::get_if<FunV>(&fn.v);
        const LambdaV* ll = std::get_if<LambdaV>(&fn.v);
        std::string name = ff ? ff->def->name
                          : ll && !ll->lam->name.empty() ? ll->lam->name
                                                         : "<fun>";
        throw EvalError("recursion limit (" + std::to_string(kMaxTailCalls) +
                        " tail calls) exceeded while evaluating '" + name +
                        "' - likely unbounded recursion (no base case "
                        "reached)");
      }
      // Assemble the parameter-name list of whatever we're applying.
      std::vector<std::string> paramNames;
      const std::map<std::string, Value>* prevBound = nullptr;
      const FunV* f = std::get_if<FunV>(&fn.v);
      const LambdaV* l = std::get_if<LambdaV>(&fn.v);
      if (f) {
        for (auto& p : f->def->params) paramNames.push_back(p.name);
        prevBound = f->bound.get();
      } else if (l) {
        for (auto& p : l->lam->params) paramNames.push_back(p.name);
        prevBound = l->bound.get();
      } else {
        throw EvalError("internal error: applying a non-function value");
      }

      auto bound = std::make_shared<std::map<std::string, Value>>();
      if (prevBound) *bound = *prevBound;
      for (auto& [label, value] : args) {
        std::string target;
        if (!label.empty()) {
          if (bound->count(label))
            throw EvalError("argument '~" + label + "' provided twice");
          target = label;
        } else {
          for (auto& name : paramNames)
            if (!bound->count(name)) {
              target = name;
              break;
            }
          if (target.empty())
            throw EvalError("internal error: too many arguments");
        }
        (*bound)[target] = std::move(value);
      }

      if (bound->size() < paramNames.size()) {
        if (f) return Value{FunV{f->def, f->mod, std::move(bound)}};
        return Value{LambdaV{l->lam, l->mod, l->captured, std::move(bound)}};
      }

      std::optional<TailNext> next;
      Value out;
      if (f) {
        // Fully applied. An external body has no expression to evaluate:
        // its arguments dispatch to the bound implementation instead.
        if (f->def->body->kind == Expr::Kind::External) {
          std::vector<Value> ordered;
          for (auto& name : paramNames) ordered.push_back((*bound)[name]);
          return callExternal(*f->def, *f->mod, mod, std::move(ordered));
        }
        if (!guard) guard.emplace(*this, f->def->name);
        Env env;
        // A recursive definition finds itself under its own (surface)
        // name - required for members of inline modules, whose stored
        // names are dotted and unreachable from an unqualified lookup.
        if (f->def->isRec)
          env[f->def->name] = Value{FunV{f->def, f->mod, nullptr}};
        for (auto& name : paramNames) env[name] = (*bound)[name];
        out = evalTail(*f->def->body, env, *f->mod, next);
      } else {
        // The captured environment plus the bound params (params shadow
        // captures), evaluated in the lambda's defining module.
        if (!guard)
          guard.emplace(*this, l->lam->name.empty() ? "<fun>"
                                                    : l->lam->name);
        Env env = *l->captured;
        // A recursive local function finds itself under its own name,
        // reconstructed per call from the lambda's own fields - no cyclic
        // ownership. Parameters bind after it, so they shadow it as usual.
        if (!l->lam->name.empty())
          env[l->lam->name] =
              Value{LambdaV{l->lam, l->mod, l->captured, nullptr}};
        for (auto& name : paramNames) env[name] = (*bound)[name];
        out = evalTail(*l->lam->items[0], env, *l->mod, next);
      }
      if (!next) return out;
      fn = std::move(next->fn);
      args = std::move(next->args);
    }
  }

  // Evaluate `e` knowing it sits in tail position of a frame applyValue
  // owns: let bodies, if branches and match arms stay on the tail spine
  // (their env mutations need no restore - the frame is abandoned either
  // way), and an application of a function value is returned to the loop
  // as a TailNext instead of evaluated recursively. Everything else
  // falls through to ordinary eval.
  Value evalTail(const Expr& e, Env& env, const CheckedModule& mod,
                 std::optional<TailNext>& next) {
    switch (e.kind) {
      case Expr::Kind::Let: {
        Value bd = eval(*e.items[0], env, mod);
        env[e.name] = std::move(bd);
        return evalTail(*e.items[1], env, mod, next);
      }
      case Expr::Kind::If: {
        Value cv = eval(*e.items[0], env, mod);
        if (const BoolV* c = std::get_if<BoolV>(&cv.v))
          return evalTail(*e.items[c->v ? 1 : 2], env, mod, next);
        // Sample-wise choice (see the `if` case in eval): both branches
        // become graph, so neither is in tail position any more.
        if (auto* g = std::get_if<SigPtr>(&cv.v)) {
          SigPtr a = branchSignal(eval(*e.items[1], env, mod));
          SigPtr b = branchSignal(eval(*e.items[2], env, mod));
          return Value{makeSelect(*g, 0.5, a, b)};
        }
        throw EvalError("'if' condition is not a Bool at build time");
      }
      case Expr::Kind::Match: {
        Value scr = eval(*e.items[0], env, mod);
        for (size_t a = 0; a < e.patterns.size(); a++) {
          Env binds;
          if (!matchPattern(e.patterns[a], scr, binds)) continue;
          for (auto& [n, v] : binds) env[n] = v;
          return evalTail(*e.items[a + 1], env, mod, next);
        }
        throw EvalError("no pattern matched the value at build time");
      }
      case Expr::Kind::App: {
        // Constructor applications build data, not calls.
        if (e.items[0]->kind == Expr::Kind::Ctor) return eval(e, env, mod);
        std::vector<std::pair<std::string, Value>> args;
        for (size_t i = 1; i < e.items.size(); i++) {
          std::string label =
              i - 1 < e.argLabels.size() ? e.argLabels[i - 1] : "";
          args.emplace_back(std::move(label), eval(*e.items[i], env, mod));
        }
        Value fn = eval(*e.items[0], env, mod);
        next = TailNext{std::move(fn), std::move(args)};
        return {};
      }
      default:
        return eval(e, env, mod);
    }
  }

  // Build-time evaluation is recursive (let rec); the depth guard turns
  // runaway recursion into a diagnostic instead of exhausting the
  // process stack. The limit is far above anything musical (a recursive
  // List function recurses once per element).
  static constexpr int kMaxApplyDepth = 4096;
  int applyDepth_ = 0;
  struct ApplyDepthGuard {
    Interp& in;
    ApplyDepthGuard(Interp& in, const std::string& name) : in(in) {
      // Throw before incrementing: the destructor only runs after a
      // completed constructor, so the counter stays balanced when the
      // error unwinds to the per-definition handler.
      if (in.applyDepth_ >= kMaxApplyDepth)
        throw EvalError(
            "recursion limit (" + std::to_string(kMaxApplyDepth) +
            " nested calls) exceeded while evaluating '" + name +
            "' - likely unbounded recursion (no base case reached)");
      ++in.applyDepth_;
    }
    ~ApplyDepthGuard() { --in.applyDepth_; }
  };

  // Dispatch an external-bodied definition: C++ compiled into a cached
  // shared object on first use - the bundled Core library's
  // implementations and user code go through the same path. `declMod` is
  // the module that declares the external (anchors the .cpp path);
  // `callerMod` is where this application happens (anchors audio paths
  // and render-target attribution).
  Value callExternal(const TopDef& def, const CheckedModule& declMod,
                     const CheckedModule& callerMod,
                     std::vector<Value> args) {
    auto it = userExternals_.find(&def);
    if (it == userExternals_.end()) {
      // The C++ file resolves relative to the declaring source file and
      // is a build input like an audio file: the daemon watches it.
      fs::path p(def.body->str);
      if (p.is_relative())
        p = fs::path(declMod.parsed.path).parent_path() / p;
      std::string resolved = p.lexically_normal().string();
      if (loadedFiles_) loadedFiles_->push_back(resolved);
      // The bundled stdlib's objects go to the shared per-user cache
      // (empty dir = temp default): they are identical across projects,
      // so each project's _build/externals holds only its own code.
      std::string cacheDir = extCacheDir_;
      std::error_code ec;
      fs::path stdroot = fs::absolute(bundledStdlibDir(), ec);
      fs::path rel = fs::relative(fs::absolute(p, ec), stdroot, ec);
      if (!rel.empty() && rel.string().rfind("..", 0) != 0) cacheDir.clear();
      it = userExternals_
               .emplace(&def,
                        loadUserExternal(resolved, def.name, cacheDir))
               .first;
    }
    ExtServices svc;
    svc.coreList = coreList();
    svc.coreSample = coreSample();
    svc.apply = [this, &callerMod](const Value& fn, std::vector<Value> a) {
      return apply(fn, std::move(a), callerMod);
    };
    svc.loadAudio = [this, &callerMod](const std::string& p) {
      return loadFile(p, callerMod);
    };
    svc.declareTarget = [this, &callerMod](RenderTarget t) {
      t.file = callerMod.parsed.path;
      t.span = currentDef_ ? currentDef_->span : Span{};
      t.declModule = currentModule_;
      t.declDef = currentDef_;
      targets_.push_back(std::move(t));
    };
    svc.declareControl = [this, &callerMod](ControlDecl c) {
      c.file = callerMod.parsed.path;
      c.span = currentDef_ ? currentDef_->span : Span{};
      return registerControl(std::move(c));
    };
    svc.declareControlGroup = [this, &callerMod](ControlGroupDecl g) {
      g.file = callerMod.parsed.path;
      g.span = currentDef_ ? currentDef_->span : Span{};
      return registerControlGroup(std::move(g));
    };
    svc.declarePanel = [this, &callerMod](PanelDecl p) {
      p.file = callerMod.parsed.path;
      p.span = currentDef_ ? currentDef_->span : Span{};
      registerPanel(std::move(p));
    };
    return it->second(svc, args);
  }

  // Register a dev-app panel. Panel names share their own name space -
  // separate from controls and targets, since a panel is not one of
  // either - and redeclaring one is an error even if the two agree:
  // unlike a control, a panel yields no value, so there is no reason to
  // declare the same one twice. Member names are recorded as written and
  // resolved later, by the build, once every control and target is known.
  void registerPanel(PanelDecl p) {
    if (p.name.empty()) throw EvalError("panel: empty panel name");
    auto it = panelsByName_.find(p.name);
    if (it != panelsByName_.end())
      throw EvalError("panel '" + p.name + "' redeclared (also declared in " +
                      it->second.file + ")");
    // Duplicates inside one panel would draw the same widget twice.
    auto dupe = [&p](const std::vector<std::string>& names, const char* what) {
      std::set<std::string> seen;
      for (auto& n : names)
        if (!seen.insert(n).second)
          throw EvalError("panel '" + p.name + "': " + what + " '" + n +
                          "' listed twice");
    };
    dupe(p.controls, "control");
    dupe(p.targets, "target");
    panelsByName_.emplace(p.name, p);
    if (panels_) panels_->push_back(std::move(p));
  }

  // Register a live control declaration and resolve its value for this
  // build. Control names share one project-wide name space: redeclaring
  // a name is fine as long as kind and range agree (the same value comes
  // back), and a conflicting redeclaration is a build error.
  double registerControl(ControlDecl c) {
    if (c.name.empty()) throw EvalError("control: empty control name");
    if (!(c.max > c.min))
      throw EvalError("control '" + c.name + "': max (" +
                      std::to_string(c.max) + ") must exceed min (" +
                      std::to_string(c.min) + ")");
    if (c.def < c.min || c.def > c.max)
      throw EvalError("control '" + c.name + "': default " +
                      std::to_string(c.def) + " is outside [" +
                      std::to_string(c.min) + ", " + std::to_string(c.max) +
                      "]");
    auto it = controlsByName_.find(c.name);
    if (it != controlsByName_.end()) {
      const ControlDecl& prev = it->second;
      if (prev.kind != c.kind || prev.min != c.min || prev.max != c.max ||
          prev.def != c.def)
        throw EvalError("control '" + c.name +
                        "' redeclared with a different kind or range (also "
                        "declared in " + prev.file + ")");
      return prev.value;
    }
    c.value = c.def;
    if (overrides_) {
      auto ov = overrides_->find(c.name);
      if (ov != overrides_->end())
        c.value = std::clamp(ov->second, c.min, c.max);
    }
    double v = c.value;
    controlsByName_.emplace(c.name, c);
    if (controls_) controls_->push_back(std::move(c));
    return v;
  }

  // Register a `multi_slider` group and resolve every lane's value for
  // this build. Lanes are ordinary controls named "<group>.<lane>", so
  // they share the project-wide name space and reach the metadata (and
  // the overrides file) exactly like a slider does. What the group adds
  // is a bound on the sum, which no per-lane clamp can enforce - so the
  // whole group resolves in one call, here.
  std::vector<double> registerControlGroup(ControlGroupDecl g) {
    if (g.name.empty()) throw EvalError("multi_slider: empty group name");
    if (g.lanes.empty())
      throw EvalError("multi_slider '" + g.name + "': no lanes");
    if (g.sumMax < g.sumMin)
      throw EvalError("multi_slider '" + g.name + "': sum_max (" +
                      std::to_string(g.sumMax) + ") is below sum_min (" +
                      std::to_string(g.sumMin) + ")");
    double sumMin = 0, sumMax = 0, sumDef = 0;
    std::set<std::string> laneNames;
    for (auto& l : g.lanes) {
      if (l.name.empty())
        throw EvalError("multi_slider '" + g.name + "': empty lane name");
      if (!laneNames.insert(l.name).second)
        throw EvalError("multi_slider '" + g.name + "': lane '" + l.name +
                        "' declared twice");
      if (!(l.max > l.min))
        throw EvalError("multi_slider '" + g.name + "', lane '" + l.name +
                        "': max (" + std::to_string(l.max) +
                        ") must exceed min (" + std::to_string(l.min) + ")");
      if (l.def < l.min || l.def > l.max)
        throw EvalError("multi_slider '" + g.name + "', lane '" + l.name +
                        "': default " + std::to_string(l.def) +
                        " is outside [" + std::to_string(l.min) + ", " +
                        std::to_string(l.max) + "]");
      sumMin += l.min;
      sumMax += l.max;
      sumDef += l.def;
    }
    // A group whose lane ranges cannot reach the sum bounds has no
    // solution at all; better to say so at the declaration than to hand
    // back silently projected values every build.
    if (sumMin > g.sumMax)
      throw EvalError("multi_slider '" + g.name +
                      "': the lane minimums already sum to " +
                      std::to_string(sumMin) + ", above sum_max " +
                      std::to_string(g.sumMax));
    if (sumMax < g.sumMin)
      throw EvalError("multi_slider '" + g.name +
                      "': the lane maximums only sum to " +
                      std::to_string(sumMax) + ", below sum_min " +
                      std::to_string(g.sumMin));
    if (sumDef < g.sumMin || sumDef > g.sumMax)
      throw EvalError("multi_slider '" + g.name + "': the defaults sum to " +
                      std::to_string(sumDef) + ", outside [" +
                      std::to_string(g.sumMin) + ", " +
                      std::to_string(g.sumMax) + "]");

    // Redeclaration follows the slider/knob rule: an identical group is
    // the same group and yields the same values.
    std::vector<double> values(g.lanes.size());
    auto prevIt = groupsByName_.find(g.name);
    if (prevIt != groupsByName_.end()) {
      const ControlGroupDecl& prev = prevIt->second;
      bool same = prev.lanes.size() == g.lanes.size() &&
                  prev.sumMin == g.sumMin && prev.sumMax == g.sumMax;
      for (size_t i = 0; same && i < g.lanes.size(); i++)
        same = prev.lanes[i].name == g.lanes[i].name &&
               prev.lanes[i].min == g.lanes[i].min &&
               prev.lanes[i].max == g.lanes[i].max &&
               prev.lanes[i].def == g.lanes[i].def;
      if (!same)
        throw EvalError("multi_slider '" + g.name +
                        "' redeclared with different lanes or sum bounds "
                        "(also declared in " + prev.file + ")");
      for (size_t i = 0; i < g.lanes.size(); i++)
        values[i] = controlsByName_[g.name + "." + g.lanes[i].name].value;
      return values;
    }

    // Start from the defaults, apply each lane's own override clamped to
    // its own range, then put the group back inside its sum bounds: the
    // overrides file is dev-tool state a hand edit can put out of range,
    // and evaluation must not see an infeasible group.
    for (size_t i = 0; i < g.lanes.size(); i++) {
      values[i] = g.lanes[i].def;
      if (overrides_) {
        auto ov = overrides_->find(g.name + "." + g.lanes[i].name);
        if (ov != overrides_->end())
          values[i] = std::clamp(ov->second, g.lanes[i].min, g.lanes[i].max);
      }
    }
    projectOntoSum(g, values);

    for (size_t i = 0; i < g.lanes.size(); i++) {
      ControlDecl c;
      c.kind = ControlDecl::Kind::MultiSlider;
      c.name = g.name + "." + g.lanes[i].name;
      c.min = g.lanes[i].min;
      c.max = g.lanes[i].max;
      c.def = g.lanes[i].def;
      c.value = values[i];
      c.group = g.name;
      c.groupIndex = (int)i;
      c.sumMin = g.sumMin;
      c.sumMax = g.sumMax;
      c.file = g.file;
      c.span = g.span;
      if (controlsByName_.count(c.name))
        throw EvalError("multi_slider '" + g.name + "': lane control '" +
                        c.name + "' collides with an existing control (also "
                        "declared in " + controlsByName_[c.name].file + ")");
      controlsByName_.emplace(c.name, c);
      if (controls_) controls_->push_back(std::move(c));
    }
    groupsByName_.emplace(g.name, std::move(g));
    return values;
  }

  // Move `values` the shortest sensible distance back into
  // [sumMin, sumMax]: shed the excess (or take up the shortfall)
  // proportionally to how much room each lane has left toward its own
  // bound, pinning lanes that run out and re-sharing what is left. Each
  // pass pins at least one lane, so this settles in at most N passes.
  static void projectOntoSum(const ControlGroupDecl& g,
                             std::vector<double>& values) {
    const double eps = 1e-12;
    for (size_t pass = 0; pass <= g.lanes.size(); pass++) {
      double sum = 0;
      for (double v : values) sum += v;
      double excess = sum > g.sumMax   ? sum - g.sumMax
                      : sum < g.sumMin ? sum - g.sumMin
                                       : 0.0;
      if (std::fabs(excess) <= eps) return;
      // Shedding (excess > 0) draws on the room down to each lane's min;
      // taking up a shortfall draws on the room up to each lane's max.
      double room = 0;
      for (size_t i = 0; i < values.size(); i++)
        room += excess > 0 ? values[i] - g.lanes[i].min
                           : g.lanes[i].max - values[i];
      if (room <= eps) return;  // nothing left to give; declaration-checked
      double take = std::min(std::fabs(excess), room);
      for (size_t i = 0; i < values.size(); i++) {
        double lane = excess > 0 ? values[i] - g.lanes[i].min
                                 : g.lanes[i].max - values[i];
        double share = take * (lane / room);
        values[i] += excess > 0 ? -share : share;
        values[i] = std::clamp(values[i], g.lanes[i].min, g.lanes[i].max);
      }
    }
  }

  // Audio file paths resolve relative to the source file that mentions them;
  // files are build inputs (§5.3) and are read (and validated) here, at
  // build time.
  SigPtr loadFile(const std::string& path, const CheckedModule& mod) {
    fs::path p(path);
    if (p.is_relative())
      p = fs::path(mod.parsed.path).parent_path() / p;
    std::string key = p.lexically_normal().string();
    auto it = fileCache_.find(key);
    if (it != fileCache_.end()) return it->second;
    if (loadedFiles_) loadedFiles_->push_back(key);
    WavData wav;
    try {
      wav = readWav(key);
    } catch (const std::exception& e) {
      throw EvalError(std::string("cannot load audio file: ") + e.what());
    }
    SigPtr sig = makeFileSignal(std::move(wav.channels), wav.rate);
    fileCache_[key] = sig;
    return sig;
  }

  // --- Operators ---------------------------------------------------------

  static SigPtr asSignal(const Value& v) {
    if (auto* s = std::get_if<SigPtr>(&v.v)) return *s;
    if (auto* sc = std::get_if<ScalarV>(&v.v)) return makeConst(sc->v);
    throw EvalError("internal error: operand is not a signal");
  }

  // Under `signal ~f`'s symbolic substitution the argument is a Signal,
  // so a comparison or an `if` inside `f` sees signals where the types
  // promised Scalars. Both become sample-wise graph nodes rather than
  // one build-time answer, and a condition is carried as a 0/1 signal:
  // 1.0 wherever it holds. `select` tests `gate >= threshold`, so each
  // ordering is a difference against zero, and the strict forms are the
  // non-strict one with its branches swapped.
  static SigPtr cmpSignal(BinOpKind op, SigPtr a, SigPtr b) {
    auto ge = [](SigPtr x, SigPtr y) {  // x >= y
      return makeSelect(makeBinOp(SigBinOp::Sub, std::move(x), std::move(y)),
                        0.0, makeConst(1.0), makeConst(0.0));
    };
    auto lt = [](SigPtr x, SigPtr y) {  // x < y, i.e. !(x >= y)
      return makeSelect(makeBinOp(SigBinOp::Sub, std::move(x), std::move(y)),
                        0.0, makeConst(0.0), makeConst(1.0));
    };
    switch (op) {
      case BinOpKind::Ge: return ge(a, b);
      case BinOpKind::Le: return ge(b, a);
      case BinOpKind::Lt: return lt(a, b);
      case BinOpKind::Gt: return lt(b, a);
      case BinOpKind::Eq: return makeBinOp(SigBinOp::Mul, ge(a, b), ge(b, a));
      default:  // Ne
        return makeBinOp(SigBinOp::Sub, makeConst(1.0),
                         makeBinOp(SigBinOp::Mul, ge(a, b), ge(b, a)));
    }
  }

  // A condition as a 0/1 signal: either it already is one, or it is a
  // build-time Bool that is constant across the whole signal.
  static SigPtr condSignal(const Value& v) {
    if (auto* s = std::get_if<SigPtr>(&v.v)) return *s;
    if (auto* b = std::get_if<BoolV>(&v.v)) return makeConst(b->v ? 1.0 : 0.0);
    throw EvalError("'&&'/'||' operand is not a Bool at build time");
  }

  // A branch of a sample-wise `if`. Both branches are built (there is no
  // per-sample control flow to skip one), so a branch that is not a
  // number has nowhere to go.
  static SigPtr branchSignal(const Value& v) {
    if (std::holds_alternative<SigPtr>(v.v) ||
        std::holds_alternative<ScalarV>(v.v))
      return asSignal(v);
    throw EvalError(
        "a sample-wise 'if' (its condition is a signal) needs Scalar or "
        "Signal branches: both branches become part of the graph and the "
        "choice happens per sample, so a branch that is not a number has "
        "no sample-wise meaning");
  }

  // A Timestamp is a point on (or a span of) a timeline that starts at
  // the epoch: subtracting past zero clamps rather than going negative,
  // and a NaN (0s * inf, say) has no position at all.
  static double clampTime(double x) {
    if (std::isnan(x)) throw EvalError("timestamp arithmetic produced NaN");
    return x < 0 ? 0.0 : x;
  }

  static double arith(BinOpKind op, double a, double b) {
    switch (op) {
      case BinOpKind::Add: return a + b;
      case BinOpKind::Sub: return a - b;
      case BinOpKind::Mul: return a * b;
      case BinOpKind::Div: return a / b;
      default: return 0;  // comparisons/logic never reach arith
    }
  }

  // Int arithmetic wraps like two's complement (add/sub/mul run on the
  // unsigned representation, so overflow is defined); division truncates
  // towards zero and its two undefined cases are build errors.
  static int64_t intArith(BinOpKind op, int64_t a, int64_t b) {
    switch (op) {
      case BinOpKind::Add: return (int64_t)((uint64_t)a + (uint64_t)b);
      case BinOpKind::Sub: return (int64_t)((uint64_t)a - (uint64_t)b);
      case BinOpKind::Mul: return (int64_t)((uint64_t)a * (uint64_t)b);
      case BinOpKind::Div:
        if (b == 0) throw EvalError("integer division by zero");
        if (a == INT64_MIN && b == -1)
          throw EvalError("integer division overflow");
        return a / b;
      default: return 0;  // comparisons/logic never reach intArith
    }
  }

  static SigBinOp toSigOp(BinOpKind op) {
    switch (op) {
      case BinOpKind::Add: return SigBinOp::Add;
      case BinOpKind::Sub: return SigBinOp::Sub;
      case BinOpKind::Mul: return SigBinOp::Mul;
      case BinOpKind::Div: return SigBinOp::Div;
      default: return SigBinOp::Add;  // unreachable (see binop)
    }
  }

  static bool isCmp(BinOpKind op) {
    switch (op) {
      case BinOpKind::Lt:
      case BinOpKind::Le:
      case BinOpKind::Gt:
      case BinOpKind::Ge:
      case BinOpKind::Eq:
      case BinOpKind::Ne:
        return true;
      default:
        return false;
    }
  }

  static bool cmp(BinOpKind op, double a, double b) {
    switch (op) {
      case BinOpKind::Lt: return a < b;
      case BinOpKind::Le: return a <= b;
      case BinOpKind::Gt: return a > b;
      case BinOpKind::Ge: return a >= b;
      case BinOpKind::Eq: return a == b;
      case BinOpKind::Ne: return a != b;
      default: return false;
    }
  }

  Value binop(BinOpKind op, Value l, Value r) {
    if (isCmp(op)) {
      // Two Scalars or two Timestamps (checker-enforced). Under
      // `signal ~f` symbolic substitution the time signal can arrive
      // here instead; that is a build-time error - a signal has no
      // single value to compare.
      if (auto* a = std::get_if<ScalarV>(&l.v))
        if (auto* b = std::get_if<ScalarV>(&r.v))
          return Value{BoolV{cmp(op, a->v, b->v)}};
      if (auto* a = std::get_if<IntV>(&l.v))
        if (auto* b = std::get_if<IntV>(&r.v)) {
          switch (op) {
            case BinOpKind::Lt: return Value{BoolV{a->v < b->v}};
            case BinOpKind::Le: return Value{BoolV{a->v <= b->v}};
            case BinOpKind::Gt: return Value{BoolV{a->v > b->v}};
            case BinOpKind::Ge: return Value{BoolV{a->v >= b->v}};
            case BinOpKind::Eq: return Value{BoolV{a->v == b->v}};
            case BinOpKind::Ne: return Value{BoolV{a->v != b->v}};
            default: break;
          }
        }
      if (auto* a = std::get_if<TimeV>(&l.v))
        if (auto* b = std::get_if<TimeV>(&r.v))
          return Value{BoolV{cmp(op, a->seconds, b->seconds)}};
      // Under `signal ~f` the operands are signals: the comparison has no
      // single answer, so it becomes a 0/1 signal instead of one Bool.
      if (std::holds_alternative<SigPtr>(l.v) ||
          std::holds_alternative<SigPtr>(r.v))
        return Value{cmpSignal(op, asSignal(l), asSignal(r))};
      throw EvalError(
          "comparison needs two Ints, two Scalars or two Timestamps at "
          "build time");
    }
    bool lSig = std::holds_alternative<SigPtr>(l.v);
    bool rSig = std::holds_alternative<SigPtr>(r.v);
    if (lSig || rSig)
      return Value{makeBinOp(toSigOp(op), asSignal(l), asSignal(r))};

    // Timestamps: durations add and subtract, and scale by a Scalar.
    // Every result clamps at the epoch - the timeline starts at 0s and a
    // negative Timestamp has no meaning, exactly as `jitter` clamps its
    // humanized steps. The checker admits only the combinations handled
    // here (see checkBinOp).
    if (auto* a = std::get_if<TimeV>(&l.v)) {
      if (auto* b = std::get_if<TimeV>(&r.v))
        return Value{TimeV{clampTime(arith(op, a->seconds, b->seconds))}};
      if (auto* b = std::get_if<ScalarV>(&r.v))
        return Value{TimeV{clampTime(arith(op, a->seconds, b->v))}};
    }
    if (auto* a = std::get_if<ScalarV>(&l.v))
      if (auto* b = std::get_if<TimeV>(&r.v))
        return Value{TimeV{clampTime(arith(op, a->v, b->seconds))}};

    if (auto* a = std::get_if<IntV>(&l.v))
      if (auto* b = std::get_if<IntV>(&r.v))
        return Value{IntV{intArith(op, a->v, b->v)}};
    if (auto* a = std::get_if<ScalarV>(&l.v)) {
      if (auto* b = std::get_if<ScalarV>(&r.v))
        return Value{ScalarV{arith(op, a->v, b->v)}};
      if (auto* vb = std::get_if<VectorV>(&r.v)) {
        VectorV out = *vb;
        for (double& x : out.v) x = arith(op, a->v, x);
        return Value{std::move(out)};
      }
    }
    if (auto* va = std::get_if<VectorV>(&l.v)) {
      if (auto* b = std::get_if<ScalarV>(&r.v)) {
        VectorV out = *va;
        for (double& x : out.v) x = arith(op, x, b->v);
        return Value{std::move(out)};
      }
      if (auto* vb = std::get_if<VectorV>(&r.v)) {
        if (va->v.size() != vb->v.size())
          throw EvalError("vector arithmetic: channel count mismatch (" +
                          std::to_string(va->v.size()) + " vs " +
                          std::to_string(vb->v.size()) + ")");
        VectorV out = *va;
        for (size_t i = 0; i < out.v.size(); i++)
          out.v[i] = arith(op, out.v[i], vb->v[i]);
        return Value{std::move(out)};
      }
    }
    throw EvalError("internal error: operator applied to unsupported values");
  }
};

}  // namespace

namespace {

// Build-time evaluation is recursive, and one language-level frame costs
// several interpreter frames of C++ stack. 4096 guarded language frames
// can exceed the OS default thread stack, so the interpreter runs on its
// own thread with an explicit stack large enough that the depth guard -
// not the OS - is always what stops deep recursion.
constexpr size_t kEvalStackBytes = 256 * 1024 * 1024;

struct EvalThreadCtx {
  std::function<bool()> fn;
  bool result = false;
  std::exception_ptr error;
};

void* evalThreadMain(void* arg) {
  auto* ctx = static_cast<EvalThreadCtx*>(arg);
  try {
    ctx->result = ctx->fn();
  } catch (...) {
    ctx->error = std::current_exception();
  }
  return nullptr;
}

}  // namespace

bool evaluateProgram(const Program& prog, std::vector<RenderTarget>& targets,
                     DiagnosticBag& diags,
                     std::vector<std::string>* loadedFiles,
                     const std::string& externalCacheDir,
                     const std::map<std::string, double>* controlOverrides,
                     std::vector<ControlDecl>* controls,
                     std::vector<PanelDecl>* panels) {
  EvalThreadCtx ctx;
  ctx.fn = [&] {
    return Interp(prog, targets, diags, loadedFiles, externalCacheDir,
                  controlOverrides, controls, panels)
        .run();
  };
  pthread_attr_t attr;
  pthread_t thread;
  if (pthread_attr_init(&attr) != 0 ||
      pthread_attr_setstacksize(&attr, kEvalStackBytes) != 0 ||
      pthread_create(&thread, &attr, evalThreadMain, &ctx) != 0) {
    // Cannot get the big stack: evaluate on the calling thread (the
    // guard still bounds depth; only extreme recursion is at risk).
    return ctx.fn();
  }
  pthread_attr_destroy(&attr);
  pthread_join(thread, nullptr);
  if (ctx.error) std::rethrow_exception(ctx.error);
  return ctx.result;
}

}  // namespace synth
