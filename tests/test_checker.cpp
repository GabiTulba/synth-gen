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
