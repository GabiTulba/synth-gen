#include "lexer.hpp"
#include "parser.hpp"
#include "test_framework.hpp"

using namespace synth;

namespace {
std::vector<TopDef> parseSrc(const std::string& src, DiagnosticBag& diags) {
  auto toks = lex(src, "t.synth", diags);
  return parse(toks, "t.synth", diags);
}
}  // namespace

TEST(parser_function_def) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let pluck freq:Scalar : Scalar Signal = (sine freq) * (exp_decay 6.0) ;;",
      diags);
  CHECK(!diags.hasErrors());
  CHECK(defs.size() == 1);
  CHECK(defs[0].kind == TopDef::Kind::Let);
  CHECK(defs[0].name == "pluck");
  CHECK(defs[0].params.size() == 1);
  CHECK(defs[0].params[0].name == "freq");
  CHECK(defs[0].params[0].type->kind == Type::Kind::Scalar);
  CHECK(defs[0].retType->kind == Type::Kind::Signal);
  CHECK(defs[0].retType->elem->kind == Type::Kind::Scalar);
  CHECK(defs[0].body->kind == Expr::Kind::BinOp);
  CHECK(defs[0].body->op == BinOpKind::Mul);
}

TEST(parser_full_example) {
  // The complete example from design doc §3.4.
  DiagnosticBag diags;
  auto defs = parseSrc(R"(
(* pluck.synth *)
let pluck freq:Scalar : Scalar Signal =
  (sine freq) * (exp_decay 6.0)
;;
let pluck_sample freq:Scalar : Scalar Sample =
  sample (pluck freq) 0s 800ms
;;
let place_pluck at:Timestamp : Scalar Signal =
  place (pluck_sample 440.0) at
;;
let song : Scalar Signal =
  mix_all (map place_pluck [0s; 500ms; 1s; 1500ms])
;;
let _ = render "demo" 48000.0 (sample song 0s 2s)
;;
)",
                       diags);
  CHECK(!diags.hasErrors());
  CHECK(defs.size() == 5);
  CHECK(defs[4].name == "_");
  CHECK(defs[3].name == "song");
  CHECK(defs[3].params.empty());
  // song body: mix_all applied to (map ...)
  CHECK(defs[3].body->kind == Expr::Kind::App);
  CHECK(defs[3].body->items[0]->name == "mix_all");
}

TEST(parser_import_and_qualified) {
  DiagnosticBag diags;
  auto defs = parseSrc("import A\nlet g : Scalar Signal = A.f 0.8 440.0 ;;",
                       diags);
  CHECK(!diags.hasErrors());
  CHECK(defs.size() == 2);
  CHECK(defs[0].kind == TopDef::Kind::Import);
  CHECK(defs[0].moduleName == "A");
  const Expr& body = *defs[1].body;
  CHECK(body.kind == Expr::Kind::App);
  CHECK(body.items[0]->moduleName == "A");
  CHECK(body.items[0]->name == "f");
}

TEST(parser_list_literal_semicolons) {
  DiagnosticBag diags;
  auto defs =
      parseSrc("let xs : Timestamp list = [0s; 500ms; 1s] ;;", diags);
  CHECK(!diags.hasErrors());
  CHECK(defs[0].body->kind == Expr::Kind::ListLit);
  CHECK(defs[0].body->items.size() == 3);
  CHECK(defs[0].retType->kind == Type::Kind::List);
}

TEST(parser_tuple_type_and_literal) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let two : (Scalar, Timestamp) = (1.0, 500ms) ;;", diags);
  CHECK(!diags.hasErrors());
  CHECK(defs[0].retType->kind == Type::Kind::Tuple);
  CHECK(defs[0].body->kind == Expr::Kind::TupleLit);
}

TEST(parser_function_typed_param) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let ap f:(Timestamp -> Scalar Signal) t:Timestamp : Scalar Signal = f t ;;",
      diags);
  CHECK(!diags.hasErrors());
  CHECK(defs[0].params[0].type->kind == Type::Kind::Fun);
  CHECK(defs[0].params[0].type->items.size() == 1);
  CHECK(defs[0].params[0].type->ret->kind == Type::Kind::Signal);
}

TEST(parser_precedence) {
  DiagnosticBag diags;
  auto defs = parseSrc("let x : Scalar = 1.0 + 2.0 * 3.0 ;;", diags);
  CHECK(!diags.hasErrors());
  const Expr& b = *defs[0].body;
  CHECK(b.kind == Expr::Kind::BinOp);
  CHECK(b.op == BinOpKind::Add);
  CHECK(b.items[1]->op == BinOpKind::Mul);
}

