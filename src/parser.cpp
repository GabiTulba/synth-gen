#include "parser.hpp"

#include <cctype>
#include <filesystem>
#include <map>

namespace synth {

namespace {

class Parser {
 public:
  Parser(const std::vector<Token>& toks, const std::string& file,
         DiagnosticBag& diags)
      : toks_(toks), file_(file), diags_(diags) {}

  std::vector<TopDef> parseModule() {
    std::vector<TopDef> defs;
    while (!at(Tok::Eof)) {
      try {
        if (at(Tok::Import)) {
          defs.push_back(parseImport());
        } else if (at(Tok::Open)) {
          defs.push_back(parseOpen());
        } else if (at(Tok::Module)) {
          defs.push_back(parseModuleDef(/*insideStruct=*/false));
        } else if (at(Tok::Let)) {
          defs.push_back(parseLet());
        } else {
          fail(peek().span,
               std::string(
                   "expected 'let', 'import', 'open' or 'module', found ") +
                   tokenName(peek().kind));
        }
      } catch (const Recover&) {
        // Skip to just past the next ';;' (or EOF) and continue.
        while (!at(Tok::Eof) && !at(Tok::SemiSemi)) pos_++;
        if (at(Tok::SemiSemi)) pos_++;
      }
    }
    return defs;
  }

 private:
  struct Recover {};

  const std::vector<Token>& toks_;
  const std::string& file_;
  DiagnosticBag& diags_;
  size_t pos_ = 0;
  // Type variables are scoped to one top-level definition: every 'a in its
  // parameters, return type, lambda parameters and local annotations is the
  // same variable, and the next definition starts over. Ids stay unique
  // file-wide anyway (nextTypeVarSeq_ never resets) so a stored signature
  // can never be confused with another definition's.
  std::map<std::string, TypePtr> typeVars_;
  int nextTypeVarSeq_ = 0;

  const Token& peek(int ahead = 0) const {
    size_t i = pos_ + ahead;
    return i < toks_.size() ? toks_[i] : toks_.back();
  }
  bool at(Tok k) const { return peek().kind == k; }
  const Token& advance() { return toks_[pos_++]; }

  [[noreturn]] void fail(Span span, std::string msg) {
    diags_.error(file_, span, std::move(msg));
    throw Recover{};
  }

  const Token& expect(Tok k, const char* what) {
    if (!at(k))
      fail(peek().span, std::string("expected ") + what + ", found " +
                            tokenName(peek().kind));
    return toks_[pos_++];
  }

  // A dotted module path: UpIdent { "." UpIdent }. Returns the joined
  // path ("Lib" or "Lib.File") and extends `hi` to the last segment.
  std::string parseModulePath(uint32_t& hi) {
    const Token& first = expect(Tok::UpIdent, "module name");
    std::string path = first.text;
    hi = first.span.hi;
    while (at(Tok::Dot) && peek(1).kind == Tok::UpIdent) {
      advance();  // '.'
      const Token& seg = advance();
      path += "." + seg.text;
      hi = seg.span.hi;
    }
    return path;
  }

  TopDef parseImport() {
    TopDef d{};
    d.kind = TopDef::Kind::Import;
    Span lo = advance().span;  // 'import'
    uint32_t hi = lo.hi;
    d.moduleName = parseModulePath(hi);
    d.span = {lo.lo, hi};
    if (at(Tok::SemiSemi)) advance();  // optional ';;'
    return d;
  }

  TopDef parseOpen() {
    TopDef d{};
    d.kind = TopDef::Kind::Open;
    Span lo = advance().span;  // 'open'
    uint32_t hi = lo.hi;
    d.moduleName = parseModulePath(hi);
    d.span = {lo.lo, hi};
    if (at(Tok::SemiSemi)) advance();  // optional ';;'
    return d;
  }

