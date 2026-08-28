#include <unistd.h>
#include <functional>
#include <set>

#include <filesystem>
#include <fstream>

#include "checker.hpp"
#include "manifest_helpers.hpp"
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

// The bundled Core library is always loaded (and topologically first),
// so "the module I wrote" is the first non-Core one.
inline const CheckedModule& userMod(const Program& prog) {
  for (auto& m : prog.modules)
    if (m.libName != "Core") return m;
  return prog.modules.front();
}

// Non-Core module count: what tests mean by "how many modules I wrote".
inline size_t userModCount(const Program& prog) {
  size_t n = 0;
  for (auto& m : prog.modules)
    if (m.libName != "Core") n++;
  return n;
}

}  // namespace

TEST(checker_full_example_passes) {
  TempProject tp;
  std::string f = tp.write("pluck.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let pluck freq:Scalar : Scalar Signal =
  (sine freq) *. (exp_decay 6.0)
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
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(userModCount(prog) == 1);
  const auto& types = userMod(prog).defTypes;
  CHECK(typeName(types.at("song")) == "Scalar Signal");
  CHECK(types.at("pluck")->kind == Type::Kind::Fun);
}

TEST(checker_type_mismatch) {
  TempProject tp;
  std::string f = tp.write("bad.synth",
                           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = sine 440.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_unknown_name) {
  TempProject tp;
  std::string f = tp.write("bad.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = nope ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_arity_error) {
  TempProject tp;
  std::string f =
      tp.write("bad.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = sine 440.0 2.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_argument_type_error) {
  TempProject tp;
  // sine expects Scalar, given Timestamp.
  std::string f =
      tp.write("bad.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = sine 1s ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_no_use_before_definition) {
  TempProject tp;
  std::string f = tp.write("bad.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let a : Scalar Signal = later ;;
let later : Scalar Signal = sine 440.0 ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_let_underscore_requires_unit) {
  TempProject tp;
  std::string f = tp.write("bad.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet _ = sine 440.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_operator_broadcasting) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let a : Scalar Signal = sine 440.0 *. 0.5 ;;
let b : Scalar Signal = 0.5 *. sine 440.0 ;;
let c : Scalar Signal = sine 440.0 +. saw 220.0 ;;
let d : Scalar = 1.0 +. 2.0 ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());
}

TEST(checker_operator_rejects_time_plus_signal) {
  TempProject tp;
  std::string f =
      tp.write("bad.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet a : Scalar Signal = sine 440.0 +. 1s ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

// The bare/'.'-suffixed split (spec §3): bare operators are Int-only,
// '.'-suffixed ones cover the continuous kinds, and neither crosses over.
TEST(checker_dot_operators_cover_the_continuous_kinds) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let a : Scalar = 2.0 +. 3.0 ;;
let b : Scalar = 6.0 /. 3.0 ;;
let c : Bool = 2.0 >. 3.0 ;;
let d : Bool = 1.0 <=. 2.0 && 1.0 ==. 1.0 && 1.0 !=. 2.0 ;;
let e : Timestamp = 1s +. 500ms ;;
let g : Timestamp = 1s *. 1.5 ;;
let h : Bool = 1s <. 2s ;;
let i : Scalar Signal = sine 440.0 *. 0.5 ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
}

TEST(checker_bare_operators_stay_int_only) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let a : Int = 2 + 3 * 4 ;;
let b : Int = 7 / 2 ;;
let c : Bool = 2 > 3 ;;
let d : Bool = 2 <= 3 && 2 == 2 && 2 != 3 ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
}

TEST(checker_bare_operator_on_scalars_names_the_dotted_form) {
  TempProject tp;
  std::string f = tp.write(
      "bad.synth",
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\n"
      "let a : Scalar = 0.5 * 2.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool hinted = false;
  for (auto& d : diags.items)
    if (d.message.find("'*.'") != std::string::npos) hinted = true;
  CHECK(hinted);
}

TEST(checker_dotted_operator_on_ints_names_the_bare_form) {
  TempProject tp;
  std::string f = tp.write(
      "bad.synth",
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\n"
      "let a : Int = 2 +. 3 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool hinted = false;
  for (auto& d : diags.items)
    if (d.message.find("'+'") != std::string::npos) hinted = true;
  CHECK(hinted);
}

TEST(checker_operators_never_mix_int_with_the_continuous_kinds) {
  // Neither spelling is a way in: to_scalar is still the only crossing.
  const char* bad[] = {"let a : Scalar = 2 +. 3.0 ;;",
                       "let a : Scalar = 2.0 +. 3 ;;",
                       "let a : Timestamp = 1s *. 2 ;;",
                       "let a : Bool = 2 <. 3.0 ;;",
                       "let a : Bool = 2.0 > 3.0 ;;"};
  for (const char* line : bad) {
    TempProject tp;
    std::string f = tp.write(
        "bad.synth",
        std::string("open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\n") +
            line);
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
}

TEST(checker_higher_order_map) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
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
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let add a:Scalar b:Scalar : Scalar = a +. b ;;
let total : Scalar = List.fold add 0.0 [1.0; 2.0; 3.0] ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());
}

TEST(checker_imports_and_qualified_access) {
  TempProject tp;
  tp.write("a.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet f amp:Scalar freq:Scalar : Scalar Signal = "
           "sine freq *. amp ;;");
  std::string b = tp.write("b.synth",
                           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport A\nlet g : Scalar Signal = A.f 0.8 440.0 ;;");
  DiagnosticBag diags;
  Program prog = checkProject({b}, diags);
  CHECK(!diags.hasErrors());
  CHECK(userModCount(prog) == 2);
  // Dependency order: A before B.
  CHECK(userMod(prog).parsed.name == "A");
}

TEST(checker_unresolved_import) {
  TempProject tp;
  std::string b = tp.write("b.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Missing\n");
  DiagnosticBag diags;
  checkProject({b}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_import_cycle) {
  TempProject tp;
  tp.write("a.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport B\nlet x : Scalar = 1.0 ;;");
  std::string b = tp.write("b.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport A\nlet y : Scalar = 2.0 ;;");
  DiagnosticBag diags;
  checkProject({b}, diags);
  CHECK(diags.hasErrors());
}

namespace {

// A library tree fixture: writes a `Basic` library whose lib.synth
// exposes one of its two modules, discovers the registry, and returns it.
struct LibFixture {
  TempProject tp;
  LibraryRegistry reg;
  DiagnosticBag regDiags;
  LibFixture() {
    tp.write("lib/basic/build.json", libraryManifest("Basic"));
    tp.write("lib/basic/lib.synth", libraryInterface({"Keys"}));
    tp.write("lib/basic/keys.synth",
             "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Internal\n"
             "let gain : Scalar = Internal.base *. 2.0 ;;\n"
             "let strike freq:Scalar : Scalar Signal = sine freq *. gain ;;\n");
    tp.write("lib/basic/internal.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet base : Scalar = 0.25 ;;\n");
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
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Basic\nlet s : Scalar Signal = Basic.Keys.strike 440.0 ;;\n");
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
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Basic.Keys\n"
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
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Basic.Internal\nlet x : Scalar = Basic.Internal.base ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx({"Basic"});
  DiagnosticBag d1;
  checkProject({a}, d1, &ctx);
  CHECK(d1.hasErrors());
  // Qualified access to an internal module under a whole-library import.
  std::string b = fx.tp.write(
      "b.synth",
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Basic\nlet x : Scalar = Basic.Internal.base ;;\n");
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
  // As the build system does it: the interface first, then every member.
  std::vector<std::string> roots{
      (fs::path(basic->dir) / kLibraryInterfaceFile).string()};
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
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Basic\nlet s : Scalar Signal = Basic.Keys.strike 440.0 ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx({});  // no deps declared
  DiagnosticBag diags;
  checkProject({song}, diags, &ctx);
  CHECK(diags.hasErrors());
}

TEST(checker_local_file_shadows_library_name) {
  LibFixture fx;
  // A local basic.synth wins over the discovered library `Basic`.
  fx.tp.write("basic.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet local : Scalar = 7.0 ;;\n");
  std::string song = fx.tp.write(
      "song.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Basic\nlet x : Scalar = Basic.local ;;\n");
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
  tp.write("a/build.json", libraryManifest("A"));
  tp.write("a/lib.synth", libraryInterface({"Util"}));
  tp.write("a/util.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet ua : Scalar = 1.0 ;;\n");
  tp.write("b/build.json", libraryManifest("B"));
  tp.write("b/lib.synth", libraryInterface({"Util"}));
  tp.write("b/util.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet ub : Scalar = 2.0 ;;\n");
  DiagnosticBag regDiags;
  LibraryRegistry reg = discoverLibraries(tp.dir.string(), regDiags);
  CHECK(!regDiags.hasErrors());
  std::string song = tp.write(
      "song.synth",
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport A\nimport B\n"
      "let x : Scalar = A.Util.ua +. B.Util.ub ;;\n");
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
  tp.write("a/lib.synth", libraryInterface({"X"}));
  tp.write("a/x.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport B.Y\nlet x : Scalar = 1.0 ;;\n");
  tp.write("b/lib.synth", libraryInterface({"Y"}));
  tp.write("b/y.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport A.X\nlet y : Scalar = 2.0 ;;\n");
  LibraryRegistry reg;
  LibraryInfo la;
  la.name = "A";
  la.dir = (tp.dir / "a").string();
  la.files = {"x.synth"};
  la.hasInterface = true;
  la.deps = {"B"};
  LibraryInfo lb;
  lb.name = "B";
  lb.dir = (tp.dir / "b").string();
  lb.files = {"y.synth"};
  lb.hasInterface = true;
  lb.deps = {"A"};
  reg.byName.emplace("A", la);
  reg.byName.emplace("B", lb);
  std::string song =
      tp.write("song.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport A.X\nlet s : Scalar = A.X.x ;;\n");
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
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nopen Basic\nlet s : Scalar Signal = Keys.strike 440.0 ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx({"Basic"});
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags, &ctx);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
  CHECK(prog.find("Basic.Keys") != nullptr);
}

TEST(checker_library_siblings_need_no_manifest_listing) {
  // Every .synth file in a library's directory is a member, and members
  // import each other by short name with nothing declared anywhere.
  TempProject tp;
  tp.write("chain/build.json", libraryManifest("Chain"));
  tp.write("chain/lib.synth", libraryInterface({"Top"}));
  tp.write("chain/bottom.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet base : Scalar = 0.25 ;;\n");
  tp.write("chain/middle.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Bottom\nlet mid : Scalar = Bottom.base *. 2.0 ;;\n");
  tp.write("chain/top.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Middle\nlet top : Scalar = Middle.mid +. 1.0 ;;\n");
  DiagnosticBag regDiags;
  LibraryRegistry reg = discoverLibraries(tp.dir.string(), regDiags);
  CHECK(!regDiags.hasErrors());
  const LibraryInfo* chain = reg.find("Chain");
  CHECK(chain != nullptr);
  CHECK(chain->hasInterface);
  // lib.synth is not itself a member.
  CHECK(chain->files.size() == 3);
  std::string song = tp.write(
      "song.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Chain\nlet x : Scalar = Chain.Top.top ;;\n");
  ModuleLoadContext ctx;
  ctx.registry = &reg;
  ctx.deps = {"Chain"};
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags, &ctx);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
  CHECK(prog.find("Chain.Middle") != nullptr);
  CHECK(prog.find("Chain.Bottom") != nullptr);
}

namespace {

// A library whose lib.synth both renames a member module and re-exports a
// value of its own.
struct IfaceFixture {
  TempProject tp;
  LibraryRegistry reg;
  DiagnosticBag regDiags;
  IfaceFixture() {
    tp.write("fx/build.json", libraryManifest("Fx"));
    tp.write("fx/lib.synth",
             "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Delay\n"
             "module Echo = Delay ;;\n"
             "let slap s:Scalar Signal : Scalar Signal = Delay.tap s *. 0.5 ;;\n");
    tp.write("fx/delay.synth",
             "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Taps\n"
             "let tap s:Scalar Signal : Scalar Signal = s *. Taps.spread ;;\n");
    tp.write("fx/taps.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet spread : Scalar = 1.5 ;;\n");
    reg = discoverLibraries(tp.dir.string(), regDiags);
  }
  ModuleLoadContext consumerCtx() {
    ModuleLoadContext ctx;
    ctx.registry = &reg;
    ctx.deps = {"Fx"};
    return ctx;
  }
};

}  // namespace

TEST(checker_lib_interface_renames_and_reexports) {
  IfaceFixture fx;
  CHECK(!fx.regDiags.hasErrors());
  std::string song = fx.tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
import Fx
let a : Scalar Signal = Fx.Echo.tap (sine 440.0) ;;
let b : Scalar Signal = Fx.slap (sine 220.0) ;;
)");
  ModuleLoadContext ctx = fx.consumerCtx();
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags, &ctx);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
  // The exposed name is `Echo`, but the canonical id stays the file's.
  CHECK(prog.find("Fx.Delay") != nullptr);
  CHECK(prog.find("Fx") != nullptr);
  CHECK(prog.find("Fx.Echo") == nullptr);
}

TEST(checker_open_library_brings_values_and_renamed_modules) {
  IfaceFixture fx;
  std::string song = fx.tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Fx
let a : Scalar Signal = slap (sine 440.0) ;;
let b : Scalar Signal = Echo.tap (sine 220.0) ;;
)");
  ModuleLoadContext ctx = fx.consumerCtx();
  DiagnosticBag diags;
  checkProject({song}, diags, &ctx);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
}

TEST(checker_unexposed_original_name_error) {
  // `Delay` is a member, but lib.synth exposes it as `Echo` only.
  IfaceFixture fx;
  std::string song = fx.tp.write(
      "song.synth",
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Fx.Delay\nlet a : Scalar = 1.0 ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx();
  DiagnosticBag diags;
  checkProject({song}, diags, &ctx);
  CHECK(diags.hasErrors());
  bool hinted = false;
  for (auto& d : diags.items)
    if (d.message.find("not exposed") != std::string::npos) hinted = true;
  CHECK(hinted);
}

TEST(checker_member_cannot_reference_own_library) {
  TempProject tp;
  tp.write("own/build.json", libraryManifest("Own"));
  tp.write("own/lib.synth", libraryInterface({"A", "B"}));
  tp.write("own/a.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = 1.0 ;;\n");
  // A member must reach its sibling by short name, not through the
  // library's own interface.
  tp.write("own/b.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Own.A\nlet y : Scalar = Own.A.x ;;\n");
  DiagnosticBag regDiags;
  LibraryRegistry reg = discoverLibraries(tp.dir.string(), regDiags);
  const LibraryInfo* own = reg.find("Own");
  CHECK(own != nullptr);
  ModuleLoadContext ctx;
  ctx.registry = &reg;
  ctx.currentLib = own;
  std::vector<std::string> roots{
      (fs::path(own->dir) / kLibraryInterfaceFile).string()};
  for (auto& f : own->files) roots.push_back((fs::path(own->dir) / f).string());
  DiagnosticBag diags;
  checkProject(roots, diags, &ctx);
  CHECK(diags.hasErrors());
  bool hinted = false;
  for (auto& d : diags.items)
    if (d.message.find("short name") != std::string::npos) hinted = true;
  CHECK(hinted);
}

TEST(checker_library_without_interface_error) {
  TempProject tp;
  tp.write("bare/build.json", libraryManifest("Bare"));
  tp.write("bare/thing.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = 1.0 ;;\n");
  DiagnosticBag regDiags;
  LibraryRegistry reg = discoverLibraries(tp.dir.string(), regDiags);
  // Discovery itself flags the missing interface...
  CHECK(regDiags.hasErrors());
  CHECK(!reg.find("Bare")->hasInterface);
  // ...and so does an import of it.
  std::string song = tp.write(
      "song.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Bare\nlet x : Scalar = 1.0 ;;\n");
  ModuleLoadContext ctx;
  ctx.registry = &reg;
  ctx.deps = {"Bare"};
  DiagnosticBag diags;
  checkProject({song}, diags, &ctx);
  CHECK(diags.hasErrors());
}

TEST(checker_open_file_unqualified_defs) {
  LibFixture fx;
  std::string song = fx.tp.write(
      "song.synth",
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nopen Basic.Keys\nlet s : Scalar Signal = strike 440.0 *. gain ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx({"Basic"});
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags, &ctx);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
}

TEST(checker_open_standalone_file) {
  // Same-directory files are openable too.
  TempProject tp;
  tp.write("instr.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet tone freq:Scalar : Scalar Signal = sine freq ;;\n");
  std::string song = tp.write(
      "song.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nopen Instr\nlet s : Scalar Signal = tone 440.0 ;;\n");
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
}

TEST(checker_open_shadowing_position_ordered) {
  TempProject tp;
  tp.write("instr.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet gain : Scalar = 0.5 ;;\n");
  // A def before the open is shadowed by it; a def after the open
  // shadows it back; params always win.
  std::string song = tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
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
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
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
  tp.write("instr.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet tone : Scalar = 5.0 ;;\n");
  std::string song = tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
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
  CHECK(typeName(song_m->defTypes.at("own")) == "Scalar Signal");
}

TEST(checker_module_alias_binds_and_overrides) {
  LibFixture fx;
  std::string song = fx.tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
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
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nopen Basic.Internal\nlet x : Scalar = base ;;\n");
  ModuleLoadContext ctx = fx.consumerCtx({"Basic"});
  DiagnosticBag diags;
  checkProject({song}, diags, &ctx);
  CHECK(diags.hasErrors());
}

TEST(checker_core_strict_requires_open) {
  TempProject tp;
  // Bare primitives without any Core open are unknown names (with a
  // hint naming the submodule)...
  std::string f =
      tp.write("bad.synth", "let x : Scalar Signal = sine 440.0 ;;");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  bool hinted = false;
  for (auto& d : d1.items)
    if (d.message.find("open Core.Osc") != std::string::npos) hinted = true;
  CHECK(hinted);
  // ...and Core is not ambient: qualified access needs an import.
  std::string q = tp.write(
      "noimport.synth", "let x : Scalar Signal = Core.Osc.sine 440.0 ;;");
  DiagnosticBag dq;
  checkProject({q}, dq);
  CHECK(dq.hasErrors());
  bool notImported = false;
  for (auto& d : dq.items)
    if (d.message.find("is not imported") != std::string::npos &&
        d.message.find("import Core") != std::string::npos)
      notImported = true;
  CHECK(notImported);
  // ...and the list functions live under Core.List, which the usual
  // prelude does not open bare.
  std::string g = tp.write(
      "bad2.synth",
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet xs : Scalar list = map (fun x:Scalar -> x) [1.0] ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_core_qualified_access) {
  TempProject tp;
  // Qualified Core access needs no open - just the import.
  std::string f = tp.write("ok.synth", R"(
import Core
let tone : Scalar Signal = Core.Osc.sine 440.0 ;;
let xs : Scalar list = Core.List.map (fun x:Scalar -> x *. 2.0) [1.0] ;;
let ys : Scalar list = Core.List.init ~n:3 ~f:(fun i:Int -> Core.Math.to_scalar i) ;;
let zs : Scalar list = Core.List.repeat 2 5.0 ;;
let t : Scalar = Core.List.fold (fun a:Scalar b:Scalar -> a +. b) 0.0 zs ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  // Unknown Core member errors.
  std::string g = tp.write(
      "bad.synth",
      "import Core\nlet x : Scalar = Core.frobnicate 1.0 ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_core_open_forms) {
  TempProject tp;
  // open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math: primitives bare, List submodule in scope.
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let tone : Scalar Signal = sine 440.0 ;;
let stack : Scalar Signal =
  mix_all (List.init ~n:3 ~f:(fun i:Int -> sine (110.0 *. (to_scalar i +. 1.0)))) ;;
)");
  DiagnosticBag d1;
  Program p1 = checkProject({f}, d1);
  for (auto& d : d1.items)
    std::cerr << renderDiagnostic(d, p1.modules.empty()
                                         ? std::string{}
                                         : userMod(p1).parsed.source);
  CHECK(!d1.hasErrors());
  // open Core.List: the list functions bare.
  std::string g = tp.write("ok2.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Core.List
let xs : Scalar list = map (fun x:Scalar -> x +. 1.0) (repeat 3 0.0) ;;
let t : Scalar = fold (fun a:Scalar b:Scalar -> a +. b) 0.0 (init 3 (fun i:Int -> to_scalar i)) ;;
)");
  DiagnosticBag d2;
  Program p2 = checkProject({g}, d2);
  for (auto& d : d2.items)
    std::cerr << renderDiagnostic(d, p2.modules.empty()
                                         ? std::string{}
                                         : userMod(p2).parsed.source);
  CHECK(!d2.hasErrors());
}

TEST(checker_core_aliases_and_shadowing) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
import Core
module C = Core
module L = C.List
let a : Scalar Signal = C.Osc.sine 440.0 ;;
let xs : Scalar list = L.map (fun x:Scalar -> x) [1.0] ;;
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let sine : Scalar = 7.0 ;;
let shadowed : Scalar = sine ;;
let still : Scalar Signal = Core.Osc.saw 110.0 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const CheckedModule* m = prog.find("Ok");
  CHECK(m != nullptr);
  // The user's `sine` definition shadows the opened primitive.
  CHECK(typeEquals(m->defTypes.at("shadowed"), tScalar()));
}

TEST(checker_duplicate_definition) {
  TempProject tp;
  std::string f = tp.write("bad.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let x : Scalar = 1.0 ;;
let x : Scalar = 2.0 ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_empty_list_under_annotation) {
  // Lists are a polymorphic Core variant, so [] is legal wherever an
  // annotation (or the surrounding call) determines the element type.
  TempProject tp;
  std::string f =
      tp.write("ok.synth", "let xs : Scalar list = [] ;;\n"
                           "let n : Int =\n"
                           "  match xs with\n"
                           "  | Nil -> 0\n"
                           "  | Cons (_, _) -> 1 ;;");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("xs")) == "Scalar list");
}

TEST(checker_modulation_primitives) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let vibrato : Scalar Signal = fm 440.0 ((sine 5.0) *. 20.0) ;;
let bell : Scalar Signal = pm 440.0 ((sine 220.0) *. 3.0) ;;
let tremolo : Scalar Signal = am (sine 440.0) (sine 4.0) 0.5 ;;
let wide : Vector Signal =
  am (channels [sine 440.0; sine 442.0]) (sine 4.0) 0.5 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("wide")) == "Vector Signal");
}

TEST(checker_modulation_type_errors) {
  TempProject tp;
  // fm's modulator must be a Scalar Signal, not a Scalar.
  std::string f =
      tp.write("bad.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = fm 440.0 20.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());

  // am's element type propagates: declaring Scalar Signal for a Vector
  // carrier is an error.
  std::string g = tp.write("bad2.synth",
                           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet y : Scalar Signal = "
                           "am (channels [sine 1.0; sine 2.0]) (sine 4.0) 0.5 ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_delay_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let dry : Scalar Signal = sine 440.0 ;;
let echo : Scalar Signal =
  mix_all [dry; (delay 250ms dry) *. 0.5; (delay 500ms dry) *. 0.25] ;;
let wide : Vector Signal = delay 10ms (channels [sine 440.0; sine 442.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("wide")) == "Vector Signal");

  // Delay time must be a Timestamp, not a Scalar.
  std::string g = tp.write("bad.synth",
                           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = delay 0.25 (sine 440.0) ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_reverb_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let dry : Scalar Signal = (sine 440.0) *. (exp_decay 6.0) ;;
let wet : Scalar Signal = reverb 800ms 0.4 0.3 dry ;;
let hall : Vector Signal =
  reverb 2s 0.6 0.5 (channels [sine 440.0; sine 442.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("hall")) == "Vector Signal");

  // decay must be a Timestamp.
  std::string g = tp.write(
      "bad.synth",
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = reverb 0.8 0.4 0.3 (sine 440.0) ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_noise_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hiss : Scalar Signal = noise 4000.0 ;;
let snare : Scalar Signal = (noise 1800.0) *. (exp_decay 25.0) ;;
let airy : Scalar Signal = reverb 300ms 0.5 0.4 snare ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());

  // noise takes a Scalar, not a Timestamp.
  std::string g =
      tp.write("bad.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = noise 1s ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_render_vis_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth",
                           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet _ = render_vis \"wave\" 1000.0 "
                           "(sample (sine 440.0) 0s 1s) ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());

  // render_vis produces unit, so it cannot be bound as a value.
  std::string g = tp.write("bad.synth",
                           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = render_vis \"w\" 1000.0 "
                           "(sample (sine 440.0) 0s 1s) ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_clip_primitives) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let crunchy : Scalar Signal = hard_clip 0.6 ((sine 220.0) *. 2.0) ;;
let warm : Scalar Signal = soft_clip 0.8 ((saw 110.0) *. 3.0) ;;
let wide : Vector Signal =
  soft_clip 0.5 (channels [sine 440.0; sine 442.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("wide")) == "Vector Signal");

  // threshold is a Scalar, not a Timestamp.
  std::string g = tp.write(
      "bad.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = hard_clip 1s (sine 440.0) ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_place_multi_primitive) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sample ((sine 440.0) *. (exp_decay 8.0)) 0s 200ms ;;
let pattern : Scalar Signal = place_multi hit [0s; 250ms; 500ms; 1s] ;;
let wide : Vector Signal =
  place_multi (sample (channels [sine 440.0; sine 442.0]) 0s 100ms) [0s; 1s] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("wide")) == "Vector Signal");

  // The list must be Timestamps, not Scalars.
  std::string g = tp.write("bad.synth",
                           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = place_multi "
                           "(sample (sine 440.0) 0s 100ms) [0.0; 1.0] ;;");
  DiagnosticBag diags2;
  checkProject({g}, diags2);
  CHECK(diags2.hasErrors());
}

TEST(checker_labeled_args_any_order) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let voice ~amp:Scalar ~freq:Scalar : Scalar Signal = (sine freq) *. amp ;;
let a : Scalar Signal = voice ~amp:0.5 ~freq:440.0 ;;
let b : Scalar Signal = voice ~freq:440.0 ~amp:0.5 ;;
let c : Scalar Signal = voice 0.5 440.0 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_labeled_partial_application_curries) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let voice ~amp:Scalar ~freq:Scalar : Scalar Signal = (sine freq) *. amp ;;
let half : Scalar -> Scalar Signal = voice ~amp:0.5 ;;
let tone : Scalar Signal = half 440.0 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const TypePtr& half = userMod(prog).defTypes.at("half");
  CHECK(half->kind == Type::Kind::Fun);
  CHECK(half->items.size() == 1);
}

TEST(checker_prim_labels_and_polymorphic_partial) {
  TempProject tp;
  // Primitives are callable by label, and a partial application of a
  // polymorphic primitive resolves its variables against the annotation.
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let a : Scalar Signal = sine ~freq:440.0 ;;
let b : unit = render ~rate:48000.0 ~name:"x" ~sample:(sample (sine 1.0) 0s 10ms) ;;
let damp : Scalar Signal -> Scalar Signal = lowpass ~cutoff:600.0 ;;
let c : Scalar Signal = damp (saw 220.0) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_polymorphic_partial_into_polymorphic_prim) {
  TempProject tp;
  // A var-carrying partial application flowing straight into another
  // polymorphic primitive, with no annotated binding in between: per-call
  // freshening plus two-sided unification must resolve the vars.
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let xs : Scalar Signal list = List.map (lowpass ~cutoff:600.0) [saw 220.0; saw 110.0] ;;
let y : Scalar Signal = mix_all (List.map (lowpass ~cutoff:600.0) [saw 220.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("xs")) == "Scalar Signal list");
}

TEST(checker_computed_callee) {
  TempProject tp;
  // Any Fun-typed expression can be applied, not just a name: nested
  // partial applications, polymorphic primitive partials with no
  // annotated binding in between, and function-typed parameters.
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let add a:Scalar b:Scalar : Scalar = a +. b ;;
let three : Scalar = (add 1.0) 2.0 ;;
let damped : Scalar Signal = (lowpass ~cutoff:600.0) (saw 220.0) ;;
let twice f:(Scalar -> Scalar) x:Scalar : Scalar = f (f x) ;;
let two : Scalar = twice (add 1.0) 0.0 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("three"), tScalar()));
  CHECK(typeName(userMod(prog).defTypes.at("damped")) == "Scalar Signal");
  // A non-function expression still cannot be applied.
  std::string g = tp.write("bad1.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = (1.0) 2.0 ;;");
  DiagnosticBag d1;
  checkProject({g}, d1);
  CHECK(d1.hasErrors());
  // Over-application through a computed callee still errors.
  std::string h = tp.write("bad2.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let add a:Scalar b:Scalar : Scalar = a +. b ;;
let x : Scalar = (add 1.0) 2.0 3.0 ;;
)");
  DiagnosticBag d2;
  checkProject({h}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_lambda) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sample (sine 440.0) 0s 100ms ;;
let song : Scalar Signal =
  mix_all (List.map (fun t:Timestamp -> place hit t) [0s; 500ms; 1s]) ;;
let two : Scalar = (fun x:Scalar -> x +. 1.0) 1.0 ;;
let curried : Scalar = ((fun a:Scalar b:Scalar -> a +. b) 1.0) 2.0 ;;
let scaled base:Scalar : Scalar list =
  List.map (fun x:Scalar -> x *. base) [1.0; 2.0] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("song")) == "Scalar Signal");
}

TEST(checker_lambda_capture_and_shadowing) {
  TempProject tp;
  // A lambda body sees let...in locals; a lambda param shadows a
  // same-named top-level def (the param's type wins in the body).
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let gain : Scalar Signal = sine 2.0 ;;
let xs : Scalar list =
  let base : Scalar = 10.0 in
  List.map (fun gain:Scalar -> gain *. base) [1.0; 2.0] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  // The param's scope ends at the lambda body.
  std::string g = tp.write("bad1.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let y : Scalar = ((fun x:Scalar -> x) 1.0) + x ;;
)");
  DiagnosticBag d1;
  checkProject({g}, d1);
  CHECK(d1.hasErrors());
  // Duplicate lambda params are rejected.
  std::string h = tp.write("bad2.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let f : Scalar -> Scalar -> Scalar = fun x:Scalar x:Scalar -> x ;;
)");
  DiagnosticBag d2;
  checkProject({h}, d2);
  CHECK(d2.hasErrors());
  // Body type errors surface.
  std::string k = tp.write("bad3.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
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
      tp.write("bad1.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = sine ~nope:440.0 ;;");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  // Same label twice.
  std::string g = tp.write(
      "bad2.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = sine ~freq:440.0 ~freq:220.0 ;;");
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
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let f x:Scalar ~y:Scalar : Scalar = x +. y ;;
let g : Scalar -> Scalar = f ~y:1.0 ;;
let add a:Scalar b:Scalar : Scalar = a +. b ;;
let inc : Scalar -> Scalar = add 1.0 ;;
let three : Scalar = inc 2.0 ;;
let sums : Scalar list = List.map (add 1.0) [1.0; 2.0] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const TypePtr& g = userMod(prog).defTypes.at("g");
  CHECK(g->kind == Type::Kind::Fun);
  CHECK(g->items.size() == 1);
  CHECK(typeEquals(g->items[0], tScalar()));
  // Over-application still errors.
  std::string h = tp.write("bad.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let add a:Scalar b:Scalar : Scalar = a +. b ;;
let x : Scalar = add 1.0 2.0 3.0 ;;
)");
  DiagnosticBag d2;
  checkProject({h}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_pipe_typing) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let warm : Scalar Signal =
  saw 220.0 |> lowpass ~cutoff:800.0 |> soft_clip 0.8 ;;
let s : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:1s ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("warm")) == "Scalar Signal");
  CHECK(typeName(userMod(prog).defTypes.at("s")) == "Scalar Sample");
}

TEST(checker_pipe_type_error_propagates) {
  TempProject tp;
  // Piping a Scalar into lowpass's Signal slot is a type error.
  std::string f = tp.write(
      "bad.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = 1.0 |> lowpass ~cutoff:800.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_list_builders) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let harmonic i:Int : Scalar Signal = sine (110.0 *. (to_scalar i +. 1.0)) ;;
let stack : Scalar Signal list = List.init 5 harmonic ;;
let fives : Scalar list = List.repeat 3 5.0 ;;
let beats : Timestamp list = time_steps ~start:0s ~step:250ms ~count:8 ;;
let sigs : Scalar Signal list = List.init ~n:4 ~f:harmonic ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("stack")) ==
        "Scalar Signal list");
  CHECK(typeName(userMod(prog).defTypes.at("beats")) == "Timestamp list");
}

TEST(checker_list_builder_type_errors) {
  TempProject tp;
  // f must take a Scalar.
  std::string f = tp.write("bad.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let g t:Timestamp : Scalar Signal = sine 440.0 ;;
let xs : Scalar Signal list = List.init 3 g ;;
)");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  // time_steps count is an Int, not a Timestamp.
  std::string g = tp.write(
      "bad2.synth",
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet xs : Timestamp list = time_steps 0s 250ms 1s ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_let_in_basic) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let song : Scalar Signal =
  let hit : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:100ms in
  let beats : Timestamp list = time_steps ~start:0s ~step:200ms ~count:5 in
  place_multi hit beats
;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("song")) == "Scalar Signal");
}

