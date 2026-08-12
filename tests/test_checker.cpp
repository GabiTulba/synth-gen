#include <unistd.h>
#include <functional>

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
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(userModCount(prog) == 1);
  const auto& types = userMod(prog).defTypes;
  CHECK(typeEquals(types.at("song"), tSignal(tScalar())));
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
      tp.write("bad.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet a : Scalar Signal = sine 440.0 + 1s ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
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
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet f amp:Scalar freq:Scalar : Scalar Signal = "
           "sine freq * amp ;;");
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
             "let gain : Scalar = Internal.base * 2.0 ;;\n"
             "let strike freq:Scalar : Scalar Signal = sine freq * gain ;;\n");
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
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Bottom\nlet mid : Scalar = Bottom.base * 2.0 ;;\n");
  tp.write("chain/top.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Middle\nlet top : Scalar = Middle.mid + 1.0 ;;\n");
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
             "let slap s:Scalar Signal : Scalar Signal = Delay.tap s * 0.5 ;;\n");
    tp.write("fx/delay.synth",
             "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Taps\n"
             "let tap s:Scalar Signal : Scalar Signal = s * Taps.spread ;;\n");
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
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nopen Basic.Keys\nlet s : Scalar Signal = strike 440.0 * gain ;;\n");
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
  CHECK(typeEquals(song_m->defTypes.at("own"), tSignal(tScalar())));
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
let xs : Scalar list = Core.List.map (fun x:Scalar -> x * 2.0) [1.0] ;;
let ys : Scalar list = Core.List.init ~n:3 ~f:(fun i:Int -> Core.Math.to_scalar i) ;;
let zs : Scalar list = Core.List.repeat 2 5.0 ;;
let t : Scalar = Core.List.fold (fun a:Scalar b:Scalar -> a + b) 0.0 zs ;;
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
  mix_all (List.init ~n:3 ~f:(fun i:Int -> sine (110.0 * (to_scalar i + 1.0)))) ;;
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
let xs : Scalar list = map (fun x:Scalar -> x + 1.0) (repeat 3 0.0) ;;
let t : Scalar = fold (fun a:Scalar b:Scalar -> a + b) 0.0 (init 3 (fun i:Int -> to_scalar i)) ;;
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

TEST(checker_empty_list_rejected) {
  TempProject tp;
  std::string f =
      tp.write("bad.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet xs : Scalar list = [] ;;");
  DiagnosticBag diags;
  checkProject({f}, diags);
  CHECK(diags.hasErrors());
}

TEST(checker_modulation_primitives) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let vibrato : Scalar Signal = fm 440.0 ((sine 5.0) * 20.0) ;;
let bell : Scalar Signal = pm 440.0 ((sine 220.0) * 3.0) ;;
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
  CHECK(typeEquals(userMod(prog).defTypes.at("wide"), tSignal(tVector())));
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
  mix_all [dry; (delay 250ms dry) * 0.5; (delay 500ms dry) * 0.25] ;;
let wide : Vector Signal = delay 10ms (channels [sine 440.0; sine 442.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("wide"), tSignal(tVector())));

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
let dry : Scalar Signal = (sine 440.0) * (exp_decay 6.0) ;;
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
  CHECK(typeEquals(userMod(prog).defTypes.at("hall"), tSignal(tVector())));

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
let snare : Scalar Signal = (noise 1800.0) * (exp_decay 25.0) ;;
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
let crunchy : Scalar Signal = hard_clip 0.6 ((sine 220.0) * 2.0) ;;
let warm : Scalar Signal = soft_clip 0.8 ((saw 110.0) * 3.0) ;;
let wide : Vector Signal =
  soft_clip 0.5 (channels [sine 440.0; sine 442.0]) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("wide"), tSignal(tVector())));

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
let hit : Scalar Sample = sample ((sine 440.0) * (exp_decay 8.0)) 0s 200ms ;;
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
  CHECK(typeEquals(userMod(prog).defTypes.at("wide"), tSignal(tVector())));

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
let voice ~amp:Scalar ~freq:Scalar : Scalar Signal = (sine freq) * amp ;;
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
let voice ~amp:Scalar ~freq:Scalar : Scalar Signal = (sine freq) * amp ;;
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
  CHECK(typeEquals(userMod(prog).defTypes.at("xs"),
                   tList(tSignal(tScalar()))));
}

