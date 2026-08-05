#pragma once
#include <memory>
#include <string>
#include <vector>

#include "diagnostics.hpp"
#include "types.hpp"

namespace synth {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

enum class BinOpKind { Add, Sub, Mul, Div };

struct Expr {
  enum class Kind {
    NumLit,    // num
    TimeLit,   // num (seconds)
    StrLit,    // str
    Ident,     // moduleName ("" if unqualified) + name
    App,       // items[0] = callee, items[1..] = args
    BinOp,     // op, items[0], items[1]
    ListLit,   // items
    TupleLit,  // items (size >= 2)
    Let,       // name, declType; items[0] = bound expr, items[1] = body
  };
  Kind kind;
  Span span{};
  double num = 0.0;
  std::string str;
  std::string moduleName;  // Ident: qualifier, empty if none
  std::string name;        // Ident
  BinOpKind op = BinOpKind::Add;
  std::vector<ExprPtr> items;
  // App only: label per argument, parallel to items[1..]; "" = positional.
  std::vector<std::string> argLabels;
  // Let only: the local binding's annotation.
  TypePtr declType;

  // Filled in by the type checker.
  TypePtr type;

  explicit Expr(Kind k, Span s) : kind(k), span(s) {}
};

struct Param {
  std::string name;
  TypePtr type;
  bool labeled = false;  // declared with ~name:Type
  Span span{};
};

struct TopDef {
  enum class Kind {
    Import,  // moduleName
    Let,     // name ("_" for effect bindings), params, retType, body
  };
  Kind kind;
  Span span{};
  std::string moduleName;      // Import
  std::string name;            // Let
  std::vector<Param> params;   // Let (empty for constants)
  TypePtr retType;             // Let; null for `let _ = ...` (implied unit)
  ExprPtr body;                // Let
};

struct ParsedModule {
  std::string name;         // module name derived from file name (capitalized)
  std::string path;         // path used in diagnostics
  std::string source;
  std::vector<TopDef> defs;
};

}  // namespace synth
