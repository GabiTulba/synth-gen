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
  // Fun: per-param optional flags, parallel to `labels` (local-function
  // desugaring only). An optional parameter's item is its *element* type
  // as annotated (`?x:T` contributes T).
  std::vector<char> optionals;
  TypeExprPtr ret;  // Fun
  // Filled by the checker: the resolved semantic type. Doubles as the
  // idempotency guard - a node shared by the local-function desugaring
  // resolves once, which matters because resolution also canonicalizes
  // the surface name in place.
  TypePtr resolved;
  explicit TypeExpr(Kind k, Span s) : kind(k), span(s) {}
};

// What the operator *does*. Which types it accepts is a separate axis:
// see Expr::dotted. Evaluation only needs this kind - by the time a
// BinOp runs, the checker has already settled the operand types.
enum class BinOpKind {
  Add, Sub, Mul, Div,        // arithmetic (§3, pointwise lifting)
  Lt, Le, Gt, Ge, Eq, Ne,    // comparisons -> Bool
  And, Or,                   // Bool -> Bool -> Bool, short-circuit
};

struct Param {
  std::string name;
  TypeExprPtr typeExpr;  // the annotation as written (parser)
  TypePtr type;          // resolved by the checker: always the *element*
                         // type as annotated - for an optional parameter
                         // without a default the body sees `type Option`
  bool labeled = false;  // declared with ~name:Type (optional parameters
                         // are always labeled by their own name)
  // Declared with ?name:Type or ?(name = default : Type). Optional
  // parameters precede every required one, are filled only by label
  // (~name:value passes a determined value, ?name:opt passes an Option),
  // and default when the call completes: to `defaultExpr`'s value when
  // one is declared (the body then sees a plain Type), to None otherwise
  // (the body sees Type Option).
  bool optional = false;
  // Shared (not owned uniquely) so Param stays copyable; evaluated at
  // call time in the function's own scope, with earlier parameters bound.
  std::shared_ptr<Expr> defaultExpr;
  Span span{};
};

// A match/destructuring pattern. Patterns carry no annotations: their
// types derive from the scrutinee's (already known) type - they only
// take values apart, no inference happens.
struct Pattern {
  enum class Kind {
    Wildcard,  // _
    Bind,      // a lowercase name
    Ctor,      // [Module.]Name [payload pattern in items]
    Tuple,     // items
    Record,    // fieldNames + items (a subset of the record's fields;
               // a punned field { attack } binds under its own name)
  };
  Kind kind = Kind::Wildcard;
  Span span{};
  std::string moduleName;  // Ctor qualifier, "" if none
  std::string name;        // Bind / Ctor
  std::vector<Pattern> items;           // Ctor payload (0/1), Tuple, Record
  std::vector<std::string> fieldNames;  // Record, parallel to items
  // Filled by the checker: the type this pattern matches, and for Ctor
  // the constructor's index in its declaration.
  TypePtr type;
  int ctorIndex = -1;
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
    Let,       // name, declType; items[0] = bound expr, items[1] = body.
               // isRec: the name is in scope in the bound expr too.
    Lambda,    // params; items[0] = body. name is set (only) when this
               // is the desugared body of a recursive local function -
               // the evaluator rebinds it to the lambda itself per call.
    External,  // str = C++ file; whole body of a top-level let only
    RecordLit,     // items = field values, argLabels = field names;
                   // the checked type identifies the declaration
    RecordUpdate,  // items[0] = base, items[1..] = new field values,
                   // argLabels = their names (parallel to items[1..])
    Project,   // items[0] = record, name = field
    Ctor,      // moduleName/name; checker fills inum = ctor index and
               // type = the Named result. Applied via App (items[0] =
               // this node) - constructors are not first-class values.
    Match,     // items[0] = scrutinee, items[1..] = arm bodies,
               // patterns parallel to items[1..]. declType is set when
               // this is a destructuring let (checked irrefutable).
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
  // App: label per argument, parallel to items[1..]; "" = positional. A
  // label written with the optional-argument marker (`?x:opt`, passing an
  // Option through) is stored with a leading '?' ("?x") - labels are
  // identifiers, so the prefix is unambiguous.
  // RecordLit: field name per value, parallel to items.
  // RecordUpdate: field name per new value, parallel to items[1..].
  std::vector<std::string> argLabels;
  // Let only: the local binding's annotation as written (parser) and as
  // resolved (checker).
  TypeExprPtr declTypeExpr;
  TypePtr declType;
  // Lambda only: the anonymous function's parameters.
  std::vector<Param> params;
  // Match only: one pattern per arm, parallel to items[1..].
  std::vector<Pattern> patterns;
  // Let only: declared with `let rec`.
  bool isRec = false;
  // Ident only, and only in labeled-argument position: written punned,
  // as `~gain` for `~gain:gain`. The node is exactly the Ident the long
  // form would have produced - nothing downstream distinguishes them -
  // except that a rename of the *value* has to expand the pun back out,
  // since the label and the name share one piece of text.
  bool punned = false;
  // BinOp only: written in the '.'-suffixed form (`+.`, `>.`, ...), which
  // takes the continuous kinds - Scalar, Timestamp, Vector, Signal. The
  // bare form is Int-only. Nothing downstream of the checker looks at
  // this: it selects a typing rule, not a runtime behaviour.
  bool dotted = false;

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
  bool isRec = false;          // Let: declared with `let rec`
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