  // module N = struct <defs> end [;;]   (an inline module definition)
  // module Alias = Dotted.Path [;;]     (a module alias; top level only)
  // `struct` and `end` are contextual: they lex as ordinary identifiers
  // (like `list` and `unit`) and only mean anything right here, so
  // existing bindings named `end` keep working.
  TopDef parseModuleDef(bool insideStruct) {
    Span lo = advance().span;  // 'module'
    const Token& name = expect(Tok::UpIdent, "module name");
    expect(Tok::Equals, "'=' after module name");
    if (at(Tok::Ident) && peek().text == "struct") {
      advance();  // 'struct'
      TopDef d{};
      d.kind = TopDef::Kind::ModuleDef;
      d.name = name.text;
      d.defs = parseStructBody();
      if (!(at(Tok::Ident) && peek().text == "end"))
        fail(peek().span,
             "expected 'end' to close 'module " + d.name + " = struct'");
      const Token& end = advance();
      d.span = {lo.lo, end.span.hi};
      if (at(Tok::SemiSemi)) advance();  // optional ';;'
      return d;
    }
    if (insideStruct)
      fail(name.span,
           "module aliases are only allowed at the top level of a file");
    TopDef d{};
    d.kind = TopDef::Kind::ModuleAlias;
    d.name = name.text;
    uint32_t hi = name.span.hi;
    d.moduleName = parseModulePath(hi);
    d.span = {lo.lo, hi};
    if (at(Tok::SemiSemi)) advance();  // optional ';;'
    return d;
  }

  // The defs between `struct` and `end`: lets, opens (scoped to the
  // module), and nested module definitions. Imports are file-scoped by
  // design and stay at the top level.
  std::vector<TopDef> parseStructBody() {
    std::vector<TopDef> defs;
    while (!at(Tok::Eof) && !(at(Tok::Ident) && peek().text == "end")) {
      try {
        if (at(Tok::Import)) {
          fail(peek().span, "'import' is not allowed inside a module "
                            "(imports are file-scoped; move it to the top "
                            "level)");
        } else if (at(Tok::Open)) {
          defs.push_back(parseOpen());
        } else if (at(Tok::Module)) {
          defs.push_back(parseModuleDef(/*insideStruct=*/true));
        } else if (at(Tok::Let)) {
          defs.push_back(parseLet());
        } else {
          fail(peek().span,
               std::string("expected 'let', 'open', 'module' or 'end', "
                           "found ") + tokenName(peek().kind));
        }
      } catch (const Recover&) {
        // Recover within the body: skip past the next ';;' but never past
        // the module's own 'end'.
        while (!at(Tok::Eof) && !at(Tok::SemiSemi) &&
               !(at(Tok::Ident) && peek().text == "end"))
          pos_++;
        if (at(Tok::SemiSemi)) pos_++;
      }
    }
    return defs;
  }

  TopDef parseLet() {
    TopDef d{};
    d.kind = TopDef::Kind::Let;
    Span lo = advance().span;  // 'let'
    typeVars_.clear();         // 'a is scoped to this definition
    const Token& name = expect(Tok::Ident, "binding name");
    d.name = name.text;
    parseParams(d.params);
    if (at(Tok::Colon)) {
      advance();
      d.retType = parseType();
    } else if (d.name != "_") {
      fail(peek().span, "missing return type annotation (every binding "
                        "except 'let _' must be annotated)");
    }
    expect(Tok::Equals, "'='");
    // `external "file.cpp"` as the *entire* body binds the definition to
    // a C++ implementation. `external` is contextual (an ordinary
    // identifier elsewhere); the string literal after it is what commits
    // this parse, so a binding named `external` still works - except
    // applied directly to a string literal, which needs parentheses.
    if (at(Tok::Ident) && peek().text == "external" &&
        peek(1).kind == Tok::String) {
      const Token& kw = advance();
      const Token& file = advance();
      auto ext = std::make_unique<Expr>(Expr::Kind::External,
                                        Span{kw.span.lo, file.span.hi});
      ext->str = file.text;
      d.body = std::move(ext);
    } else {
      d.body = parseExpr();
    }
    const Token& end = expect(Tok::SemiSemi, "';;' to end definition");
    d.span = {lo.lo, end.span.hi};
    return d;
  }

