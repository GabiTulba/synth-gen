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

TEST(parser_type_variables) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let dampen ~input:'a Signal : 'a Signal = lowpass ~cutoff:600.0 input ;;",
      diags);
  CHECK(!diags.hasErrors());
  CHECK(defs.size() == 1);
  const TypePtr& param = defs[0].params[0].type;
  CHECK(param->kind == Type::Kind::Signal);
  CHECK(param->elem->kind == Type::Kind::Var);
  CHECK(param->elem->varName == "a");
  CHECK(isRigidVar(param->elem));
  // The same name in the return type is the same variable.
  CHECK(defs[0].retType->elem->var == param->elem->var);
}

TEST(parser_type_variables_are_scoped_per_definition) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let f ~x:'a : 'a = x ;;\n"
      "let g ~y:'a : 'a = y ;;",
      diags);
  CHECK(!diags.hasErrors());
  CHECK(defs.size() == 2);
  // Same spelling, different definitions: distinct variables.
  CHECK(defs[0].retType->var != defs[1].retType->var);
}

TEST(parser_type_variable_in_function_type_and_list) {
  DiagnosticBag diags;
  auto defs = parseSrc("let twice ~f:('a -> 'a) ~xs:'a list : 'a = f 1.0 ;;",
                       diags);
  CHECK(!diags.hasErrors());
  const TypePtr& f = defs[0].params[0].type;
  CHECK(f->kind == Type::Kind::Fun);
  CHECK(f->items[0]->kind == Type::Kind::Var);
  CHECK(f->ret->var == f->items[0]->var);
  CHECK(defs[0].params[1].type->kind == Type::Kind::List);
  CHECK(defs[0].params[1].type->elem->var == f->items[0]->var);
}

TEST(parser_inline_module) {
  DiagnosticBag diags;
  auto defs = parseSrc(R"(
module Voices = struct
  open Core
  let lead freq:Scalar : Scalar Signal = sine freq ;;
  module Fx = struct
    let gain : Scalar = 0.5 ;;
  end
end ;;
let after : Scalar = 1.0 ;;
)",
                       diags);
  CHECK(!diags.hasErrors());
  CHECK(defs.size() == 2);
  CHECK(defs[0].kind == TopDef::Kind::ModuleDef);
  CHECK(defs[0].name == "Voices");
  CHECK(defs[0].defs.size() == 3);
  CHECK(defs[0].defs[0].kind == TopDef::Kind::Open);
  CHECK(defs[0].defs[1].kind == TopDef::Kind::Let);
  CHECK(defs[0].defs[1].name == "lead");
  CHECK(defs[0].defs[2].kind == TopDef::Kind::ModuleDef);
  CHECK(defs[0].defs[2].name == "Fx");
  CHECK(defs[0].defs[2].defs.size() == 1);
  CHECK(defs[1].kind == TopDef::Kind::Let);
  CHECK(defs[1].name == "after");
}

TEST(parser_inline_module_end_without_semisemi) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "module A = struct let x : Scalar = 1.0 ;; end\n"
      "let y : Scalar = 2.0 ;;",
      diags);
  CHECK(!diags.hasErrors());
  CHECK(defs.size() == 2);
  CHECK(defs[0].kind == TopDef::Kind::ModuleDef);
}

TEST(parser_inline_module_rejects_import) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "module A = struct import Other ;; let x : Scalar = 1.0 ;; end ;;",
      diags);
  CHECK(diags.hasErrors());
  // Recovery keeps the rest of the body.
  CHECK(defs.size() == 1);
  CHECK(defs[0].defs.size() == 1);
  CHECK(defs[0].defs[0].name == "x");
}

TEST(parser_inline_module_rejects_nested_alias) {
  DiagnosticBag diags;
  parseSrc("module A = struct module K = Core end ;;", diags);
  CHECK(diags.hasErrors());
}

TEST(parser_inline_module_requires_end) {
  DiagnosticBag diags;
  parseSrc("module A = struct let x : Scalar = 1.0 ;;", diags);
  CHECK(diags.hasErrors());
}

TEST(parser_struct_and_end_stay_ordinary_identifiers) {
  // `struct` and `end` are contextual, so they still work as binding
  // names and expression references everywhere else.
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let end : Scalar = 1.0 ;;\n"
      "module A = struct let x : Scalar = end ;; end ;;",
      diags);
  CHECK(!diags.hasErrors());
  CHECK(defs.size() == 2);
  CHECK(defs[0].name == "end");
  CHECK(defs[1].defs.size() == 1);
  CHECK(defs[1].defs[0].body->kind == Expr::Kind::Ident);
  CHECK(defs[1].defs[0].body->name == "end");
}

