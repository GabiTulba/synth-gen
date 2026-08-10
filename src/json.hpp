#pragma once
#include <string>
#include <utility>
#include <vector>

namespace synth::json {

// Minimal JSON value + recursive-descent parser, dependency-free. Used by
// the compiler core to read build.json manifests and by the dev app to
// read build metadata (§9).
struct Value {
  enum class Kind { Null, Bool, Number, String, Array, Object };
  Kind kind = Kind::Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Value> array;
  std::vector<std::pair<std::string, Value>> object;  // insertion order

  // Object field lookup; nullptr when absent or not an object.
  const Value* get(const std::string& key) const;

  // Typed accessors with defaults (never throw).
  std::string getString(const std::string& key,
                        const std::string& fallback = {}) const;
  double getNumber(const std::string& key, double fallback = 0) const;

  // Object field write: overwrites an existing key, appends otherwise.
  // Turns a non-object into an (empty) object first.
  void set(const std::string& key, Value v);
};

// Value construction helpers for writers (the LSP server builds its
// responses as Values and serializes them).
Value makeNull();
Value makeBool(bool b);
Value makeNumber(double n);
Value makeString(std::string s);
Value makeArray(std::vector<Value> items = {});
Value makeObject();

// Parses `text` into `out`. On failure returns false and sets `error` to a
// message with a byte offset.
bool parse(const std::string& text, Value& out, std::string& error);

// Serializes `v` as compact JSON (integral numbers without a decimal
// point; strings escaped, non-ASCII bytes passed through as UTF-8).
std::string serialize(const Value& v);

}  // namespace synth::json