  // Parameters: [~]ident ':' type, until ':' (return type), '=' or '->'.
  // A leading '~' declares a labeled parameter. Shared by top-level lets
  // and lambdas.
  void parseParams(std::vector<Param>& out) {
    while (at(Tok::Ident) || at(Tok::Tilde)) {
      Param p;
      if (at(Tok::Tilde)) {
        advance();
        p.labeled = true;
      }
      const Token& pn = expect(Tok::Ident, "parameter name");
      p.name = pn.text;
      expect(Tok::Colon, "':' after parameter name");
      p.type = parseParamType();
      p.span = {pn.span.lo, peek().span.lo};
      out.push_back(std::move(p));
    }
  }

  // --- Types -------------------------------------------------------------

  // Parameter annotations: postfix types only; function types must be
  // parenthesized: f:(Scalar -> Scalar Signal).
  TypePtr parseParamType() { return parsePostfixType(); }

  // Full type; arrows allowed (right-associative).
  TypePtr parseType() {
    TypePtr left = parsePostfixType();
    if (at(Tok::Arrow)) {
      advance();
      TypePtr right = parseType();
      // Flatten right-nested arrows into a single Fun.
      if (right->kind == Type::Kind::Fun) {
        std::vector<TypePtr> params{left};
        for (auto& p : right->items) params.push_back(p);
        return tFun(std::move(params), right->ret);
      }
      return tFun({left}, right);
    }
    return left;
  }

  TypePtr parsePostfixType() {
    TypePtr t = parseAtomType();
    for (;;) {
      if (at(Tok::UpIdent) && peek().text == "Signal") {
        advance();
        t = tSignal(t);
      } else if (at(Tok::UpIdent) && peek().text == "Sample") {
        advance();
        t = tSample(t);
      } else if (at(Tok::Ident) && peek().text == "list") {
        advance();
        t = tList(t);
      } else {
        return t;
      }
    }
  }

  // 'a: the same name is the same variable throughout one top-level
  // definition, so `~x:'a ... : 'a` ties the result to the argument.
  TypePtr typeVarFor(const std::string& name) {
    auto it = typeVars_.find(name);
    if (it != typeVars_.end()) return it->second;
    TypePtr t = tRigidVar(nextTypeVarSeq_++, name);
    typeVars_.emplace(name, t);
    return t;
  }

  TypePtr parseAtomType() {
    if (at(Tok::TypeVar)) return typeVarFor(advance().text);
    if (at(Tok::UpIdent)) {
      const Token& t = advance();
      if (t.text == "Scalar") return tScalar();
      if (t.text == "Vector") return tVector();
      if (t.text == "Timestamp") return tTimestamp();
      if (t.text == "String") return tString();
      if (t.text == "Bool") return tBool();
      fail(t.span, "unknown type '" + t.text + "'");
    }
    if (at(Tok::Ident) && peek().text == "unit") {
      advance();
      return tUnit();
    }
    if (at(Tok::LParen)) {
      advance();
      TypePtr first = parseType();
      if (at(Tok::Comma)) {
        std::vector<TypePtr> items{first};
        while (at(Tok::Comma)) {
          advance();
          items.push_back(parseType());
        }
        expect(Tok::RParen, "')'");
        return tTuple(std::move(items));
      }
      expect(Tok::RParen, "')'");
      return first;
    }
    fail(peek().span,
         std::string("expected a type, found ") + tokenName(peek().kind));
  }

  // --- Expressions -------------------------------------------------------

  ExprPtr parseExpr() {
    if (at(Tok::Let)) return parseLetIn();
    if (at(Tok::Fun)) return parseLambda();
    if (at(Tok::If)) return parseIf();
    return parsePipe();
  }

