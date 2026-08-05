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