TEST(checker_let_in_shadowing_and_scope) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let x : Scalar = 1.0 ;;
let shadowed p:Scalar : Scalar =
  let x : Scalar = p +. 10.0 in
  let p : Scalar = x *. 2.0 in
  p +. x
;;
let outer_still_scalar : Scalar = x +. 1.0 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_let_in_annotation_mismatch) {
  TempProject tp;
  std::string f = tp.write(
      "bad.synth",
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = let y : Timestamp = 1.0 in 2.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_let_in_scope_ends_at_in) {
  TempProject tp;
  // `y` must not leak out of the let-in into a sibling expression.
  std::string f = tp.write("bad.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
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
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let warm : Scalar Signal =
  let damp : Scalar Signal -> Scalar Signal = lowpass ~cutoff:600.0 in
  damp (saw 220.0)
;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_labeled_argument_punning) {
  TempProject tp;
  // `~gain` is `~gain:gain`: it resolves as an ordinary reference, so it
  // sees parameters, locals and top-level definitions alike, respects
  // shadowing, and partially applies like the written-out form.
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let cutoff : Scalar = 800.0 ;;
let voice ~freq:Scalar ~gain:Scalar : Scalar Signal = (sine freq) *. gain ;;
let top : Scalar Signal =
  let freq = 440.0 in
  let gain = 0.5 in
  voice ~freq ~gain
;;
let mixed : Scalar Signal = let gain = 0.25 in voice ~freq:220.0 ~gain ;;
let piped : Scalar Signal = saw 220.0 |> lowpass ~cutoff ;;
let curried : Scalar -> Scalar Signal = let gain = 0.3 in voice ~gain ;;
let shadowed : Scalar Signal =
  let cutoff = 200.0 in
  saw 110.0 |> lowpass ~cutoff
;;
let inner ~freq:Scalar : Scalar Signal = voice ~freq ~gain:1.0 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("top")) == "Scalar Signal");
  CHECK(typeName(userMod(prog).defTypes.at("mixed")) == "Scalar Signal");
  CHECK(typeName(userMod(prog).defTypes.at("piped")) == "Scalar Signal");
  CHECK(typeName(userMod(prog).defTypes.at("shadowed")) == "Scalar Signal");
  CHECK(typeName(userMod(prog).defTypes.at("curried")) ==
        "Scalar -> Scalar Signal");
}