  // Conditional: if cond then e1 else e2 - a build-time expression, so
  // both branches are required and must have the same type. Branches
  // extend maximally right like lambda bodies; parenthesize an `if` used
  // as an argument or on either side of an operator.
  ExprPtr parseIf() {
    Span lo = advance().span;  // 'if'
    ExprPtr cond = parseExpr();
    expect(Tok::Then, "'then' after the condition");
    ExprPtr thenE = parseExpr();
    expect(Tok::Else,
           "'else' (if/then/else is an expression: both branches are "
           "required)");
    ExprPtr elseE = parseExpr();
    auto e = std::make_unique<Expr>(Expr::Kind::If,
                                    Span{lo.lo, elseE->span.hi});
    e->items.push_back(std::move(cond));
    e->items.push_back(std::move(thenE));
    e->items.push_back(std::move(elseE));
    return e;
  }

  // Anonymous function: fun param+ -> body. Params are annotated exactly
  // like top-level def params; the return type is synthesized from the
  // body. The body extends maximally right, so a lambda used as an
  // argument or pipe right-hand side must be parenthesized.
  ExprPtr parseLambda() {
    Span lo = advance().span;  // 'fun'
    auto e = std::make_unique<Expr>(Expr::Kind::Lambda, lo);
    parseParams(e->params);
    if (e->params.empty())
      fail(peek().span, "expected parameter after 'fun'");
    expect(Tok::Arrow, "'->' after lambda parameters");
    ExprPtr body = parseExpr();
    e->span = {lo.lo, body->span.hi};
    e->items.push_back(std::move(body));
    return e;
  }

  // Local binding: let name : Type = expr in body. Annotated like every
  // other binding; value bindings only (no parameters) in v1.
  ExprPtr parseLetIn() {
    Span lo = advance().span;  // 'let'
    const Token& name = expect(Tok::Ident, "binding name");
    expect(Tok::Colon,
           "':' (local bindings are annotated: let name : Type = ... in ...)");
    TypePtr ty = parseType();
    expect(Tok::Equals, "'='");
    ExprPtr bound = parseExpr();
    expect(Tok::In, "'in' to close the local binding");
    ExprPtr body = parseExpr();
    auto e = std::make_unique<Expr>(Expr::Kind::Let,
                                    Span{lo.lo, body->span.hi});
    e->name = name.text;
    e->declType = std::move(ty);
    e->items.push_back(std::move(bound));
    e->items.push_back(std::move(body));
    return e;
  }

  // Lowest precedence, left-associative. `x |> f a b` desugars to the
  // application `f a b x`: the piped value becomes the final positional
  // argument, so the type checker and evaluator see one ordinary call
  // (this also lets pipes feed label-curried calls without materializing
  // an intermediate closure).
  ExprPtr parsePipe() {
    ExprPtr left = parseOr();
    while (at(Tok::PipeGt)) {
      advance();
      if (at(Tok::Fun))
        fail(peek().span,
             "parenthesize a lambda on the right of |>: x |> (fun ...)");
      ExprPtr right = parseAdditive();
      Span span{left->span.lo, right->span.hi};
      if (right->kind == Expr::Kind::Ident ||
          right->kind == Expr::Kind::Lambda) {
        auto app = std::make_unique<Expr>(Expr::Kind::App, span);
        app->items.push_back(std::move(right));
        app->items.push_back(std::move(left));
        app->argLabels.push_back("");
        left = std::move(app);
      } else if (right->kind == Expr::Kind::App) {
        right->items.push_back(std::move(left));
        right->argLabels.push_back("");
        right->span = span;
        left = std::move(right);
      } else {
        fail(right->span,
             "the right-hand side of |> must be a function name, "
             "application, or parenthesized lambda");
      }
    }
    return left;
  }

