#include "external.hpp"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <synth/external.hpp>

#include "incremental.hpp"

namespace fs = std::filesystem;

namespace synth {

// Salts the cache key alongside the API header contents, so stale objects
// are recompiled rather than loaded after an incompatible change.
static constexpr int kExternalApiVersion = 4;

namespace {

using EntryFn = bool (*)(ext::Ctx&, const ext::Value*, int, ext::Value*,
                         std::string*);

std::string readFileOrEmpty(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The readable half of a variant's tag: a constructor's payload, when it
// is one plain value. Anything with structure of its own (a tuple, a
// record, another variant, a signal) is left off - the constructor name
// alone is the label then.
std::string payloadTag(const Value& p) {
  char buf[32];
  if (auto* sc = std::get_if<ScalarV>(&p.v)) {
    std::snprintf(buf, sizeof buf, " %g", sc->v);
    return buf;
  }
  if (auto* iv = std::get_if<IntV>(&p.v)) return " " + std::to_string(iv->v);
  if (auto* t = std::get_if<TimeV>(&p.v)) {
    std::snprintf(buf, sizeof buf, " %gs", t->seconds);
    return buf;
  }
  if (auto* b = std::get_if<BoolV>(&p.v)) return b->v ? " true" : " false";
  if (auto* str = std::get_if<StringV>(&p.v)) return " " + str->s;
  return "";
}

ext::Value toExt(const Value& v, const ExtServices& svc) {
  const CoreListInfo& list = svc.coreList;
  ext::Value out;
  // A Core list value (a Cons/Nil chain) flattens to an ext-level list,
  // so implementations keep seeing plain vectors.
  if (auto* var = std::get_if<VariantV>(&v.v);
      var && list && var->decl == list.decl) {
    out.kind = ext::Value::Kind::List;
    const VariantV* cur = var;
    for (;;) {
      if (cur->ctor == list.nilIndex) break;
      const TupleV* cell =
          cur->payload ? std::get_if<TupleV>(&cur->payload->v) : nullptr;
      if (!cell || cell->items.size() != 2)
        throw EvalError("internal error: malformed list value");
      out.items.push_back(toExt(cell->items[0], svc));
      const VariantV* next = std::get_if<VariantV>(&cell->items[1].v);
      if (!next)
        throw EvalError("internal error: malformed list value");
      cur = next;
    }
    return out;
  }
  if (std::holds_alternative<UnitV>(v.v)) {
    out.kind = ext::Value::Kind::Unit;
  } else if (auto* s = std::get_if<ScalarV>(&v.v)) {
    out.kind = ext::Value::Kind::Scalar;
    out.num = s->v;
  } else if (auto* iv = std::get_if<IntV>(&v.v)) {
    out.kind = ext::Value::Kind::Int;
    out.inum = iv->v;
  } else if (auto* t = std::get_if<TimeV>(&v.v)) {
    out.kind = ext::Value::Kind::Time;
    out.num = t->seconds;
  } else if (auto* b = std::get_if<BoolV>(&v.v)) {
    out.kind = ext::Value::Kind::Bool;
    out.num = b->v ? 1.0 : 0.0;
  } else if (auto* str = std::get_if<StringV>(&v.v)) {
    out.kind = ext::Value::Kind::String;
    out.str = str->s;
  } else if (auto* vec = std::get_if<VectorV>(&v.v)) {
    out.kind = ext::Value::Kind::Vector;
    for (double c : vec->v) {
      ext::Value e;
      e.kind = ext::Value::Kind::Scalar;
      e.num = c;
      out.items.push_back(std::move(e));
    }
  } else if (auto* tup = std::get_if<TupleV>(&v.v)) {
    out.kind = ext::Value::Kind::Tuple;
    for (auto& x : tup->items) out.items.push_back(toExt(x, svc));
  } else if (auto* sig = std::get_if<SigPtr>(&v.v)) {
    out.kind = ext::Value::Kind::Signal;
    out.sig = *sig;
  } else if (auto* rec = std::get_if<RecordV>(&v.v);
             rec && svc.coreSample && rec->decl == svc.coreSample.decl) {
    // A Core Sample record becomes the engine-level sample.
    const CoreSampleInfo& cs = svc.coreSample;
    const SigPtr* sig = std::get_if<SigPtr>(&rec->fields[(size_t)cs.sigField].v);
    const TimeV* from = std::get_if<TimeV>(&rec->fields[(size_t)cs.fromField].v);
    const TimeV* to = std::get_if<TimeV>(&rec->fields[(size_t)cs.toField].v);
    if (!sig || !from || !to)
      throw EvalError("internal error: malformed Sample value");
    out.kind = ext::Value::Kind::Sample;
    out.samp = ext::Sample{*sig, from->seconds, to->seconds};
  } else {
    // Functions, records, variants of any other type (and any future
    // kind): an opaque box the implementation can apply through ctx or
    // hand back unchanged. A variant lends its constructor name - plus a
    // simple payload where it has one - as the box's tag, the one
    // readable thing about a value whose structure does not cross, so an
    // implementation can label it (a `choice` over Fx.Linear /
    // Fx.Exponential 5.0 draws "Linear" and "Exponential 5") without
    // understanding it.
    out.kind = ext::Value::Kind::Opaque;
    if (auto* var = std::get_if<VariantV>(&v.v); var && var->decl &&
        var->ctor >= 0 && var->ctor < (int)var->decl->ctors.size()) {
      out.str = var->decl->ctors[(size_t)var->ctor].name;
      if (var->payload) out.str += payloadTag(*var->payload);
    }
    out.opaque = std::make_shared<Value>(v);
  }
  return out;
}

Value fromExt(const ext::Value& v, const ExtServices& svc) {
  const CoreListInfo& list = svc.coreList;
  switch (v.kind) {
    case ext::Value::Kind::Unit: return Value{UnitV{}};
    case ext::Value::Kind::Scalar: return Value{ScalarV{v.num}};
    case ext::Value::Kind::Int: return Value{IntV{v.inum}};
    case ext::Value::Kind::Time: return Value{TimeV{v.num}};
    case ext::Value::Kind::Bool: return Value{BoolV{v.num != 0.0}};
    case ext::Value::Kind::String: return Value{StringV{v.str}};
    case ext::Value::Kind::Vector: {
      VectorV out;
      for (auto& x : v.items) out.v.push_back(x.num);
      return Value{std::move(out)};
    }
    case ext::Value::Kind::List: {
      if (!list)
        throw EvalError("internal error: Core's list type is not loaded");
      Value out{VariantV{list.decl, list.nilIndex, nullptr}};
      for (auto it = v.items.rbegin(); it != v.items.rend(); ++it) {
        TupleV cell;
        cell.items.push_back(fromExt(*it, svc));
        cell.items.push_back(std::move(out));
        out = Value{VariantV{list.decl, list.consIndex,
                             std::make_shared<Value>(
                                 Value{std::move(cell)})}};
      }
      return out;
    }
    case ext::Value::Kind::Tuple: {
      TupleV out;
      for (auto& x : v.items) out.items.push_back(fromExt(x, svc));
      return Value{std::move(out)};
    }
    case ext::Value::Kind::Signal:
      if (!v.sig) throw EvalError("external: returned a null Signal");
      return Value{v.sig};
    case ext::Value::Kind::Sample: {
      if (!v.samp.signal)
        throw EvalError("external: returned a Sample with a null Signal");
      if (!svc.coreSample)
        throw EvalError("internal error: Core's Sample type is not loaded");
      const CoreSampleInfo& cs = svc.coreSample;
      RecordV rec;
      rec.decl = cs.decl;
      rec.fields.resize(3);
      rec.fields[(size_t)cs.sigField] = Value{v.samp.signal};
      rec.fields[(size_t)cs.fromField] = Value{TimeV{v.samp.from}};
      rec.fields[(size_t)cs.toField] = Value{TimeV{v.samp.to}};
      return Value{std::move(rec)};
    }
    case ext::Value::Kind::Opaque:
      if (!v.opaque)
        throw EvalError("external: returned an empty opaque value");
      return *std::static_pointer_cast<Value>(v.opaque);
  }
  throw EvalError("external: implementation returned an invalid value");
}

RenderTarget fromDecl(ext::RenderDecl&& d) {
  RenderTarget t;
  switch (d.kind) {
    case ext::RenderDecl::Kind::Audio: t.kind = RenderTarget::Kind::Audio; break;
    case ext::RenderDecl::Kind::Visual: t.kind = RenderTarget::Kind::Visual; break;
    case ext::RenderDecl::Kind::VisualStems:
      t.kind = RenderTarget::Kind::VisualStems;
      break;
  }
  t.name = std::move(d.name);
  t.rate = d.rate;
  t.sample = SampleV{d.sample.signal, d.sample.from, d.sample.to};
  for (auto& [label, s] : d.stems)
    t.stems.emplace_back(label, SampleV{s.signal, s.from, s.to});
  return t;
}

std::string shellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  return out + "'";
}

struct LoadedObject {
  void* handle = nullptr;
};

// Shared objects stay loaded for the life of the process: values handed
// out (strings, signal graphs) may reference code/data in them, so
// unloading is never safe. Keyed by object path; one compile per content
// hash.
std::map<std::string, LoadedObject>& loadedObjects() {
  static std::map<std::string, LoadedObject> objects;
  return objects;
}

}  // namespace

std::string externalIncludeDir() {
  if (const char* env = std::getenv("SYNTH_EXT_INCLUDE_DIR"); env && *env)
    return env;
#ifdef SYNTH_EXT_INCLUDE_DEFAULT
  return SYNTH_EXT_INCLUDE_DEFAULT;
#else
  return "src/ext";
#endif
}

ExternalFn loadUserExternal(const std::string& cppPath,
                            const std::string& name,
                            const std::string& cacheDir) {
  std::string source = readFileOrEmpty(cppPath);
  if (source.empty())
    throw EvalError("external: cannot read '" + cppPath + "'");

  fs::path cache = cacheDir.empty()
                       ? fs::temp_directory_path() / "synth-externals"
                       : fs::path(cacheDir);
  std::error_code ec;
  fs::create_directories(cache, ec);
  fs::path includeDir(externalIncludeDir());

  // The key covers everything the object's behavior depends on: the
  // implementation source, the API headers it compiles against, and the
  // contract version.
  uint64_t key = fnv1a(&kExternalApiVersion, sizeof kExternalApiVersion,
                       kFnvOffset);
  key = fnv1a(source.data(), source.size(), key);
  for (const char* hdr : {"synth/external.hpp", "synth/engine.hpp"}) {
    std::string text = readFileOrEmpty(includeDir / hdr);
    if (text.empty())
      throw EvalError(std::string("external: cannot read API header '") +
                      (includeDir / hdr).string() +
                      "' (set SYNTH_EXT_INCLUDE_DIR to the directory "
                      "holding synth/external.hpp)");
    key = fnv1a(text.data(), text.size(), key);
  }
  char hex[17];
  snprintf(hex, sizeof hex, "%016llx", (unsigned long long)key);
  fs::path object =
      cache / (fs::path(cppPath).stem().string() + "-" + hex + ".so");

  if (!fs::exists(object)) {
    const char* env = std::getenv("CXX");
    std::string cxx = env && *env ? env : "c++";
    fs::path tmp = object;
    tmp += ".tmp";
    fs::path errFile = object;
    errFile += ".log";
    // Engine symbols (make* constructors) stay undefined in the object
    // and resolve against the host executable's exports at dlopen time.
    std::string cmd = cxx + " -std=c++20 -O2 -fPIC -shared " +
                      shellQuote(cppPath) + " -o " +
                      shellQuote(tmp.string()) + " -I " +
                      shellQuote(includeDir.string()) + " 2> " +
                      shellQuote(errFile.string());
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
      std::string log = readFileOrEmpty(errFile);
      if (log.size() > 2000) log = log.substr(0, 2000) + "\n...";
      throw EvalError("external: compiling '" + cppPath + "' failed:\n" +
                      log);
    }
    fs::rename(tmp, object, ec);
    if (ec && !fs::exists(object))
      throw EvalError("external: cannot write compiled object '" +
                      object.string() + "'");
  }

