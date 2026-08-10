#include "external.hpp"

#include <dlfcn.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "incremental.hpp"

namespace fs = std::filesystem;

namespace synth {

// The API header user externals compile against. Bump kExternalApiVersion
// whenever this text changes incompatibly - it salts the cache key, so
// stale objects are recompiled rather than loaded.
static constexpr int kExternalApiVersion = 1;

const char* externalApiHeader() {
  return R"HDR(// synth external API v1 (generated - do not edit).
//
// Implement a synth `external "this_file.cpp"` binding by defining, for a
// declaration `let name ... = external "this_file.cpp"`:
//
//   #include <synth/external.hpp>
//   SYNTH_EXTERNAL(name) {
//     *result = synth::ext::Value::scalar(args[0].asScalar() + 1.0);
//     return true;
//   }
//
// `args`/`argc` are the fully-applied arguments in declaration order.
// Return true on success; on failure either fill *error and return
// false, or throw std::exception. Only build-time data crosses this
// boundary: scalars, timestamps, bools, strings, vectors, and
// lists/tuples of those. Signals never do.
#pragma once
#include <stdexcept>
#include <string>
#include <vector>

namespace synth::ext {

struct Value {
  enum class Kind { Unit, Scalar, Time, Bool, String, List, Tuple, Vector };
  Kind kind = Kind::Unit;
  double num = 0.0;          // Scalar / Time (seconds) / Bool (0 or 1)
  std::string str;           // String
  std::vector<Value> items;  // List / Tuple / Vector (scalar items)

  static Value unit() { return {}; }
  static Value scalar(double v) { Value x; x.kind = Kind::Scalar; x.num = v; return x; }
  static Value time(double seconds) { Value x; x.kind = Kind::Time; x.num = seconds; return x; }
  static Value boolean(bool v) { Value x; x.kind = Kind::Bool; x.num = v ? 1.0 : 0.0; return x; }
  static Value string(std::string s) { Value x; x.kind = Kind::String; x.str = std::move(s); return x; }
  static Value list(std::vector<Value> xs) { Value x; x.kind = Kind::List; x.items = std::move(xs); return x; }
  static Value tuple(std::vector<Value> xs) { Value x; x.kind = Kind::Tuple; x.items = std::move(xs); return x; }

  double asScalar() const { require(Kind::Scalar, "Scalar"); return num; }
  double asTime() const { require(Kind::Time, "Timestamp"); return num; }
  bool asBool() const { require(Kind::Bool, "Bool"); return num != 0.0; }
  const std::string& asString() const { require(Kind::String, "String"); return str; }
  const std::vector<Value>& asList() const { require(Kind::List, "list"); return items; }
  const std::vector<Value>& asTuple() const { require(Kind::Tuple, "tuple"); return items; }

 private:
  void require(Kind k, const char* what) const {
    if (kind != k)
      throw std::runtime_error(std::string("external: argument is not a ") + what);
  }
};

}  // namespace synth::ext

// Entry-point contract. `name` must match the synth definition's name.
#define SYNTH_EXTERNAL(name)                                              \
  extern "C" bool synth_external_##name(                                  \
      const ::synth::ext::Value* args, int argc,                          \
      ::synth::ext::Value* result, ::std::string* error)
)HDR";
}