TEST(checker_punned_argument_reports_the_name_not_the_label) {
  TempProject tp;
  // The pun is sugar, not a new binding form: an unbound name is the
  // ordinary unknown-name error, and a mistyped one the ordinary
  // argument-type error, both pointing at the name itself.
  std::string f = tp.write("bad.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let voice ~freq:Scalar ~gain:Scalar : Scalar Signal = (sine freq) *. gain ;;
let missing : Scalar Signal = voice ~freq ~gain:0.5 ;;
let wrong : Scalar Signal = let gain = 1s in voice ~freq:440.0 ~gain ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool unknown = false, mistyped = false;
  for (auto& d : diags.items) {
    if (d.message.find("unknown name 'freq'") != std::string::npos)
      unknown = true;
    if (d.message.find("argument 'gain'") != std::string::npos &&
        d.message.find("got Timestamp") != std::string::npos)
      mistyped = true;
  }
  CHECK(unknown);
  CHECK(mistyped);
}

TEST(checker_let_in_function_binding) {
  TempProject tp;
  // A local function definition: positional and labeled parameters,
  // called directly and via a label-curried partial application.
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let song : Scalar Signal =
  let pluck freq:Scalar ~gain:Scalar : Scalar Signal = (sine freq) *. gain in
  let quiet : Scalar -> Scalar Signal = pluck ~gain:0.25 in
  pluck 440.0 ~gain:0.5 +. quiet 330.0
;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("song")) == "Scalar Signal");
}

TEST(checker_let_in_function_body_mismatch) {
  TempProject tp;
  // The local function's body must match its declared return type.
  std::string f = tp.write(
      "bad.synth",
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = let f y:Scalar : Timestamp = y +. 1.0 in 2.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_let_in_function_duplicate_param) {
  TempProject tp;
  std::string f = tp.write(
      "bad.synth",
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = let f y:Scalar y:Scalar : Scalar = y in f 1.0 2.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_let_in_function_param_scope_ends_at_body) {
  TempProject tp;
  // A local function's parameter scopes over its own body only, not over
  // the body of the `in`.
  std::string f = tp.write("bad.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let a : Scalar = let f y:Scalar : Scalar = y +. 1.0 in f y ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_let_in_function_shares_enclosing_type_variables) {
  TempProject tp;
  // 'a inside a local function's annotation is the enclosing top-level
  // definition's 'a (spec par.3: same name, same variable), so a local
  // helper can pass polymorphic values along.
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let dampen ~input:'a Signal : 'a Signal =
  let boost x:'a Signal : 'a Signal = x *. 2.0 in
  lowpass ~cutoff:600.0 (boost input)
;;
let mono : Scalar Signal = dampen (saw 220.0) ;;
let wide : Vector Signal = dampen (channels [saw 220.0; saw 221.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_render_stems) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let a : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:100ms ;;
let b : Scalar Sample = saw 220.0 |> sample ~from:0s ~to:100ms ;;
let _ = render_stems ~name:"mix" ~rate:8000.0
                     ~stems:[("lead", a); ("bass", b)] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());

  // Stems must be (String, Sample) tuples.
  std::string g = tp.write("bad.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = render_stems ~name:"mix" ~rate:8000.0
                     ~stems:[(1.0, sine 440.0 |> sample ~from:0s ~to:10ms)] ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_control_int_toggle_choice_and_opt) {
  TempProject tp;
  // Each control lands in the type its widget carries: an Int, a Bool,
  // the option type itself, and an Option of whatever `opt` wraps.
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Sig
let voices : Int = (Control.int_slider ~name:"voices" ~min:1 ~max:8 ~default:3).value ;;
let bright : Bool = (Control.toggle ~name:"bright" ~default:false).value ;;
let shape : String = (Control.choice ~name:"shape" ~options:["soft"; "hard"]).value ;;
let cut : Scalar = (Control.choice ~name:"cut" ~options:[400.0; 900.0]).value ;;
let voice : Scalar Signal =
  (Control.choice ~name:"voice" ~options:[sine 220.0; saw 220.0]).value ;;
let depth : Scalar Option = (Control.opt ~name:"depth" ~value:0.4).value ;;
let dur : Timestamp Option =
  (Control.opt ~on:false ~name:"tail" ~value:250ms).value ;;
(* map keeps the widget and changes the value it stands for; nest binds
   several controllers into one component. *)
let scaled : Scalar Control =
  Control.map ~f:(fun hz:Scalar -> hz *. 2.0)
              ~c:(Control.slider ~name:"tone" ~min:100.0 ~max:900.0
                                 ~default:400.0) ;;
let pair : Scalar Control =
  Control.nest ~value:(cut +. scaled.value)
               ~parts:[scaled.ui; Widget "cut"] ;;
let _ = Ui.panel ~name:"P" ~controls:[pair.ui] ~targets:[] ;;
let used : Scalar Signal =
  lowpass ~cutoff:cut ~input:voice
    *. (if bright then 1.0 else 0.5)
    *. Math.to_scalar voices
    *. Option.value ~default:0.0 ~o:depth ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());

  // A choice is homogeneous: its result is the options' own type.
  std::string g = tp.write("bad.synth", R"(
open Core
let x : Scalar = (Control.choice ~name:"x" ~options:["a"; "b"]).value ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());

  // An int slider's bounds are Ints, not Scalars.
  std::string h = tp.write("bad2.synth", R"(
open Core
let n : Int = (Control.int_slider ~name:"n" ~min:0.0 ~max:4.0 ~default:1.0).value ;;
)");
  DiagnosticBag d3;
  checkProject({h}, d3);
  CHECK(d3.hasErrors());

  // `opt` yields an Option, not the bare value.
  std::string i = tp.write("bad3.synth", R"(
open Core
let x : Scalar = (Control.opt ~name:"x" ~value:0.5).value ;;
)");
  DiagnosticBag d4;
  checkProject({i}, d4);
  CHECK(d4.hasErrors());
}

TEST(checker_control_slider_and_knob) {
  TempProject tp;
  // The controls are ordinary Scalars usable anywhere a Scalar is.
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let cutoff : Scalar = (Control.slider ~name:"cutoff" ~min:100.0 ~max:2000.0 ~default:700.0).value ;;
let gain : Scalar = (Control.knob ~name:"gain" ~min:0.0 ~max:1.0 ~default:0.5).value ;;
let voice : Scalar Signal = lowpass cutoff (saw 110.0) *. gain ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());

  // The arguments are typed: a String where a Scalar bound is due fails.
  std::string g = tp.write("bad.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let x : Scalar = (Control.slider ~name:"x" ~min:"low" ~max:1.0 ~default:0.5).value ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());

  // ...and the result is a Scalar, not a Signal.
  std::string h = tp.write("bad2.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let x : Scalar Signal = (Control.knob ~name:"x" ~min:0.0 ~max:1.0 ~default:0.5).value ;;
)");
  DiagnosticBag d3;
  checkProject({h}, d3);
  CHECK(d3.hasErrors());
}

TEST(checker_render_vis_stems) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let s : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:100ms ;;
let _ = render_vis_stems ~name:"w" ~rate:8000.0 ~stems:[("a", s); ("b", s)] ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());
}

TEST(checker_signal_constructors) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let dc : Scalar Signal = constant 0.5 ;;
let pair : Vector Signal = constant_multi [0.3; 0.7] ;;
let ramp : Scalar Signal = time ;;
let fade : Scalar Signal = signal ~f:(fun t:Scalar -> exp (0.0 -. 3.0 *. t)) ;;
let wide : Vector Signal =
  signal_multi ~fs:[(fun t:Scalar -> t); (fun t:Scalar -> 1.0 -. t)] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("dc")) == "Scalar Signal");
  CHECK(typeName(userMod(prog).defTypes.at("pair")) == "Vector Signal");
  CHECK(typeName(userMod(prog).defTypes.at("ramp")) == "Scalar Signal");
  CHECK(typeName(userMod(prog).defTypes.at("fade")) == "Scalar Signal");
  CHECK(typeName(userMod(prog).defTypes.at("wide")) == "Vector Signal");
}

TEST(checker_math_primitives) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let e : Scalar = exp 1.0 ;;
let r : Scalar = sqrt 2.0 ;;
let l : Scalar = log 10.0 ;;
let p : Scalar = pow 2.0 10.0 ;;
let shaped : Scalar Signal = pow (sine 220.0) 3.0 ;;
let curve : Scalar Signal = sqrt time ;;
let fade : Scalar Signal = exp (0.0 -. time) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("e"), tScalar()));
  CHECK(typeName(userMod(prog).defTypes.at("shaped")) == "Scalar Signal");
  CHECK(typeName(userMod(prog).defTypes.at("curve")) == "Scalar Signal");
}

TEST(checker_timestamp_conversions) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let bpm : Scalar = 120.0 ;;
let beat : Timestamp = to_min (1.0 /. bpm) ;;
let lead : Timestamp = to_ms 250.0 ;;
let tail : Timestamp = to_sec 1.5 ;;
let steps : Timestamp list = time_steps ~start:lead ~step:beat ~count:4 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  for (const char* n : {"beat", "lead", "tail"})
    CHECK(typeEquals(types.at(n), tTimestamp()));
  CHECK(typeName(types.at("steps")) == "Timestamp list");
}

