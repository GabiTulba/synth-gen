#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "checker.hpp"
#include "library.hpp"
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
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << src;
    return p.string();
  }
};

}  // namespace

TEST(checker_full_example_passes) {
  TempProject tp;
  std::string f = tp.write("pluck.synth", R"(
open Core
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
  mix_all (List.map place_pluck [0s; 500ms; 1s; 1500ms])
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
                           "open Core\nlet x : Scalar = sine 440.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_unknown_name) {
  TempProject tp;
  std::string f = tp.write("bad.synth", "open Core\nlet x : Scalar = nope ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_arity_error) {
  TempProject tp;
  std::string f =
      tp.write("bad.synth", "open Core\nlet x : Scalar Signal = sine 440.0 2.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_argument_type_error) {
  TempProject tp;
  // sine expects Scalar, given Timestamp.
  std::string f =
      tp.write("bad.synth", "open Core\nlet x : Scalar Signal = sine 1s ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_no_use_before_definition) {
  TempProject tp;
  std::string f = tp.write("bad.synth", R"(
open Core
let a : Scalar Signal = later ;;
let later : Scalar Signal = sine 440.0 ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_let_underscore_requires_unit) {
  TempProject tp;
  std::string f = tp.write("bad.synth", "open Core\nlet _ = sine 440.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_operator_broadcasting) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core
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
      tp.write("bad.synth", "open Core\nlet a : Scalar Signal = sine 440.0 + 1s ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_higher_order_map) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core
let up t:Timestamp : Scalar Signal = place (sample (sine 440.0) 0s 100ms) t ;;
let songs : Scalar Signal list = List.map up [0s; 1s] ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());
}

TEST(checker_fold) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core
let add a:Scalar b:Scalar : Scalar = a + b ;;
let total : Scalar = List.fold add 0.0 [1.0; 2.0; 3.0] ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());
}

TEST(checker_imports_and_qualified_access) {
  TempProject tp;
  tp.write("a.synth",
           "open Core\nlet f amp:Scalar freq:Scalar : Scalar Signal = "
           "sine freq * amp ;;");
  std::string b = tp.write("b.synth",
                           "open Core\nimport A\nlet g : Scalar Signal = A.f 0.8 440.0 ;;");
  DiagnosticBag diags;
  Program prog = checkProject({b}, diags);
  CHECK(!diags.hasErrors());
  CHECK(prog.modules.size() == 2);
  // Dependency order: A before B.
  CHECK(prog.modules[0].parsed.name == "A");
}

TEST(checker_unresolved_import) {
  TempProject tp;
  std::string b = tp.write("b.synth", "open Core\nimport Missing\n");
  DiagnosticBag diags;
  checkProject({b}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_import_cycle) {
  TempProject tp;
  tp.write("a.synth", "open Core\nimport B\nlet x : Scalar = 1.0 ;;");
  std::string b = tp.write("b.synth", "open Core\nimport A\nlet y : Scalar = 2.0 ;;");
  DiagnosticBag diags;
  checkProject({b}, diags);
  CHECK(diags.hasErrors());
}

namespace {

// A library tree fixture: writes a `Basic` library with one exposed and
// one internal module, discovers the registry, and returns it.
struct LibFixture {
  TempProject tp;
  LibraryRegistry reg;
  DiagnosticBag regDiags;
  LibFixture() {
    tp.write("lib/basic/.build",
             "library Basic\nexpose keys.synth\nsource internal.synth\n");
    tp.write("lib/basic/keys.synth",
             "open Core\nimport Internal\n"
             "let gain : Scalar = Internal.base * 2.0 ;;\n"
             "let strike freq:Scalar : Scalar Signal = sine freq * gain ;;\n");
    tp.write("lib/basic/internal.synth", "open Core\nlet base : Scalar = 0.25 ;;\n");
    reg = discoverLibraries(tp.dir.string(), regDiags);
  }
  ModuleLoadContext consumerCtx(std::vector<std::string> deps) {
    ModuleLoadContext ctx;
    ctx.registry = &reg;
    ctx.deps = std::move(deps);
    return ctx;
  }
};

}  // namespace

TEST(checker_import_library_qualified_access) {
  LibFixture fx;
  CHECK(!fx.regDiags.hasErrors());
  std::string song = fx.tp.write(
      "song.synth",
      "open Core\nimport Basic\nlet s : Scalar Signal = Basic.Keys.strike 440.0 ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx({"Basic"});
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags, &ctx);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
  // Canonical ids: the library file is Basic.Keys, its internal dep
  // Basic.Internal; the consumer stays flat.
  CHECK(prog.find("Basic.Keys") != nullptr);
  CHECK(prog.find("Basic.Internal") != nullptr);
  CHECK(prog.find("Song") != nullptr);
}

TEST(checker_import_library_file) {
  LibFixture fx;
  std::string song = fx.tp.write(
      "song.synth",
      "open Core\nimport Basic.Keys\n"
      "let s : Scalar Signal = Basic.Keys.strike 440.0 ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx({"Basic"});
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags, &ctx);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
}

TEST(checker_unexposed_file_import_error) {
  LibFixture fx;
  // Direct import of an internal module.
  std::string a = fx.tp.write(
      "a.synth",
      "open Core\nimport Basic.Internal\nlet x : Scalar = Basic.Internal.base ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx({"Basic"});
  DiagnosticBag d1;
  checkProject({a}, d1, &ctx);
  CHECK(d1.hasErrors());
  // Qualified access to an internal module under a whole-library import.
  std::string b = fx.tp.write(
      "b.synth",
      "open Core\nimport Basic\nlet x : Scalar = Basic.Internal.base ;;\n");
  DiagnosticBag d2;
  checkProject({b}, d2, &ctx);
  CHECK(d2.hasErrors());
}

TEST(checker_within_library_internal_import_ok) {
  LibFixture fx;
  const LibraryInfo* basic = fx.reg.find("Basic");
  CHECK(basic != nullptr);
  ModuleLoadContext ctx;
  ctx.registry = &fx.reg;
  ctx.currentLib = basic;
  std::vector<std::string> roots;
  for (auto& f : basic->files)
    roots.push_back((fs::path(basic->dir) / f).string());
  DiagnosticBag diags;
  Program prog = checkProject(roots, diags, &ctx);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
  CHECK(prog.find("Basic.Keys") != nullptr);
  CHECK(prog.find("Basic.Internal") != nullptr);
  // The library's own modules are not external.
  CHECK(!prog.find("Basic.Keys")->external);
}

TEST(checker_import_undeclared_dep_error) {
  LibFixture fx;
  std::string song = fx.tp.write(
      "song.synth",
      "open Core\nimport Basic\nlet s : Scalar Signal = Basic.Keys.strike 440.0 ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx({});  // no deps declared
  DiagnosticBag diags;
  checkProject({song}, diags, &ctx);
  CHECK(diags.hasErrors());
}

TEST(checker_local_file_shadows_library_name) {
  LibFixture fx;
  // A local basic.synth wins over the discovered library `Basic`.
  fx.tp.write("basic.synth", "open Core\nlet local : Scalar = 7.0 ;;\n");
  std::string song = fx.tp.write(
      "song.synth", "open Core\nimport Basic\nlet x : Scalar = Basic.local ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx({"Basic"});
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags, &ctx);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
  CHECK(prog.find("Basic") != nullptr);        // the flat local module
  CHECK(prog.find("Basic.Keys") == nullptr);   // the library was not loaded
}

TEST(checker_library_short_name_scoped_to_library) {
  // Two libraries with same-stem member files no longer alias: each gets
  // its own canonical id.
  TempProject tp;
  tp.write("a/.build", "library A\nexpose util.synth\n");
  tp.write("a/util.synth", "open Core\nlet ua : Scalar = 1.0 ;;\n");
  tp.write("b/.build", "library B\nexpose util.synth\n");
  tp.write("b/util.synth", "open Core\nlet ub : Scalar = 2.0 ;;\n");
  DiagnosticBag regDiags;
  LibraryRegistry reg = discoverLibraries(tp.dir.string(), regDiags);
  CHECK(!regDiags.hasErrors());
  std::string song = tp.write(
      "song.synth",
      "open Core\nimport A\nimport B\n"
      "let x : Scalar = A.Util.ua + B.Util.ub ;;\n");
  ModuleLoadContext ctx;
  ctx.registry = &reg;
  ctx.deps = {"A", "B"};
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags, &ctx);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
  CHECK(prog.find("A.Util") != nullptr);
  CHECK(prog.find("B.Util") != nullptr);
}

TEST(checker_cross_library_module_cycle_error) {
  // Module-level cycle across two libraries (registry built by hand so
  // the library-level dep validation doesn't mask the module cycle).
  TempProject tp;
  tp.write("a/x.synth", "open Core\nimport B.Y\nlet x : Scalar = 1.0 ;;\n");
  tp.write("b/y.synth", "open Core\nimport A.X\nlet y : Scalar = 2.0 ;;\n");
  LibraryRegistry reg;
  LibraryInfo la;
  la.name = "A";
  la.dir = (tp.dir / "a").string();
  la.files = {"x.synth"};
  la.exposedFiles = {"x.synth"};
  la.deps = {"B"};
  LibraryInfo lb;
  lb.name = "B";
  lb.dir = (tp.dir / "b").string();
  lb.files = {"y.synth"};
  lb.exposedFiles = {"y.synth"};
  lb.deps = {"A"};
  reg.byName.emplace("A", la);
  reg.byName.emplace("B", lb);
  std::string song =
      tp.write("song.synth", "open Core\nimport A.X\nlet s : Scalar = A.X.x ;;\n");
  ModuleLoadContext ctx;
  ctx.registry = &reg;
  ctx.deps = {"A", "B"};
  DiagnosticBag diags;
  checkProject({song}, diags, &ctx);
  CHECK(diags.hasErrors());
}

TEST(checker_open_library_brings_file_modules) {
  LibFixture fx;
  std::string song = fx.tp.write(
      "song.synth",
      "open Core\nopen Basic\nlet s : Scalar Signal = Keys.strike 440.0 ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx({"Basic"});
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags, &ctx);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
  CHECK(prog.find("Basic.Keys") != nullptr);
}

TEST(checker_open_file_unqualified_defs) {
  LibFixture fx;
  std::string song = fx.tp.write(
      "song.synth",
      "open Core\nopen Basic.Keys\nlet s : Scalar Signal = strike 440.0 * gain ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx({"Basic"});
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags, &ctx);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
}

TEST(checker_open_standalone_file) {
  // Same-directory files are openable too.
  TempProject tp;
  tp.write("instr.synth", "open Core\nlet tone freq:Scalar : Scalar Signal = sine freq ;;\n");
  std::string song = tp.write(
      "song.synth", "open Core\nopen Instr\nlet s : Scalar Signal = tone 440.0 ;;\n");
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
}

TEST(checker_open_shadowing_position_ordered) {
  TempProject tp;
  tp.write("instr.synth", "open Core\nlet gain : Scalar = 0.5 ;;\n");
  // A def before the open is shadowed by it; a def after the open
  // shadows it back; params always win.
  std::string song = tp.write("song.synth", R"(
open Core
let gain : Scalar = 1.0 ;;
let before : Scalar = gain ;;
open Instr
let after_open : Scalar = gain ;;
let gain2 : Scalar = 2.0 ;;
let with_param gain:Scalar : Scalar = gain ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
  // Re-defining `gain` after the open would be a duplicate-definition
  // error (own defs are one namespace); shadowing an open with a new
  // name works, and both binders type-check.
  std::string bad = tp.write("bad.synth", R"(
open Core
let gain : Scalar = 1.0 ;;
open Instr
let gain : Scalar = 2.0 ;;
)");
  DiagnosticBag d2;
  checkProject({bad}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_later_def_shadows_open) {
  TempProject tp;
  tp.write("instr.synth", "open Core\nlet tone : Scalar = 5.0 ;;\n");
  std::string song = tp.write("song.synth", R"(
open Core
open Instr
let opened : Scalar = tone ;;
let tone : Scalar Signal = sine 440.0 ;;
let own : Scalar Signal = tone ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  // `opened` sees Instr's Scalar tone; `own` sees the local Signal tone.
  CHECK(!diags.hasErrors());
  const CheckedModule* song_m = prog.find("Song");
  CHECK(song_m != nullptr);
  CHECK(typeEquals(song_m->defTypes.at("opened"), tScalar()));
  CHECK(typeEquals(song_m->defTypes.at("own"), tSignal(tScalar())));
}

TEST(checker_module_alias_binds_and_overrides) {
  LibFixture fx;
  std::string song = fx.tp.write("song.synth", R"(
open Core
import Basic.Keys
module Keys = Basic.Keys
module K2 = Keys
module B = Basic
let a : Scalar Signal = Keys.strike 440.0 ;;
let b : Scalar Signal = K2.strike 220.0 ;;
let c : Scalar Signal = B.Keys.strike 110.0 ;;
)");
  ModuleLoadContext ctx = fx.consumerCtx({"Basic"});
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags, &ctx);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
}

TEST(checker_open_unexposed_cross_library_error) {
  LibFixture fx;
  std::string song = fx.tp.write(
      "song.synth",
      "open Core\nopen Basic.Internal\nlet x : Scalar = base ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx({"Basic"});
  DiagnosticBag diags;
  checkProject({song}, diags, &ctx);
  CHECK(diags.hasErrors());
}

TEST(checker_core_strict_requires_open) {
  TempProject tp;
  // Bare primitives without `open Core` are unknown names...
  std::string f =
      tp.write("bad.synth", "let x : Scalar Signal = sine 440.0 ;;");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  bool hinted = false;
  for (auto& d : d1.items)
    if (d.message.find("open Core") != std::string::npos) hinted = true;
  CHECK(hinted);
  // ...and the list functions live under Core.List, not Core.
  std::string g = tp.write(
      "bad2.synth",
      "open Core\nlet xs : Scalar list = map (fun x:Scalar -> x) [1.0] ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_core_qualified_access) {
  TempProject tp;
  // Qualified Core access needs no open at all.
  std::string f = tp.write("ok.synth", R"(
let tone : Scalar Signal = Core.sine 440.0 ;;
let xs : Scalar list = Core.List.map (fun x:Scalar -> x * 2.0) [1.0] ;;
let ys : Scalar list = Core.List.init ~n:3.0 ~f:(fun i:Scalar -> i) ;;
let zs : Scalar list = Core.List.repeat 2.0 5.0 ;;
let t : Scalar = Core.List.fold (fun a:Scalar b:Scalar -> a + b) 0.0 zs ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  // Unknown Core member errors.
  std::string g =
      tp.write("bad.synth", "let x : Scalar = Core.frobnicate 1.0 ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_core_open_forms) {
  TempProject tp;
  // open Core: primitives bare, List submodule in scope.
  std::string f = tp.write("ok.synth", R"(
open Core
let tone : Scalar Signal = sine 440.0 ;;
let stack : Scalar Signal =
  mix_all (List.init ~n:3.0 ~f:(fun i:Scalar -> sine (110.0 * (i + 1.0)))) ;;
)");
  DiagnosticBag d1;
  Program p1 = checkProject({f}, d1);
  for (auto& d : d1.items)
    std::cerr << renderDiagnostic(d, p1.modules.empty()
                                         ? std::string{}
                                         : p1.modules[0].parsed.source);
  CHECK(!d1.hasErrors());
  // open Core.List: the list functions bare.
  std::string g = tp.write("ok2.synth", R"(
open Core
open Core.List
let xs : Scalar list = map (fun x:Scalar -> x + 1.0) (repeat 3.0 0.0) ;;
let t : Scalar = fold (fun a:Scalar b:Scalar -> a + b) 0.0 (init 3.0 (fun i:Scalar -> i)) ;;
)");
  DiagnosticBag d2;
  Program p2 = checkProject({g}, d2);
  for (auto& d : d2.items)
    std::cerr << renderDiagnostic(d, p2.modules.empty()
                                         ? std::string{}
                                         : p2.modules[0].parsed.source);
  CHECK(!d2.hasErrors());
}

TEST(checker_core_aliases_and_shadowing) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
module C = Core
module L = C.List
let a : Scalar Signal = C.sine 440.0 ;;
let xs : Scalar list = L.map (fun x:Scalar -> x) [1.0] ;;
open Core
let sine : Scalar = 7.0 ;;
let shadowed : Scalar = sine ;;
let still : Scalar Signal = Core.saw 110.0 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty()
                                         ? std::string{}
                                         : prog.modules[0].parsed.source);
  CHECK(!diags.hasErrors());
  const CheckedModule* m = prog.find("Ok");
  CHECK(m != nullptr);
  // The user's `sine` definition shadows the opened primitive.
  CHECK(typeEquals(m->defTypes.at("shadowed"), tScalar()));
}

TEST(checker_duplicate_definition) {
  TempProject tp;
  std::string f = tp.write("bad.synth", R"(
open Core
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
      tp.write("bad.synth", "open Core\nlet xs : Scalar list = [] ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_modulation_primitives) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core
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
      tp.write("bad.synth", "open Core\nlet x : Scalar Signal = fm 440.0 20.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());

  // am's element type propagates: declaring Scalar Signal for a Vector
  // carrier is an error.
  std::string g = tp.write("bad2.synth",
                           "open Core\nlet y : Scalar Signal = "
                           "am (channels [sine 1.0; sine 2.0]) (sine 4.0) 0.5 ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_delay_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core
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
                           "open Core\nlet x : Scalar Signal = delay 0.25 (sine 440.0) ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_reverb_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core
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
      "open Core\nlet x : Scalar Signal = reverb 0.8 0.4 0.3 (sine 440.0) ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_noise_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core
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
      tp.write("bad.synth", "open Core\nlet x : Scalar Signal = noise 1s ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_render_vis_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth",
                           "open Core\nlet _ = render_vis \"wave\" 1000.0 "
                           "(sample (sine 440.0) 0s 1s) ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());

  // render_vis produces unit, so it cannot be bound as a value.
  std::string g = tp.write("bad.synth",
                           "open Core\nlet x : Scalar = render_vis \"w\" 1000.0 "
                           "(sample (sine 440.0) 0s 1s) ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_clip_primitives) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core
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
      "bad.synth", "open Core\nlet x : Scalar Signal = hard_clip 1s (sine 440.0) ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_place_multi_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core
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
                           "open Core\nlet x : Scalar Signal = place_multi "
                           "(sample (sine 440.0) 0s 100ms) [0.0; 1.0] ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_labeled_args_any_order) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core
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
open Core
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
open Core
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
open Core
let xs : Scalar Signal list = List.map (lowpass ~cutoff:600.0) [saw 220.0; saw 110.0] ;;
let y : Scalar Signal = mix_all (List.map (lowpass ~cutoff:600.0) [saw 220.0]) ;;
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
open Core
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
  std::string g = tp.write("bad1.synth", "open Core\nlet x : Scalar = (1.0) 2.0 ;;");
  DiagnosticBag d1;
  checkProject({g}, d1);
  CHECK(d1.hasErrors());
  // Over-application through a computed callee still errors.
  std::string h = tp.write("bad2.synth", R"(
open Core
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
open Core
let hit : Scalar Sample = sample (sine 440.0) 0s 100ms ;;
let song : Scalar Signal =
  mix_all (List.map (fun t:Timestamp -> place hit t) [0s; 500ms; 1s]) ;;
let two : Scalar = (fun x:Scalar -> x + 1.0) 1.0 ;;
let curried : Scalar = ((fun a:Scalar b:Scalar -> a + b) 1.0) 2.0 ;;
let scaled base:Scalar : Scalar list =
  List.map (fun x:Scalar -> x * base) [1.0; 2.0] ;;
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
open Core
let gain : Scalar Signal = sine 2.0 ;;
let xs : Scalar list =
  let base : Scalar = 10.0 in
  List.map (fun gain:Scalar -> gain * base) [1.0; 2.0] ;;
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
open Core
let y : Scalar = ((fun x:Scalar -> x) 1.0) + x ;;
)");
  DiagnosticBag d1;
  checkProject({g}, d1);
  CHECK(d1.hasErrors());
  // Duplicate lambda params are rejected.
  std::string h = tp.write("bad2.synth", R"(
open Core
let f : Scalar -> Scalar -> Scalar = fun x:Scalar x:Scalar -> x ;;
)");
  DiagnosticBag d2;
  checkProject({h}, d2);
  CHECK(d2.hasErrors());
  // Body type errors surface.
  std::string k = tp.write("bad3.synth", R"(
open Core
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
      tp.write("bad1.synth", "open Core\nlet x : Scalar Signal = sine ~nope:440.0 ;;");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  // Same label twice.
  std::string g = tp.write(
      "bad2.synth", "open Core\nlet x : Scalar Signal = sine ~freq:440.0 ~freq:220.0 ;;");
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
open Core
let f x:Scalar ~y:Scalar : Scalar = x + y ;;
let g : Scalar -> Scalar = f ~y:1.0 ;;
let add a:Scalar b:Scalar : Scalar = a + b ;;
let inc : Scalar -> Scalar = add 1.0 ;;
let three : Scalar = inc 2.0 ;;
let sums : Scalar list = List.map (add 1.0) [1.0; 2.0] ;;
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
open Core
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
open Core
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
      "bad.synth", "open Core\nlet x : Scalar Signal = 1.0 |> lowpass ~cutoff:800.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_list_builders) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core
let harmonic i:Scalar : Scalar Signal = sine (110.0 * (i + 1.0)) ;;
let stack : Scalar Signal list = List.init 5.0 harmonic ;;
let fives : Scalar list = List.repeat 3.0 5.0 ;;
let beats : Timestamp list = time_steps ~start:0s ~step:250ms ~count:8.0 ;;
let sigs : Scalar Signal list = List.init ~n:4.0 ~f:sine ;;
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
open Core
let g t:Timestamp : Scalar Signal = sine 440.0 ;;
let xs : Scalar Signal list = List.init 3.0 g ;;
)");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  // time_steps count is a Scalar, not a Timestamp.
  std::string g = tp.write(
      "bad2.synth",
      "open Core\nlet xs : Timestamp list = time_steps 0s 250ms 1s ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_let_in_basic) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core
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
open Core
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
      "open Core\nlet x : Scalar = let y : Timestamp = 1.0 in 2.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_let_in_scope_ends_at_in) {
  TempProject tp;
  // `y` must not leak out of the let-in into a sibling expression.
  std::string f = tp.write("bad.synth", R"(
open Core
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
open Core
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
open Core
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
open Core
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
open Core
let s : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:100ms ;;
let _ = render_vis_stems ~name:"w" ~rate:8000.0 ~stems:[("a", s); ("b", s)] ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());
}