TEST(parser_if_expression) {
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let x : Scalar = if a < b then 1.0 else 2.0 + 3.0 ;;", diags);
  CHECK(!diags.hasErrors());
  const ExprPtr& body = defs[0].body;
  CHECK(body->kind == Expr::Kind::If);
  CHECK(body->items.size() == 3);
  CHECK(body->items[0]->kind == Expr::Kind::BinOp);
  CHECK(body->items[0]->op == BinOpKind::Lt);
  CHECK(body->items[1]->kind == Expr::Kind::NumLit);
  // The else branch extends maximally: 2.0 + 3.0, not (2.0) + dangling.
  CHECK(body->items[2]->kind == Expr::Kind::BinOp);
  CHECK(body->items[2]->op == BinOpKind::Add);
}

TEST(parser_bool_literals_and_type) {
  DiagnosticBag diags;
  auto defs = parseSrc("let flag ~on:Bool : Bool = true ;;", diags);
  CHECK(!diags.hasErrors());
  CHECK(defs[0].params[0].type->kind == Type::Kind::Bool);
  CHECK(defs[0].retType->kind == Type::Kind::Bool);
  CHECK(defs[0].body->kind == Expr::Kind::BoolLit);
  CHECK(defs[0].body->num == 1.0);
}

TEST(parser_logic_precedence) {
  // a + 1.0 < b && c || d  ==>  (((a + 1.0) < b) && c) || d
  DiagnosticBag diags;
  auto defs = parseSrc("let x : Bool = a + 1.0 < b && c || d ;;", diags);
  CHECK(!diags.hasErrors());
  const ExprPtr& body = defs[0].body;
  CHECK(body->op == BinOpKind::Or);
  CHECK(body->items[0]->op == BinOpKind::And);
  CHECK(body->items[0]->items[0]->op == BinOpKind::Lt);
  CHECK(body->items[0]->items[0]->items[0]->op == BinOpKind::Add);
  CHECK(body->items[1]->kind == Expr::Kind::Ident);
}

TEST(parser_comparison_feeds_pipe) {
  // A comparison can be piped: a < b |> f  ==>  f (a < b).
  DiagnosticBag diags;
  auto defs = parseSrc("let x : Bool = a < b |> f ;;", diags);
  CHECK(!diags.hasErrors());
  const ExprPtr& body = defs[0].body;
  CHECK(body->kind == Expr::Kind::App);
  CHECK(body->items[0]->name == "f");
  CHECK(body->items[1]->op == BinOpKind::Lt);
}

TEST(parser_if_requires_else) {
  DiagnosticBag diags;
  parseSrc("let x : Scalar = if true then 1.0 ;;", diags);
  CHECK(diags.hasErrors());
}

TEST(parser_external_body) {
  DiagnosticBag diags;
  auto defs = parseSrc("let succ a:Scalar : Scalar = external \"succ.cpp\" ;;",
                       diags);
  CHECK(!diags.hasErrors());
  CHECK(defs.size() == 1);
  CHECK(defs[0].body->kind == Expr::Kind::External);
  CHECK(defs[0].body->str == "succ.cpp");
}

TEST(parser_external_is_contextual) {
  // `external` is an ordinary identifier everywhere but a full-body
  // `external "file"` position.
  DiagnosticBag diags;
  auto defs = parseSrc(
      "let external : Scalar = 1.0 ;;\n"
      "let x : Scalar = external ;;",
      diags);
  CHECK(!diags.hasErrors());
  CHECK(defs[0].name == "external");
  CHECK(defs[1].body->kind == Expr::Kind::Ident);
}

TEST(parser_int_type_and_literal) {
  DiagnosticBag diags;
  auto defs = parseSrc("let n : Int = 42 ;;", diags);
  CHECK(!diags.hasErrors());
  CHECK(defs[0].retType->kind == Type::Kind::Int);
  CHECK(defs[0].body->kind == Expr::Kind::IntLit);
  CHECK(defs[0].body->inum == 42);
}

TEST(parser_unary_minus_is_neg_node) {
  DiagnosticBag diags;
  auto defs = parseSrc("let n : Int = -5 ;;", diags);
  CHECK(!diags.hasErrors());
  const Expr& b = *defs[0].body;
  CHECK(b.kind == Expr::Kind::Neg);
  CHECK(b.items[0]->kind == Expr::Kind::IntLit);
  CHECK(b.items[0]->inum == 5);
}

TEST(parser_unary_minus_binds_tighter_than_mul) {
  DiagnosticBag diags;
  auto defs = parseSrc("let x : Scalar = -a * 2.0 ;;", diags);
  CHECK(!diags.hasErrors());
  const Expr& b = *defs[0].body;
  CHECK(b.kind == Expr::Kind::BinOp);
  CHECK(b.op == BinOpKind::Mul);
  CHECK(b.items[0]->kind == Expr::Kind::Neg);
}