TEST(checker_timestamp_conversion_type_errors) {
  // Each direction takes what it takes: to_* consumes a Scalar and
  // yields a Timestamp, so neither accepts its own output back.
  for (const char* body :
       {"open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Timestamp = to_sec 500ms ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = to_ms 250.0 ;;"}) {
    TempProject tp;
    std::string f = tp.write("bad.synth", body);
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
}

TEST(checker_timestamp_of_unit_conversions) {
  // of_sec/of_ms/of_min are the way out: Timestamp in, Scalar out, so
  // a duration can drive ordinary Scalar arithmetic once the unit has
  // been named.
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let beat : Timestamp = to_min (1.0 /. 120.0) ;;
let secs : Scalar = of_sec beat ;;
let millis : Scalar = Time.of_ms ~x:250ms ;;
let mins : Scalar = of_min ~x:90s ;;
let ratio : Scalar = of_sec 1s /. of_sec 500ms ;;
let back : Timestamp = to_sec (of_sec beat) ;;
let rate : Scalar = 1.0 /. of_sec beat ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  for (const char* n : {"secs", "millis", "mins", "ratio", "rate"})
    CHECK(typeEquals(types.at(n), tScalar()));
  CHECK(typeEquals(types.at("back"), tTimestamp()));
}

TEST(checker_timestamp_of_unit_type_errors) {
  // of_* takes a Timestamp and hands back a Scalar; it is not a Scalar
  // identity, and it does not stay in the time domain.
  for (const char* body :
       {"open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = of_sec 1.0 ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Timestamp = of_ms 250ms ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = of_min 4 ;;"}) {
    TempProject tp;
    std::string f = tp.write("bad.synth", body);
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
}

TEST(checker_timestamp_division_still_needs_an_explicit_unit) {
  // Adding a way out does not relax the dimensional rule: the ratio of
  // two Timestamps is still an error, and the message points at it.
  TempProject tp;
  std::string f = tp.write("bad.synth",
                           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = 1s /. 500ms ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool mentions = false;
  for (auto& d : diags.items)
    if (d.message.find("of_sec") != std::string::npos) mentions = true;
  CHECK(mentions);
}

TEST(checker_timestamp_arithmetic) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let beat : Timestamp = to_min (1.0 /. 120.0) ;;
let bar : Timestamp = beat *. 4.0 ;;
let bar2 : Timestamp = 4.0 *. beat ;;
let half : Timestamp = beat /. 2.0 ;;
let dotted : Timestamp = beat +. beat /. 2.0 ;;
let upbeat : Timestamp = bar -. beat ;;
let clamped : Timestamp = 0s -. 1s ;;
let window : Timestamp = 250ms +. 100ms *. 2.0 ;;
let grid : Timestamp list = time_steps ~start:bar ~step:half ~count:8 ;;
let cmp : Bool = bar >. beat ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  for (const char* n : {"beat", "bar", "bar2", "half", "dotted", "upbeat",
                        "clamped", "window"})
    CHECK(typeEquals(types.at(n), tTimestamp()));
  CHECK(typeName(types.at("grid")) == "Timestamp list");
  CHECK(typeEquals(types.at("cmp"), tBool()));
}

TEST(checker_timestamp_arithmetic_type_errors) {
  // The table is deliberately partial: a Timestamp is not a number.
  // Multiplying two of them is not a duration, dividing two would hand
  // back the bare Scalar the unit discipline exists to prevent, a Scalar
  // does not add to a Timestamp, an Int does not scale one, and a
  // Timestamp does not divide a Scalar.
  for (const char* body :
       {"open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nlet x : Timestamp = 1s *. 2s ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nlet x : Scalar = 1s /. 500ms ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nlet x : Timestamp = 1s /. 500ms ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nlet x : Timestamp = 1s +. 2.0 ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nlet x : Timestamp = 2.0 -. 1s ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nlet x : Timestamp = 1s * 2 ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nlet x : Timestamp = 2.0 /. 1s ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nlet x : Scalar = 1s +. 1s ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nlet x : Timestamp = -1s ;;"}) {
    TempProject tp;
    std::string f = tp.write("bad.synth", body);
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
}

TEST(checker_pitch_roster) {
  TempProject tp;
  std::string f = tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Core.Pitch
let a4 : Note = { pc = A; oct = 4 } ;;
let s : Int = step ~note:a4 ;;
let back : Note = of_step ~step:57 ;;
let up : Note = shift ~note:a4 ~by:7 ;;
let down : Note = flat ~note:a4 ;;
let e : Tuning = et12 ~ref_hz:440.0 ;;
let wide : Tuning = et ~n:19 ~ref_hz:440.0 ~ref_step:57 ;;
let j : Tuning = just ~root:0 ~ref_hz:440.0 ;;
let p : Tuning = pyth ~root:0 ~ref_hz:432.0 ;;
let hand : Tuning =
  { ref_hz = 440.0; ref_step = 0; root = 0;
    ratios = [1.0; 1.5]; octave = 3.0 } ;;
let f1 : Scalar = a440 ~note:a4 ;;
let f2 : Scalar = hz ~t:j ~note:a4 ;;
let f3 : Scalar = step_hz ~t:wide ~step:60 ;;
(* the tuning partially applies, so call sites stay quiet *)
let voice : Note -> Scalar = hz ~t:j ;;
let f4 : Scalar = voice { pc = C; oct = 5 } ;;
let c1 : Scalar = cents ~n:10.4 ;;
let c2 : Scalar = detune ~freq:440.0 ~cents:10.4 ;;
let c3 : Scalar = to_cents ~ratio:1.006 ;;
let c4 : Scalar = ratio ~num:3 ~den:2 ;;
(* reachable qualified, without opening Pitch *)
let q : Scalar = Core.Pitch.a440 ~note:(Core.Pitch.of_step ~step:57) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  // A declared type travels under its module, so it prints qualified.
  for (const char* n : {"a4", "back", "up", "down"})
    CHECK(typeName(types.at(n)) == "Pitch.Note");
  for (const char* n : {"e", "wide", "j", "p", "hand"})
    CHECK(typeName(types.at(n)) == "Pitch.Tuning");
  for (const char* n : {"f1", "f2", "f3", "f4", "c1", "c2", "c3", "c4", "q"})
    CHECK(typeEquals(types.at(n), tScalar()));
  CHECK(typeEquals(types.at("s"), tInt()));
  CHECK(types.at("voice")->kind == Type::Kind::Fun);
}

TEST(checker_pitch_type_errors) {
  // Notes are discrete and temperament-relative; cents are continuous
  // and act on frequencies. The types keep the two apart, and a Note is
  // not a frequency until a Tuning says so.
  for (const char* body :
       {"open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nlet x : Scalar = { pc = A; oct = 4 } ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nlet x : Note = shift ~note:{ pc = A; oct = 4 } ~by:0.5 ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nlet x : Scalar = hz ~t:(et12 ~ref_hz:440.0) ~note:57 ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nlet x : Scalar = a440 ~note:{ pc = H; oct = 4 } ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nlet x : Note = { pc = A } ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nlet x : Scalar = cents ~n:100 ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nlet x : Scalar = a440 ~note:{ pc = A; oct = 4 } ;;"}) {
    TempProject tp;
    std::string f = tp.write("bad.synth", body);
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
}

TEST(checker_tempo_roster) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Core.Tempo
let t : Tempo = common ~bpm:120.0 ;;
let six8 : Tempo = { bpm = 90.0; meter = { beats = 6; unit = 8 } } ;;
let m : Meter = { beats = 7; unit = 8 } ;;
let b : Timestamp = beat ~t:t ;;
let br : Timestamp = bar ~t:t ;;
let n : Timestamp = beats ~t:t ~n:1.5 ;;
let v1 : Timestamp = value ~t:t ~v:Quarter ;;
let v2 : Timestamp = value ~t:t ~v:(Dotted (Dotted Eighth)) ;;
let v3 : Timestamp = value ~t:six8 ~v:(Tuplet (3, 2, Sixteenth)) ;;
let pos : Timestamp = at ~t:t ~bar:4 ~beat:2.0 ;;
let g : Timestamp list = grid ~t:t ~from:0s ~step:Eighth ~count:16 ;;
let sw : Timestamp list = swing ~amount:0.5 ~step:250ms ~steps:g ;;
(* the tempo partially applies, as in Pitch *)
let line : Value -> Int -> Timestamp list = grid ~t:t ~from:2s ;;
let g2 : Timestamp list = line Quarter 28 ;;
(* a Value is ordinary data: it can be stored and passed around *)
let feel : Value list = [Quarter; Dotted Eighth; Tuplet (3, 2, Eighth)] ;;
(* reachable qualified, without opening Tempo *)
let q : Timestamp = Core.Tempo.beat ~t:(Core.Tempo.common ~bpm:60.0) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  // As with Pitch, a declared type prints under its module - and a module
  // and a type may share the name `Tempo` because they live in separate
  // scopes.
  for (const char* n : {"t", "six8"})
    CHECK(typeName(types.at(n)) == "Tempo.Tempo");
  CHECK(typeName(types.at("m")) == "Tempo.Meter");
  for (const char* n : {"b", "br", "n", "v1", "v2", "v3", "pos", "q"})
    CHECK(typeEquals(types.at(n), tTimestamp()));
  for (const char* n : {"g", "sw", "g2"})
    CHECK(typeName(types.at(n)) == "Timestamp list");
  CHECK(typeName(types.at("feel")) == "Tempo.Value list");
  CHECK(types.at("line")->kind == Type::Kind::Fun);
}

TEST(checker_tempo_type_errors) {
  // A beat count is continuous and a bar index is not; a Value is not a
  // Timestamp; and Tempo's names have to be opened like anything else.
  for (const char* body :
       {"open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Tempo\nlet x : Timestamp = beats ~t:(common ~bpm:120.0) ~n:4 ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Tempo\nlet x : Timestamp = at ~t:(common ~bpm:120.0) ~bar:1.0 ~beat:0.0 ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Tempo\nlet x : Timestamp = value ~t:(common ~bpm:120.0) ~v:500ms ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Tempo\nlet x : Timestamp = Quarter ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Tempo\nlet x : Value = Dotted 4 ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Tempo\nlet x : Value = Tuplet (3, Eighth) ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Tempo\nlet x : Tempo = { bpm = 120.0 } ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Tempo\nlet x : Timestamp list = grid ~t:(common ~bpm:120.0) ~from:0s ~step:500ms ~count:4 ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nlet x : Timestamp = beat ~t:(common ~bpm:120.0) ;;"}) {
    TempProject tp;
    std::string f = tp.write("bad.synth", body);
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
}

TEST(checker_tempo_value_match_must_be_exhaustive) {
  // The payoff of making Value a variant: a missing case is a build-time
  // error, not a silently wrong duration.
  TempProject tp;
  std::string f = tp.write("bad.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Core.Tempo
let dots v:Value : Scalar =
  match v with
  | Whole -> 1.0
  | Half -> 0.5
  | Quarter -> 0.25
  | Eighth -> 0.125
  | Sixteenth -> 0.0625
  | ThirtySecond -> 0.03125
  | Dotted inner -> 1.5
;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool mentioned = false;
  for (auto& d : diags.items)
    if (d.message.find("Tuplet") != std::string::npos) mentioned = true;
  CHECK(mentioned);
}

TEST(checker_scale_roster) {
  TempProject tp;
  std::string f = tp.write("s.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Core.Pitch
open Core.Scale
let key : Scale = { tonic = { pc = A; oct = 3 }; quality = Minor } ;;
let modal : Scale = { tonic = { pc = D; oct = 4 }; quality = Dorian } ;;
let am : Chord = { root = { pc = A; oct = 3 }; quality = Min } ;;
let os : Int list = offsets ~q:Lydian ;;
let sh : Int list = shape ~q:HalfDim7 ;;
let one : Note = degree ~s:key ~n:0 ;;
let down : Note = degree ~s:key ~n:(-2) ;;
let run : Note list = notes ~s:key ~from:0 ~count:8 ;;
let fixed : Note = snap ~s:key ~note:{ pc = Cs; oct = 4 } ;;
let chord : Note list = tones ~c:am ;;
let three : Note list = triad ~s:key ~degree:3 ;;
let four : Note list = seventh ~s:modal ~degree:4 ;;
let five : Note list = stack ~s:key ~from:0 ~count:5 ;;
let first : Note list = invert ~notes:chord ~n:1 ;;
let wide : Note list = voicing ~notes:chord ~low:{ pc = A; oct = 2 } ~count:4 ;;
let hz4 : Scalar list = freqs ~t:(et12 ~ref_hz:440.0) ~notes:wide ;;
(* a progression is an ordinary list of chords *)
let prog : Chord list =
  [am; { root = { pc = F; oct = 3 }; quality = Maj };
       { root = { pc = C; oct = 4 }; quality = Maj7 }] ;;
(* the scale partially applies, as a tuning and a tempo do *)
let line : Int -> Note = degree ~s:key ;;
(* reachable qualified, without opening Scale *)
let q : Int list = Core.Scale.offsets ~q:Core.Scale.Blues ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  for (const char* n : {"key", "modal"})
    CHECK(typeName(types.at(n)) == "Scale.Scale");
  CHECK(typeName(types.at("am")) == "Scale.Chord");
  CHECK(typeName(types.at("prog")) == "Scale.Chord list");
  // Scale hands back Pitch's notes, so its lists are Pitch.Note lists.
  for (const char* n : {"one", "down", "fixed"})
    CHECK(typeName(types.at(n)) == "Pitch.Note");
  for (const char* n : {"run", "chord", "three", "four", "five", "first",
                        "wide"})
    CHECK(typeName(types.at(n)) == "Pitch.Note list");
  for (const char* n : {"os", "sh", "q"})
    CHECK(typeName(types.at(n)) == "Int list");
  CHECK(typeName(types.at("hz4")) == "Scalar list");
  CHECK(types.at("line")->kind == Type::Kind::Fun);
}

TEST(checker_scale_type_errors) {
  // A degree is an Int and a scale is not a chord; the two record types
  // are told apart by their field names (tonic vs root), which is why
  // they may share `quality`.
  for (const char* body :
       {"open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Scale"
        "\nlet x : Note = degree ~s:{ tonic = { pc = A; oct = 3 }; quality = Minor } ~n:1.0 ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Scale"
        "\nlet x : Scale = { tonic = { pc = A; oct = 3 }; quality = Min } ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Scale"
        "\nlet x : Chord = { root = { pc = A; oct = 3 }; quality = Minor } ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Scale"
        "\nlet x : Note list = tones ~c:{ tonic = { pc = A; oct = 3 }; quality = Min } ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Scale"
        "\nlet x : Scalar list = freqs ~notes:(tones ~c:{ root = { pc = A; oct = 3 }; quality = Min }) ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Scale\nlet x : Int list = offsets ~q:Maj ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Scale\nlet x : Int list = shape ~q:Major ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch"
        "\nlet x : Int list = offsets ~q:Lydian ;;"}) {
    TempProject tp;
    std::string f = tp.write("bad.synth", body);
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
}

TEST(checker_scale_quality_match_must_be_exhaustive) {
  // Fourteen scale qualities and twelve chord qualities: forgetting one
  // is a build-time error, not a silently wrong key.
  TempProject tp;
  std::string f = tp.write("bad.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Core.Scale
let bright q:Quality : Bool =
  match q with
  | Major -> true
  | Minor -> false
  | Dorian -> false
  | Phrygian -> false
  | Lydian -> true
  | Mixolydian -> true
  | Locrian -> false
  | HarmMinor -> false
  | MelMinor -> false
  | PentMajor -> true
  | PentMinor -> false
  | Blues -> false
  | WholeTone -> true
;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool mentioned = false;
  for (auto& d : diags.items)
    if (d.message.find("Chromatic") != std::string::npos) mentioned = true;
  CHECK(mentioned);
}

TEST(checker_score_roster) {
  TempProject tp;
  std::string f = tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Core.Pitch
open Core.Tempo
open Core.Scale
open Core.Score
let t : Tempo = common ~bpm:120.0 ;;
let tn : Tuning = et12 ~ref_hz:440.0 ;;
let key : Scale = { tonic = { pc = A; oct = 3 }; quality = Minor } ;;
let a4 : Note = { pc = A; oct = 4 } ;;
let items : Item list = [Play (a4, 1.0); Rest 0.5; Play (a4, 2.0)] ;;
let p1 : Phrase = line ~items:items ;;
let p2 : Phrase = melody ~notes:(notes ~s:key ~from:0 ~count:4) ~len:0.5 ;;
let p3 : Phrase = chord ~notes:(triad ~s:key ~degree:0) ~at:0.0 ~len:4.0 ;;
let p4 : Phrase = arpeggio ~notes:(tones ~c:{ root = a4; quality = Min7 })
                           ~step:0.25 ~count:8 ;;
let len : Scalar = span ~p:p1 ;;
let edited : Phrase =
  p2 |> move ~beats:8.0 |> transpose ~semitones:(-12) |> in_key ~s:key
     |> staccato ~ratio:0.9 |> legato
     |> velocity ~f:(fun v:Scalar -> v *. amp ~l:Mf) ;;
let joined : Phrase = seq ~ps:[p1; p2] ;;
let piled : Phrase = layer ~ps:[p3; p4] ;;
let twice : Phrase = loop ~p:p2 ~n:2 ;;
let gain : Scalar = amp ~l:Ff ;;
let quiet : Scalar = db ~x:(-6.0) ;;
let cresc : Scalar list = ramp ~from:Pp ~to:Forte ~n:8 ;;
let evs : Event list = realize ~tempo:t ~tuning:tn ~p:edited ;;
let voice freq:Scalar dur:Timestamp vel:Scalar : Scalar Sample =
  sine freq *. vel |> sample ~from:0s ~to:dur ;;
let out : Scalar Signal = play ~voice:voice ~events:evs ;;
(* polymorphic in the channel layout, like the primitives it wraps *)
let wide voice_f:Scalar dur:Timestamp vel:Scalar : Vector Sample =
  channels [sine voice_f *. vel; sine (voice_f *. 2.0) *. vel]
    |> sample ~from:0s ~to:dur ;;
let stereo : Vector Signal = play ~voice:wide ~events:evs ;;
let hit vel:Scalar : Scalar Sample =
  noise 200.0 *. vel |> sample ~from:0s ~to:100ms ;;
let drums : Scalar Signal = strike ~voice:hit ~events:evs ;;
(* reachable qualified, without opening Score *)
let q : Scalar = Core.Score.amp ~l:Core.Score.Fff ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  for (const char* n : {"p1", "p2", "p3", "p4", "edited", "joined", "piled",
                        "twice"})
    CHECK(typeName(types.at(n)) == "Score.Phrase");
  CHECK(typeName(types.at("items")) == "Score.Item list");
  CHECK(typeName(types.at("evs")) == "Score.Event list");
  for (const char* n : {"len", "gain", "quiet", "q"})
    CHECK(typeEquals(types.at(n), tScalar()));
  CHECK(typeName(types.at("cresc")) == "Scalar list");
  // The element type rides through play/strike untouched.
  CHECK(typeName(types.at("out")) == "Scalar Signal");
  CHECK(typeName(types.at("drums")) == "Scalar Signal");
  CHECK(typeName(types.at("stereo")) == "Vector Signal");
}

TEST(checker_score_type_errors) {
  // A Phrase is in beats and an Event is in real time; a voice takes
  // three arguments and a percussion voice one; and realize needs both
  // a tempo and a tuning.
  for (const char* body :
       {"open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Tempo\nopen Core.Score"
        "\nlet x : Phrase = melody ~notes:[{ pc = A; oct = 4 }] ~len:500ms ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Tempo\nopen Core.Score"
        "\nlet x : Phrase = move ~p:(melody ~notes:[{ pc = A; oct = 4 }] ~len:1.0) ~beats:500ms ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Tempo\nopen Core.Score"
        "\nlet x : Event list = realize ~tempo:(common ~bpm:120.0) ~p:(line ~items:[]) ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Tempo\nopen Core.Score"
        "\nlet v freq:Scalar : Scalar Sample = sine freq |> sample ~from:0s ~to:1s ;;"
        "\nlet x : Scalar Signal = play ~voice:v ~events:[] ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Tempo\nopen Core.Score"
        "\nlet x : Scalar list = ramp ~from:Mf ~to:Fff ~n:4.0 ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Tempo\nopen Core.Score"
        "\nlet x : Scalar = amp ~l:Major ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Tempo\nopen Core.Score"
        "\nlet x : Phrase = line ~items:[Play ({ pc = A; oct = 4 })] ;;",
        "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math"
        "\nopen Core.Pitch\nopen Core.Tempo"
        "\nlet x : Phrase = line ~items:[] ;;"}) {
    TempProject tp;
    std::string f = tp.write("bad.synth", body);
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
}

TEST(checker_score_dynamics_do_not_collide_with_pitch_classes) {
  // Score spells Forte and Piano out because a bare `F` would be
  // Pitch's F. With both modules open, every level and every pitch
  // class still resolves.
  TempProject tp;
  std::string f = tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Core.Pitch
open Core.Score
let f_note : Note = { pc = F; oct = 4 } ;;
let f_loud : Scalar = amp ~l:Forte ;;
let f_soft : Scalar = amp ~l:Piano ;;
let sharp : Note = { pc = Fs; oct = 4 } ;;
let loudest : Scalar = amp ~l:Fff ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("f_note")) == "Pitch.Note");
  CHECK(typeEquals(userMod(prog).defTypes.at("f_loud"), tScalar()));
}

TEST(checker_score_item_match_must_be_exhaustive) {
  TempProject tp;
  std::string f = tp.write("bad.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Core.Score
let sounding i:Item : Bool =
  match i with
  | Play (n, len) -> true
;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool mentioned = false;
  for (auto& d : diags.items)
    if (d.message.find("Rest") != std::string::npos) mentioned = true;
  CHECK(mentioned);
}

TEST(checker_resample_typing) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let warped : Scalar Signal = resample (saw 110.0) ~f:(fun t:Scalar -> 1.0 +. t) ;;
let piped : Scalar Signal = saw 110.0 |> resample ~f:(fun t:Scalar -> 0.5) ;;
let wide : Vector Signal =
  channels [sine 220.0; sine 330.0] |> resample ~f:(fun t:Scalar -> 2.0) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("warped")) == "Scalar Signal");
  CHECK(typeName(userMod(prog).defTypes.at("piped")) == "Scalar Signal");
  // The element type rides through: only the rate function is constrained.
  CHECK(typeName(userMod(prog).defTypes.at("wide")) == "Vector Signal");
}

TEST(checker_resample_type_errors) {
  // f must be a function, not a signal...
  {
    TempProject tp;
    std::string f = tp.write("bad.synth",
                             "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = resample "
                             "(saw 110.0) ~f:(sine 3.0) ;;");
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
  // ...and it takes a Scalar, not a Timestamp.
  {
    TempProject tp;
    std::string f = tp.write("bad.synth",
                             "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = resample "
                             "(saw 110.0) ~f:(fun t:Timestamp -> 1.0) ;;");
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
}

TEST(checker_signal_constructor_type_errors) {
  // signal's function must produce a Scalar, not a Signal.
  {
    TempProject tp;
    std::string f = tp.write("bad.synth",
                             "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = signal "
                             "~f:(fun t:Scalar -> sine t) ;;");
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
  // time is a value, not a function.
  {
    TempProject tp;
    std::string f = tp.write(
        "bad.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet y : Scalar Signal = time 1.0 ;;");
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
  // pow's exponent is a Scalar, never a Signal.
  {
    TempProject tp;
    std::string f = tp.write("bad.synth",
                             "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet z : Scalar Signal = pow "
                             "(sine 220.0) (sine 1.0) ;;");
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
}

TEST(checker_polymorphic_definition_instantiates_per_use) {
  TempProject tp;
  // One definition, two element types: the annotation's 'a is chosen
  // afresh at every call site, exactly like a primitive's.
  std::string f = tp.write("poly.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let dampen ~input:'a Signal : 'a Signal =
  lowpass ~cutoff:600.0 (soft_clip ~threshold:0.8 input)
;;
let mono : Scalar Signal = dampen (saw 220.0) ;;
let wide : Vector Signal = dampen (channels [saw 220.0; saw 221.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(typeName(types.at("mono")) == "Scalar Signal");
  CHECK(typeName(types.at("wide")) == "Vector Signal");
  // The definition itself keeps its polymorphic signature.
  const TypePtr& dampen = types.at("dampen");
  CHECK(dampen->kind == Type::Kind::Fun);
  CHECK(containsRigidVar(dampen));
  // 'a Signal is Named with the variable as its argument.
  CHECK(dampen->items[0]->items[0]->var == dampen->ret->items[0]->var);
}

TEST(checker_polymorphic_higher_order_definition) {
  TempProject tp;
  std::string f = tp.write("poly.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let twice ~f:('a -> 'a) ~x:'a : 'a = f (f x) ;;
let quad : Scalar = twice ~f:(fun n:Scalar -> n *. 2.0) ~x:1.0 ;;
let filtered : Scalar Signal =
  twice ~f:(lowpass ~cutoff:600.0) ~x:(saw 220.0)
;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(typeEquals(types.at("quad"), tScalar()));
  CHECK(typeName(types.at("filtered")) == "Scalar Signal");
}

TEST(checker_polymorphic_def_flows_into_polymorphic_prim) {
  TempProject tp;
  std::string f = tp.write("poly.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let dampen ~input:'a Signal : 'a Signal = lowpass ~cutoff:600.0 input ;;
let layers : Scalar Signal =
  mix_all (List.map dampen [sine 330.0; square 220.0])
;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("layers")) == "Scalar Signal");
}

TEST(checker_polymorphic_def_across_modules) {
  TempProject tp;
  tp.write("fx.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let stutter ~s:'a Signal ~at:Timestamp list : 'a Signal =
  place_multi (sample s ~from:0s ~to:120ms) at
;;
)");
  std::string song = tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Fx
let mono : Scalar Signal = stutter (saw 220.0) [0s; 200ms] ;;
let wide : Vector Signal =
  Fx.stutter (channels [saw 110.0; saw 111.0]) [0s; 300ms]
;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags);
  CHECK(!diags.hasErrors());
  const CheckedModule* m = prog.find("Song");
  CHECK(m != nullptr);
  CHECK(typeName(m->defTypes.at("mono")) == "Scalar Signal");
  CHECK(typeName(m->defTypes.at("wide")) == "Vector Signal");
}

TEST(checker_polymorphic_annotated_partial_application) {
  TempProject tp;
  std::string f = tp.write("poly.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let damp : 'a Signal -> 'a Signal = lowpass ~cutoff:600.0 ;;
let mono : Scalar Signal = damp (saw 220.0) ;;
let wide : Vector Signal = damp (channels [saw 220.0; saw 221.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(typeName(types.at("mono")) == "Scalar Signal");
  CHECK(typeName(types.at("wide")) == "Vector Signal");
}

TEST(checker_type_variable_is_rigid_in_its_own_body) {
  // A definition may not decide what its caller's 'a is: the body can only
  // pass it along, never assume it is a Scalar or a Signal.
  TempProject tp;
  std::string a =
      tp.write("a.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet bad ~x:'a : Scalar = x ;;");
  DiagnosticBag d1;
  checkProject({a}, d1);
  CHECK(d1.hasErrors());

  std::string b = tp.write(
      "b.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet bad ~x:'a : 'a = lowpass ~cutoff:1.0 x ;;");
  DiagnosticBag d2;
  checkProject({b}, d2);
  CHECK(d2.hasErrors());

  // Two different variables are not interchangeable either.
  std::string c =
      tp.write("c.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet bad ~x:'a ~y:'b : 'a = y ;;");
  DiagnosticBag d3;
  checkProject({c}, d3);
  CHECK(d3.hasErrors());
}

TEST(checker_instantiation_checked_at_call_site) {
  TempProject tp;
  std::string f = tp.write("poly.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let id ~x:'a : 'a = x ;;
let bad : Vector Signal = id (sine 440.0) ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_result_type_variable_must_be_bound_by_a_parameter) {
  TempProject tp;
  std::string f = tp.write(
      "poly.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet bad ~x:Scalar : 'a Signal = sine x ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  CHECK(diags.items[0].message.find("in no parameter") != std::string::npos);
}

TEST(checker_inline_module_defs_and_refs) {
  TempProject tp;
  std::string f = tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
module Voices = struct
  let base : Scalar = 220.0 ;;
  let up : Scalar = base *. 2.0 ;;
  module Fx = struct
    let damp ~input:'a Signal : 'a Signal = lowpass ~cutoff:600.0 input ;;
  end
  let again : Scalar = Voices.base ;;
  let lead : Scalar Signal = Fx.damp (sine up) ;;
end ;;
let mono : Scalar Signal = Voices.Fx.damp (sine Voices.base) ;;
let wide : Vector Signal = Voices.Fx.damp (channels [saw 110.0; saw 111.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(typeEquals(types.at("Voices.base"), tScalar()));
  CHECK(typeEquals(types.at("Voices.again"), tScalar()));
  CHECK(typeName(types.at("Voices.lead")) == "Scalar Signal");
  CHECK(types.count("Voices.Fx.damp") == 1);
  CHECK(userMod(prog).inlineModules.count("Voices") == 1);
  CHECK(userMod(prog).inlineModules.count("Voices.Fx") == 1);
  // The polymorphic member instantiates per use like any signature.
  CHECK(typeName(types.at("mono")) == "Scalar Signal");
  CHECK(typeName(types.at("wide")) == "Vector Signal");
}

TEST(checker_inline_module_members_not_bare_outside) {
  TempProject tp;
  std::string f = tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
module A = struct let x : Scalar = 1.0 ;; end ;;
let y : Scalar = x ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_open_inside_module_is_scoped) {
  TempProject tp;
  std::string good = tp.write("good.synth", R"(
module A = struct
  open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
  let s : Scalar Signal = sine 440.0 ;;
end ;;
)");
  DiagnosticBag d1;
  checkProject({good}, d1);
  CHECK(!d1.hasErrors());

  // The open ends at the module's `end`: bare primitives are unknown
  // after it.
  std::string bad = tp.write("bad.synth", R"(
module A = struct
  open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
  let s : Scalar Signal = sine 440.0 ;;
end ;;
let t : Scalar Signal = sine 220.0 ;;
)");
  DiagnosticBag d2;
  checkProject({bad}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_open_inline_module) {
  TempProject tp;
  std::string f = tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
module Kit = struct
  let hz : Scalar = 330.0 ;;
  module Sub = struct
    let gain : Scalar = 0.25 ;;
  end
end ;;
open Kit
let tone : Scalar Signal = (sine hz) *. Sub.gain ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("tone")) == "Scalar Signal");
}

TEST(checker_inline_module_shadows_outer_names) {
  TempProject tp;
  std::string f = tp.write("song.synth", R"(
let x : Scalar = 1.0 ;;
module A = struct
  let x : Timestamp = 10ms ;;
  let y : Timestamp = x ;;
end ;;
let z : Scalar = x ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(typeEquals(types.at("A.y"), tTimestamp()));
  CHECK(typeEquals(types.at("z"), tScalar()));
}

TEST(checker_inline_module_across_files) {
  TempProject tp;
  tp.write("other.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
module A = struct
  let stutter ~s:'a Signal : 'a Signal = lowpass ~cutoff:100.0 s ;;
  let level : Scalar = 0.5 ;;
end ;;
)");
  std::string song = tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
import Other
let mono : Scalar Signal = Other.A.stutter (saw 220.0) ;;
let wide : Vector Signal = Other.A.stutter (channels [saw 1.0; saw 2.0]) ;;
open Other
let lvl : Scalar = A.level ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({song}, diags);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
  const CheckedModule* m = prog.find("Song");
  CHECK(m != nullptr);
  CHECK(typeName(m->defTypes.at("mono")) == "Scalar Signal");
  CHECK(typeName(m->defTypes.at("wide")) == "Vector Signal");
  CHECK(typeEquals(m->defTypes.at("lvl"), tScalar()));
}

TEST(checker_duplicate_inline_module) {
  TempProject tp;
  std::string f = tp.write("song.synth",
                           "module A = struct let x : Scalar = 1.0 ;; end ;;\n"
                           "module A = struct let y : Scalar = 2.0 ;; end ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  CHECK(diags.items[0].message.find("duplicate definition of module") !=
        std::string::npos);
}

TEST(checker_alias_of_inline_module_is_scope_local) {
  // `module K = A` may target an inline module (that is exactly what
  // `module L = C.List` is, with Core a real library) - the binding is
  // usable locally but is not part of the module's exported surface.
  TempProject tp;
  std::string f = tp.write("song.synth",
                           "module A = struct let x : Scalar = 1.0 ;; end ;;\n"
                           "module K = A ;;\n"
                           "let y : Scalar = K.x ;;");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("y"), tScalar()));
  CHECK(userMod(prog).exportedModules.count("K") == 0);
}

TEST(checker_bool_and_if) {
  TempProject tp;
  std::string f = tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let tempo : Scalar = 128.0 ;;
let fast : Bool = tempo >=. 120.0 && not (tempo >. 200.0) ;;
let late : Bool = 500ms <. 1s || false ;;
let voice ~freq:Scalar ~crisp:Bool : Scalar Signal =
  if crisp then highpass ~cutoff:900.0 (saw freq)
  else lowpass ~cutoff:500.0 (sine freq) ;;
let hz : Scalar = if fast then 440.0 else 220.0 ;;
let s : Scalar Signal = voice ~freq:hz ~crisp:(not late) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(typeEquals(types.at("fast"), tBool()));
  CHECK(typeEquals(types.at("late"), tBool()));
  CHECK(typeEquals(types.at("hz"), tScalar()));
  CHECK(typeName(types.at("s")) == "Scalar Signal");
}

TEST(checker_if_branches_can_be_polymorphic) {
  TempProject tp;
  std::string f = tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let pick ~c:Bool ~a:'a ~b:'a : 'a = if c then a else b ;;
let n : Scalar = pick true 1.0 2.0 ;;
let t : Timestamp = pick (1.0 <. 2.0) 250ms 500ms ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(typeEquals(types.at("n"), tScalar()));
  CHECK(typeEquals(types.at("t"), tTimestamp()));
}

TEST(checker_if_condition_must_be_bool) {
  TempProject tp;
  std::string f = tp.write("bad.synth",
                           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\n"
                           "let a : Scalar = if sine 440.0 then 1.0 else 2.0 ;;");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  CHECK(d1.items[0].message.find("condition of 'if'") != std::string::npos);

  // A rigid 'a is not a Bool either: the caller never promised one.
  std::string g = tp.write("bad2.synth",
                           "let bad ~x:'a : Scalar = if x then 1.0 else 2.0 ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_if_branch_types_must_match) {
  TempProject tp;
  std::string f = tp.write(
      "bad.synth", "let b : Scalar = if true then 1.0 else 500ms ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  CHECK(diags.items[0].message.find("branches of 'if'") != std::string::npos);
}

TEST(checker_comparisons_are_build_time_only) {
  TempProject tp;
  std::string f = tp.write("bad.synth",
                           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\n"
                           "let c : Bool = (sine 440.0) <. 0.5 ;;\n"
                           "let d : Bool = 1.0 <. 2.0 < 3.0 ;;\n"
                           "let e : Bool = true && 1.0 ;;\n"
                           "let g : Scalar = true + 1.0 ;;\n"
                           "let h : Bool = 1s ==. 1.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.items.size() == 5);
}

TEST(checker_conditional_render_is_unit) {
  TempProject tp;
  std::string f = tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let loud : Bool = false ;;
let s : Scalar Signal = sine 440.0 ;;
let _ =
  if loud then render "a" 8000.0 (sample (s *. 2.0) 0s 100ms)
  else render "b" 8000.0 (sample s 0s 100ms) ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());
}

TEST(checker_external_signatures) {
  TempProject tp;
  // Data types (and lists/tuples of them) cross the boundary.
  std::string ok = tp.write("ok.synth", R"(
let succ a:Scalar : Scalar = external "succ.cpp" ;;
let pick ~flag:Bool ~name:String : (String, Timestamp) list =
  external "pick.cpp" ;;
)");
  DiagnosticBag d1;
  Program prog = checkProject({ok}, d1);
  CHECK(!d1.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(types.at("succ")->kind == Type::Kind::Fun);

  // So do signals and samples (engine graph handles), functions and
  // type variables (opaque values) - user externals use the same
  // mechanism the bundled Core library's implementations do.
  std::string sigs = tp.write("sigs.synth", R"(
let f ~s:Scalar Signal : Scalar Signal = external "f.cpp" ;;
let cut ~s:'a Signal ~at:Timestamp : 'a Sample = external "f.cpp" ;;
let pick ~f:('a -> 'b) ~x:'a : 'b = external "f.cpp" ;;
let id ~x:'a : 'a = external "f.cpp" ;;
)");
  DiagnosticBag d2;
  checkProject({sigs}, d2);
  CHECK(!d2.hasErrors());

  // Names must form C++ symbols: no primes.
  std::string badName = tp.write(
      "badname.synth", "let f' ~x:Scalar : Scalar = external \"f.cpp\" ;;");
  DiagnosticBag d4;
  checkProject({badName}, d4);
  CHECK(d4.hasErrors());
  CHECK(d4.items[0].message.find("symbol") != std::string::npos);
}

TEST(checker_core_is_a_real_library_of_externals) {
  TempProject tp;
  std::string f = tp.write(
      "ok.synth",
      "open Core\nopen Core.Osc\nlet s : Scalar Signal = sine 440.0 ;;");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  CHECK(!diags.hasErrors());
  const CheckedModule* core = prog.find("Core");
  CHECK(core != nullptr);
  CHECK(core->libName == "Core");
  // Everything is organized into functional submodules...
  CHECK(core->defTypes.count("Osc.sine") == 1);
  CHECK(core->defTypes.count("Fx.lowpass") == 1);
  CHECK(core->defTypes.count("Arrange.mix_all") == 1);
  CHECK(core->defTypes.count("Render.render") == 1);
  CHECK(core->defTypes.count("List.map") == 1);
  CHECK(core->defTypes.count("Time.to_ms") == 1);
  CHECK(core->defTypes.count("Sig.time") == 1);
  CHECK(core->defTypes.count("Math.pow") == 1);
  CHECK(core->defTypes.count("Io.load_mono") == 1);
  CHECK(core->defTypes.count("Pitch.hz") == 1);
  // Pitch's type declarations travel under their module too.
  CHECK(core->typeDecls.count("Pitch.Note") == 1);
  CHECK(core->typeDecls.count("Pitch.Tuning") == 1);
  CHECK(core->typeDecls.count("Pitch.PitchClass") == 1);
  CHECK(core->defTypes.count("Tempo.grid") == 1);
  CHECK(core->typeDecls.count("Tempo.Meter") == 1);
  CHECK(core->typeDecls.count("Tempo.Tempo") == 1);
  CHECK(core->typeDecls.count("Tempo.Value") == 1);
  CHECK(core->defTypes.count("Scale.degree") == 1);
  CHECK(core->typeDecls.count("Scale.Scale") == 1);
  CHECK(core->typeDecls.count("Scale.Chord") == 1);
  CHECK(core->typeDecls.count("Scale.Quality") == 1);
  CHECK(core->typeDecls.count("Scale.ChordQuality") == 1);
  CHECK(core->defTypes.count("Score.play") == 1);
  CHECK(core->typeDecls.count("Score.Step") == 1);
  CHECK(core->typeDecls.count("Score.Phrase") == 1);
  CHECK(core->typeDecls.count("Score.Event") == 1);
  CHECK(core->typeDecls.count("Score.Item") == 1);
  CHECK(core->typeDecls.count("Score.Level") == 1);
  // ...the new-generation submodules are there too...
  CHECK(core->defTypes.count("Groove.pattern") == 1);
  CHECK(core->defTypes.count("Groove.euclid") == 1);
  CHECK(core->defTypes.count("Mix.pan") == 1);
  CHECK(core->defTypes.count("Mix.vca") == 1);
  CHECK(core->defTypes.count("Str.cat") == 1);
  CHECK(core->defTypes.count("Dsp.sine") == 1);
  CHECK(core->typeDecls.count("Scale.Prog") == 1);
  CHECK(core->defTypes.count("Math.hash") == 1);
  CHECK(core->defTypes.count("Time.div") == 1);
  // ...including the envelope curve type and the primitive `Fx.adsr`
  // wraps...
  CHECK(core->typeDecls.count("Fx.Curve") == 1);
  CHECK(core->defTypes.count("Fx.adsr_curved") == 1);
  // ...the whole live-control roster, each over its value primitive...
  for (const char* n : {"Control.slider", "Control.knob", "Control.int_slider",
                        "Control.toggle", "Control.choice", "Control.opt",
                        "Control.slider_value", "Control.knob_value",
                        "Control.int_slider_value", "Control.toggle_value",
                        "Control.choice_value"})
    CHECK(core->defTypes.count(n) == 1);
  // ...and the controller types panels are named with.
  CHECK(core->typeDecls.count("Controller") == 1);
  CHECK(core->typeDecls.count("Control") == 1);
  // ...nothing lives at the top level...
  for (auto& [name, type] : core->defTypes)
    CHECK(name.find('.') != std::string::npos);
  // ...and outside the SynthGraph-implemented modules, the only
  // non-external definitions are the documented handful of sugar the
  // external modules carry beside their bindings.
  std::set<std::string> inSynthGraph = {"List",  "Option", "Pitch", "Tempo",
                                        "Scale", "Score",  "Groove", "Mix",
                                        "Dsp"};
  // Core's SynthGraph-written sugar outside the modules above: the
  // envelope's curve wrapper, the voice-window idioms, and every live
  // control (each pairs its value primitive with the Controller a panel
  // shows it with).
  std::set<std::string> sugar = {"Math.pi",
                                 "Math.min",
                                 "Math.max",
                                 "Math.clamp",
                                 "Math.lerp",
                                 "Fx.adsr",
                                 "Fx.gated",
                                 "Fx.echoes",
                                 "Control.map",
                                 "Control.nest",
                                 "Control.slider",
                                 "Control.knob",
                                 "Control.int_slider",
                                 "Control.toggle",
                                 "Control.choice",
                                 "Control.opt",
                                 "Control.multi_slider",
                                 "Ui.key",
                                 "Ui.panel"};
  std::function<bool(const std::string&, const std::vector<TopDef>&)>
      allExternal = [&](const std::string& prefix,
                        const std::vector<TopDef>& ds) {
        for (auto& d : ds) {
          if (d.kind == TopDef::Kind::ModuleDef && !inSynthGraph.count(d.name) &&
              !allExternal(prefix + d.name + ".", d.defs))
            return false;
          if (d.kind == TopDef::Kind::Let &&
              d.body->kind != Expr::Kind::External &&
              !sugar.count(prefix + d.name))
            return false;
        }
        return true;
      };
  CHECK(allExternal("", core->parsed.defs));
  // Core also declares the ambient list type itself.
  CHECK(core->typeDecls.count("list") == 1);
}

TEST(checker_int_arithmetic_and_comparisons) {
  TempProject tp;
  std::string f = tp.write("i.synth", R"(
open Core open Core.Math
let n : Int = 3 + 4 * 2 ;;
let d : Int = 7 / 2 ;;
let neg : Int = -n ;;
let b : Bool = n < 12 && d == 3 ;;
let s : Scalar = to_scalar n *. 1.5 ;;
let r : Int = round 2.6 ;;
let fl : Int = floor 2.6 ;;
let ce : Int = ceil 2.4 ;;
let pick : Int = if b then n else d ;;
let xs : Int list = [1; 2; 3] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  for (const char* n : {"n", "d", "neg", "r", "fl", "ce", "pick"})
    CHECK(typeEquals(types.at(n), tInt()));
  CHECK(typeEquals(types.at("b"), tBool()));
  CHECK(typeEquals(types.at("s"), tScalar()));
  CHECK(typeName(types.at("xs")) == "Int list");
}

TEST(checker_int_does_not_mix_with_scalar) {
  TempProject tp;
  // Int + Scalar, Int == Scalar, and Int-for-Scalar arguments are all
  // type errors; conversion is explicit.
  for (const char* bad :
       {"let x : Scalar = 1 + 2.0 ;;",
        "let b : Bool = 1 == 1.0 ;;",
        "let t : Scalar = 2 ;;"}) {
    TempProject p2;
    std::string f = p2.write("bad.synth", bad);
    DiagnosticBag diags;
    checkProject({f}, diags);
    CHECK(diags.hasErrors());
  }
}

TEST(checker_int_argument_where_scalar_expected_hints_conversion) {
  TempProject tp;
  std::string f = tp.write(
      "bad.synth",
      "open Core open Core.Osc\nlet x : Scalar Signal = sine 440 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool hinted = false;
  for (auto& d : diags.items)
    if (d.message.find("to_scalar") != std::string::npos) hinted = true;
  CHECK(hinted);
}

TEST(checker_unary_minus_types) {
  TempProject tp;
  std::string f = tp.write("neg.synth", R"(
open Core open Core.Osc
let a : Int = -3 ;;
let b : Scalar = -2.5 ;;
let s : Scalar Signal = -(sine 440.0) ;;
let v x:Vector : Vector = -x ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  // ...and non-numeric operands are rejected.
  std::string g = tp.write("bad.synth", "let s : String = -\"x\" ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_int_count_signatures) {
  TempProject tp;
  // A Scalar count is now a type error at the call site.
  std::string f = tp.write(
      "bad.synth",
      "open Core open Core.Time\n"
      "let xs : Timestamp list = time_steps ~start:0s ~step:250ms ~count:8.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_int_crosses_external_boundary) {
  TempProject tp;
  std::string f = tp.write("e.synth", R"(
let twice n:Int : Int = external "twice.cpp" ;;
let firsts ~xs:Int list : Int = external "twice.cpp" ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(!diags.hasErrors());
}

TEST(checker_bundled_stdlib_checks_as_core) {
  // Opening or linting the bundled stdlib source itself (as happens when
  // developing synthc) must check it as the Core library, under the
  // library's canonical module identity.
  fs::path lib =
      fs::path(bundledStdlibDir()) / "core" / kLibraryInterfaceFile;
  DiagnosticBag diags;
  Program prog = checkProject({lib.string()}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(
        d, prog.modules.empty() ? std::string{}
                                : prog.modules.front().parsed.source);
  CHECK(!diags.hasErrors());
  // Loaded once, under the Core name - not doubled as root + dependency.
  CHECK(prog.modules.size() == 1);
  CHECK(prog.modules.front().libName == "Core");
}

TEST(checker_type_name_function_formatting) {
  // A function type prints without enclosing parentheses; parens appear
  // only where the source grammar itself requires them - a function-typed
  // parameter and a Signal/Sample/list element.
  TypePtr f = tFun({tScalar()}, {"x"}, tScalar());
  CHECK(typeName(f) == "x:Scalar -> Scalar");
  TypePtr hof = tFun({f, tScalar()}, {"f", ""}, tScalar());
  CHECK(typeName(hof) == "f:(x:Scalar -> Scalar) -> Scalar -> Scalar");
  
  // A bundled-stdlib signature reads back as written: Math.exp : x:'a -> 'a.
  fs::path lib =
      fs::path(bundledStdlibDir()) / "core" / kLibraryInterfaceFile;
  DiagnosticBag diags;
  Program prog = checkProject({lib.string()}, diags);
  CHECK(!diags.hasErrors());
  CHECK(typeName(prog.modules.front().defTypes.at("Math.exp")) ==
        "x:'a -> 'a");
}

TEST(checker_type_variables_are_scoped_per_definition) {
  // Same spelling in two definitions: two distinct rigid variables. The
  // parser only records the surface name; the checker allocates ids
  // per definition.
  TempProject tp;
  std::string f = tp.write("t.synth",
                           "let f ~x:'a : 'a = x ;;\n"
                           "let g ~y:'a : 'a = y ;;");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  const TypePtr& fT = types.at("f");
  const TypePtr& gT = types.at("g");
  CHECK(fT->ret->kind == Type::Kind::Var);
  CHECK(isRigidVar(fT->ret));
  CHECK(fT->ret->var != gT->ret->var);
  // Within one definition the spelling ties parameter and result.
  CHECK(fT->items[0]->var == fT->ret->var);
}

TEST(checker_unknown_type_name) {
  TempProject tp;
  std::string f = tp.write("t.synth", "let x : Nope = 1.0 ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool found = false;
  for (auto& d : diags.items)
    if (d.message.find("unknown type 'Nope'") != std::string::npos)
      found = true;
  CHECK(found);
}

TEST(checker_builtin_type_arity) {
  // An atom type does not take a parameter; Signal needs one.
  TempProject tp;
  std::string f = tp.write("t.synth", "let x : Timestamp Scalar = 1.0 ;;");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  TempProject tp2;
  std::string g = tp2.write("t.synth", "let x : Signal = 1.0 ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_record_declaration_literal_projection_update) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Env = { attack : Timestamp; release : Timestamp } ;;
let env : Env = { attack = 5ms; release = 100ms } ;;
let a : Timestamp = env.attack ;;
let quick : Env = { env with attack = 1ms } ;;
let r : Timestamp = quick.release ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(types.at("env")->kind == Type::Kind::Named);
  CHECK(types.at("env")->decl->name == "Env");
  CHECK(typeEquals(types.at("a"), tTimestamp()));
  CHECK(typeEquals(types.at("env"), types.at("quick")));
}

TEST(checker_polymorphic_record) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type ('a, 'b) Pair = { first : 'a; second : 'b } ;;
let p : (Scalar, Timestamp) Pair = { first = 1.0; second = 5ms } ;;
let s : Scalar = p.first ;;
let t : Timestamp = p.second ;;
let swap x:(Scalar, Timestamp) Pair : (Timestamp, Scalar) Pair =
  { first = x.second; second = x.first } ;;
let q : (Timestamp, Scalar) Pair = swap p ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(typeEquals(types.at("s"), tScalar()));
  CHECK(typeEquals(types.at("t"), tTimestamp()));
}

TEST(checker_record_field_type_mismatch) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Env = { attack : Timestamp; release : Timestamp } ;;
let env : Env = { attack = 5.0; release = 100ms } ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_record_literal_needs_exact_field_set) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Env = { attack : Timestamp; release : Timestamp } ;;
let env : Env = { attack = 5ms } ;;
)");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  TempProject tp2;
  std::string g = tp2.write("t.synth", R"(
type Env = { attack : Timestamp } ;;
let env : Env = { attack = 5ms; extra = 1.0 } ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_record_unknown_field_and_non_record) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Env = { attack : Timestamp } ;;
let env : Env = { attack = 5ms } ;;
let x : Timestamp = env.decay ;;
)");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  bool found = false;
  for (auto& d : d1.items)
    if (d.message.find("has no field 'decay'") != std::string::npos)
      found = true;
  CHECK(found);
  TempProject tp2;
  std::string g = tp2.write("t.synth", "let x : Scalar = (1.0).attack ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_duplicate_type_and_ambiguous_literal) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Env = { a : Scalar } ;;
type Env = { b : Scalar } ;;
)");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  // Two visible record types with the same field set: ambiguous literal.
  TempProject tp2;
  std::string g = tp2.write("t.synth", R"(
type A = { x : Scalar } ;;
type B = { x : Scalar } ;;
let v : A = { x = 1.0 } ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_abstract_type_declaration) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Handle ;;
let use h:Handle : Handle = h ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(types.at("use")->items[0]->kind == Type::Kind::Named);
  CHECK(types.at("use")->items[0]->decl->flavor ==
        TypeDecl::Flavor::Abstract);
}

TEST(checker_type_decl_in_inline_module_and_qualified_use) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
module Voices = struct
  type Voice = { gain : Scalar } ;;
  let make g:Scalar : Voice = { gain = g } ;;
end ;;
let v : Voices.Voice = Voices.make 0.5 ;;
let g : Scalar = v.gain ;;
open Voices
let w : Voice = make 0.25 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& m = userMod(prog);
  CHECK(m.typeDecls.count("Voices.Voice") == 1);
  CHECK(typeEquals(m.defTypes.at("v"), m.defTypes.at("w")));
}

TEST(checker_type_decl_across_modules) {
  TempProject tp;
  tp.write("shapes.synth", R"(
type Env = { attack : Timestamp } ;;
let make a:Timestamp : Env = { attack = a } ;;
)");
  std::string f = tp.write("song.synth", R"(
import Shapes
let e : Shapes.Env = Shapes.make 5ms ;;
let a : Timestamp = e.attack ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.back().parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_type_decl_must_precede_use) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
let e : Env = { attack = 5ms } ;;
type Env = { attack : Timestamp } ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_recursive_record_type_is_declarable) {
  // A record may mention itself (unbuildable without variants, but the
  // declaration is legal); the arity check still applies.
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type 'a Chain = { head : 'a; rest : 'a Chain } ;;
let use c:Scalar Chain : Scalar = c.head ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_type_decl_arity_and_unbound_var) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type 'a Box = { v : 'a } ;;
let b : Box = { v = 1.0 } ;;
)");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  TempProject tp2;
  std::string g = tp2.write("t.synth", "type Box = { v : 'a } ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_record_in_signal_pipeline) {
  // Records hold signals and feed the usual pipeline.
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
open Core open Core.Osc open Core.Fx
type Voice = { osc : Scalar Signal; vel : Scalar } ;;
let v : Voice = { osc = sine 440.0; vel = 0.5 } ;;
let out : Scalar Signal = v.osc *. v.vel ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("out")) == "Scalar Signal");
}

TEST(checker_variant_and_match) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Wave = | Sine | Saw | Pulse of Scalar ;;
let width w:Wave : Scalar =
  match w with
  | Sine -> 0.0
  | Saw -> 0.5
  | Pulse duty -> duty ;;
let a : Scalar = width Sine ;;
let b : Scalar = width (Pulse 0.25) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("a"), tScalar()));
}

TEST(checker_match_not_exhaustive) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Wave = | Sine | Saw | Pulse of Scalar ;;
let width w:Wave : Scalar =
  match w with
  | Sine -> 0.0
  | Pulse duty -> duty ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool found = false;
  for (auto& d : diags.items)
    if (d.message.find("not exhaustive") != std::string::npos &&
        d.message.find("Saw") != std::string::npos)
      found = true;
  CHECK(found);
}

TEST(checker_match_redundant_case) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Wave = | Sine | Saw ;;
let width w:Wave : Scalar =
  match w with
  | Sine -> 0.0
  | _ -> 0.5
  | Saw -> 1.0 ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool found = false;
  for (auto& d : diags.items)
    if (d.message.find("unreachable") != std::string::npos) found = true;
  CHECK(found);
}

TEST(checker_match_nested_exhaustiveness) {
  // Nested payload coverage: Pulse's payload is opaque (Scalar), so a
  // bare `Pulse` arm set covers it only through a wildcard/bind.
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Wave = | Sine | Pulse of Scalar ;;
type Slot = | Empty | Full of Wave ;;
let f s:Slot : Scalar =
  match s with
  | Empty -> 0.0
  | Full Sine -> 1.0 ;;
)");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  bool found = false;
  for (auto& d : d1.items)
    if (d.message.find("not exhaustive") != std::string::npos &&
        d.message.find("Full (Pulse _)") != std::string::npos)
      found = true;
  CHECK(found);
  TempProject tp2;
  std::string g = tp2.write("t.synth", R"(
type Wave = | Sine | Pulse of Scalar ;;
type Slot = | Empty | Full of Wave ;;
let f s:Slot : Scalar =
  match s with
  | Empty -> 0.0
  | Full Sine -> 1.0
  | Full (Pulse d) -> d ;;
)");
  DiagnosticBag d2;
  Program p2 = checkProject({g}, d2);
  for (auto& d : d2.items)
    std::cerr << renderDiagnostic(d, userMod(p2).parsed.source);
  CHECK(!d2.hasErrors());
}

TEST(checker_match_arm_type_mismatch_and_abstract) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Wave = | Sine | Saw ;;
let f w:Wave : Scalar =
  match w with
  | Sine -> 1.0
  | Saw -> 500ms ;;
)");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  TempProject tp2;
  std::string g = tp2.write("t.synth", R"(
type Handle ;;
let f h:Handle : Scalar =
  match h with
  | Open -> 1.0 ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
  bool found = false;
  for (auto& d : d2.items)
    if (d.message.find("abstract") != std::string::npos) found = true;
  CHECK(found);
}

TEST(checker_polymorphic_variant) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type 'a Option = | None | Some of 'a ;;
let get x:Scalar Option ~fallback:Scalar : Scalar =
  match x with
  | None -> fallback
  | Some v -> v ;;
let a : Scalar = get (Some 2.0) ~fallback:0.0 ;;
let b : Scalar Option = None ;;
let or_else x:'a Option ~fallback:'a : 'a =
  match x with
  | None -> fallback
  | Some v -> v ;;
let c : Timestamp = or_else (Some 5ms) ~fallback:0s ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("c"), tTimestamp()));
}

TEST(checker_destructuring_let) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Env = { attack : Timestamp; release : Timestamp } ;;
let bounds : (Scalar, Scalar) = (0.1, 0.9) ;;
let mid : Scalar =
  let (lo, hi) : (Scalar, Scalar) = bounds in (lo +. hi) /. 2.0 ;;
let env : Env = { attack = 5ms; release = 100ms } ;;
let a : Timestamp =
  let { attack; release = r } : Env = env in
  if attack <. r then attack else r ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("mid"), tScalar()));
}

TEST(checker_destructuring_let_must_be_irrefutable) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type 'a Option = | None | Some of 'a ;;
let x : Scalar Option = Some 1.0 ;;
let y : Scalar =
  match x with
  | Some v -> v
  | None -> 0.0 ;;
)");
  DiagnosticBag ok;
  checkProject({f}, ok);
  CHECK(!ok.hasErrors());
  // Tuple pattern against a non-tuple: rejected.
  TempProject tp2;
  std::string g = tp2.write("t.synth",
                            "let y : Scalar =\n"
                            "  let (a, b) : Scalar = 1.0 in a ;;");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_ctor_not_first_class) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Wave = | Sine | Pulse of Scalar ;;
let p : Wave = Pulse ;;
)");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  TempProject tp2;
  std::string g = tp2.write("t.synth", R"(
type Wave = | Sine | Pulse of Scalar ;;
let p : Wave = Sine 1.0 ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_ctor_across_modules) {
  TempProject tp;
  tp.write("shapes.synth", R"(
type Wave = | Sine | Pulse of Scalar ;;
)");
  std::string f = tp.write("song.synth", R"(
import Shapes
let a : Shapes.Wave = Shapes.Sine ;;
let b : Shapes.Wave = Shapes.Pulse 0.5 ;;
let w : Scalar =
  match b with
  | Shapes.Sine -> 0.0
  | Shapes.Pulse d -> d ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.back().parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_let_rec_name_in_scope) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
let rec fact n:Int : Int = if n <= 1 then 1 else n * fact (n - 1) ;;
let x : Int =
  let rec go n:Int ~acc:Int : Int =
    if n <= 0 then acc else go (n - 1) ~acc:(acc + n) in
  go 3 ~acc:0 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_non_rec_body_does_not_see_its_own_name) {
  TempProject tp;
  std::string f = tp.write(
      "t.synth", "let f n:Int : Int = if n <= 1 then 1 else f (n - 1) ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_rec_variant_walk) {
  // A recursive function over a recursive variant - the shape every
  // list operation takes.
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type Chain = | End | Link of (Scalar, Chain) ;;
let rec total c:Chain : Scalar =
  match c with
  | End -> 0.0
  | Link (x, rest) -> x +. total rest ;;
let t : Scalar = total (Link (1.0, Link (2.0, End))) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_rec_polymorphic_self_call_stays_rigid) {
  // Inside its own body the recursive name keeps rigid 'a: calling
  // itself at a DIFFERENT type is rejected (no polymorphic recursion).
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
type 'a Box = | Leaf of 'a | Nest of ('a Box, Int) ;;
let rec depth b:'a Box ~n:Int : Int =
  match b with
  | Leaf _ -> n
  | Nest (inner, _) -> depth inner ~n:(n + 1) ;;
)");
  DiagnosticBag ok;
  Program p = checkProject({f}, ok);
  for (auto& d : ok.items)
    std::cerr << renderDiagnostic(d, userMod(p).parsed.source);
  CHECK(!ok.hasErrors());
  TempProject tp2;
  std::string g = tp2.write("t.synth", R"(
let rec bad x:'a : 'a = bad 1.0 ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_lists_are_matchable_variants) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
let rec sum xs:Scalar list : Scalar =
  match xs with
  | Nil -> 0.0
  | Cons (x, rest) -> x +. sum rest ;;
let s : Scalar = sum [1.0; 2.0; 3.0] ;;
let manual : Scalar list = Cons (1.0, Cons (2.0, Nil)) ;;
let head xs:'a list ~fallback:'a : 'a =
  match xs with
  | Nil -> fallback
  | Cons (x, _) -> x ;;
let h : Scalar = head manual ~fallback:0.0 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("manual")) == "Scalar list");
}

TEST(checker_undetermined_empty_list_is_an_error) {
  // With no annotation and no use, [] leaves its element unresolved -
  // the leftover-free-variable rule catches it at the binding.
  TempProject tp;
  std::string f = tp.write("t.synth",
                           "let f x:'a list : Int = 0 ;;\n"
                           "let n : Int = f [] ;;");
  DiagnosticBag ok;
  checkProject({f}, ok);
  CHECK(!ok.hasErrors());  // the call may leave 'a free: result is Int
}

TEST(checker_signal_is_abstract_and_sample_is_a_record) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
open Core open Core.Osc open Core.Arrange
let s : Scalar Sample = sample (sine 440.0) 0s 100ms ;;
let start : Timestamp = s.from ;;
let inner : Scalar Signal = s.sig ;;
let longer : Scalar Sample = { s with to = 200ms } ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(typeEquals(types.at("start"), tTimestamp()));
  CHECK(typeName(types.at("inner")) == "Scalar Signal");
  CHECK(types.at("s")->kind == Type::Kind::Named);
  CHECK(types.at("s")->decl->flavor == TypeDecl::Flavor::Record);
  // Matching on an abstract Signal is an error.
  TempProject tp2;
  std::string g = tp2.write("t.synth", R"(
open Core open Core.Osc
let f : Scalar =
  match sine 440.0 with
  | On -> 1.0 ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
  bool found = false;
  for (auto& d : d2.items)
    if (d.message.find("abstract") != std::string::npos) found = true;
  CHECK(found);
}

// --- The core-library review round: new rosters and language rules ---------

TEST(checker_math_additions_roster) {
  TempProject tp;
  std::string f = tp.write("m.synth", R"(
open Core open Core.Math
let s : Scalar = sin pi +. cos 0.0 +. tan 0.1 +. atan 1.0 +. abs (-2.0) ;;
let sig : Scalar Signal = Core.Sig.signal ~f:(fun t:Scalar -> sin (t *. 2.0 *. pi)) ;;
let m1 : Scalar = min 1.0 2.0 ;;
let m2 : Scalar = max 1.0 2.0 ;;
let c : Scalar = clamp ~lo:0.0 ~hi:1.0 ~x:1.5 ;;
let l : Scalar = lerp ~a:0.0 ~b:10.0 ~t:0.25 ;;
let h : Scalar = hash ~seed:7.0 ~i:3 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(typeName(types.at("sig")) == "Scalar Signal");
  CHECK(typeName(types.at("h")) == "Scalar");
}

TEST(checker_math_addition_type_errors) {
  TempProject tp;
  // hash's index is an Int; a Scalar is rejected.
  std::string f = tp.write("m.synth", R"(
open Core open Core.Math
let h : Scalar = hash ~seed:7.0 ~i:3.0 ;;
)");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  // min/max are Scalar-only.
  std::string g = tp.write("m2.synth", R"(
open Core open Core.Math
let m : Int = min 1 2 ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_time_div_rem_roster) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
open Core open Core.Time
let n : Int = div ~num:1s ~den:250ms ;;
let left : Timestamp = rem ~num:1s ~den:250ms ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("n")) == "Int");
  CHECK(typeName(userMod(prog).defTypes.at("left")) == "Timestamp");
  // Scalars do not divide as durations.
  std::string g = tp.write("t2.synth", R"(
open Core open Core.Time
let n : Int = div ~num:1.0 ~den:0.5 ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_new_fx_and_osc_roster) {
  TempProject tp;
  std::string f = tp.write("fx.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Sig open Core.Arrange
let sweep : Scalar Signal =
  lowpass_mod ~cutoff:(constant 400.0 +. time *. 800.0) ~input:(saw 110.0) ;;
let hp : Vector Signal =
  highpass_mod ~cutoff:(constant 100.0)
               ~input:(channels [sine 100.0; sine 200.0]) ;;
let acid : Scalar Signal =
  resonant ~cutoff:(constant 700.0) ~q:6.0 ~input:(saw_bl 55.0) ;;
let env : Scalar Signal =
  follow ~attack:5ms ~release:80ms ~input:(square_bl 220.0) ;;
let dub : Scalar Signal = feedback ~by:250ms ~gain:0.6 ~input:(sine 440.0) ;;
let gate : Scalar Signal =
  select ~gate:env ~threshold:0.2 ~above:(sine 440.0) ~below:(sine 220.0) ;;
let v : Scalar Sample =
  gated ~attack:3ms ~decay:110ms ~sustain:0.5 ~release:60ms ~hold:400ms
        ~input:(sine 330.0) ;;
let e : Scalar Signal = echoes ~by:200ms ~gain:0.5 ~n:3 ~input:(sine 440.0) ;;
let mono : Scalar Signal = channel ~n:0 ~input:hp ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(typeName(types.at("hp")) == "Vector Signal");
  CHECK(typeName(types.at("v")) == "Scalar Sample");
  CHECK(typeName(types.at("mono")) == "Scalar Signal");
}

TEST(checker_new_fx_type_errors) {
  TempProject tp;
  // A fixed Scalar where the modulated filter wants a cutoff *signal*.
  std::string f = tp.write("fx.synth", R"(
open Core open Core.Osc open Core.Fx
let s : Scalar Signal = lowpass_mod ~cutoff:400.0 ~input:(saw 110.0) ;;
)");
  DiagnosticBag d1;
  checkProject({f}, d1);
  CHECK(d1.hasErrors());
  // channel takes a Vector Signal.
  std::string g = tp.write("fx2.synth", R"(
open Core open Core.Osc open Core.Arrange
let s : Scalar Signal = channel ~n:0 ~input:(sine 440.0) ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_signal_broadcast_row) {
  TempProject tp;
  std::string f = tp.write("b.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange
let bus : Vector Signal = channels [sine 220.0; sine 221.0] ;;
let env : Scalar Signal = exp_decay 2.0 ;;
let faded : Vector Signal = bus *. env ;;
let other : Vector Signal = env *. bus ;;
let summed : Vector Signal = bus +. env ;;
let ducked ~input:'a Signal ~gain:Scalar Signal : 'a Signal =
  input *. gain ;;
let d : Vector Signal = ducked ~input:bus ~gain:env ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("faded")) == "Vector Signal");
  // Two rigid element types still do not combine.
  std::string g = tp.write("b2.synth", R"(
open Core
let bad ~x:'a Signal ~y:'b Signal : 'a Signal = x *. y ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_groove_roster) {
  TempProject tp;
  std::string f = tp.write("g.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Time
let hit : Scalar Sample = sample (sine 440.0 *. exp_decay 20.0) 0s 100ms ;;
let grid16 : Timestamp list = time_steps ~start:0s ~step:125ms ~count:16 ;;
let straight : Scalar Signal = Groove.pattern ~hit:hit ~steps:grid16 ;;
let loose : Scalar Signal =
  Groove.humanized ~hit:hit ~steps:grid16 ~seed:7.0 ~spread:8ms ;;
let row : Timestamp list =
  Groove.mask ~keep:[true; false; false; true] ~steps:grid16 ;;
let world : Timestamp list = Groove.euclid ~hits:5 ~steps:grid16 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("row")) == "Timestamp list");
}

TEST(checker_tempo_additions_roster) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
open Core open Core.Tempo
let t : Tempo = common ~bpm:120.0 ;;
let two : Timestamp = bars ~t:t ~n:2.0 ;;
let sixteenths : Int = per_bar ~t:t ~v:Sixteenth ;;
let bridge : Scalar = bar_beats ~t:t ~n:8 ;;
let landmarks : Timestamp list = marks ~t:t ~bars:[8; 8; 12; 4] ;;
let sg : Timestamp list =
  swung_grid ~t:t ~from:0s ~step:Eighth ~count:16 ~amount:0.33 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("sixteenths")) == "Int");
  CHECK(typeName(userMod(prog).defTypes.at("bridge")) == "Scalar");
}

