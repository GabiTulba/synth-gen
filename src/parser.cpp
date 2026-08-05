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
    // Parameters: ident ':' type, until ':' (return type) or '='.
    while (at(Tok::Ident)) {
      Param p;
      const Token& pn = advance();
      p.name = pn.text;
      expect(Tok::Colon, "':' after parameter name");
      p.type = parseParamType();
      p.span = {pn.span.lo, peek().span.lo};
      d.params.push_back(std::move(p));
    }
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

  ExprPtr parseExpr() { return parseAdditive(); }

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
    while (startsAtom()) args.push_back(parseAtom());
    if (args.empty()) return head;
    auto e = std::make_unique<Expr>(
        Expr::Kind::App, Span{head->span.lo, args.back()->span.hi});
    e->items.push_back(std::move(head));
    for (auto& a : args) e->items.push_back(std::move(a));
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
