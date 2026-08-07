#pragma once
#include <string>
#include <vector>

#include "diagnostics.hpp"

namespace synth {

enum class Tok {
  Let,
  In,
  Fun,
  Import,
  Ident,      // lowercase-initial identifier or `_`
  UpIdent,    // uppercase-initial identifier (types, module names)
  Number,     // numeric literal (Scalar)
  Time,       // numeric literal with unit suffix (Timestamp); value in seconds
  String,     // "..." literal
  Colon,
  SemiSemi,   // ;;
  Semi,       // ;
  Equals,
  LParen,
  RParen,
  LBracket,
  RBracket,
  Comma,
  Dot,
  Arrow,      // ->
  Tilde,      // ~ (labeled parameter/argument marker)
  PipeGt,     // |>
  Plus,
  Minus,
  Star,
  Slash,
  Eof,
};

struct Token {
  Tok kind;
  Span span;
  std::string text;   // identifier / string contents
  double num = 0.0;   // Number value, or Time value in seconds
};

// Tokenizes `source`. Lexical errors are reported into `diags` (with `file`
// as the diagnostic path) and lexing continues where possible. Always ends
// with an Eof token.
std::vector<Token> lex(const std::string& source, const std::string& file,
                       DiagnosticBag& diags);

const char* tokenName(Tok t);

}  // namespace synth
