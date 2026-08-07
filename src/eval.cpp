#include "eval.hpp"

#include <cstring>

#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>

#include "primitives.hpp"
#include "wav.hpp"

namespace fs = std::filesystem;

namespace synth {

namespace {

struct EvalError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Interp {
 public:
  Interp(const Program& prog, std::vector<RenderTarget>& targets,
         DiagnosticBag& diags, std::vector<std::string>* loadedFiles)
      : prog_(prog), targets_(targets), diags_(diags),
        loadedFiles_(loadedFiles) {}

  bool run() {
    bool ok = true;
    for (auto& mod : prog_.modules) {
      currentModule_ = &mod;
      for (auto& def : mod.parsed.defs) {
        if (def.kind != TopDef::Kind::Let) continue;
        // Modules pulled in from dependency libraries evaluate for their
        // values only: their `let _` render effects belong to the
        // library's own build, not to every consumer's.
        if (mod.external && def.name == "_") continue;
        currentDef_ = &def;
        try {
          if (!def.params.empty()) {
            // Functions are values; the body runs at call time.
            globals_[mod.parsed.name][def.name] =
                Value{FunV{&def, &mod, nullptr}};
          } else {
            Env empty;
            Value v = eval(*def.body, empty, mod);
            if (def.name != "_") globals_[mod.parsed.name][def.name] = v;
          }
        } catch (const EvalError& e) {
          diags_.error(mod.parsed.path, def.span, e.what());
          ok = false;
        } catch (const EngineError& e) {
          diags_.error(mod.parsed.path, def.span, e.what());
          ok = false;
        }
      }
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
  std::map<std::string, Env> globals_;             // module -> name -> value
  std::map<std::string, SigPtr> fileCache_;        // absolute path -> signal

  Value eval(const Expr& e, Env& env, const CheckedModule& mod) {
    switch (e.kind) {
      case Expr::Kind::NumLit: return Value{ScalarV{e.num}};
      case Expr::Kind::TimeLit: return Value{TimeV{e.num}};
      case Expr::Kind::StrLit: return Value{StringV{e.str}};
      case Expr::Kind::Ident: return lookup(e, env, mod);
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
        Value l = eval(*e.items[0], env, mod);
        Value r = eval(*e.items[1], env, mod);
        return binop(e.op, std::move(l), std::move(r));
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
      if (const PrimSig* p = findPrimitive(ident.name))
        return Value{PrimClosureV{(int)p->id, nullptr}};
      throw EvalError("unbound name '" + ident.name + "' at build time");
    }
    // The built-in Core namespace: primitives as first-class values.
    if (ident.moduleName == "Core") {
      if (const PrimSig* p = findCorePrim(ident.name))
        return Value{PrimClosureV{(int)p->id, nullptr}};
      throw EvalError("'Core' has no primitive named '" + ident.name + "'");
    }
    if (ident.moduleName == "Core.List") {
      if (const PrimSig* p = findCoreListPrim(ident.name))
        return Value{PrimClosureV{(int)p->id, nullptr}};
      throw EvalError("'Core.List' has no function named '" + ident.name +
                      "'");
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

  // Label-aware application over both user functions and primitives.
  // Positional values fill the leftmost unbound parameters; labeled ones
  // bind by name. Incomplete applications yield a closure carrying the
  // bindings; complete ones evaluate.
  Value applyValue(const Value& fn,
                   std::vector<std::pair<std::string, Value>> args,
                   const CheckedModule& mod) {
    // Assemble the parameter-name list of whatever we're applying.
    std::vector<std::string> paramNames;
    const std::map<std::string, Value>* prevBound = nullptr;
    const FunV* f = std::get_if<FunV>(&fn.v);
    const PrimClosureV* pc = std::get_if<PrimClosureV>(&fn.v);
    const LambdaV* l = std::get_if<LambdaV>(&fn.v);
    const PrimSig* sig = nullptr;
    if (f) {
      for (auto& p : f->def->params) paramNames.push_back(p.name);
      prevBound = f->bound.get();
    } else if (pc) {
      sig = findPrimitiveById((PrimId)pc->primId);
      if (!sig) throw EvalError("internal error: unknown primitive id");
      paramNames = sig->paramNames;
      prevBound = pc->bound.get();
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
      if (l) return Value{LambdaV{l->lam, l->mod, l->captured,
                                  std::move(bound)}};
      return Value{PrimClosureV{pc->primId, std::move(bound)}};
    }

    if (f) {
      Env env;
      for (auto& name : paramNames) env[name] = (*bound)[name];
      return eval(*f->def->body, env, *f->mod);
    }
    if (l) {
      // The captured environment plus the bound params (params shadow
      // captures), evaluated in the lambda's defining module.
      Env env = *l->captured;
      for (auto& name : paramNames) env[name] = (*bound)[name];
      return eval(*l->lam->items[0], env, *l->mod);
    }
    std::vector<Value> ordered;
    for (auto& name : paramNames) ordered.push_back((*bound)[name]);
    return applyPrim(*sig, std::move(ordered), mod);
  }

  // --- Primitive implementations ----------------------------------------

  static double scalarArg(const Value& v) { return std::get<ScalarV>(v.v).v; }

  // Counts are Scalars in the language; here they must be whole,
  // non-negative, and sane (build-time validation).
  static int64_t wholeCount(double v, const char* prim) {
    double rounded = std::round(v);
    if (std::fabs(v - rounded) > 1e-9 || rounded < 0)
      throw EvalError(std::string(prim) +
                      ": count must be a whole non-negative number (got " +
                      std::to_string(v) + ")");
    if (rounded > 1e6)
      throw EvalError(std::string(prim) + ": count " + std::to_string(v) +
                      " is unreasonably large");
    return (int64_t)rounded;
  }
  static double timeArg(const Value& v) { return std::get<TimeV>(v.v).seconds; }
  static const std::string& strArg(const Value& v) {
    return std::get<StringV>(v.v).s;
  }
  static SigPtr signalArg(const Value& v) { return std::get<SigPtr>(v.v); }

  Value applyPrim(const PrimSig& p, std::vector<Value> args,
                  const CheckedModule& mod) {
    switch (p.id) {
      case PrimId::Sine:
        return Value{makeOsc(OscKind::Sine, scalarArg(args[0]))};
      case PrimId::Saw:
        return Value{makeOsc(OscKind::Saw, scalarArg(args[0]))};
      case PrimId::Square:
        return Value{makeOsc(OscKind::Square, scalarArg(args[0]))};
      case PrimId::Noise:
        return Value{makeNoise(scalarArg(args[0]))};
      case PrimId::Fm:
        return Value{makeFm(scalarArg(args[0]), signalArg(args[1]))};
      case PrimId::Pm:
        return Value{makePm(scalarArg(args[0]), signalArg(args[1]))};
      case PrimId::Am:
        return Value{makeAm(signalArg(args[0]), signalArg(args[1]),
                            scalarArg(args[2]))};
      case PrimId::Delay:
        return Value{makeDelay(timeArg(args[0]), signalArg(args[1]))};
      case PrimId::Reverb:
        return Value{makeReverb(timeArg(args[0]), scalarArg(args[1]),
                                scalarArg(args[2]), signalArg(args[3]))};
      case PrimId::ExpDecay:
        return Value{makeExpDecay(scalarArg(args[0]))};
      case PrimId::Adsr:
        return Value{makeAdsr(timeArg(args[0]), timeArg(args[1]),
                              scalarArg(args[2]), timeArg(args[3]),
                              timeArg(args[4]))};
      case PrimId::Lowpass:
        return Value{makeFilter(FilterKind::Lowpass, scalarArg(args[0]),
                                signalArg(args[1]))};
      case PrimId::Highpass:
        return Value{makeFilter(FilterKind::Highpass, scalarArg(args[0]),
                                signalArg(args[1]))};
      case PrimId::HardClip:
        return Value{makeClip(ClipKind::Hard, scalarArg(args[0]),
                              signalArg(args[1]))};
      case PrimId::SoftClip:
        return Value{makeClip(ClipKind::Soft, scalarArg(args[0]),
                              signalArg(args[1]))};
      case PrimId::MixAll: {
        std::vector<SigPtr> items;
        for (auto& x : std::get<ListV>(args[0].v).items)
          items.push_back(signalArg(x));
        return Value{makeMix(std::move(items))};
      }
      case PrimId::Channels: {
        std::vector<SigPtr> items;
        for (auto& x : std::get<ListV>(args[0].v).items)
          items.push_back(signalArg(x));
        return Value{makeChannels(std::move(items))};
      }
      case PrimId::Sample: {
        SampleV s;
        s.sig = signalArg(args[0]);
        s.from = timeArg(args[1]);
        s.to = timeArg(args[2]);
        if (s.from < 0 || s.to < s.from)
          throw EvalError("sample: invalid window (from=" +
                          std::to_string(s.from) + "s, to=" +
                          std::to_string(s.to) + "s)");
        return Value{std::move(s)};
      }
      case PrimId::Place: {
        const SampleV& s = std::get<SampleV>(args[0].v);
        double at = timeArg(args[1]);
        return Value{makePlace(s.sig, s.from, s.to, at)};
      }
      case PrimId::PlaceMulti: {
        const SampleV& s = std::get<SampleV>(args[0].v);
        std::vector<SigPtr> placed;
        for (auto& t : std::get<ListV>(args[1].v).items)
          placed.push_back(makePlace(s.sig, s.from, s.to, timeArg(t)));
        return Value{makeMix(std::move(placed))};
      }
      case PrimId::RenderVisStems: {
        const std::string& name = strArg(args[0]);
        double rate = scalarArg(args[1]);
        if (name.empty())
          throw EvalError("render_vis_stems: empty artifact name");
        if (rate <= 0)
          throw EvalError("render_vis_stems: sample rate must be positive");
        RenderTarget t;
        t.kind = RenderTarget::Kind::VisualStems;
        t.name = name;
        t.rate = rate;
        for (auto& item : std::get<ListV>(args[2].v).items) {
          const TupleV& stem = std::get<TupleV>(item.v);
          const std::string& label = std::get<StringV>(stem.items[0].v).s;
          if (label.empty())
            throw EvalError("render_vis_stems: empty lane label");
          t.stems.emplace_back(label, std::get<SampleV>(stem.items[1].v));
        }
        t.file = mod.parsed.path;
        t.span = currentDef_ ? currentDef_->span : Span{};
        t.declModule = currentModule_;
        t.declDef = currentDef_;
        targets_.push_back(std::move(t));
        return Value{UnitV{}};
      }
      case PrimId::RenderStems: {
        const std::string& base = strArg(args[0]);
        double rate = scalarArg(args[1]);
        if (base.empty()) throw EvalError("render_stems: empty base name");
        if (rate <= 0)
          throw EvalError("render_stems: sample rate must be positive");
        for (auto& item : std::get<ListV>(args[2].v).items) {
          const TupleV& stem = std::get<TupleV>(item.v);
          const std::string& label = std::get<StringV>(stem.items[0].v).s;
          if (label.empty())
            throw EvalError("render_stems: empty stem label");
          RenderTarget t;
          t.kind = RenderTarget::Kind::Audio;
          t.name = base + "-" + label;
          t.rate = rate;
          t.sample = std::get<SampleV>(stem.items[1].v);
          t.file = mod.parsed.path;
          t.span = currentDef_ ? currentDef_->span : Span{};
          t.declModule = currentModule_;
          t.declDef = currentDef_;
          targets_.push_back(std::move(t));
        }
        return Value{UnitV{}};
      }
      case PrimId::Render:
      case PrimId::RenderVis: {
        RenderTarget t;
        t.kind = p.id == PrimId::RenderVis ? RenderTarget::Kind::Visual
                                           : RenderTarget::Kind::Audio;
        t.name = strArg(args[0]);
        t.rate = scalarArg(args[1]);
        t.sample = std::get<SampleV>(args[2].v);
        t.file = mod.parsed.path;
        t.span = currentDef_ ? currentDef_->span : Span{};
        t.declModule = currentModule_;
        t.declDef = currentDef_;
        if (t.name.empty()) throw EvalError("render: empty artifact name");
        if (t.rate <= 0)
          throw EvalError("render: sample rate must be positive");
        targets_.push_back(std::move(t));
        return Value{UnitV{}};
      }
      case PrimId::LoadMono: {
        SigPtr sig = loadFile(strArg(args[0]), mod);
        if (sig->channels() != 1)
          throw EvalError("load_mono: '" + strArg(args[0]) + "' has " +
                          std::to_string(sig->channels()) +
                          " channels (expected 1); use load_multi");
        return Value{sig};
      }
      case PrimId::LoadMulti:
        return Value{loadFile(strArg(args[0]), mod)};
      case PrimId::Map: {
        ListV out;
        const Value& f = args[0];
        for (auto& x : std::get<ListV>(args[1].v).items)
          out.items.push_back(apply(f, {x}, mod));
        return Value{std::move(out)};
      }
      case PrimId::Fold: {
        const Value& f = args[0];
        Value acc = args[1];
        for (auto& x : std::get<ListV>(args[2].v).items)
          acc = apply(f, {acc, x}, mod);
        return acc;
      }
      case PrimId::ListInit: {
        int64_t n = wholeCount(scalarArg(args[0]), "list_init");
        ListV out;
        for (int64_t i = 0; i < n; i++)
          out.items.push_back(apply(args[1], {Value{ScalarV{(double)i}}}, mod));
        return Value{std::move(out)};
      }
      case PrimId::Repeat: {
        int64_t n = wholeCount(scalarArg(args[0]), "repeat");
        ListV out;
        for (int64_t i = 0; i < n; i++) out.items.push_back(args[1]);
        return Value{std::move(out)};
      }
      case PrimId::TimeSteps: {
        double start = timeArg(args[0]);
        double step = timeArg(args[1]);
        int64_t n = wholeCount(scalarArg(args[2]), "time_steps");
        ListV out;
        for (int64_t i = 0; i < n; i++)
          out.items.push_back(Value{TimeV{start + step * (double)i}});
        return Value{std::move(out)};
      }
      case PrimId::Jitter: {
        double seed = scalarArg(args[0]);
        double spread = timeArg(args[1]);
        if (spread < 0) throw EvalError("jitter: negative spread");
        ListV out;
        int64_t i = 0;
        for (auto& x : std::get<ListV>(args[2].v).items) {
          double t = std::get<TimeV>(x.v).seconds;
          // splitmix64 over (seed bits, index): integer-only, so the
          // deltas are bit-identical on every platform.
          uint64_t h;
          std::memcpy(&h, &seed, sizeof h);
          h ^= (uint64_t)i * 0x9E3779B97F4A7C15ull;
          h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
          h ^= h >> 27; h *= 0x94D049BB133111EBull;
          h ^= h >> 31;
          double unit = (double)(h >> 11) / 9007199254740992.0;  // [0,1)
          double t2 = t + (unit * 2.0 - 1.0) * spread;
          out.items.push_back(Value{TimeV{std::max(0.0, t2)}});
          i++;
        }
        return Value{std::move(out)};
      }
    }
    throw EvalError("internal error: unimplemented primitive");
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
    }
    return 0;
  }

  static SigBinOp toSigOp(BinOpKind op) {
    switch (op) {
      case BinOpKind::Add: return SigBinOp::Add;
      case BinOpKind::Sub: return SigBinOp::Sub;
      case BinOpKind::Mul: return SigBinOp::Mul;
      case BinOpKind::Div: return SigBinOp::Div;
    }
    return SigBinOp::Add;
  }

  Value binop(BinOpKind op, Value l, Value r) {
    bool lSig = std::holds_alternative<SigPtr>(l.v);
    bool rSig = std::holds_alternative<SigPtr>(r.v);
    if (lSig || rSig)
      return Value{makeBinOp(toSigOp(op), asSignal(l), asSignal(r))};

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
                     std::vector<std::string>* loadedFiles) {
  return Interp(prog, targets, diags, loadedFiles).run();
}

}  // namespace synth
