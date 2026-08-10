#include "json.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace synth::json {

namespace {

class Parser {
 public:
  Parser(const std::string& text, std::string& error)
      : text_(text), error_(error) {}

  bool parseTop(Value& out) {
    skipWs();
    if (!parseValue(out)) return false;
    skipWs();
    if (pos_ != text_.size()) return fail("trailing content");
    return true;
  }

 private:
  const std::string& text_;
  std::string& error_;
  size_t pos_ = 0;

  bool fail(const std::string& msg) {
    error_ = msg + " at byte " + std::to_string(pos_);
    return false;
  }

  void skipWs() {
    while (pos_ < text_.size() &&
           std::isspace((unsigned char)text_[pos_]))
      pos_++;
  }

  bool consume(char c) {
    if (pos_ < text_.size() && text_[pos_] == c) {
      pos_++;
      return true;
    }
    return false;
  }

  bool literal(const char* word, size_t len) {
    if (text_.compare(pos_, len, word) != 0) return fail("invalid literal");
    pos_ += len;
    return true;
  }

  bool parseValue(Value& out) {
    if (pos_ >= text_.size()) return fail("unexpected end of input");
    char c = text_[pos_];
    switch (c) {
      case '{': return parseObject(out);
      case '[': return parseArray(out);
      case '"': out.kind = Value::Kind::String; return parseString(out.string);
      case 't': out.kind = Value::Kind::Bool; out.boolean = true;
                return literal("true", 4);
      case 'f': out.kind = Value::Kind::Bool; out.boolean = false;
                return literal("false", 5);
      case 'n': out.kind = Value::Kind::Null; return literal("null", 4);
      default: return parseNumber(out);
    }
  }

  bool parseObject(Value& out) {
    out.kind = Value::Kind::Object;
    pos_++;  // '{'
    skipWs();
    if (consume('}')) return true;
    for (;;) {
      skipWs();
      std::string key;
      if (pos_ >= text_.size() || text_[pos_] != '"')
        return fail("expected object key");
      if (!parseString(key)) return false;
      skipWs();
      if (!consume(':')) return fail("expected ':'");
      skipWs();
      Value v;
      if (!parseValue(v)) return false;
      out.object.emplace_back(std::move(key), std::move(v));
      skipWs();
      if (consume(',')) continue;
      if (consume('}')) return true;
      return fail("expected ',' or '}'");
    }
  }

  bool parseArray(Value& out) {
    out.kind = Value::Kind::Array;
    pos_++;  // '['
    skipWs();
    if (consume(']')) return true;
    for (;;) {
      skipWs();
      Value v;
      if (!parseValue(v)) return false;
      out.array.push_back(std::move(v));
      skipWs();
      if (consume(',')) continue;
      if (consume(']')) return true;
      return fail("expected ',' or ']'");
    }
  }

  bool parseString(std::string& out) {
    pos_++;  // '"'
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == '"') {
        pos_++;
        return true;
      }
      if (c == '\\') {
        if (pos_ + 1 >= text_.size()) return fail("bad escape");
        char e = text_[pos_ + 1];
        pos_ += 2;
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            if (pos_ + 4 > text_.size()) return fail("bad \\u escape");
            unsigned cp = 0;
            for (int i = 0; i < 4; i++) {
              char h = text_[pos_ + (size_t)i];
              cp <<= 4;
              if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
              else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
              else return fail("bad \\u escape");
            }
            pos_ += 4;
            // UTF-8 encode (BMP only; our writer only emits control chars).
            if (cp < 0x80) {
              out += (char)cp;
            } else if (cp < 0x800) {
              out += (char)(0xC0 | (cp >> 6));
              out += (char)(0x80 | (cp & 0x3F));
            } else {
              out += (char)(0xE0 | (cp >> 12));
              out += (char)(0x80 | ((cp >> 6) & 0x3F));
              out += (char)(0x80 | (cp & 0x3F));
            }
            break;
          }
          default: return fail("bad escape");
        }
        continue;
      }
      out += c;
      pos_++;
    }
    return fail("unterminated string");
  }

  bool parseNumber(Value& out) {
    size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') pos_++;
    while (pos_ < text_.size() &&
           (std::isdigit((unsigned char)text_[pos_]) || text_[pos_] == '.' ||
            text_[pos_] == 'e' || text_[pos_] == 'E' || text_[pos_] == '+' ||
            text_[pos_] == '-'))
      pos_++;
    if (pos_ == start) return fail("expected a value");
    out.kind = Value::Kind::Number;
    out.number = std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr);
    return true;
  }
};

}  // namespace

const Value* Value::get(const std::string& key) const {
  if (kind != Kind::Object) return nullptr;
  for (auto& [k, v] : object)
    if (k == key) return &v;
  return nullptr;
}

std::string Value::getString(const std::string& key,
                             const std::string& fallback) const {
  const Value* v = get(key);
  return v && v->kind == Kind::String ? v->string : fallback;
}

double Value::getNumber(const std::string& key, double fallback) const {
  const Value* v = get(key);
  return v && v->kind == Kind::Number ? v->number : fallback;
}

void Value::set(const std::string& key, Value v) {
  if (kind != Kind::Object) {
    kind = Kind::Object;
    object.clear();
  }
  for (auto& [k, existing] : object)
    if (k == key) {
      existing = std::move(v);
      return;
    }
  object.emplace_back(key, std::move(v));
}

Value makeNull() { return Value{}; }

Value makeBool(bool b) {
  Value v;
  v.kind = Value::Kind::Bool;
  v.boolean = b;
  return v;
}

Value makeNumber(double n) {
  Value v;
  v.kind = Value::Kind::Number;
  v.number = n;
  return v;
}

Value makeString(std::string s) {
  Value v;
  v.kind = Value::Kind::String;
  v.string = std::move(s);
  return v;
}

Value makeArray(std::vector<Value> items) {
  Value v;
  v.kind = Value::Kind::Array;
  v.array = std::move(items);
  return v;
}

Value makeObject() {
  Value v;
  v.kind = Value::Kind::Object;
  return v;
}

bool parse(const std::string& text, Value& out, std::string& error) {
  return Parser(text, error).parseTop(out);
}

namespace {

void serializeString(const std::string& s, std::string& out) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 0xF];
        } else {
          out += (char)c;
        }
    }
  }
  out += '"';
}

void serializeInto(const Value& v, std::string& out) {
  switch (v.kind) {
    case Value::Kind::Null: out += "null"; return;
    case Value::Kind::Bool: out += v.boolean ? "true" : "false"; return;
    case Value::Kind::Number: {
      double n = v.number;
      if (n == (double)(long long)n) {
        out += std::to_string((long long)n);
      } else {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.17g", n);
        out += buf;
      }
      return;
    }
    case Value::Kind::String: serializeString(v.string, out); return;
    case Value::Kind::Array: {
      out += '[';
      for (size_t i = 0; i < v.array.size(); i++) {
        if (i) out += ',';
        serializeInto(v.array[i], out);
      }
      out += ']';
      return;
    }
    case Value::Kind::Object: {
      out += '{';
      for (size_t i = 0; i < v.object.size(); i++) {
        if (i) out += ',';
        serializeString(v.object[i].first, out);
        out += ':';
        serializeInto(v.object[i].second, out);
      }
      out += '}';
      return;
    }
  }
}

}  // namespace

std::string serialize(const Value& v) {
  std::string out;
  serializeInto(v, out);
  return out;
}

}  // namespace synth::json
