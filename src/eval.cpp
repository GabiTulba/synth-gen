#include "eval.hpp"

#include <cstring>

#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>

#include "core/native.hpp"
#include "external.hpp"
#include "wav.hpp"

namespace fs = std::filesystem;

namespace synth {

namespace {

class Interp {
 public:
  Interp(const Program& prog, std::vector<RenderTarget>& targets,
         DiagnosticBag& diags, std::vector<std::string>* loadedFiles,
         std::string extCacheDir)
      : prog_(prog), targets_(targets), diags_(diags),
        loadedFiles_(loadedFiles), extCacheDir_(std::move(extCacheDir)) {}

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
  std::map<std::string, Env> globals_;             // module -> name -> value
  std::map<std::string, SigPtr> fileCache_;        // absolute path -> signal
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
        const BoolV* c = std::get_if<BoolV>(&cv.v);
        if (!c)
          throw EvalError(
              "'if' condition is not a Bool at build time (a signal has "
              "no single value to branch on)");
        return eval(*e.items[c->v ? 1 : 2], env, mod);
      }
      case Expr::Kind::App: {
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
          Value lv = eval(*e.items[0], env, mod);
          const BoolV* lb = std::get_if<BoolV>(&lv.v);
          if (!lb)
            throw EvalError("'&&'/'||' operand is not a Bool at build time");
          if (e.op == BinOpKind::And && !lb->v) return Value{BoolV{false}};
          if (e.op == BinOpKind::Or && lb->v) return Value{BoolV{true}};
          Value rv = eval(*e.items[1], env, mod);
          const BoolV* rb = std::get_if<BoolV>(&rv.v);
          if (!rb)
            throw EvalError("'&&'/'||' operand is not a Bool at build time");
          return Value{BoolV{rb->v}};
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
        ListV out;
        for (auto& x : e.items) out.items.push_back(eval(*x, env, mod));
        return Value{std::move(out)};
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
      case Expr::Kind::External:
        // evalDefs and applyValue dispatch external bodies before eval.
        throw EvalError("internal error: external body evaluated directly");
    }
    throw EvalError("internal error: unknown expression kind");
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

  // Label-aware application over user functions, lambdas, and
  // external-bodied definitions. Positional values fill the leftmost
  // unbound parameters; labeled ones bind by name. Incomplete
  // applications yield a closure carrying the bindings; complete ones
  // evaluate. `mod` is the module the application happens in: bodies
  // evaluate in their own defining module, but an external implementation
  // resolves relative paths (and attributes render targets) against the
  // *calling* module - `load_mono "kick.wav"` means next to the file
  // that wrote the string, not next to Core's lib.synth.
  Value applyValue(const Value& fn,
                   std::vector<std::pair<std::string, Value>> args,
                   const CheckedModule& mod) {
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

    if (f) {
      // Fully applied. An external body has no expression to evaluate:
      // its arguments dispatch to the bound implementation instead.
      if (f->def->body->kind == Expr::Kind::External) {
        std::vector<Value> ordered;
        for (auto& name : paramNames) ordered.push_back((*bound)[name]);
        return callExternal(*f->def, *f->mod, mod, std::move(ordered));
      }
      Env env;
      for (auto& name : paramNames) env[name] = (*bound)[name];
      return eval(*f->def->body, env, *f->mod);
    }
    // The captured environment plus the bound params (params shadow
    // captures), evaluated in the lambda's defining module.
    Env env = *l->captured;
    for (auto& name : paramNames) env[name] = (*bound)[name];
    return eval(*l->lam->items[0], env, *l->mod);
  }

  // Dispatch an external-bodied definition. Core library declarations
  // bind to built-in implementations (src/core/*.cpp); anything else is
  // user C++, compiled into a cached shared object on first use.
  // `declMod` is the module that declares the external (decides Core vs
  // user, anchors the .cpp path); `callerMod` is where this application
  // happens (anchors audio paths and render-target attribution).
  Value callExternal(const TopDef& def, const CheckedModule& declMod,
                     const CheckedModule& callerMod,
                     std::vector<Value> args) {
    if (declMod.libName == "Core") {
      const native::Impl* impl = native::find(def.body->str, def.name);
      if (!impl)
        throw EvalError("no built-in implementation for external '" +
                        def.name + "' (" + def.body->str + ")");
      native::Ctx ctx;
      ctx.apply = [this, &callerMod](const Value& fn, std::vector<Value> a) {
        return apply(fn, std::move(a), callerMod);
      };
      ctx.loadAudio = [this, &callerMod](const std::string& p) {
        return loadFile(p, callerMod);
      };
      ctx.targets = &targets_;
      ctx.mod = &callerMod;
      ctx.currentModule = currentModule_;
      ctx.currentDef = currentDef_;
      return impl->fn(ctx, args);
    }
    auto it = userExternals_.find(&def);
    if (it == userExternals_.end()) {
      // The C++ file resolves relative to the declaring source file and
      // is a build input like an audio file: the daemon watches it.
      fs::path p(def.body->str);
      if (p.is_relative())
        p = fs::path(declMod.parsed.path).parent_path() / p;
      std::string resolved = p.lexically_normal().string();
      if (loadedFiles_) loadedFiles_->push_back(resolved);
      it = userExternals_
               .emplace(&def,
                        loadUserExternal(resolved, def.name, extCacheDir_))
               .first;
    }
    return it->second(args);
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
      throw EvalError(
          "comparison needs two Ints, two Scalars or two Timestamps at "
          "build time (a signal cannot be compared sample-wise)");
    }
    bool lSig = std::holds_alternative<SigPtr>(l.v);
    bool rSig = std::holds_alternative<SigPtr>(r.v);
    if (lSig || rSig)
      return Value{makeBinOp(toSigOp(op), asSignal(l), asSignal(r))};

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

bool evaluateProgram(const Program& prog, std::vector<RenderTarget>& targets,
                     DiagnosticBag& diags,
                     std::vector<std::string>* loadedFiles,
                     const std::string& externalCacheDir) {
  return Interp(prog, targets, diags, loadedFiles, externalCacheDir).run();
}

}  // namespace synth