TEST(parser_application_binds_tighter_than_ops) {
  DiagnosticBag diags;
  auto defs = parseSrc("let x : Scalar Signal = sine 440.0 * 0.5 ;;", diags);
  CHECK(!diags.hasErrors());
  const Expr& b = *defs[0].body;
  CHECK(b.kind == Expr::Kind::BinOp);
  CHECK(b.items[0]->kind == Expr::Kind::App);
}

TEST(parser_error_recovery) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let bad : = 1.0 ;;\nlet good : Scalar = 2.0 ;;", diags);
  CHECK(diags.hasErrors());
  // Recovered and parsed the second definition.
  CHECK(defs.size() == 1);
  CHECK(defs[0].name == "good");
}

TEST(parser_module_name_from_path) {
  CHECK(moduleNameForPath("/x/y/pluck.synth") == "Pluck");
  CHECK(moduleNameForPath("a.synth") == "A");
}

TEST(parser_labeled_params_and_args) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let f ~amp:Scalar ~freq:Scalar : Scalar Signal = sine freq * amp ;;\n"
      "let g : Scalar Signal = f ~freq:440.0 ~amp:0.5 ;;",
      diags);
  CHECK(!diags.hasErrors());
  CHECK(defs[0].params[0].labeled);
  CHECK(defs[0].params[1].labeled);
  const Expr& call = *defs[1].body;
  CHECK(call.kind == Expr::Kind::App);
  CHECK(call.argLabels.size() == 2);
  CHECK(call.argLabels[0] == "freq");
  CHECK(call.argLabels[1] == "amp");
}

TEST(parser_pipe_desugars_to_application) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let x : Scalar Signal = saw 220.0 |> lowpass ~cutoff:800.0 ;;", diags);
  CHECK(!diags.hasErrors());
  const Expr& app = *defs[0].body;
  CHECK(app.kind == Expr::Kind::App);
  CHECK(app.items[0]->name == "lowpass");
  // args: ~cutoff:800.0 then the piped (saw 220.0) as final positional.
  CHECK(app.items.size() == 3);
  CHECK(app.argLabels[0] == "cutoff");
  CHECK(app.argLabels[1] == "");
  CHECK(app.items[2]->kind == Expr::Kind::App);
  CHECK(app.items[2]->items[0]->name == "saw");
}

TEST(parser_pipe_chains_left_associative) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let x : Scalar Signal = sine 440.0 |> f |> g ;;", diags);
  CHECK(!diags.hasErrors());
  const Expr& outer = *defs[0].body;
  CHECK(outer.items[0]->name == "g");
  CHECK(outer.items[1]->items[0]->name == "f");
}

TEST(parser_pipe_rejects_non_application_rhs) {
  DiagnosticBag diags;
  parseSrc("let x : Scalar = 1.0 |> 2.0 ;;", diags);
  CHECK(diags.hasErrors());
}

TEST(parser_import_dotted_path) {
  DiagnosticBag diags;
  auto defs = parseSrc("import Basic\nimport Basic.Keys ;;", diags);
  CHECK(!diags.hasErrors());
  CHECK(defs.size() == 2);
  CHECK(defs[0].kind == TopDef::Kind::Import);
  CHECK(defs[0].moduleName == "Basic");
  CHECK(defs[1].kind == TopDef::Kind::Import);
  CHECK(defs[1].moduleName == "Basic.Keys");
}

TEST(parser_open_single_and_dotted) {
  DiagnosticBag diags;
  auto defs = parseSrc("open Basic\nopen Basic.Keys ;;", diags);
  CHECK(!diags.hasErrors());
  CHECK(defs.size() == 2);
  CHECK(defs[0].kind == TopDef::Kind::Open);
  CHECK(defs[0].moduleName == "Basic");
  CHECK(defs[1].kind == TopDef::Kind::Open);
  CHECK(defs[1].moduleName == "Basic.Keys");
}

TEST(parser_module_alias) {
  DiagnosticBag diags;
  auto defs = parseSrc("module Keys = Basic.Keys ;;\nmodule B = Basic\n",
                       diags);
  CHECK(!diags.hasErrors());
  CHECK(defs.size() == 2);
  CHECK(defs[0].kind == TopDef::Kind::ModuleAlias);
  CHECK(defs[0].name == "Keys");
  CHECK(defs[0].moduleName == "Basic.Keys");
  CHECK(defs[1].name == "B");
  CHECK(defs[1].moduleName == "Basic");
  // Alias name must be capitalized (an UpIdent).
  DiagnosticBag bad;
  parseSrc("module keys = Basic.Keys ;;", bad);
  CHECK(bad.hasErrors());
}

