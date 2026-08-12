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
using TypeExprPtr = std::shared_ptr<TypeExpr>;

// A type annotation exactly as written. The parser produces these; the
// checker resolves them to semantic TypePtrs (filling Param::type,
// Expr::declType and TopDef::retType) once it knows what type names are
// in scope. Nodes are shared (the local-function desugaring reuses
// parameter annotations); the checker rewrites Name nodes to canonical
// module ids + stored declaration names, exactly as it does for value
// identifiers, so the incremental hasher sees stable identities.
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
  // Filled by the checker: the resolved semantic type. Doubles as the
  // idempotency guard - a node shared by the local-function desugaring
  // resolves once, which matters because resolution also canonicalizes
  // the surface name in place.
  TypePtr resolved;
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
    RecordLit,     // items = field values, argLabels = field names;
                   // moduleName/name = resolved declaration (checker)
    RecordUpdate,  // items[0] = base, items[1..] = new field values,
                   // argLabels = their names (parallel to items[1..]);
                   // moduleName/name = resolved declaration (checker)
    Project,   // items[0] = record, name = field
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
  // App: label per argument, parallel to items[1..]; "" = positional.
  // RecordLit: field name per value, parallel to items.
  // RecordUpdate: field name per new value, parallel to items[1..].
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

// One record field or variant constructor in a `type` declaration.
struct TypeDeclField {
  std::string name;
  TypeExprPtr type;  // Ctor: null when the constructor has no payload
  Span span{};
};

struct TopDef {
  enum class Kind {
    Import,       // moduleName (possibly dotted: "Lib" or "Lib.File")
    Open,         // moduleName (dotted path brought into scope)
    ModuleAlias,  // name = alias, moduleName = dotted target path
    ModuleDef,    // name = module, defs = its body (module N = struct ... end)
    Let,          // name ("_" for effect bindings), params, retType, body
    TypeDecl,     // name, typeParams, and fields (record) or ctors
                  // (variant) or neither (abstract)
  };
  enum class TypeFlavor { Record, Variant, Abstract };
  Kind kind;
  Span span{};
  std::string moduleName;      // Import / Open / ModuleAlias target
  std::string name;            // Let / ModuleAlias / ModuleDef / TypeDecl
  std::vector<Param> params;   // Let (empty for constants)
  TypeExprPtr retTypeExpr;     // Let: annotation as written; null for `let _`
  TypePtr retType;             // Let: resolved by the checker
  ExprPtr body;                // Let
  std::vector<TopDef> defs;    // ModuleDef body (lets, opens, nested modules)
  TypeFlavor typeFlavor = TypeFlavor::Abstract;   // TypeDecl
  std::vector<std::string> typeParams;            // TypeDecl: 'a names
  std::vector<TypeDeclField> fields;              // TypeDecl: record fields
  std::vector<TypeDeclField> ctors;               // TypeDecl: variant ctors
};

struct ParsedModule {
  std::string name;         // module name derived from file name (capitalized)
  std::string path;         // path used in diagnostics
  std::string source;
  std::vector<TopDef> defs;
};

}  // namespace synth
