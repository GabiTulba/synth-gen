#include "lexer.hpp"
#include "test_framework.hpp"

using namespace synth;

TEST(lexer_basic_tokens) {
  DiagnosticBag diags;
  auto toks = lex("let f x:Scalar : Scalar = x * 2.0 ;;", "t", diags);
  CHECK(!diags.hasErrors());
  CHECK(toks[0].kind == Tok::Let);
  CHECK(toks[1].kind == Tok::Ident);
  CHECK(toks[1].text == "f");
  CHECK(toks[2].kind == Tok::Ident);
  CHECK(toks[3].kind == Tok::Colon);
  CHECK(toks[4].kind == Tok::UpIdent);
  CHECK(toks[4].text == "Scalar");
  CHECK(toks.back().kind == Tok::Eof);
}

TEST(lexer_time_literals) {
  DiagnosticBag diags;
  auto toks = lex("100ns 30us 10ms 1s 1m 1.5s 500ms", "t", diags);
  CHECK(!diags.hasErrors());
  CHECK(toks[0].kind == Tok::Time);
  CHECK_NEAR(toks[0].num, 100e-9, 1e-15);
  CHECK_NEAR(toks[1].num, 30e-6, 1e-12);
  CHECK_NEAR(toks[2].num, 10e-3, 1e-9);
  CHECK_NEAR(toks[3].num, 1.0, 1e-9);
  CHECK_NEAR(toks[4].num, 60.0, 1e-9);
  CHECK_NEAR(toks[5].num, 1.5, 1e-9);
  CHECK_NEAR(toks[6].num, 0.5, 1e-9);
}

TEST(lexer_numbers_vs_time) {
  DiagnosticBag diags;
  auto toks = lex("48000.0 440.0 0s", "t", diags);
  CHECK(!diags.hasErrors());
  CHECK(toks[0].kind == Tok::Number);
  CHECK_NEAR(toks[0].num, 48000.0, 1e-9);
  CHECK(toks[1].kind == Tok::Number);
  CHECK(toks[2].kind == Tok::Time);
  CHECK_NEAR(toks[2].num, 0.0, 1e-9);
}

TEST(lexer_bad_unit_suffix) {
  DiagnosticBag diags;
  lex("10xyz", "t", diags);
  CHECK(diags.hasErrors());
}

TEST(lexer_comments_and_strings) {
  DiagnosticBag diags;
  auto toks = lex("(* a (* nested *) comment *) \"hi\\n\" ;;", "t", diags);
  CHECK(!diags.hasErrors());
  CHECK(toks[0].kind == Tok::String);
  CHECK(toks[0].text == "hi\n");
  CHECK(toks[1].kind == Tok::SemiSemi);
}

TEST(lexer_operators) {
  DiagnosticBag diags;
  auto toks = lex("-> - + * / [ ] ( ) ; . ,", "t", diags);
  CHECK(!diags.hasErrors());
  CHECK(toks[0].kind == Tok::Arrow);
  CHECK(toks[1].kind == Tok::Minus);
  CHECK(toks[2].kind == Tok::Plus);
  CHECK(toks[3].kind == Tok::Star);
  CHECK(toks[4].kind == Tok::Slash);
  CHECK(toks[5].kind == Tok::LBracket);
}

TEST(lexer_tilde_and_pipe) {
  DiagnosticBag diags;
  auto toks = lex("~x:1.0 |> f", "t", diags);
  CHECK(!diags.hasErrors());
  CHECK(toks[0].kind == Tok::Tilde);
  CHECK(toks[1].kind == Tok::Ident);
  CHECK(toks[2].kind == Tok::Colon);
  CHECK(toks[3].kind == Tok::Number);
  CHECK(toks[4].kind == Tok::PipeGt);
}

TEST(lexer_type_variables) {
  DiagnosticBag diags;
  auto toks = lex("~x:'a Signal : 'elem list", "t", diags);
  CHECK(!diags.hasErrors());
  CHECK(toks[0].kind == Tok::Tilde);
  CHECK(toks[3].kind == Tok::TypeVar);
  CHECK(toks[3].text == "a");
  CHECK(toks[4].kind == Tok::UpIdent);
  CHECK(toks[6].kind == Tok::TypeVar);
  CHECK(toks[6].text == "elem");
}

TEST(lexer_quote_still_continues_an_identifier) {
  // x' is one identifier; the quote only starts a token when it leads.
  DiagnosticBag diags;
  auto toks = lex("x' 'a", "t", diags);
  CHECK(!diags.hasErrors());
  CHECK(toks[0].kind == Tok::Ident);
  CHECK(toks[0].text == "x'");
  CHECK(toks[1].kind == Tok::TypeVar);
  CHECK(toks[1].text == "a");
}

TEST(lexer_bool_keywords_and_literals) {
  DiagnosticBag diags;
  auto toks = lex("if true then else false", "t", diags);
  CHECK(!diags.hasErrors());
  CHECK(toks[0].kind == Tok::If);
  CHECK(toks[1].kind == Tok::Bool);
  CHECK(toks[1].num == 1.0);
  CHECK(toks[2].kind == Tok::Then);
  CHECK(toks[3].kind == Tok::Else);
  CHECK(toks[4].kind == Tok::Bool);
  CHECK(toks[4].num == 0.0);
}

TEST(lexer_comparison_and_logic_operators) {
  DiagnosticBag diags;
  auto toks = lex("< <= > >= == != && || |> = |", "t", diags);
  CHECK(toks[0].kind == Tok::Lt);
  CHECK(toks[1].kind == Tok::Le);
  CHECK(toks[2].kind == Tok::Gt);
  CHECK(toks[3].kind == Tok::Ge);
  CHECK(toks[4].kind == Tok::EqEq);
  CHECK(toks[5].kind == Tok::BangEq);
  CHECK(toks[6].kind == Tok::AndAnd);
  CHECK(toks[7].kind == Tok::OrOr);
  CHECK(toks[8].kind == Tok::PipeGt);
  CHECK(toks[9].kind == Tok::Equals);
  // A lone '|' is still an error (only '|>' and '||' exist).
  CHECK(diags.hasErrors());
}

TEST(lexer_arrow_and_ge_disambiguation) {
  // '->' stays Arrow; '>=' is one token; '> =' is two.
  DiagnosticBag diags;
  auto toks = lex("-> >= > =", "t", diags);
  CHECK(!diags.hasErrors());
  CHECK(toks[0].kind == Tok::Arrow);
  CHECK(toks[1].kind == Tok::Ge);
  CHECK(toks[2].kind == Tok::Gt);
  CHECK(toks[3].kind == Tok::Equals);
}

TEST(lexer_int_vs_scalar_literals) {
  DiagnosticBag diags;
  auto toks = lex("42 42.0 0 007 42ms", "t", diags);
  CHECK(!diags.hasErrors());
  CHECK(toks[0].kind == Tok::IntNum);
  CHECK(toks[0].inum == 42);
  CHECK(toks[1].kind == Tok::Number);
  CHECK_NEAR(toks[1].num, 42.0, 1e-9);
  CHECK(toks[2].kind == Tok::IntNum);
  CHECK(toks[2].inum == 0);
  CHECK(toks[3].kind == Tok::IntNum);
  CHECK(toks[3].inum == 7);
  // A unit suffix still wins: 42ms is a Timestamp, not an Int.
  CHECK(toks[4].kind == Tok::Time);
  CHECK_NEAR(toks[4].num, 0.042, 1e-12);
}

TEST(lexer_int_literal_too_large) {
  DiagnosticBag diags;
  lex("99999999999999999999999", "t", diags);
  CHECK(diags.hasErrors());
}