  LoadedObject& obj = loadedObjects()[object.string()];
  if (!obj.handle) {
    obj.handle = dlopen(object.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!obj.handle)
      throw EvalError("external: cannot load '" + object.string() +
                      "': " + dlerror());
  }
  std::string symbol = "synth_external_" + name;
  void* sym = dlsym(obj.handle, symbol.c_str());
  if (!sym)
    throw EvalError("external: '" + cppPath + "' does not define " + symbol +
                    " (declare it with SYNTH_EXTERNAL(" + name + "))");
  EntryFn entry = reinterpret_cast<EntryFn>(sym);

  return [entry, name, cppPath](const ExtServices& svc,
                                const std::vector<Value>& args) -> Value {
    ext::Ctx ctx;
    ctx.apply = [&svc](const ext::Value& fn,
                       std::vector<ext::Value> a) -> ext::Value {
      std::vector<Value> hostArgs;
      hostArgs.reserve(a.size());
      for (auto& x : a) hostArgs.push_back(fromExt(x, svc));
      return toExt(svc.apply(fromExt(fn, svc), std::move(hostArgs)), svc);
    };
    ctx.loadAudio = svc.loadAudio;
    ctx.render = [&svc](ext::RenderDecl d) {
      if (d.kind != ext::RenderDecl::Kind::VisualStems && !d.sample.signal)
        throw std::runtime_error("render: target has no sample");
      svc.declareTarget(fromDecl(std::move(d)));
    };
    ctx.control = [&svc](ext::ControlDecl d) -> double {
      ControlDecl c;
      switch (d.kind) {
        case ext::ControlDecl::Kind::Knob:
          c.kind = ControlDecl::Kind::Knob;
          break;
        case ext::ControlDecl::Kind::IntSlider:
          c.kind = ControlDecl::Kind::IntSlider;
          break;
        case ext::ControlDecl::Kind::Toggle:
          c.kind = ControlDecl::Kind::Toggle;
          break;
        case ext::ControlDecl::Kind::Choice:
          c.kind = ControlDecl::Kind::Choice;
          break;
        case ext::ControlDecl::Kind::Slider:
          c.kind = ControlDecl::Kind::Slider;
          break;
      }
      c.options = std::move(d.options);
      c.name = std::move(d.name);
      c.min = d.min;
      c.max = d.max;
      c.def = d.def;
      return svc.declareControl(std::move(c));
    };
    ctx.controlGroup = [&svc](ext::ControlGroupDecl d) {
      ControlGroupDecl g;
      g.name = std::move(d.name);
      g.sumMin = d.sumMin;
      g.sumMax = d.sumMax;
      for (auto& l : d.lanes)
        g.lanes.push_back(
            ControlGroupDecl::Lane{std::move(l.name), l.min, l.max, l.def});
      return svc.declareControlGroup(std::move(g));
    };
    ctx.panel = [&svc](ext::PanelDecl d) {
      PanelDecl p;
      p.name = std::move(d.name);
      for (auto& m : d.controls)
        p.controls.push_back(
            PanelDecl::Member{std::move(m.name), m.depth, std::move(m.keys)});
      p.targets = std::move(d.targets);
      svc.declarePanel(std::move(p));
    };

    std::vector<ext::Value> extArgs;
    extArgs.reserve(args.size());
    for (auto& a : args) extArgs.push_back(toExt(a, svc));
    ext::Value result;
    std::string error;
    bool ok;
    try {
      ok = entry(ctx, extArgs.data(), (int)extArgs.size(), &result, &error);
    } catch (const EvalError&) {
      throw;
    } catch (const std::exception& e) {
      throw EvalError("external '" + name + "' (" + cppPath +
                      "): " + e.what());
    }
    if (!ok)
      throw EvalError("external '" + name + "' (" + cppPath + "): " +
                      (error.empty() ? "implementation reported failure"
                                     : error));
    return fromExt(result, svc);
  };
}

}  // namespace synth