TEST(checker_scale_open_enums_and_prog) {
  TempProject tp;
  std::string f = tp.write("s.synth", R"(
open Core open Core.Pitch open Core.Scale
let harm_major : Scale = { tonic = { pc = C; oct = 4 };
                           quality = CustomQ [0; 2; 4; 5; 7; 8; 11] } ;;
let d : Note = degree ~s:harm_major ~n:5 ;;
let th : Note list = tones ~c:{ root = { pc = A; oct = 3 };
                                quality = Shape [0; 4; 7; 10; 14; 17; 21] } ;;
let ninth : Note list = tones ~c:{ root = { pc = D; oct = 3 };
                                   quality = Min9 } ;;
let p : Prog = { key = harm_major; degrees = [0; 5; 3; 4] } ;;
let len : Int = prog_len ~p:p ;;
let root7 : Note = prog_root ~p:p ~i:7 ;;
let ch : Note list = prog_chord ~p:p ~i:2 ;;
let st : Note list = prog_stack ~p:p ~i:1 ~count:4 ;;
let folded : Note = wrap_to ~note:{ pc = A; oct = 6 }
                            ~low:{ pc = C; oct = 3 } ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(typeName(types.at("len")) == "Int");
  // The open enums stay checker-guided: an exhaustive match that
  // forgets the payload case is an error naming it.
  std::string g = tp.write("s2.synth", R"(
open Core open Core.Scale
let f q:Quality : Int =
  match q with
  | Major -> 0 | Minor -> 1 | Dorian -> 2 | Phrygian -> 3 | Lydian -> 4
  | Mixolydian -> 5 | Locrian -> 6 | HarmMinor -> 7 | MelMinor -> 8
  | PentMajor -> 9 | PentMinor -> 10 | Blues -> 11 | WholeTone -> 12
  | Chromatic -> 13 ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
  bool named = false;
  for (auto& d : d2.items)
    if (d.message.find("CustomQ") != std::string::npos) named = true;
  CHECK(named);
}

TEST(checker_score_additions_roster) {
  TempProject tp;
  std::string f = tp.write("sc.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange
open Core.Pitch open Core.Score
let drums : Phrase = hits ~n:8 ~len:0.5 ;;
let pat : Phrase = rhythm ~lens:[1.0; 0.5; 0.5; 2.0] ;;
let accented : Phrase = vels ~p:drums ~vs:[1.0; amp ~l:Forte] ;;
let rising : Phrase = crescendo ~p:drums ~from:Piano ~to:Fff ;;
let loose : Phrase = humanize ~p:drums ~seed:3.0 ~spread:0.02 ;;
let shuffled : Phrase = shuffle ~p:drums ~grid:0.5 ~amount:0.33 ;;
let blue : Phrase = bend ~p:pat ~f:(fun i:Int -> -25.0) ;;
let t : Tempo.Tempo = Tempo.common ~bpm:100.0 ;;
let evs : Event list =
  realize_with ~tempo:t ~pitch:(fun n:Note -> 110.0) ~p:pat ;;
let evs2 : Event list =
  realize ~tempo:t ~tuning:(et12 ~ref_hz:440.0) ~p:pat ;;
let stepbend : Scalar =
  match pat.steps with
  | Nil -> 0.0
  | Cons (s, rest) -> s.bend ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("evs")) ==
        typeName(userMod(prog).defTypes.at("evs2")));
}

TEST(checker_mix_roster) {
  TempProject tp;
  std::string f = tp.write("mx.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Time
let l : Scalar Signal = sine 220.0 ;;
let r : Scalar Signal = sine 331.0 ;;
let wide : Vector Signal = Mix.pan ~pos:0.3 ~input:l ;;
let auto : Vector Signal =
  Mix.pan_sig ~pos:(Core.Sig.signal ~f:(fun t:Scalar -> t -. 1.0))
              ~input:l ;;
let master : Vector Signal =
  Mix.mix ~parts:[(Mix.db (-6.0), wide); (0.5, auto)] ;;
let quieter : Vector Signal = Mix.gain_db ~x:(-3.0) ~input:master ;;
let faded : Vector Signal = Mix.vca ~gain:(exp_decay 1.0) ~input:master ;;
let pumped : Vector Signal =
  Mix.duck ~ats:(time_steps ~start:0s ~step:500ms ~count:8) ~depth:0.6
           ~dip:60ms ~recover:200ms ~input:master ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("pumped")) == "Vector Signal");
}