TEST(checker_computed_callee) {
  TempProject tp;
  // Any Fun-typed expression can be applied, not just a name: nested
  // partial applications, polymorphic primitive partials with no
  // annotated binding in between, and function-typed parameters.
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let add a:Scalar b:Scalar : Scalar = a + b ;;
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
  CHECK(typeEquals(userMod(prog).defTypes.at("damped"),
                   tSignal(tScalar())));
  // A non-function expression still cannot be applied.
  std::string g = tp.write("bad1.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = (1.0) 2.0 ;;");
  DiagnosticBag d1;
  checkProject({g}, d1);
  CHECK(d1.hasErrors());
  // Over-application through a computed callee still errors.
  std::string h = tp.write("bad2.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
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
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
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
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("song"), tSignal(tScalar())));
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
  List.map (fun gain:Scalar -> gain * base) [1.0; 2.0] ;;
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
  CHECK(typeEquals(userMod(prog).defTypes.at("warm"), tSignal(tScalar())));
  CHECK(typeEquals(userMod(prog).defTypes.at("s"), tSample(tScalar())));
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
let harmonic i:Int : Scalar Signal = sine (110.0 * (to_scalar i + 1.0)) ;;
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
  CHECK(typeEquals(userMod(prog).defTypes.at("stack"),
                   tList(tSignal(tScalar()))));
  CHECK(typeEquals(userMod(prog).defTypes.at("beats"),
                   tList(tTimestamp())));
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
  CHECK(typeEquals(userMod(prog).defTypes.at("song"), tSignal(tScalar())));
}

TEST(checker_let_in_shadowing_and_scope) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
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

TEST(checker_let_in_function_binding) {
  TempProject tp;
  // A local function definition: positional and labeled parameters,
  // called directly and via a label-curried partial application.
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let song : Scalar Signal =
  let pluck freq:Scalar ~gain:Scalar : Scalar Signal = (sine freq) * gain in
  let quiet : Scalar -> Scalar Signal = pluck ~gain:0.25 in
  pluck 440.0 ~gain:0.5 + quiet 330.0
;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("song"), tSignal(tScalar())));
}

TEST(checker_let_in_function_body_mismatch) {
  TempProject tp;
  // The local function's body must match its declared return type.
  std::string f = tp.write(
      "bad.synth",
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = let f y:Scalar : Timestamp = y + 1.0 in 2.0 ;;");
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
let a : Scalar = let f y:Scalar : Scalar = y + 1.0 in f y ;;
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
  let boost x:'a Signal : 'a Signal = x * 2.0 in
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
let fade : Scalar Signal = signal ~f:(fun t:Scalar -> exp (0.0 - 3.0 * t)) ;;
let wide : Vector Signal =
  signal_multi ~fs:[(fun t:Scalar -> t); (fun t:Scalar -> 1.0 - t)] ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("dc"), tSignal(tScalar())));
  CHECK(typeEquals(userMod(prog).defTypes.at("pair"), tSignal(tVector())));
  CHECK(typeEquals(userMod(prog).defTypes.at("ramp"), tSignal(tScalar())));
  CHECK(typeEquals(userMod(prog).defTypes.at("fade"), tSignal(tScalar())));
  CHECK(typeEquals(userMod(prog).defTypes.at("wide"), tSignal(tVector())));
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
let fade : Scalar Signal = exp (0.0 - time) ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("e"), tScalar()));
  CHECK(typeEquals(userMod(prog).defTypes.at("shaped"),
                   tSignal(tScalar())));
  CHECK(typeEquals(userMod(prog).defTypes.at("curve"), tSignal(tScalar())));
}