  // `||`, then `&&`, then the comparisons sit between the pipe and
  // arithmetic: `a + 1.0 < b && c` parses as `((a + 1.0) < b) && c`.
  // Comparing a comparison (`a < b < c`) parses left-associatively and is
  // rejected by the type checker (Bool has no ordering).
  ExprPtr parseOr() {
    ExprPtr left = parseAnd();
    while (at(Tok::OrOr)) {
      advance();
      ExprPtr right = parseAnd();
      left = mkBinOp(BinOpKind::Or, std::move(left), std::move(right));
    }
    return left;
  }

  ExprPtr parseAnd() {
    ExprPtr left = parseComparison();
    while (at(Tok::AndAnd)) {
      advance();
      ExprPtr right = parseComparison();
      left = mkBinOp(BinOpKind::And, std::move(left), std::move(right));
    }
    return left;
  }

  ExprPtr parseComparison() {
    ExprPtr left = parseAdditive();
    for (;;) {
      BinOpKind op;
      if (at(Tok::Lt)) op = BinOpKind::Lt;
      else if (at(Tok::Le)) op = BinOpKind::Le;
      else if (at(Tok::Gt)) op = BinOpKind::Gt;
      else if (at(Tok::Ge)) op = BinOpKind::Ge;
      else if (at(Tok::EqEq)) op = BinOpKind::Eq;
      else if (at(Tok::BangEq)) op = BinOpKind::Ne;
      else return left;
      advance();
      ExprPtr right = parseAdditive();
      left = mkBinOp(op, std::move(left), std::move(right));
    }
  }

  ExprPtr parseAdditive() {
    ExprPtr left = parseMultiplicative();
    while (at(Tok::Plus) || at(Tok::Minus)) {
      BinOpKind op = at(Tok::Plus) ? BinOpKind::Add : BinOpKind::Sub;
      advance();
      ExprPtr right = parseMultiplicative();
      left = mkBinOp(op, std::move(left), std::move(right));
    }
    return left;
  }

  ExprPtr parseMultiplicative() {
    ExprPtr left = parseUnary();
    while (at(Tok::Star) || at(Tok::Slash)) {
      BinOpKind op = at(Tok::Star) ? BinOpKind::Mul : BinOpKind::Div;
      advance();
      ExprPtr right = parseUnary();
      left = mkBinOp(op, std::move(left), std::move(right));
    }
    return left;
  }

  ExprPtr parseUnary() {
    if (at(Tok::Minus)) {
      Span lo = advance().span;
      ExprPtr operand = parseUnary();
      // Desugar unary minus to (0 - x); Scalar-only per operator typing.
      auto zero = std::make_unique<Expr>(Expr::Kind::NumLit, lo);
      zero->num = 0.0;
      auto e = mkBinOp(BinOpKind::Sub, std::move(zero), std::move(operand));
      e->span.lo = lo.lo;
      return e;
    }
    return parseApp();
  }

  ExprPtr parseApp() {
    ExprPtr head = parseAtom();
    std::vector<ExprPtr> args;
    std::vector<std::string> labels;
    for (;;) {
      if (at(Tok::Tilde)) {
        // Labeled argument: ~name:atom
        advance();
        const Token& name = expect(Tok::Ident, "label after '~'");
        expect(Tok::Colon, "':' after argument label");
        args.push_back(parseAtom());
        labels.push_back(name.text);
      } else if (startsAtom()) {
        args.push_back(parseAtom());
        labels.push_back("");
      } else {
        break;
      }
    }
    if (args.empty()) return head;
    auto e = std::make_unique<Expr>(
        Expr::Kind::App, Span{head->span.lo, args.back()->span.hi});
    e->items.push_back(std::move(head));
    for (auto& a : args) e->items.push_back(std::move(a));
    e->argLabels = std::move(labels);
    return e;
  }

  bool startsAtom() const {
    switch (peek().kind) {
      case Tok::Number:
      case Tok::Time:
      case Tok::Bool:
      case Tok::String:
      case Tok::Ident:
      case Tok::UpIdent:
      case Tok::LParen:
      case Tok::LBracket:
        return true;
      default:
        return false;
    }
  }