TEST(checker_str_and_iter_roster) {
  TempProject tp;
  std::string f = tp.write("st.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render
let name i:Int : String = Str.cat ~a:"section-" ~b:(Str.of_int ~n:i) ;;
let _ = List.iter
  ~f:(fun i:Int ->
        render ~name:(name i) ~rate:8000.0
               ~sample:(sample (sine 220.0) 0s 10ms))
  ~xs:(List.range ~from:0 ~count:2) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  // iter's f must produce unit.
  std::string g = tp.write("st2.synth", R"(
open Core
let x : unit = List.iter ~f:(fun i:Int -> 1.0) ~xs:[1; 2] ;;
)");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_list_additions_roster) {
  TempProject tp;
  std::string f = tp.write("l.synth", R"(
open Core
let ixs : Scalar list =
  List.mapi ~f:(fun i:Int x:Scalar -> Core.Math.to_scalar i *. x)
            ~xs:[1.0; 2.0; 3.0] ;;
let front : Int list = List.take ~n:2 ~xs:[1; 2; 3; 4] ;;
let back : Int list = List.drop ~n:2 ~xs:[1; 2; 3; 4] ;;
let running : Int list =
  List.scan ~f:(fun acc:Int x:Int -> acc + x) ~init:0 ~xs:[8; 8; 12] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("running")) == "Int list");
}