TEST(checker_timestamp_conversions) {
  TempProject tp;
  std::string f = tp.write("t.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let bpm : Scalar = 120.0 ;;
let beat : Timestamp = to_min (1.0 / bpm) ;;
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
  CHECK(typeEquals(types.at("steps"), tList(tTimestamp())));
}

TEST(checker_timestamp_conversion_type_errors) {
  // The argument is a Scalar - a Timestamp does not round-trip back
  // through the conversions, and the result is not a Scalar either.
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

TEST(checker_resample_typing) {
  TempProject tp;
  std::string f = tp.write("ok.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let warped : Scalar Signal = resample (saw 110.0) ~f:(fun t:Scalar -> 1.0 + t) ;;
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
  CHECK(typeEquals(userMod(prog).defTypes.at("warped"), tSignal(tScalar())));
  CHECK(typeEquals(userMod(prog).defTypes.at("piped"), tSignal(tScalar())));
  // The element type rides through: only the rate function is constrained.
  CHECK(typeEquals(userMod(prog).defTypes.at("wide"), tSignal(tVector())));
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
  CHECK(typeEquals(types.at("mono"), tSignal(tScalar())));
  CHECK(typeEquals(types.at("wide"), tSignal(tVector())));
  // The definition itself keeps its polymorphic signature.
  const TypePtr& dampen = types.at("dampen");
  CHECK(dampen->kind == Type::Kind::Fun);
  CHECK(containsRigidVar(dampen));
  CHECK(dampen->items[0]->elem->var == dampen->ret->elem->var);
}

TEST(checker_polymorphic_higher_order_definition) {
  TempProject tp;
  std::string f = tp.write("poly.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let twice ~f:('a -> 'a) ~x:'a : 'a = f (f x) ;;
let quad : Scalar = twice ~f:(fun n:Scalar -> n * 2.0) ~x:1.0 ;;
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
  CHECK(typeEquals(types.at("filtered"), tSignal(tScalar())));
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
  CHECK(typeEquals(userMod(prog).defTypes.at("layers"), tSignal(tScalar())));
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
  CHECK(typeEquals(m->defTypes.at("mono"), tSignal(tScalar())));
  CHECK(typeEquals(m->defTypes.at("wide"), tSignal(tVector())));
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
  CHECK(typeEquals(types.at("mono"), tSignal(tScalar())));
  CHECK(typeEquals(types.at("wide"), tSignal(tVector())));
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
  let up : Scalar = base * 2.0 ;;
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
  CHECK(typeEquals(types.at("Voices.lead"), tSignal(tScalar())));
  CHECK(types.count("Voices.Fx.damp") == 1);
  CHECK(userMod(prog).inlineModules.count("Voices") == 1);
  CHECK(userMod(prog).inlineModules.count("Voices.Fx") == 1);
  // The polymorphic member instantiates per use like any signature.
  CHECK(typeEquals(types.at("mono"), tSignal(tScalar())));
  CHECK(typeEquals(types.at("wide"), tSignal(tVector())));
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
let tone : Scalar Signal = (sine hz) * Sub.gain ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, prog.modules.empty() ? std::string{}
                                       : userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("tone"), tSignal(tScalar())));
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
  CHECK(typeEquals(m->defTypes.at("mono"), tSignal(tScalar())));
  CHECK(typeEquals(m->defTypes.at("wide"), tSignal(tVector())));
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
let fast : Bool = tempo >= 120.0 && not (tempo > 200.0) ;;
let late : Bool = 500ms < 1s || false ;;
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
  CHECK(typeEquals(types.at("s"), tSignal(tScalar())));
}

TEST(checker_if_branches_can_be_polymorphic) {
  TempProject tp;
  std::string f = tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let pick ~c:Bool ~a:'a ~b:'a : 'a = if c then a else b ;;
let n : Scalar = pick true 1.0 2.0 ;;
let t : Timestamp = pick (1.0 < 2.0) 250ms 500ms ;;
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
                           "let c : Bool = (sine 440.0) < 0.5 ;;\n"
                           "let d : Bool = 1.0 < 2.0 < 3.0 ;;\n"
                           "let e : Bool = true && 1.0 ;;\n"
                           "let g : Scalar = true + 1.0 ;;\n"
                           "let h : Bool = 1s == 1.0 ;;");
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
  if loud then render "a" 8000.0 (sample (s * 2.0) 0s 100ms)
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
  // ...nothing lives at the top level...
  for (auto& [name, type] : core->defTypes)
    CHECK(name.find('.') != std::string::npos);
  // ...and every definition is an external binding.
  std::function<bool(const std::vector<TopDef>&)> allExternal =
      [&](const std::vector<TopDef>& ds) {
        for (auto& d : ds) {
          if (d.kind == TopDef::Kind::ModuleDef && !allExternal(d.defs))
            return false;
          if (d.kind == TopDef::Kind::Let &&
              d.body->kind != Expr::Kind::External)
            return false;
        }
        return true;
      };
  CHECK(allExternal(core->parsed.defs));
}

TEST(checker_int_arithmetic_and_comparisons) {
  TempProject tp;
  std::string f = tp.write("i.synth", R"(
open Core open Core.Math
let n : Int = 3 + 4 * 2 ;;
let d : Int = 7 / 2 ;;
let neg : Int = -n ;;
let b : Bool = n < 12 && d == 3 ;;
let s : Scalar = to_scalar n * 1.5 ;;
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
  CHECK(typeEquals(types.at("xs"), tList(tInt())));
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
  CHECK(typeName(tList(tFun({tScalar()}, tScalar()))) ==
        "(Scalar -> Scalar) list");
  CHECK(typeName(tSignal(tFun({tScalar()}, tScalar()))) ==
        "(Scalar -> Scalar) Signal");

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
let out : Scalar Signal = v.osc * v.vel ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({f}, diags);
  for (auto& d : diags.items)
    std::cerr << renderDiagnostic(d, userMod(prog).parsed.source);
  CHECK(!diags.hasErrors());
  CHECK(typeEquals(userMod(prog).defTypes.at("out"), tSignal(tScalar())));
}
