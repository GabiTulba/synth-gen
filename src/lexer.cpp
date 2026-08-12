#include "lexer.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <map>

namespace synth {

namespace {

bool isIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool isIdentCont(char c) {
  return std::isalnum((unsigned char)c) || c == '_' || c == '\'';
}

// Unit suffix -> multiplier to seconds. Longest-match applied by caller.
const std::map<std::string, double, std::greater<std::string>>& unitTable() {
  static const std::map<std::string, double, std::greater<std::string>> t = {
      {"ns", 1e-9}, {"us", 1e-6}, {"ms", 1e-3}, {"s", 1.0}, {"m", 60.0},
  };
  return t;
}

}  // namespace

const char* tokenName(Tok t) {
  switch (t) {
    case Tok::Let: return "'let'";
    case Tok::In: return "'in'";
    case Tok::Fun: return "'fun'";
    case Tok::Import: return "'import'";
    case Tok::Open: return "'open'";
    case Tok::Module: return "'module'";
    case Tok::If: return "'if'";
    case Tok::Then: return "'then'";
    case Tok::Else: return "'else'";
    case Tok::Match: return "'match'";
    case Tok::With: return "'with'";
    case Tok::Ident: return "identifier";
    case Tok::UpIdent: return "capitalized identifier";
    case Tok::TypeVar: return "type variable";
    case Tok::Number: return "number";
    case Tok::IntNum: return "integer literal";
    case Tok::Time: return "timestamp literal";
    case Tok::Bool: return "boolean literal";
    case Tok::String: return "string literal";
    case Tok::Colon: return "':'";
    case Tok::SemiSemi: return "';;'";
    case Tok::Semi: return "';'";
    case Tok::Equals: return "'='";
    case Tok::LParen: return "'('";
    case Tok::RParen: return "')'";
    case Tok::LBracket: return "'['";
    case Tok::RBracket: return "']'";
    case Tok::LBrace: return "'{'";
    case Tok::RBrace: return "'}'";
    case Tok::Comma: return "','";
    case Tok::Dot: return "'.'";
    case Tok::Arrow: return "'->'";
    case Tok::Tilde: return "'~'";
    case Tok::PipeGt: return "'|>'";
    case Tok::Bar: return "'|'";
    case Tok::Plus: return "'+'";
    case Tok::Minus: return "'-'";
    case Tok::Star: return "'*'";
    case Tok::Slash: return "'/'";
    case Tok::Lt: return "'<'";
    case Tok::Le: return "'<='";
    case Tok::Gt: return "'>'";
    case Tok::Ge: return "'>='";
    case Tok::EqEq: return "'=='";
    case Tok::BangEq: return "'!='";
    case Tok::AndAnd: return "'&&'";
    case Tok::OrOr: return "'||'";
    case Tok::Eof: return "end of file";
  }
  return "?";
}

std::vector<Token> lex(const std::string& src, const std::string& file,
                       DiagnosticBag& diags) {
  std::vector<Token> out;
  uint32_t i = 0;
  const uint32_t n = (uint32_t)src.size();

  auto push = [&](Tok k, uint32_t lo, uint32_t hi, std::string text = {},
                  double num = 0.0) {
    out.push_back({k, {lo, hi}, std::move(text), num});
  };

  while (i < n) {
    char c = src[i];
    if (std::isspace((unsigned char)c)) {
      i++;
      continue;
    }
    // Comments: (* ... *), nested.
    if (c == '(' && i + 1 < n && src[i + 1] == '*') {
      uint32_t lo = i;
      int depth = 0;
      while (i < n) {
        if (src[i] == '(' && i + 1 < n && src[i + 1] == '*') {
          depth++;
          i += 2;
        } else if (src[i] == '*' && i + 1 < n && src[i + 1] == ')') {
          depth--;
          i += 2;
          if (depth == 0) break;
        } else {
          i++;
        }
      }
      if (depth != 0)
        diags.error(file, {lo, i}, "unterminated comment");
      continue;
    }
    uint32_t lo = i;
    // A quote only ever *starts* a token as a type variable: it is an
    // identifier continuation character (x'), never an identifier start,
    // and the language has no character literals.
    if (c == '\'' && i + 1 < n && isIdentStart(src[i + 1])) {
      i++;  // the quote
      uint32_t nameLo = i;
      while (i < n && isIdentCont(src[i])) i++;
      push(Tok::TypeVar, lo, i, src.substr(nameLo, i - nameLo));
      continue;
    }
    if (isIdentStart(c)) {
      while (i < n && isIdentCont(src[i])) i++;
      std::string word = src.substr(lo, i - lo);
      if (word == "let")
        push(Tok::Let, lo, i);
      else if (word == "in")
        push(Tok::In, lo, i);
      else if (word == "fun")
        push(Tok::Fun, lo, i);
      else if (word == "import")
        push(Tok::Import, lo, i);
      else if (word == "open")
        push(Tok::Open, lo, i);
      else if (word == "module")
        push(Tok::Module, lo, i);
      else if (word == "if")
        push(Tok::If, lo, i);
      else if (word == "then")
        push(Tok::Then, lo, i);
      else if (word == "else")
        push(Tok::Else, lo, i);
      else if (word == "match")
        push(Tok::Match, lo, i);
      else if (word == "with")
        push(Tok::With, lo, i);
      else if (word == "true")
        push(Tok::Bool, lo, i, {}, 1.0);
      else if (word == "false")
        push(Tok::Bool, lo, i, {}, 0.0);
      else if (std::isupper((unsigned char)word[0]))
        push(Tok::UpIdent, lo, i, word);
      else
        push(Tok::Ident, lo, i, word);
      continue;
    }
    if (std::isdigit((unsigned char)c)) {
      while (i < n && std::isdigit((unsigned char)src[i])) i++;
      bool sawDot = false;
      if (i < n && src[i] == '.' && i + 1 < n &&
          std::isdigit((unsigned char)src[i + 1])) {
        sawDot = true;
        i++;
        while (i < n && std::isdigit((unsigned char)src[i])) i++;
      }
      std::string digits = src.substr(lo, i - lo);
      double value = std::strtod(digits.c_str(), nullptr);
      // Unit suffix => Timestamp literal.
      bool isTime = false;
      for (auto& [suffix, mult] : unitTable()) {
        if (src.compare(i, suffix.size(), suffix) == 0) {
          uint32_t after = i + (uint32_t)suffix.size();
          if (after >= n || !isIdentCont(src[after])) {
            i = after;
            push(Tok::Time, lo, i, {}, value * mult);
            isTime = true;
            break;
          }
        }
      }
      if (isTime) continue;
      if (i < n && isIdentStart(src[i])) {
        uint32_t bad = i;
        while (i < n && isIdentCont(src[i])) i++;
        diags.error(file, {bad, i},
                    "unknown unit suffix '" + src.substr(bad, i - bad) +
                        "' (expected ns, us, ms, s or m)");
        continue;
      }
      // Without a decimal point the literal is an Int; with one, a Scalar.
      if (!sawDot) {
        errno = 0;
        long long iv = std::strtoll(digits.c_str(), nullptr, 10);
        if (errno == ERANGE) {
          diags.error(file, {lo, i}, "integer literal is too large");
          iv = 0;
        }
        out.push_back({Tok::IntNum, {lo, i}, {}, value, (int64_t)iv});
        continue;
      }
      push(Tok::Number, lo, i, {}, value);
      continue;
    }
    if (c == '"') {
      i++;
      std::string text;
      bool closed = false;
      while (i < n) {
        if (src[i] == '"') {
          i++;
          closed = true;
          break;
        }
        if (src[i] == '\\' && i + 1 < n) {
          char e = src[i + 1];
          if (e == 'n') text += '\n';
          else if (e == 't') text += '\t';
          else text += e;
          i += 2;
        } else {
          text += src[i++];
        }
      }
      if (!closed) diags.error(file, {lo, i}, "unterminated string literal");
      push(Tok::String, lo, i, std::move(text));
      continue;
    }
    // Multi-char operators first.
    if (c == ';' && i + 1 < n && src[i + 1] == ';') {
      i += 2;
      push(Tok::SemiSemi, lo, i);
      continue;
    }
    if (c == '-' && i + 1 < n && src[i + 1] == '>') {
      i += 2;
      push(Tok::Arrow, lo, i);
      continue;
    }
    if (c == '|' && i + 1 < n && src[i + 1] == '>') {
      i += 2;
      push(Tok::PipeGt, lo, i);
      continue;
    }
    if (c == '|' && i + 1 < n && src[i + 1] == '|') {
      i += 2;
      push(Tok::OrOr, lo, i);
      continue;
    }
    if (c == '&' && i + 1 < n && src[i + 1] == '&') {
      i += 2;
      push(Tok::AndAnd, lo, i);
      continue;
    }
    if (c == '<' && i + 1 < n && src[i + 1] == '=') {
      i += 2;
      push(Tok::Le, lo, i);
      continue;
    }
    if (c == '>' && i + 1 < n && src[i + 1] == '=') {
      i += 2;
      push(Tok::Ge, lo, i);
      continue;
    }
    if (c == '=' && i + 1 < n && src[i + 1] == '=') {
      i += 2;
      push(Tok::EqEq, lo, i);
      continue;
    }
    if (c == '!' && i + 1 < n && src[i + 1] == '=') {
      i += 2;
      push(Tok::BangEq, lo, i);
      continue;
    }
    i++;
    switch (c) {
      case ':': push(Tok::Colon, lo, i); break;
      case ';': push(Tok::Semi, lo, i); break;
      case '=': push(Tok::Equals, lo, i); break;
      case '(': push(Tok::LParen, lo, i); break;
      case ')': push(Tok::RParen, lo, i); break;
      case '[': push(Tok::LBracket, lo, i); break;
      case ']': push(Tok::RBracket, lo, i); break;
      case '{': push(Tok::LBrace, lo, i); break;
      case '}': push(Tok::RBrace, lo, i); break;
      // `|>` and `||` were consumed above, so a lone '|' is a variant/
      // match bar.
      case '|': push(Tok::Bar, lo, i); break;
      case ',': push(Tok::Comma, lo, i); break;
      case '.': push(Tok::Dot, lo, i); break;
      case '~': push(Tok::Tilde, lo, i); break;
      case '+': push(Tok::Plus, lo, i); break;
      case '-': push(Tok::Minus, lo, i); break;
      case '*': push(Tok::Star, lo, i); break;
      case '/': push(Tok::Slash, lo, i); break;
      case '<': push(Tok::Lt, lo, i); break;
      case '>': push(Tok::Gt, lo, i); break;
      default:
        diags.error(file, {lo, i},
                    std::string("unexpected character '") + c + "'");
    }
  }
  out.push_back({Tok::Eof, {n, n}, {}, 0.0});
  return out;
}

}  // namespace synth