namespace {

using ExtValue = struct ExtValueMirror;  // never dereferenced by name here

// The host-side mirror of synth::ext::Value. Layout-identical to the
// header above (same field order and types, same compiler and standard
// library on both sides), which is what lets values cross the dlopen
// boundary directly.
struct ExtVal {
  enum class Kind { Unit, Scalar, Time, Bool, String, List, Tuple, Vector };
  Kind kind = Kind::Unit;
  double num = 0.0;
  std::string str;
  std::vector<ExtVal> items;
};

using EntryFn = bool (*)(const ExtVal*, int, ExtVal*, std::string*);

std::string readFileOrEmpty(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

ExtVal toExt(const Value& v) {
  ExtVal out;
  if (std::holds_alternative<UnitV>(v.v)) {
    out.kind = ExtVal::Kind::Unit;
  } else if (auto* s = std::get_if<ScalarV>(&v.v)) {
    out.kind = ExtVal::Kind::Scalar;
    out.num = s->v;
  } else if (auto* t = std::get_if<TimeV>(&v.v)) {
    out.kind = ExtVal::Kind::Time;
    out.num = t->seconds;
  } else if (auto* b = std::get_if<BoolV>(&v.v)) {
    out.kind = ExtVal::Kind::Bool;
    out.num = b->v ? 1.0 : 0.0;
  } else if (auto* str = std::get_if<StringV>(&v.v)) {
    out.kind = ExtVal::Kind::String;
    out.str = str->s;
  } else if (auto* vec = std::get_if<VectorV>(&v.v)) {
    out.kind = ExtVal::Kind::Vector;
    for (double c : vec->v) {
      ExtVal e;
      e.kind = ExtVal::Kind::Scalar;
      e.num = c;
      out.items.push_back(std::move(e));
    }
  } else if (auto* list = std::get_if<ListV>(&v.v)) {
    out.kind = ExtVal::Kind::List;
    for (auto& x : list->items) out.items.push_back(toExt(x));
  } else if (auto* tup = std::get_if<TupleV>(&v.v)) {
    out.kind = ExtVal::Kind::Tuple;
    for (auto& x : tup->items) out.items.push_back(toExt(x));
  } else {
    throw EvalError(
        "external: only build-time data (Scalar, Timestamp, Bool, String, "
        "Vector, lists, tuples) can cross an external boundary");
  }
  return out;
}

Value fromExt(const ExtVal& v) {
  switch (v.kind) {
    case ExtVal::Kind::Unit: return Value{UnitV{}};
    case ExtVal::Kind::Scalar: return Value{ScalarV{v.num}};
    case ExtVal::Kind::Time: return Value{TimeV{v.num}};
    case ExtVal::Kind::Bool: return Value{BoolV{v.num != 0.0}};
    case ExtVal::Kind::String: return Value{StringV{v.str}};
    case ExtVal::Kind::Vector: {
      VectorV out;
      for (auto& x : v.items) out.v.push_back(x.num);
      return Value{std::move(out)};
    }
    case ExtVal::Kind::List: {
      ListV out;
      for (auto& x : v.items) out.items.push_back(fromExt(x));
      return Value{std::move(out)};
    }
    case ExtVal::Kind::Tuple: {
      TupleV out;
      for (auto& x : v.items) out.items.push_back(fromExt(x));
      return Value{std::move(out)};
    }
  }
  throw EvalError("external: implementation returned an invalid value");
}

std::string shellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  return out + "'";
}

// Ensure <cacheDir>/include/synth/external.hpp matches the embedded
// header (rewrites on version drift).
fs::path ensureApiHeader(const fs::path& cacheDir) {
  fs::path includeDir = cacheDir / "include";
  fs::path headerPath = includeDir / "synth" / "external.hpp";
  std::string want = externalApiHeader();
  if (readFileOrEmpty(headerPath) != want) {
    fs::create_directories(headerPath.parent_path());
    std::ofstream out(headerPath, std::ios::binary);
    out << want;
  }
  return includeDir;
}

struct LoadedObject {
  void* handle = nullptr;
};

// Shared objects stay loaded for the life of the process: values handed
// out (strings, vectors) may reference code/data in them, so unloading
// is never safe. Keyed by object path; one compile per content hash.
std::map<std::string, LoadedObject>& loadedObjects() {
  static std::map<std::string, LoadedObject> objects;
  return objects;
}

}  // namespace

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
  fs::path includeDir = ensureApiHeader(cache);

  uint64_t key = fnv1a(source.data(), source.size(),
                       fnv1a(&kExternalApiVersion, sizeof kExternalApiVersion,
                             kFnvOffset));
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

  return [entry, name, cppPath](const std::vector<Value>& args) -> Value {
    std::vector<ExtVal> extArgs;
    extArgs.reserve(args.size());
    for (auto& a : args) extArgs.push_back(toExt(a));
    ExtVal result;
    std::string error;
    bool ok;
    try {
      ok = entry(extArgs.data(), (int)extArgs.size(), &result, &error);
    } catch (const std::exception& e) {
      throw EvalError("external '" + name + "' (" + cppPath +
                      "): " + e.what());
    }
    if (!ok)
      throw EvalError("external '" + name + "' (" + cppPath + "): " +
                      (error.empty() ? "implementation reported failure"
                                     : error));
    return fromExt(result);
  };
}

}  // namespace synth
