#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "diagnostics.hpp"
#include "types.hpp"

namespace synth {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct TypeExpr;
using TypeExprPtr = std::shared_ptr<const TypeExpr>;

// A type annotation exactly as written. The parser produces these; the
// checker resolves them to semantic TypePtrs (filling Param::type,
// Expr::declType and TopDef::retType) once it knows what type names are
// in scope. Shared and immutable, like Type, so the local-function
// desugaring can reuse parameter annotations freely.
struct TypeExpr {
  enum class Kind {
    Name,   // a (possibly qualified) type name applied to `args`:
            // "Scalar", "'a list" (args = ['a]), "Scalar Signal"
    Var,    // 'a; the surface name (without the quote) is in `name`
    Tuple,  // items (size >= 2)
    Fun,    // items = params, labels, ret
  };
  Kind kind;
  Span span{};
  std::string moduleName;  // Name: dotted qualifier, "" if none
  std::string name;        // Name: the type name; Var: the variable name
  // Name: type arguments, written postfix (`'a list`, `Scalar Signal`).
  std::vector<TypeExprPtr> args;
  std::vector<TypeExprPtr> items;  // Tuple members or Fun params
  // Fun: per-param labels, "" = positional (local-function desugaring
  // only; a written arrow type has no labels).
  std::vector<std::string> labels;
  TypeExprPtr ret;  // Fun
  explicit TypeExpr(Kind k, Span s) : kind(k), span(s) {}
};

enum class BinOpKind {
  Add, Sub, Mul, Div,        // arithmetic (§3, pointwise lifting)
  Lt, Le, Gt, Ge, Eq, Ne,    // comparisons: Scalar/Timestamp pairs -> Bool
  And, Or,                   // Bool -> Bool -> Bool, short-circuit
};

struct Param {
  std::string name;
  TypeExprPtr typeExpr;  // the annotation as written (parser)
  TypePtr type;          // resolved by the checker
  bool labeled = false;  // declared with ~name:Type
  Span span{};
};

struct Expr {
  enum class Kind {
    NumLit,    // num
    IntLit,    // inum
    TimeLit,   // num (seconds)
    BoolLit,   // num (1.0 = true, 0.0 = false)
    StrLit,    // str
    Ident,     // moduleName ("" if unqualified) + name
    App,       // items[0] = callee, items[1..] = args
    BinOp,     // op, items[0], items[1]
    Neg,       // unary minus; items[0] = operand
    If,        // items[0] = condition, items[1] = then, items[2] = else
    ListLit,   // items
    TupleLit,  // items (size >= 2)
    Let,       // name, declType; items[0] = bound expr, items[1] = body
    Lambda,    // params; items[0] = body
    External,  // str = C++ file; whole body of a top-level let only
  };
  Kind kind;
  Span span{};
  double num = 0.0;
  int64_t inum = 0;
  std::string str;
  std::string moduleName;  // Ident: qualifier, empty if none
  std::string name;        // Ident
  BinOpKind op = BinOpKind::Add;
  std::vector<ExprPtr> items;
  // App only: label per argument, parallel to items[1..]; "" = positional.
  std::vector<std::string> argLabels;
  // Let only: the local binding's annotation as written (parser) and as
  // resolved (checker).
  TypeExprPtr declTypeExpr;
  TypePtr declType;
  // Lambda only: the anonymous function's parameters.
  std::vector<Param> params;

  // Filled in by the type checker.
  TypePtr type;

  explicit Expr(Kind k, Span s) : kind(k), span(s) {}
};

struct TopDef {
  enum class Kind {
    Import,       // moduleName (possibly dotted: "Lib" or "Lib.File")
    Open,         // moduleName (dotted path brought into scope)
    ModuleAlias,  // name = alias, moduleName = dotted target path
    ModuleDef,    // name = module, defs = its body (module N = struct ... end)
    Let,          // name ("_" for effect bindings), params, retType, body
  };
  Kind kind;
  Span span{};
  std::string moduleName;      // Import / Open / ModuleAlias target
  std::string name;            // Let / ModuleAlias / ModuleDef
  std::vector<Param> params;   // Let (empty for constants)
  TypeExprPtr retTypeExpr;     // Let: annotation as written; null for `let _`
  TypePtr retType;             // Let: resolved by the checker
  ExprPtr body;                // Let
  std::vector<TopDef> defs;    // ModuleDef body (lets, opens, nested modules)
};

struct ParsedModule {
  std::string name;         // module name derived from file name (capitalized)
  std::string path;         // path used in diagnostics
  std::string source;
  std::vector<TopDef> defs;
};

}  // namespace synth