  ExprPtr parseAtom() {
    const Token& t = peek();
    switch (t.kind) {
      case Tok::Number: {
        advance();
        auto e = std::make_unique<Expr>(Expr::Kind::NumLit, t.span);
        e->num = t.num;
        return e;
      }
      case Tok::Time: {
        advance();
        auto e = std::make_unique<Expr>(Expr::Kind::TimeLit, t.span);
        e->num = t.num;
        return e;
      }
      case Tok::Bool: {
        advance();
        auto e = std::make_unique<Expr>(Expr::Kind::BoolLit, t.span);
        e->num = t.num;
        return e;
      }
      case Tok::String: {
        advance();
        auto e = std::make_unique<Expr>(Expr::Kind::StrLit, t.span);
        e->str = t.text;
        return e;
      }
      case Tok::Ident: {
        advance();
        auto e = std::make_unique<Expr>(Expr::Kind::Ident, t.span);
        e->name = t.text;
        return e;
      }
      case Tok::UpIdent: {
        // Qualified reference: Module.name or Lib.File.name. All dotted
        // UpIdent segments form the module path; the final lowercase
        // identifier is the definition name (unambiguous: definitions
        // are lowercase-initial).
        advance();
        std::string qual = t.text;
        expect(Tok::Dot, "'.' after module name");
        while (at(Tok::UpIdent)) {
          const Token& seg = advance();
          qual += "." + seg.text;
          expect(Tok::Dot, "'.' after module name");
        }
        const Token& n = expect(Tok::Ident, "identifier after '.'");
        auto e = std::make_unique<Expr>(Expr::Kind::Ident,
                                        Span{t.span.lo, n.span.hi});
        e->moduleName = std::move(qual);
        e->name = n.text;
        return e;
      }
      case Tok::LParen: {
        Span lo = advance().span;
        ExprPtr first = parseExpr();
        if (at(Tok::Comma)) {
          std::vector<ExprPtr> items;
          items.push_back(std::move(first));
          while (at(Tok::Comma)) {
            advance();
            items.push_back(parseExpr());
          }
          const Token& close = expect(Tok::RParen, "')'");
          auto e = std::make_unique<Expr>(Expr::Kind::TupleLit,
                                          Span{lo.lo, close.span.hi});
          e->items = std::move(items);
          return e;
        }
        const Token& close = expect(Tok::RParen, "')'");
        first->span = {lo.lo, close.span.hi};
        return first;
      }
      case Tok::LBracket: {
        Span lo = advance().span;
        auto e = std::make_unique<Expr>(Expr::Kind::ListLit, lo);
        if (!at(Tok::RBracket)) {
          e->items.push_back(parseExpr());
          while (at(Tok::Semi)) {
            advance();
            e->items.push_back(parseExpr());
          }
        }
        const Token& close = expect(Tok::RBracket, "']'");
        e->span = {lo.lo, close.span.hi};
        return e;
      }
      default:
        fail(t.span, std::string("expected an expression, found ") +
                         tokenName(t.kind));
    }
  }

  ExprPtr mkBinOp(BinOpKind op, ExprPtr l, ExprPtr r) {
    auto e = std::make_unique<Expr>(Expr::Kind::BinOp,
                                    Span{l->span.lo, r->span.hi});
    e->op = op;
    e->items.push_back(std::move(l));
    e->items.push_back(std::move(r));
    return e;
  }
};

}  // namespace

std::vector<TopDef> parse(const std::vector<Token>& tokens,
                          const std::string& file, DiagnosticBag& diags) {
  return Parser(tokens, file, diags).parseModule();
}

std::string moduleNameForPath(const std::string& path) {
  std::string stem = std::filesystem::path(path).stem().string();
  if (!stem.empty()) stem[0] = (char)std::toupper((unsigned char)stem[0]);
  return stem;
}

}  // namespace synth
