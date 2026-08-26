#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "diagnostics.hpp"

namespace synth {

enum class Tok {
  Let,
  In,
  Fun,
  Import,
  Open,
  Module,
  If,
  Then,
  Else,
  Match,
  With,
  Ident,      // lowercase-initial identifier or `_`
  UpIdent,    // uppercase-initial identifier (types, module names)
  TypeVar,    // 'a — a type variable in an annotation; text is the name
  Number,     // numeric literal with a decimal point (Scalar)
  IntNum,     // numeric literal without a decimal point (Int); value in inum
  Time,       // numeric literal with unit suffix (Timestamp); value in seconds
  Bool,       // `true` / `false`; num is 1.0 / 0.0
  String,     // "..." literal
  Colon,
  SemiSemi,   // ;;
  Semi,       // ;
  Equals,
  LParen,
  RParen,
  LBracket,
  RBracket,
  LBrace,     // {
  RBrace,     // }
  Comma,
  Dot,
  Arrow,      // ->
  Tilde,      // ~ (labeled parameter/argument marker)
  PipeGt,     // |>
  Bar,        // | (variant declarations and match arms)
  // Discrete (Int) operators. The continuous kinds - Scalar, Timestamp,
  // Vector, Signal - use the '.'-suffixed forms below, so no operator is
  // overloaded across the discrete/continuous divide (cf. OCaml's `+.`).
  Plus,
  Minus,
  Star,
  Slash,
  Lt,         // <
  Le,         // <=
  Gt,         // >
  Ge,         // >=
  EqEq,       // ==
  BangEq,     // !=
  // Continuous operators.
  PlusDot,    // +.
  MinusDot,   // -.
  StarDot,    // *.
  SlashDot,   // /.
  LtDot,      // <.
  LeDot,      // <=.
  GtDot,      // >.
  GeDot,      // >=.
  EqEqDot,    // ==.
  BangEqDot,  // !=.
  AndAnd,     // &&
  OrOr,       // ||
  Eof,
};

struct Token {
  Tok kind;
  Span span;
  std::string text;   // identifier / string contents
  double num = 0.0;   // Number value, or Time value in seconds
  int64_t inum = 0;   // IntNum value
};

// Tokenizes `source`. Lexical errors are reported into `diags` (with `file`
// as the diagnostic path) and lexing continues where possible. Always ends
// with an Eof token.
std::vector<Token> lex(const std::string& source, const std::string& file,
                       DiagnosticBag& diags);

const char* tokenName(Tok t);

}  // namespace synth
