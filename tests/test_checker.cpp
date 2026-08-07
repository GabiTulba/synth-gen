#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "checker.hpp"
#include "test_framework.hpp"

using namespace synth;
namespace fs = std::filesystem;

namespace {

// Writes source files into a fresh temp directory and runs checkProject.
struct TempProject {
  fs::path dir;
  TempProject() {
    static int counter = 0;
    dir = fs::temp_directory_path() /
          ("synthgraph-checker-test-" + std::to_string(::getpid()) + "-" +
           std::to_string(counter++));
    fs::create_directories(dir);
  }
  ~TempProject() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  std::string write(const std::string& name, const std::string& src) {
    fs::path p = dir / name;
    std::ofstream out(p);
    out << src;
    return p.string();
  }
};

}  // namespace

TEST(checker_full_example_passes) {
  TempProject tp;
  std::string f = tp.write("pluck.synth", R"(
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
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(prog.modules.size() == 1);
  const auto& types = prog.modules[0].defTypes;
  CHECK(typeEquals(types.at("song"), tSignal(tScalar())));
  CHECK(types.at("pluck")->kind == Type::Kind::Fun);
}

TEST(checker_type_mismatch) {
  TempProject tp;
  std::string f = tp.write("bad.synth",
                           "let x : Scalar = sine 440.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_unknown_name) {
  TempProject tp;
  std::string f = tp.write("bad.synth", "let x : Scalar = nope ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_arity_error) {
  TempProject tp;
  std::string f =
      tp.write("bad.synth", "let x : Scalar Signal = sine 440.0 2.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_argument_type_error) {
  TempProject tp;
  // sine expects Scalar, given Timestamp.
  std::string f =
      tp.write("bad.synth", "let x : Scalar Signal = sine 1s ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_no_use_before_definition) {
  TempProject tp;
  std::string f = tp.write("bad.synth", R"(
let a : Scalar Signal = later ;;
let later : Scalar Signal = sine 440.0 ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_let_underscore_requires_unit) {
  TempProject tp;
  std::string f = tp.write("bad.synth", "let _ = sine 440.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_operator_broadcasting) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let a : Scalar Signal = sine 440.0 * 0.5 ;;
let b : Scalar Signal = 0.5 * sine 440.0 ;;
let c : Scalar Signal = sine 440.0 + saw 220.0 ;;
let d : Scalar = 1.0 + 2.0 ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());
}

TEST(checker_operator_rejects_time_plus_signal) {
  TempProject tp;
  std::string f =
      tp.write("bad.synth", "let a : Scalar Signal = sine 440.0 + 1s ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_higher_order_map) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let up t:Timestamp : Scalar Signal = place (sample (sine 440.0) 0s 100ms) t ;;
let songs : Scalar Signal list = map up [0s; 1s] ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());
}

TEST(checker_fold) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let add a:Scalar b:Scalar : Scalar = a + b ;;
let total : Scalar = fold add 0.0 [1.0; 2.0; 3.0] ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());
}

TEST(checker_imports_and_qualified_access) {
  TempProject tp;
  tp.write("a.synth",
           "let f amp:Scalar freq:Scalar : Scalar Signal = "
           "sine freq * amp ;;");
  std::string b = tp.write("b.synth",
                           "import A\nlet g : Scalar Signal = A.f 0.8 440.0 ;;");
  DiagnosticBag diags;
  Program prog = checkProject({b}, diags);
  CHECK(!diags.hasErrors());
  CHECK(prog.modules.size() == 2);
  // Dependency order: A before B.
  CHECK(prog.modules[0].parsed.name == "A");
}

TEST(checker_unresolved_import) {
  TempProject tp;
  std::string b = tp.write("b.synth", "import Missing\n");
  DiagnosticBag diags;
  checkProject({b}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_import_cycle) {
  TempProject tp;
  tp.write("a.synth", "import B\nlet x : Scalar = 1.0 ;;");
  std::string b = tp.write("b.synth", "import A\nlet y : Scalar = 2.0 ;;");
  DiagnosticBag diags;
  checkProject({b}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_duplicate_definition) {
  TempProject tp;
  std::string f = tp.write("bad.synth", R"(
let x : Scalar = 1.0 ;;
let x : Scalar = 2.0 ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_empty_list_rejected) {
  TempProject tp;
  std::string f =
      tp.write("bad.synth", "let xs : Scalar list = [] ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_modulation_primitives) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let vibrato : Scalar Signal = fm 440.0 ((sine 5.0) * 20.0) ;;
let bell : Scalar Signal = pm 440.0 ((sine 220.0) * 3.0) ;;
let tremolo : Scalar Signal = am (sine 440.0) (sine 4.0) 0.5 ;;
let wide : Vector Signal =
  am (channels [sine 440.0; sine 442.0]) (sine 4.0) 0.5 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(prog.modules[0].defTypes.at("wide"), tSignal(tVector())));
}

TEST(checker_modulation_type_errors) {
  TempProject tp;
  // fm's modulator must be a Scalar Signal, not a Scalar.
  std::string f =
      tp.write("bad.synth", "let x : Scalar Signal = fm 440.0 20.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());

  // am's element type propagates: declaring Scalar Signal for a Vector
  // carrier is an error.
  std::string g = tp.write("bad2.synth",
                           "let y : Scalar Signal = "
                           "am (channels [sine 1.0; sine 2.0]) (sine 4.0) 0.5 ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_delay_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let dry : Scalar Signal = sine 440.0 ;;
let echo : Scalar Signal =
  mix_all [dry; (delay 250ms dry) * 0.5; (delay 500ms dry) * 0.25] ;;
let wide : Vector Signal = delay 10ms (channels [sine 440.0; sine 442.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(prog.modules[0].defTypes.at("wide"), tSignal(tVector())));

  // Delay time must be a Timestamp, not a Scalar.
  std::string g = tp.write("bad.synth",
                           "let x : Scalar Signal = delay 0.25 (sine 440.0) ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_reverb_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let dry : Scalar Signal = (sine 440.0) * (exp_decay 6.0) ;;
let wet : Scalar Signal = reverb 800ms 0.4 0.3 dry ;;
let hall : Vector Signal =
  reverb 2s 0.6 0.5 (channels [sine 440.0; sine 442.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(prog.modules[0].defTypes.at("hall"), tSignal(tVector())));

  // decay must be a Timestamp.
  std::string g = tp.write(
      "bad.synth",
      "let x : Scalar Signal = reverb 0.8 0.4 0.3 (sine 440.0) ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_noise_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let hiss : Scalar Signal = noise 4000.0 ;;
let snare : Scalar Signal = (noise 1800.0) * (exp_decay 25.0) ;;
let airy : Scalar Signal = reverb 300ms 0.5 0.4 snare ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());

  // noise takes a Scalar, not a Timestamp.
  std::string g =
      tp.write("bad.synth", "let x : Scalar Signal = noise 1s ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_render_vis_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth",
                           "let _ = render_vis \"wave\" 1000.0 "
                           "(sample (sine 440.0) 0s 1s) ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());

  // render_vis produces unit, so it cannot be bound as a value.
  std::string g = tp.write("bad.synth",
                           "let x : Scalar = render_vis \"w\" 1000.0 "
                           "(sample (sine 440.0) 0s 1s) ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_clip_primitives) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let crunchy : Scalar Signal = hard_clip 0.6 ((sine 220.0) * 2.0) ;;
let warm : Scalar Signal = soft_clip 0.8 ((saw 110.0) * 3.0) ;;
let wide : Vector Signal =
  soft_clip 0.5 (channels [sine 440.0; sine 442.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(prog.modules[0].defTypes.at("wide"), tSignal(tVector())));

  // threshold is a Scalar, not a Timestamp.
  std::string g = tp.write(
      "bad.synth", "let x : Scalar Signal = hard_clip 1s (sine 440.0) ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_place_multi_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let hit : Scalar Sample = sample ((sine 440.0) * (exp_decay 8.0)) 0s 200ms ;;
let pattern : Scalar Signal = place_multi hit [0s; 250ms; 500ms; 1s] ;;
let wide : Vector Signal =
  place_multi (sample (channels [sine 440.0; sine 442.0]) 0s 100ms) [0s; 1s] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(prog.modules[0].defTypes.at("wide"), tSignal(tVector())));

  // The list must be Timestamps, not Scalars.
  std::string g = tp.write("bad.synth",
                           "let x : Scalar Signal = place_multi "
                           "(sample (sine 440.0) 0s 100ms) [0.0; 1.0] ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_labeled_args_any_order) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let voice ~amp:Scalar ~freq:Scalar : Scalar Signal = (sine freq) * amp ;;
let a : Scalar Signal = voice ~amp:0.5 ~freq:440.0 ;;
let b : Scalar Signal = voice ~freq:440.0 ~amp:0.5 ;;
let c : Scalar Signal = voice 0.5 440.0 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_labeled_partial_application_curries) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let voice ~amp:Scalar ~freq:Scalar : Scalar Signal = (sine freq) * amp ;;
let half : Scalar -> Scalar Signal = voice ~amp:0.5 ;;
let tone : Scalar Signal = half 440.0 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  const TypePtr& half = prog.modules[0].defTypes.at("half");
  CHECK(half->kind == Type::Kind::Fun);
  CHECK(half->items.size() == 1);
}

TEST(checker_prim_labels_and_polymorphic_partial) {
  TempProject tp;
  // Primitives are callable by label, and a partial application of a
  // polymorphic primitive resolves its variables against the annotation.
  std::string f = tp.write("ok.synth", R"(
let a : Scalar Signal = sine ~freq:440.0 ;;
let b : unit = render ~rate:48000.0 ~name:"x" ~sample:(sample (sine 1.0) 0s 10ms) ;;
let damp : Scalar Signal -> Scalar Signal = lowpass ~cutoff:600.0 ;;
let c : Scalar Signal = damp (saw 220.0) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_polymorphic_partial_into_polymorphic_prim) {
  TempProject tp;
  // A var-carrying partial application flowing straight into another
  // polymorphic primitive, with no annotated binding in between: per-call
  // freshening plus two-sided unification must resolve the vars.
  std::string f = tp.write("ok.synth", R"(
let xs : Scalar Signal list = map (lowpass ~cutoff:600.0) [saw 220.0; saw 110.0] ;;
let y : Scalar Signal = mix_all (map (lowpass ~cutoff:600.0) [saw 220.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(prog.modules[0].defTypes.at("xs"),
                   tList(tSignal(tScalar()))));
}

TEST(checker_computed_callee) {
  TempProject tp;
  // Any Fun-typed expression can be applied, not just a name: nested
  // partial applications, polymorphic primitive partials with no
  // annotated binding in between, and function-typed parameters.
  std::string f = tp.write("ok.synth", R"(
let add a:Scalar b:Scalar : Scalar = a + b ;;
let three : Scalar = (add 1.0) 2.0 ;;
let damped : Scalar Signal = (lowpass ~cutoff:600.0) (saw 220.0) ;;
let twice f:(Scalar -> Scalar) x:Scalar : Scalar = f (f x) ;;
let two : Scalar = twice (add 1.0) 0.0 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(prog.modules[0].defTypes.at("three"), tScalar()));
  CHECK(typeEquals(prog.modules[0].defTypes.at("damped"),
                   tSignal(tScalar())));
  // A non-function expression still cannot be applied.
  std::string g = tp.write("bad1.synth", "let x : Scalar = (1.0) 2.0 ;;");
  DiagnosticBag d1;
  checkProject({g}, d1);
  CHECK(d1.hasErrors());
  // Over-application through a computed callee still errors.
  std::string h = tp.write("bad2.synth", R"(
let add a:Scalar b:Scalar : Scalar = a + b ;;
let x : Scalar = (add 1.0) 2.0 3.0 ;;
)");
  DiagnosticBag d2;
  checkProject({h}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_lambda) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let hit : Scalar Sample = sample (sine 440.0) 0s 100ms ;;
let song : Scalar Signal =
  mix_all (map (fun t:Timestamp -> place hit t) [0s; 500ms; 1s]) ;;
let two : Scalar = (fun x:Scalar -> x + 1.0) 1.0 ;;
let curried : Scalar = ((fun a:Scalar b:Scalar -> a + b) 1.0) 2.0 ;;
let scaled base:Scalar : Scalar list =
  map (fun x:Scalar -> x * base) [1.0; 2.0] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(prog.modules[0].defTypes.at("song"), tSignal(tScalar())));
}

TEST(checker_lambda_capture_and_shadowing) {
  TempProject tp;
  // A lambda body sees let...in locals; a lambda param shadows a
  // same-named top-level def (the param's type wins in the body).
  std::string f = tp.write("ok.synth", R"(
let gain : Scalar Signal = sine 2.0 ;;
let xs : Scalar list =
  let base : Scalar = 10.0 in
  map (fun gain:Scalar -> gain * base) [1.0; 2.0] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  // The param's scope ends at the lambda body.
  std::string g = tp.write("bad1.synth", R"(
let y : Scalar = ((fun x:Scalar -> x) 1.0) + x ;;
)");
  DiagnosticBag d1;
  checkProject({g}, d1);
  CHECK(d1.hasErrors());
  // Duplicate lambda params are rejected.
  std::string h = tp.write("bad2.synth", R"(
let f : Scalar -> Scalar -> Scalar = fun x:Scalar x:Scalar -> x ;;
)");
  DiagnosticBag d2;
  checkProject({h}, d2);
  CHECK(d2.hasErrors());
  // Body type errors surface.
  std::string k = tp.write("bad3.synth", R"(
let f : Scalar -> Scalar = fun x:Scalar -> x + "nope" ;;
)");
  DiagnosticBag d3;
  checkProject({k}, d3);
  CHECK(d3.hasErrors());
}

TEST(checker_label_errors) {
  TempProject tp;
  // Unknown label.
  std::string f =
      tp.write("bad1.synth", "let x : Scalar Signal = sine ~nope:440.0 ;;");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  // Same label twice.
  std::string g = tp.write(
      "bad2.synth", "let x : Scalar Signal = sine ~freq:440.0 ~freq:220.0 ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_positional_partial_application) {
  TempProject tp;
  // Unfilled positional parameters curry too: providing ~y leaves the
  // positional x as the curried function's parameter, and a positional
  // prefix of a user function curries the rest.
  std::string f = tp.write("ok.synth", R"(
let f x:Scalar ~y:Scalar : Scalar = x + y ;;
let g : Scalar -> Scalar = f ~y:1.0 ;;
let add a:Scalar b:Scalar : Scalar = a + b ;;
let inc : Scalar -> Scalar = add 1.0 ;;
let three : Scalar = inc 2.0 ;;
let sums : Scalar list = map (add 1.0) [1.0; 2.0] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  const TypePtr& g = prog.modules[0].defTypes.at("g");
  CHECK(g->kind == Type::Kind::Fun);
  CHECK(g->items.size() == 1);
  CHECK(typeEquals(g->items[0], tScalar()));
  // Over-application still errors.
  std::string h = tp.write("bad.synth", R"(
let add a:Scalar b:Scalar : Scalar = a + b ;;
let x : Scalar = add 1.0 2.0 3.0 ;;
)");
  DiagnosticBag d2;
  checkProject({h}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_pipe_typing) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let warm : Scalar Signal =
  saw 220.0 |> lowpass ~cutoff:800.0 |> soft_clip 0.8 ;;
let s : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:1s ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(prog.modules[0].defTypes.at("warm"), tSignal(tScalar())));
  CHECK(typeEquals(prog.modules[0].defTypes.at("s"), tSample(tScalar())));
}

TEST(checker_pipe_type_error_propagates) {
  TempProject tp;
  // Piping a Scalar into lowpass's Signal slot is a type error.
  std::string f = tp.write(
      "bad.synth", "let x : Scalar Signal = 1.0 |> lowpass ~cutoff:800.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_list_builders) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let harmonic i:Scalar : Scalar Signal = sine (110.0 * (i + 1.0)) ;;
let stack : Scalar Signal list = list_init 5.0 harmonic ;;
let fives : Scalar list = repeat 3.0 5.0 ;;
let beats : Timestamp list = time_steps ~start:0s ~step:250ms ~count:8.0 ;;
let sigs : Scalar Signal list = list_init ~n:4.0 ~f:sine ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(prog.modules[0].defTypes.at("stack"),
                   tList(tSignal(tScalar()))));
  CHECK(typeEquals(prog.modules[0].defTypes.at("beats"),
                   tList(tTimestamp())));
}

TEST(checker_list_builder_type_errors) {
  TempProject tp;
  // f must take a Scalar.
  std::string f = tp.write("bad.synth", R"(
let g t:Timestamp : Scalar Signal = sine 440.0 ;;
let xs : Scalar Signal list = list_init 3.0 g ;;
)");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  // time_steps count is a Scalar, not a Timestamp.
  std::string g = tp.write(
      "bad2.synth",
      "let xs : Timestamp list = time_steps 0s 250ms 1s ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_let_in_basic) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let song : Scalar Signal =
  let hit : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:100ms in
  let beats : Timestamp list = time_steps ~start:0s ~step:200ms ~count:5.0 in
  place_multi hit beats
;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(prog.modules[0].defTypes.at("song"), tSignal(tScalar())));
}

TEST(checker_let_in_shadowing_and_scope) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let x : Scalar = 1.0 ;;
let shadowed p:Scalar : Scalar =
  let x : Scalar = p + 10.0 in
  let p : Scalar = x * 2.0 in
  p + x
;;
let outer_still_scalar : Scalar = x + 1.0 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_let_in_annotation_mismatch) {
  TempProject tp;
  std::string f = tp.write(
      "bad.synth",
      "let x : Scalar = let y : Timestamp = 1.0 in 2.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_let_in_scope_ends_at_in) {
  TempProject tp;
  // `y` must not leak out of the let-in into a sibling expression.
  std::string f = tp.write("bad.synth", R"(
let a : Scalar = (let y : Scalar = 1.0 in y) + y ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_let_in_local_partial_application) {
  TempProject tp;
  // A label-curried polymorphic primitive bound locally, then called.
  std::string f = tp.write("ok.synth", R"(
let warm : Scalar Signal =
  let damp : Scalar Signal -> Scalar Signal = lowpass ~cutoff:600.0 in
  damp (saw 220.0)
;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_render_stems) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let a : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:100ms ;;
let b : Scalar Sample = saw 220.0 |> sample ~from:0s ~to:100ms ;;
let _ = render_stems ~name:"mix" ~rate:8000.0
                     ~stems:[("lead", a); ("bass", b)] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());

  // Stems must be (String, Sample) tuples.
  std::string g = tp.write("bad.synth", R"(
let _ = render_stems ~name:"mix" ~rate:8000.0
                     ~stems:[(1.0, sine 440.0 |> sample ~from:0s ~to:10ms)] ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_render_vis_stems) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
let s : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:100ms ;;
let _ = render_vis_stems ~name:"w" ~rate:8000.0 ~stems:[("a", s); ("b", s)] ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());
}