TEST(parser_qualified_two_segment_ident) {
  DiagnosticBag diags;
  auto defs = parseSrc("let x : Scalar = Basic.Keys.gain + A.f 1.0 ;;",
                       diags);
  CHECK(!diags.hasErrors());
  const Expr& sum = *defs[0].body;
  CHECK(sum.kind == Expr::Kind::BinOp);
  const Expr& q = *sum.items[0];
  CHECK(q.kind == Expr::Kind::Ident);
  CHECK(q.moduleName == "Basic.Keys");
  CHECK(q.name == "gain");
  const Expr& app = *sum.items[1];
  CHECK(app.items[0]->moduleName == "A");
  CHECK(app.items[0]->name == "f");
}

TEST(parser_lambda) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let f : Scalar -> Scalar = fun x:Scalar -> x + 1.0 ;;\n"
      "let g : Scalar -> Scalar -> Scalar = fun a:Scalar ~b:Scalar -> a + b ;;",
      diags);
  CHECK(!diags.hasErrors());
  const Expr& lam = *defs[0].body;
  CHECK(lam.kind == Expr::Kind::Lambda);
  CHECK(lam.params.size() == 1);
  CHECK(lam.params[0].name == "x");
  CHECK(lam.params[0].type->kind == Type::Kind::Scalar);
  CHECK(!lam.params[0].labeled);
  CHECK(lam.items[0]->kind == Expr::Kind::BinOp);
  const Expr& lam2 = *defs[1].body;
  CHECK(lam2.params.size() == 2);
  CHECK(lam2.params[1].labeled);
  CHECK(lam2.params[1].name == "b");
}

TEST(parser_lambda_as_argument) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let xs : Scalar list = map (fun x:Scalar -> x * 2.0) [1.0; 2.0] ;;",
      diags);
  CHECK(!diags.hasErrors());
  const Expr& app = *defs[0].body;
  CHECK(app.kind == Expr::Kind::App);
  CHECK(app.items[0]->name == "map");
  CHECK(app.items[1]->kind == Expr::Kind::Lambda);
}

TEST(parser_lambda_in_pipe) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let x : Scalar = 1.0 |> (fun y:Scalar -> y + 1.0) ;;", diags);
  CHECK(!diags.hasErrors());
  // Desugars to App(lambda, [1.0]): piped value = final positional arg.
  const Expr& app = *defs[0].body;
  CHECK(app.kind == Expr::Kind::App);
  CHECK(app.items[0]->kind == Expr::Kind::Lambda);
  CHECK(app.items.size() == 2);
  CHECK(app.items[1]->kind == Expr::Kind::NumLit);
  // A bare lambda on the right of |> stays an error.
  DiagnosticBag bad;
  parseSrc("let x : Scalar = 1.0 |> fun y:Scalar -> y ;;", bad);
  CHECK(bad.hasErrors());
}

TEST(parser_lambda_errors) {
  DiagnosticBag d1;
  parseSrc("let f : Scalar -> Scalar = fun -> 1.0 ;;", d1);
  CHECK(d1.hasErrors());  // no parameters
  DiagnosticBag d2;
  parseSrc("let f : Scalar -> Scalar = fun x -> x ;;", d2);
  CHECK(d2.hasErrors());  // missing annotation
  DiagnosticBag d3;
  parseSrc("let f : Scalar -> Scalar = fun x:Scalar x ;;", d3);
  CHECK(d3.hasErrors());  // missing ->
}

TEST(parser_computed_callee) {
  DiagnosticBag diags;
  auto defs = parseSrc("let x : Scalar = (f 1.0) 2.0 ;;", diags);
  CHECK(!diags.hasErrors());
  const Expr& app = *defs[0].body;
  CHECK(app.kind == Expr::Kind::App);
  CHECK(app.items[0]->kind == Expr::Kind::App);
  CHECK(app.items[0]->items[0]->name == "f");
}

TEST(parser_let_in) {
  DiagnosticBag diags;
  auto defs = parseSrc(R"(
let song : Scalar Signal =
  let hit : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:100ms in
  let beats : Timestamp list = time_steps ~start:0s ~step:200ms ~count:5.0 in
  place_multi hit beats
;;
)",
                       diags);
  CHECK(!diags.hasErrors());
  const Expr& outer = *defs[0].body;
  CHECK(outer.kind == Expr::Kind::Let);
  CHECK(outer.name == "hit");
  CHECK(outer.declType->kind == Type::Kind::Sample);
  const Expr& inner = *outer.items[1];
  CHECK(inner.kind == Expr::Kind::Let);
  CHECK(inner.name == "beats");
  CHECK(inner.items[1]->kind == Expr::Kind::App);
}

TEST(parser_let_in_requires_annotation_and_in) {
  DiagnosticBag d1;
  parseSrc("let x : Scalar = let y = 1.0 in y ;;", d1);
  CHECK(d1.hasErrors());
  DiagnosticBag d2;
  parseSrc("let x : Scalar = let y : Scalar = 1.0 y ;;", d2);
  CHECK(d2.hasErrors());
}