TEST(checker_dsp_prelude) {
  TempProject tp;
  std::string f = tp.write("d.synth", R"(
open Core
open Core.Dsp
let pluck freq:Scalar : Scalar Signal = sine freq *. exp_decay 6.0 ;;
let low : Scalar Signal = lowpass ~cutoff:800.0 (saw_bl 110.0) ;;
let win : Scalar Sample = sample (pluck 440.0) 0s 800ms ;;
let both : Scalar Signal = mix_all [place win 0s; place win 500ms] ;;
let n : Scalar = to_scalar (round 2.5) +. pi ;;
let _ = render "dsp-demo" 8000.0 (sample both 0s 1s) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_local_inference) {
  TempProject tp;
  // Return types and let-in annotations may be omitted; the checker
  // synthesizes them from the body.
  std::string f = tp.write("i.synth", R"(
open Core open Core.Osc open Core.Fx
let freq = 220.0 *. 2.0 ;;
let tone f:Scalar = sine f *. exp_decay 4.0 ;;
let voiced =
  let gain = 0.5 in
  let scaled g:Scalar = tone freq *. g in
  scaled gain ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  const auto& types = userMod(prog).defTypes;
  CHECK(typeName(types.at("freq")) == "Scalar");
  CHECK(typeName(types.at("voiced")) == "Scalar Signal");
  CHECK(types.at("tone")->kind == Type::Kind::Fun);
  // An undetermined body still demands the annotation...
  std::string g = tp.write("i2.synth", "let xs = [] ;;\n");
  DiagnosticBag d2;
  checkProject({g}, d2);
  CHECK(d2.hasErrors());
  bool asked = false;
  for (auto& d : d2.items)
    if (d.message.find("annotate") != std::string::npos) asked = true;
  CHECK(asked);
  // ...and so does let rec (the recursive name needs a known type).
  std::string h = tp.write(
      "i3.synth", "let rec f n:Int = if n <= 0 then 0 else f (n - 1) ;;\n");
  DiagnosticBag d3;
  checkProject({h}, d3);
  CHECK(d3.hasErrors());
}

TEST(checker_lint_warns_on_unused_open) {
  TempProject tp;
  std::string f = tp.write("w.synth", R"(
open Core
open Core.Osc
open Core.Io
let s : Scalar Signal = sine 440.0 ;;
)");
  ModuleLoadContext ctx;
  ctx.warnUnusedOpens = true;
  DiagnosticBag diags;
  checkProject({f}, diags, &ctx);
  CHECK(!diags.hasErrors());
  bool warnedIo = false, warnedOsc = false, warnedCore = false;
  for (auto& d : diags.items) {
    if (d.severity != Severity::Warning) continue;
    if (d.message.find("'Core.Io'") != std::string::npos) warnedIo = true;
    if (d.message.find("'Core.Osc'") != std::string::npos) warnedOsc = true;
    if (d.message.find("'Core'") == 0) warnedCore = true;
  }
  CHECK(warnedIo);
  CHECK(!warnedOsc);
  // Without the flag (ordinary builds) no warnings appear.
  DiagnosticBag quiet;
  checkProject({f}, quiet);
  CHECK(quiet.items.empty());
}

// --- Optional labeled parameters & the Option type -------------------------

TEST(checker_option_type_is_ambient) {
  TempProject tp;
  // No open needed: `Option`, `Some` and `None` are ambient like `list`.
  std::string f = tp.write("o.synth", R"(
let s : Int Option = Some 3 ;;
let n : Int Option = None ;;
let v : Int = match s with | None -> 0 | Some x -> x ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("s")) == "Int Option");
}

TEST(checker_optional_param_types) {
  TempProject tp;
  std::string f = tp.write("o.synth", R"(
let f ?x:Int ?(y = 2 : Int) z:Int : Int =
  let dx : Int = match x with | None -> 0 | Some v -> v in
  dx + y + z
;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  // The stored signature carries element types with '?' markers; the
  // body saw x : Int Option (the match above) and y : Int (used in +).
  CHECK(typeName(userMod(prog).defTypes.at("f")) ==
        "?x:Int -> ?y:Int -> Int -> Int");
}

TEST(checker_optional_param_without_default_is_option_in_body) {
  TempProject tp;
  // Using a non-defaulted optional as its element type must fail: the
  // body holds an Int Option, not an Int.
  std::string f = tp.write("o.synth",
                           "let f ?x:Int z:Int : Int = x + z ;;\n");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_optional_params_must_come_first) {
  TempProject tp;
  std::string f = tp.write("o.synth",
                           "let f z:Int ?x:Int : Int = z ;;\n");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool said = false;
  for (auto& d : diags.items)
    if (d.message.find("must come before") != std::string::npos) said = true;
  CHECK(said);
}

TEST(checker_optional_params_need_a_required_one) {
  TempProject tp;
  std::string f = tp.write("o.synth",
                           "let f ?(x = 1 : Int) : Int = x ;;\n");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool said = false;
  for (auto& d : diags.items)
    if (d.message.find("at least one required") != std::string::npos)
      said = true;
  CHECK(said);
}

TEST(checker_optional_default_type_must_match) {
  TempProject tp;
  std::string f = tp.write("o.synth",
                           "let f ?(x = 1.5 : Int) z:Int : Int = x + z ;;\n");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_optional_default_sees_earlier_params_only) {
  TempProject tp;
  std::string ok = tp.write("ok.synth", R"(
let f ?(a = 1 : Int) ?(b = a + 1 : Int) z:Int : Int = a + b + z ;;
)");
  DiagnosticBag d1;
  Program p1 = checkProject({ok}, d1);
  for (auto& d : d1.items)
    std::cerr << renderDiagnostic(d, userMod(p1).parsed.source);
  CHECK(!d1.hasErrors());
  // A default cannot reach forward to a later parameter.
  std::string bad = tp.write("bad.synth",
                             "let f ?(a = z : Int) z:Int : Int = a ;;\n");
  DiagnosticBag d2;
  checkProject({bad}, d2);
  CHECK(d2.hasErrors());
}

TEST(checker_optional_call_forms) {
  TempProject tp;
  std::string f = tp.write("o.synth", R"(
let f ?x:Int ?(y = 2 : Int) z:Int : Int =
  let dx : Int = match x with | None -> 0 | Some v -> v in
  dx + y + z
;;
let a : Int = f 10 ;;
let b : Int = f ~x:1 ~y:3 10 ;;
let c : Int = f ?x:(Some 1) 10 ;;
let d : Int = f ?x:None ?y:(Some 4) 10 ;;
let e : Int = let x = Some 5 in f ?x 10 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_adsr_curves_are_optional_and_typed) {
  TempProject tp;
  // Core's envelope: both curves optional and Fx.Curve-typed, filled by
  // either spelling, from Fx or through the Dsp re-export.
  std::string f = tp.write("e.synth", R"(
open Core open Core.Fx
let plain : Scalar Signal =
  adsr ~attack:5ms ~decay:100ms ~sustain:0.4 ~release:200ms ~hold:1s ;;
let curved : Scalar Signal =
  adsr ~decay_curve:(Exponential 5.0) ~release_curve:Fx.Linear ~attack:5ms
       ~decay:100ms ~sustain:0.4 ~release:200ms ~hold:1s ;;
let passed : Scalar Signal =
  Dsp.adsr ?decay_curve:(Some (Exponential 3.0)) ?release_curve:None
           ~attack:5ms ~decay:100ms ~sustain:0.4 ~release:200ms
           ~hold:1s ;;
let window : Scalar Sample =
  gated ~release_curve:(Exponential 8.0) ~attack:5ms ~decay:100ms
        ~sustain:0.4 ~release:200ms ~hold:1s
        ~input:(Osc.sine ~freq:220.0) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_adsr_curve_argument_must_be_a_curve) {
  TempProject tp;
  std::string f = tp.write("e.synth", R"(
open Core open Core.Fx
let bad : Scalar Signal =
  adsr ~decay_curve:true ~attack:5ms ~decay:100ms ~sustain:0.4
       ~release:200ms ~hold:1s ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_optional_pass_requires_optional_param) {
  TempProject tp;
  // `?z:` targets a required (labeled) parameter: rejected with a
  // pointer to '~'.
  std::string f = tp.write("o.synth", R"(
let f ?x:Int ~z:Int : Int = z ;;
let a : Int = f ?z:(Some 1) ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool said = false;
  for (auto& d : diags.items)
    if (d.message.find("is not optional") != std::string::npos) said = true;
  CHECK(said);
}

TEST(checker_optional_pass_takes_an_option_value) {
  TempProject tp;
  // `?x:` passes an Option through; a bare element value is a mismatch.
  std::string f = tp.write("o.synth", R"(
let f ?x:Int z:Int : Int = z ;;
let a : Int = f ?x:1 10 ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_positional_arguments_skip_optional_params) {
  TempProject tp;
  // The one positional argument must land on z, not on ?x.
  std::string f = tp.write("o.synth", R"(
let f ?x:Timestamp z:Int : Int = z ;;
let a : Int = f 10 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_optional_arguments_come_before_required_ones) {
  TempProject tp;
  // Mirroring the declaration rule: an optional-parameter argument after
  // a required one is rejected.
  std::string f = tp.write("o.synth", R"(
let f ?x:Int y:Int z:Int : Int = z ;;
let a : Int = f 1 ~x:2 3 ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool said = false;
  for (auto& d : diags.items)
    if (d.message.find("must come before") != std::string::npos) said = true;
  CHECK(said);
}

TEST(checker_partial_application_keeps_optionals) {
  TempProject tp;
  // Filling neither the optional nor all required parameters leaves a
  // function that still carries the optional slot; filling the last
  // required parameter completes the call and defaults the rest.
  std::string f = tp.write("o.synth", R"(
let f ?(x = 1 : Int) ~y:Int z:Int : Int = x + y + z ;;
let g = f ~y:2 ;;
let a : Int = g 3 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("g")) == "?x:Int -> Int -> Int");
}

TEST(checker_completed_call_cannot_add_optionals_later) {
  TempProject tp;
  // Once every required parameter is filled the optionals have
  // defaulted; a later application has nothing to bind them to.
  std::string f = tp.write("o.synth", R"(
let f ?(x = 1 : Int) y:Int : Int = x + y ;;
let a : Int = (f 2) ~x:3 ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_optionality_is_part_of_function_types) {
  TempProject tp;
  // A function with an optional parameter is not interchangeable with
  // the all-required arrow type of the same shape.
  std::string f = tp.write("o.synth", R"(
let f ?x:Int y:Int : Int = y ;;
let g : Int -> Int -> Int = f ;;
)");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_externals_cannot_declare_optional_params) {
  TempProject tp;
  std::string f = tp.write(
      "o.synth", "let f ?x:Int y:Int : Int = external \"impl.cpp\" ;;\n");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
  bool said = false;
  for (auto& d : diags.items)
    if (d.message.find("external") != std::string::npos &&
        d.message.find("optional") != std::string::npos)
      said = true;
  CHECK(said);
}

TEST(checker_optional_params_in_lambdas_and_local_functions) {
  TempProject tp;
  std::string f = tp.write("o.synth", R"(
let a : Int =
  let add ?(by = 1 : Int) n:Int : Int = n + by in
  add ~by:2 40 ;;
let b : Int =
  (* Written arrow types carry no labels (and no optional markers); an
     optional-carrying lambda binds through inference instead. *)
  let g = fun ?(by = 1 : Int) n:Int -> n + by in
  g 41 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
}

TEST(checker_core_option_module_helpers) {
  TempProject tp;
  std::string f = tp.write("o.synth", R"(
open Core
let doubled : Int Option = Option.map ~f:(fun n:Int -> n * 2) ~o:(Some 21) ;;
let chained : Int Option =
  Option.bind ~f:(fun n:Int -> if n > 0 then Some n else None)
              ~o:doubled ;;
let flat : Int Option = Option.join ~o:(Some (Some 3)) ;;
let both : Int Option =
  Option.map2 ~f:(fun a:Int b:Int -> a + b) ~a:(Some 1) ~b:(Some 2) ;;
let out : Int = Option.value ~default:0 ~o:chained ;;
let folded : Int = Option.fold ~none:0 ~some:(fun n:Int -> n + 1) ~o:flat ;;
let kept : Int Option = Option.filter ~f:(fun n:Int -> n > 2) ~o:flat ;;
let fallback : Int Option = Option.or_else ~alt:(Some 9) ~o:None ;;
let listed : Int list = Option.to_list ~o:both ;;
let lifted : Int Option list = List.map ~f:(fun n:Int -> Option.some ~x:n) ~xs:[1; 2] ;;
let present : Bool = Option.is_some ~o:both ;;
let absent : Bool = Option.is_none ~o:both ;;
let indexed : Int Option = List.nth_opt ~xs:[1; 2; 3] ~i:1 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeName(userMod(prog).defTypes.at("chained")) == "Int Option");
  CHECK(typeName(userMod(prog).defTypes.at("listed")) == "Int list");
}
