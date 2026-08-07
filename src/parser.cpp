#include "parser.hpp"

#include <cctype>
#include <filesystem>

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
        } else if (at(Tok::Let)) {
          defs.push_back(parseLet());
        } else {
          fail(peek().span, std::string("expected 'let' or 'import', found ") +
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

  TopDef parseImport() {
    TopDef d{};
    d.kind = TopDef::Kind::Import;
    Span lo = advance().span;  // 'import'
    const Token& name = expect(Tok::UpIdent, "module name");
    d.moduleName = name.text;
    d.span = {lo.lo, name.span.hi};
    if (at(Tok::SemiSemi)) advance();  // optional ';;'
    return d;
  }

  TopDef parseLet() {
    TopDef d{};
    d.kind = TopDef::Kind::Let;
    Span lo = advance().span;  // 'let'
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
    d.body = parseExpr();
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

  TypePtr parseAtomType() {
    if (at(Tok::UpIdent)) {
      const Token& t = advance();
      if (t.text == "Scalar") return tScalar();
      if (t.text == "Vector") return tVector();
      if (t.text == "Timestamp") return tTimestamp();
      if (t.text == "String") return tString();
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
    return parsePipe();
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
    ExprPtr left = parseAdditive();
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
        // Qualified reference: Module.name
        advance();
        expect(Tok::Dot, "'.' after module name");
        const Token& n = expect(Tok::Ident, "identifier after '.'");
        auto e = std::make_unique<Expr>(Expr::Kind::Ident,
                                        Span{t.span.lo, n.span.hi});
        e->moduleName = t.text;
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
