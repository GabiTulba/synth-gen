#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>

#include "build.hpp"
#include "manifest_helpers.hpp"
#include "checker.hpp"
#include "incremental.hpp"
#include "library.hpp"
#include "test_framework.hpp"
#include "wav.hpp"

using namespace synth;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path dir;
  TempDir() {
    static int counter = 0;
    dir = fs::temp_directory_path() /
          ("synthgraph-build-test-" + std::to_string(::getpid()) + "-" +
           std::to_string(counter++));
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void write(const std::string& name, const std::string& text) {
    std::ofstream out(dir / name);
    out << text;
  }
};

// Like TempDir, but relative paths may contain directories (created on
// demand) - the fixture for library/root trees.
struct TempTree {
  fs::path dir;
  TempTree() {
    static int counter = 0;
    dir = fs::temp_directory_path() /
          ("synthgraph-tree-test-" + std::to_string(::getpid()) + "-" +
           std::to_string(counter++));
    fs::create_directories(dir);
  }
  ~TempTree() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void write(const std::string& rel, const std::string& text) {
    fs::path p = dir / rel;
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << text;
  }
};

std::string slurp(const fs::path& p) {
  std::ifstream in(p);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

const char* kPluckSource = R"(
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
)";

}  // namespace

TEST(build_manifest_parsing) {
  Manifest m;
  DiagnosticBag diags;
  bool ok = parseManifest(
      R"({ "project": "demo",
           "description": "free text is tolerated",
           "sources": ["a.synth", "b.synth"] })",
      "build.json", m, diags);
  CHECK(ok);
  CHECK(m.projectName == "demo");
  CHECK(m.sources.size() == 2);
  CHECK(m.sources[1] == "b.synth");
}

TEST(build_manifest_errors) {
  // Missing project name.
  Manifest m;
  DiagnosticBag diags;
  CHECK(!parseManifest(R"({ "sources": ["a.synth"] })", "build.json", m,
                       diags));
  // Unknown key.
  Manifest m2;
  DiagnosticBag diags2;
  CHECK(!parseManifest(R"({ "project": "x", "frobnicate": ["y"] })",
                       "build.json", m2, diags2));
  // Invalid JSON.
  Manifest m3;
  DiagnosticBag diags3;
  CHECK(!parseManifest("project x\nsource a.synth\n", "build.json", m3,
                       diags3));
  // Top level must be an object.
  Manifest m4;
  DiagnosticBag diags4;
  CHECK(!parseManifest(R"(["project", "x"])", "build.json", m4, diags4));
  // Wrong-typed fields.
  Manifest m5;
  DiagnosticBag diags5;
  CHECK(!parseManifest(R"({ "project": "x", "sources": "a.synth" })",
                       "build.json", m5, diags5));
  Manifest m6;
  DiagnosticBag diags6;
  CHECK(!parseManifest(R"({ "project": ["x"], "sources": ["a.synth"] })",
                       "build.json", m6, diags6));
  // Duplicate key.
  Manifest m7;
  DiagnosticBag diags7;
  CHECK(!parseManifest(
      R"({ "project": "x", "project": "y", "sources": ["a.synth"] })",
      "build.json", m7, diags7));
}

TEST(build_manifest_library_directives) {
  Manifest m;
  DiagnosticBag diags;
  bool ok = parseManifest(
      R"({ "library": "Basic", "dependencies": ["Fx"] })", "build.json", m,
      diags);
  CHECK(ok);
  CHECK(m.isLibrary());
  CHECK(m.libraryName == "Basic");
  // Members come from the directory, not the manifest.
  CHECK(m.sources.empty());
  CHECK(m.deps.size() == 1 && m.deps[0] == "Fx");
  CHECK(!m.isRoot());
}

TEST(build_manifest_rejects_project_and_library) {
  Manifest m;
  DiagnosticBag diags;
  CHECK(!parseManifest(R"({ "project": "x", "library": "Y" })", "build.json", m,
                       diags));
}

TEST(build_manifest_library_rules) {
  // `expose` is gone: a library's public surface lives in lib.synth.
  Manifest m1;
  DiagnosticBag d1;
  CHECK(!parseManifest(R"({ "library": "L", "expose": ["a.synth"] })",
                       "build.json", m1, d1));
  Manifest m2;
  DiagnosticBag d2;
  CHECK(!parseManifest(R"({ "project": "x", "expose": ["a.synth"] })",
                       "build.json", m2, d2));
  // A library does not list sources either.
  Manifest m3;
  DiagnosticBag d3;
  CHECK(!parseManifest(R"({ "library": "L", "sources": ["a.synth"] })",
                       "build.json", m3, d3));
  // Lowercase library name.
  Manifest m4;
  DiagnosticBag d4;
  CHECK(!parseManifest(R"({ "library": "basic" })", "build.json", m4, d4));
  // A bare library manifest is complete on its own.
  Manifest m5;
  DiagnosticBag d5;
  CHECK(parseManifest(R"({ "library": "L" })", "build.json", m5, d5));
}

TEST(build_manifest_root_build_rules) {
  Manifest m;
  DiagnosticBag diags;
  bool ok = parseManifest(
      R"({ "project": "demo", "build": ["lib/basic", "tunes/song.synth"] })",
      "build.json", m, diags);
  CHECK(ok);
  CHECK(m.isRoot());
  CHECK(m.buildRules.size() == 2);
  // Roots cannot also list sources.
  Manifest m2;
  DiagnosticBag d2;
  CHECK(!parseManifest(
      R"({ "project": "demo", "build": ["a"], "sources": ["b.synth"] })",
      "build.json", m2, d2));
}

TEST(library_discovery_finds_nested_builds) {
  TempTree tp;
  tp.write("build.json", rootManifest("root", {"tunes"}));
  tp.write("lib/basic/build.json", libraryManifest("Basic"));
  tp.write("lib/basic/lib.synth", libraryInterface({"Keys"}));
  tp.write("lib/basic/keys.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet k : Scalar = 1.0 ;;");
  tp.write("deep/nested/fx/build.json", libraryManifest("Fx", {"Basic"}));
  tp.write("deep/nested/fx/lib.synth", libraryInterface({"Fx"}));
  tp.write("deep/nested/fx/fx.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet f : Scalar = 2.0 ;;");
  DiagnosticBag diags;
  LibraryRegistry reg = discoverLibraries(tp.dir.string(), diags);
  for (auto& d : diags.items) std::cerr << d.message << "\n";
  CHECK(!diags.hasErrors());
  CHECK(reg.byName.size() == 2);
  const LibraryInfo* basic = reg.find("Basic");
  CHECK(basic != nullptr);
  CHECK(fs::path(basic->dir) == tp.dir / "lib" / "basic");
  CHECK(basic->fileForModule("Keys") == "keys.synth");
  CHECK(basic->hasInterface);
  // lib.synth is the interface, not a member.
  CHECK(basic->files.size() == 1);
  const LibraryInfo* fx = reg.find("Fx");
  CHECK(fx != nullptr);
  CHECK(fx->deps.size() == 1 && fx->deps[0] == "Basic");
}

TEST(library_discovery_skips_build_output_dirs) {
  TempTree tp;
  tp.write("lib/build.json", libraryManifest("L"));
  tp.write("lib/lib.synth", libraryInterface({"A"}));
  tp.write("lib/a.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet a : Scalar = 1.0 ;;");
  // A stray manifest inside an output dir must not register ("_build"
  // outputs and legacy/cmake "build" dirs alike).
  tp.write("lib/build/build.json", libraryManifest("Ghost"));
  tp.write("lib/build/lib.synth", libraryInterface({"G"}));
  tp.write("_build/lib/build.json", libraryManifest("Shadow"));
  tp.write("_build/lib/lib.synth", libraryInterface({"S"}));
  DiagnosticBag diags;
  LibraryRegistry reg = discoverLibraries(tp.dir.string(), diags);
  CHECK(!diags.hasErrors());
  CHECK(reg.byName.size() == 1);
  CHECK(reg.find("Ghost") == nullptr);
  CHECK(reg.find("Shadow") == nullptr);
}

TEST(library_discovery_duplicate_names_error) {
  TempTree tp;
  tp.write("a/build.json", libraryManifest("Same"));
  tp.write("a/lib.synth", libraryInterface({"X"}));
  tp.write("b/build.json", libraryManifest("Same"));
  tp.write("b/lib.synth", libraryInterface({"Y"}));
  DiagnosticBag diags;
  discoverLibraries(tp.dir.string(), diags);
  CHECK(diags.hasErrors());
}

TEST(library_registry_unknown_dep_error) {
  TempTree tp;
  tp.write("a/build.json", libraryManifest("A", {"Nope"}));
  tp.write("a/lib.synth", libraryInterface({"X"}));
  DiagnosticBag diags;
  discoverLibraries(tp.dir.string(), diags);
  CHECK(diags.hasErrors());
}

TEST(library_registry_dep_cycle_error) {
  TempTree tp;
  tp.write("a/build.json", libraryManifest("A", {"B"}));
  tp.write("a/lib.synth", libraryInterface({"X"}));
  tp.write("b/build.json", libraryManifest("B", {"A"}));
  tp.write("b/lib.synth", libraryInterface({"Y"}));
  DiagnosticBag diags;
  discoverLibraries(tp.dir.string(), diags);
  CHECK(diags.hasErrors());
}

TEST(library_find_enclosing_root) {
  TempTree tp;
  tp.write("build.json", rootManifest("root", {"tunes"}));
  tp.write("tunes/build.json", projectManifest("tunes", {"t.synth"}));
  tp.write("tunes/t.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = 1.0 ;;");
  CHECK(fs::path(findEnclosingRoot((tp.dir / "tunes").string())) == tp.dir);
  // No root above a bare temp dir tree.
  TempTree lone;
  lone.write("p/build.json", projectManifest("p", {"a.synth"}));
  CHECK(findEnclosingRoot((lone.dir / "p").string()).empty());
}

TEST(build_end_to_end_pluck) {
  // The full §3.4 example: two seconds of audio, four 440 Hz plucks.
  TempDir tp;
  tp.write("pluck.synth", kPluckSource);
  tp.write("build.json", projectManifest("pluck-demo", {"pluck.synth"}));

  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  CHECK(r.targets[0].name == "demo");
  CHECK(r.targets[0].ok);
  CHECK(r.targets[0].channelCount == 1);
  CHECK(r.targets[0].frames == 96000);  // 2s at 48 kHz
  CHECK_NEAR(r.targets[0].durationSeconds, 2.0, 1e-9);

  fs::path artifact = tp.dir / "_build" / "artifacts" / "demo.wav";
  CHECK(fs::exists(artifact));
  WavData w = readWav(artifact.string());
  CHECK(w.frames() == 96000);
  CHECK_NEAR(w.rate, 48000.0, 1e-9);
  // Audio is non-silent at each pluck onset region and decays between.
  auto peakAround = [&](double t) {
    int64_t c = (int64_t)(t * 48000.0);
    double peak = 0;
    for (int64_t i = c; i < c + 2000 && i < w.frames(); i++)
      peak = std::max(peak, std::fabs(w.channels[0][(size_t)i]));
    return peak;
  };
  CHECK(peakAround(0.0) > 0.5);
  CHECK(peakAround(0.5) > 0.5);
  CHECK(peakAround(1.0) > 0.5);
  CHECK(peakAround(1.5) > 0.5);
  CHECK(peakAround(0.45) < 0.3);  // decayed before the next pluck

  // Build metadata exists and mentions the target.
  std::string meta = slurp(tp.dir / "_build" / "metadata.json");
  CHECK(meta.find("\"project\": \"pluck-demo\"") != std::string::npos);
  CHECK(meta.find("\"name\": \"demo\"") != std::string::npos);
  CHECK(meta.find("\"status\": \"ok\"") != std::string::npos);
}

TEST(build_duplicate_render_names_fail) {
  TempDir tp;
  tp.write("a.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = render "same" 48000.0 (sample (sine 440.0) 0s 100ms) ;;
let _ = render "same" 48000.0 (sample (sine 220.0) 0s 100ms) ;;
)");
  tp.write("build.json", projectManifest("dup", {"a.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  CHECK(r.diags.hasErrors());
}

TEST(build_type_error_fails_and_emits_metadata) {
  TempDir tp;
  tp.write("a.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = sine 440.0 ;;");
  tp.write("build.json", projectManifest("broken", {"a.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  std::string meta = slurp(tp.dir / "_build" / "metadata.json");
  CHECK(meta.find("\"status\": \"error\"") != std::string::npos);
}

TEST(build_imports_across_files) {
  TempDir tp;
  tp.write("instr.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let tone freq:Scalar : Scalar Signal = (sine freq) *. (exp_decay 3.0) ;;
)");
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
import Instr
let _ = render "song" 44100.0 (sample (Instr.tone 330.0) 0s 500ms) ;;
)");
  tp.write("build.json", projectManifest("imports", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  CHECK(fs::exists(tp.dir / "_build" / "artifacts" / "song.wav"));
}

TEST(build_load_mono_channel_validation) {
  TempDir tp;
  // Write a stereo wav, then load it with load_mono: build error.
  std::vector<double> interleaved;
  for (int i = 0; i < 100; i++) {
    interleaved.push_back(0.1);
    interleaved.push_back(-0.1);
  }
  writeWav((tp.dir / "stereo.wav").string(), 44100.0, 2, interleaved);
  tp.write("a.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet _ = render \"x\" 44100.0 "
           "(sample (load_mono \"stereo.wav\") 0s 10ms) ;;");
  tp.write("build.json", projectManifest("loads", {"a.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  CHECK(r.diags.hasErrors());

  // load_multi accepts it.
  TempDir tp2;
  writeWav((tp2.dir / "stereo.wav").string(), 44100.0, 2, interleaved);
  tp2.write("a.synth",
            "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet _ = render \"x\" 44100.0 "
            "(sample (load_multi \"stereo.wav\") 0s 10ms) ;;");
  tp2.write("build.json", projectManifest("loads2", {"a.synth"}));
  BuildResult r2 = buildProject(tp2.dir.string());
  for (auto& d : r2.diags.items) std::cerr << d.message << "\n";
  CHECK(r2.ok);
  WavData out = readWav((tp2.dir / "_build" / "artifacts" / "x.wav").string());
  CHECK(out.channels.size() == 2);
}

TEST(build_lint_mode) {
  TempDir tp;
  std::string good = (tp.dir / "good.synth").string();
  tp.write("good.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar Signal = sine 440.0 ;;");
  DiagnosticBag ok = lintFiles({good});
  CHECK(!ok.hasErrors());

  tp.write("bad.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = sine 440.0 ;;");
  DiagnosticBag bad = lintFiles({(tp.dir / "bad.synth").string()});
  CHECK(bad.hasErrors());
}

TEST(build_inputs_are_tracked) {
  TempDir tp;
  tp.write("instr.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet tone freq:Scalar : Scalar Signal = sine freq ;;");
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
import Instr
let _ = render "song" 44100.0 (sample (Instr.tone 330.0) 0s 100ms) ;;
)");
  tp.write("build.json", projectManifest("inputs", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(r.ok);
  auto has = [&](const std::string& suffix) {
    for (auto& i : r.inputs)
      if (i.size() >= suffix.size() &&
          i.compare(i.size() - suffix.size(), suffix.size(), suffix) == 0)
        return true;
    return false;
  };
  CHECK(has("build.json"));
  CHECK(has("song.synth"));
  CHECK(has("instr.synth"));  // discovered via import
}

TEST(build_watch_rebuilds_on_change) {
  TempDir tp;
  tp.write("a.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet _ = render \"t\" 8000.0 (sample (sine 440.0) 0s 10ms) ;;");
  tp.write("build.json", projectManifest("watch", {"a.synth"}));

  int builds = 0;
  bool changed = false;
  watchProject(
      tp.dir.string(),
      [&](const BuildResult& r) {
        builds++;
        CHECK(r.ok);
      },
      [&] {
        if (builds == 1 && !changed) {
          // Mutate the source after the initial build; ensure the mtime
          // moves even on coarse-grained filesystems.
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          tp.write("a.synth",
                   "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet _ = render \"t\" 8000.0 "
                   "(sample (saw 220.0) 0s 10ms) ;;");
          fs::last_write_time(tp.dir / "a.synth",
                              fs::file_time_type::clock::now() +
                                  std::chrono::seconds(2));
          changed = true;
        }
        return builds < 2;
      },
      10);
  CHECK(builds == 2);
}

TEST(build_modulation_end_to_end) {
  TempDir tp;
  tp.write("modul.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let vibrato : Scalar Signal = fm 440.0 ((sine 5.0) *. 20.0) ;;
let tremolo : Scalar Signal = am vibrato (sine 4.0) 0.5 ;;
let bell : Scalar Signal = pm 220.0 ((sine 110.0) *. 2.0) ;;
let _ = render "voice" 48000.0 (sample (tremolo *. 0.5 +. bell *. 0.3) 0s 250ms) ;;
)");
  tp.write("build.json", projectManifest("modulation", {"modul.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "voice.wav").string());
  CHECK(w.frames() == 12000);
  double peak = 0;
  for (double v : w.channels[0]) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.3);  // audibly non-silent
}

TEST(build_delay_echo_end_to_end) {
  TempDir tp;
  tp.write("echo.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Signal =
  place (sample ((sine 660.0) *. (exp_decay 30.0)) 0s 100ms) 0s ;;
let echoed : Scalar Signal =
  mix_all [hit; (delay 200ms hit) *. 0.5; (delay 400ms hit) *. 0.25] ;;
let _ = render "echo" 8000.0 (sample echoed 0s 600ms) ;;
)");
  tp.write("build.json", projectManifest("echo", {"echo.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "echo.wav").string());
  auto peakAround = [&](double t) {
    int64_t c = (int64_t)(t * 8000.0);
    double peak = 0;
    for (int64_t i = c; i < c + 400 && i < w.frames(); i++)
      peak = std::max(peak, std::fabs(w.channels[0][(size_t)i]));
    return peak;
  };
  double p0 = peakAround(0.0), p1 = peakAround(0.2), p2 = peakAround(0.4);
  CHECK(p0 > 0.5);
  // Each echo is roughly half the previous one.
  CHECK_NEAR(p1, p0 * 0.5, 0.1);
  CHECK_NEAR(p2, p0 * 0.25, 0.1);
  // Silence between hit and first echo (decay rate 30 kills it fast).
  CHECK(peakAround(0.15) < 0.05);
}

TEST(build_reverb_end_to_end) {
  TempDir tp;
  tp.write("verb.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Signal =
  place (sample ((sine 660.0) *. (exp_decay 40.0)) 0s 100ms) 0s ;;
let roomy : Scalar Signal = reverb 500ms 0.3 0.6 hit ;;
let _ = render "roomy" 8000.0 (sample roomy 0s 1s) ;;
)");
  tp.write("build.json", projectManifest("verb", {"verb.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "roomy.wav").string());
  auto peakAround = [&](double t0, double t1) {
    double peak = 0;
    for (int64_t i = (int64_t)(t0 * 8000.0);
         i < (int64_t)(t1 * 8000.0) && i < w.frames(); i++)
      peak = std::max(peak, std::fabs(w.channels[0][(size_t)i]));
    return peak;
  };
  CHECK(peakAround(0.0, 0.1) > 0.3);    // the hit itself
  // The dry hit is dead by 100ms (decay rate 40), but the reverb tail
  // keeps ringing, then fades out.
  CHECK(peakAround(0.2, 0.4) > 0.01);
  CHECK(peakAround(0.2, 0.4) > peakAround(0.7, 1.0) * 3.0);
}

TEST(build_noise_snare_end_to_end) {
  TempDir tp;
  tp.write("snare.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let snare : Scalar Signal = (noise 1800.0) *. (exp_decay 25.0) ;;
let _ = render "snare" 16000.0 (sample snare 0s 400ms) ;;
)");
  tp.write("build.json", projectManifest("snare", {"snare.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "snare.wav").string());
  CHECK(w.frames() == 6400);
  auto rmsIn = [&](double t0, double t1) {
    double acc = 0;
    int64_t a = (int64_t)(t0 * 16000.0), b = (int64_t)(t1 * 16000.0);
    for (int64_t i = a; i < b && i < w.frames(); i++)
      acc += w.channels[0][(size_t)i] * w.channels[0][(size_t)i];
    return std::sqrt(acc / (double)(b - a));
  };
  CHECK(rmsIn(0.0, 0.05) > 0.3);              // loud onset
  CHECK(rmsIn(0.3, 0.4) < rmsIn(0.0, 0.05) * 0.05);  // decayed away
}

TEST(build_cache_skips_unchanged_and_invalidates_across_modules) {
  TempDir tp;
  tp.write("instr.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet tone freq:Scalar : Scalar Signal = "
           "(sine freq) *. (exp_decay 6.0) ;;");
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
import Instr
let _ = render "uses_instr" 8000.0 (sample (Instr.tone 440.0) 0s 100ms) ;;
let _ = render "standalone" 8000.0 (sample ((saw 220.0) *. 0.5) 0s 100ms) ;;
)");
  tp.write("build.json", projectManifest("cachetest", {"song.synth"}));

  BuildCache cache;
  BuildResult first = buildProject(tp.dir.string(), &cache);
  CHECK(first.ok);
  for (auto& t : first.targets) CHECK(!t.cached);

  // No edits: everything reused.
  BuildResult second = buildProject(tp.dir.string(), &cache);
  CHECK(second.ok);
  for (auto& t : second.targets) CHECK(t.cached);

  // Edit the imported instrument: only the target depending on it
  // re-renders; the standalone target stays cached.
  tp.write("instr.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet tone freq:Scalar : Scalar Signal = "
           "(sine freq) *. (exp_decay 9.0) ;;");
  BuildResult third = buildProject(tp.dir.string(), &cache);
  CHECK(third.ok);
  for (auto& t : third.targets) {
    if (t.name == "uses_instr") CHECK(!t.cached);
    if (t.name == "standalone") CHECK(t.cached);
  }
}

TEST(build_cache_invalidates_on_audio_input_change) {
  TempDir tp;
  std::vector<double> quiet(400, 0.1), loud(400, 0.5);
  writeWav((tp.dir / "in.wav").string(), 8000.0, 1, quiet);
  tp.write("a.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet _ = render \"fromfile\" 8000.0 "
           "(sample (load_mono \"in.wav\") 0s 40ms) ;;");
  tp.write("build.json", projectManifest("audiocache", {"a.synth"}));

  BuildCache cache;
  BuildResult first = buildProject(tp.dir.string(), &cache);
  CHECK(first.ok);
  BuildResult second = buildProject(tp.dir.string(), &cache);
  CHECK(second.targets[0].cached);

  writeWav((tp.dir / "in.wav").string(), 8000.0, 1, loud);
  fs::last_write_time(tp.dir / "in.wav", fs::file_time_type::clock::now() +
                                             std::chrono::seconds(2));
  BuildResult third = buildProject(tp.dir.string(), &cache);
  CHECK(third.ok);
  CHECK(!third.targets[0].cached);
  // And the artifact really reflects the new file.
  WavData w =
      readWav((tp.dir / "_build" / "artifacts" / "fromfile.wav").string());
  CHECK(std::fabs(w.channels[0][10] - 0.5) < 0.01);
}

TEST(build_parallel_targets_all_render) {
  TempDir tp;
  tp.write("many.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = render "t1" 8000.0 (sample ((sine 220.0) *. 0.5) 0s 200ms) ;;
let _ = render "t2" 8000.0 (sample ((saw 220.0) *. 0.5) 0s 200ms) ;;
let _ = render "t3" 8000.0 (sample ((square 220.0) *. 0.5) 0s 200ms) ;;
let _ = render "t4" 8000.0 (sample ((noise 1000.0) *. 0.5) 0s 200ms) ;;
let _ = render "t5" 8000.0 (sample ((fm 110.0 ((sine 55.0) *. 50.0)) *. 0.5) 0s 200ms) ;;
let _ = render "t6" 8000.0 (sample ((reverb 200ms 0.4 0.5 ((sine 330.0) *. (exp_decay 10.0)))) 0s 200ms) ;;
)");
  tp.write("build.json", projectManifest("many", {"many.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 6);
  for (auto& t : r.targets) {
    CHECK(t.ok);
    CHECK(t.frames == 1600);
    WavData w = readWav((tp.dir / t.artifact).string());
    double peak = 0;
    for (double v : w.channels[0]) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.05);  // every target produced real audio
  }
}

TEST(build_parallel_matches_serial_output) {
  // Same project built fresh twice (parallel rendering both times) must
  // produce byte-identical artifacts: evaluation is deterministic and
  // per-render state is isolated per target.
  auto makeProject = [](TempDir& tp) {
    tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let voice : Scalar Signal = fm 220.0 ((sine 3.0) *. 12.0) ;;
let _ = render "a" 8000.0 (sample ((lowpass 900.0 voice) *. 0.6) 0s 300ms) ;;
let _ = render "b" 8000.0 (sample ((delay 50ms voice) *. 0.4) 0s 300ms) ;;
)");
    tp.write("build.json", projectManifest("det", {"p.synth"}));
  };
  TempDir one, two;
  makeProject(one);
  makeProject(two);
  CHECK(buildProject(one.dir.string()).ok);
  CHECK(buildProject(two.dir.string()).ok);
  for (const char* name : {"a.wav", "b.wav"}) {
    std::string x = slurp(one.dir / "_build" / "artifacts" / name);
    std::string y = slurp(two.dir / "_build" / "artifacts" / name);
    CHECK(!x.empty());
    CHECK(x == y);
  }
}

TEST(build_watch_uses_incremental_cache) {
  TempDir tp;
  tp.write("w.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = render "one" 8000.0 (sample ((sine 440.0) *. 0.5) 0s 50ms) ;;
let _ = render "two" 8000.0 (sample ((saw 110.0) *. 0.5) 0s 50ms) ;;
)");
  tp.write("build.json", projectManifest("watchcache", {"w.synth"}));

  int builds = 0;
  bool touched = false;
  int cachedInSecondBuild = -1;
  watchProject(
      tp.dir.string(),
      [&](const BuildResult& r) {
        builds++;
        if (builds == 2) {
          cachedInSecondBuild = 0;
          for (auto& t : r.targets)
            if (t.cached) cachedInSecondBuild++;
        }
      },
      [&] {
        if (builds == 1 && !touched) {
          // Touch the manifest (a comment): sources unchanged, so both
          // targets should come from the cache on the rebuild.
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          tp.write("build.json", projectManifest("watchcache", {"w.synth"}));
          fs::last_write_time(tp.dir / "build.json",
                              fs::file_time_type::clock::now() +
                                  std::chrono::seconds(2));
          touched = true;
        }
        return builds < 2;
      },
      10);
  CHECK(builds == 2);
  CHECK(cachedInSecondBuild == 2);
}

TEST(build_render_vis_writes_svg_artifact) {
  TempDir tp;
  tp.write("v.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let tone : Scalar Signal = (sine 440.0) *. (exp_decay 6.0) ;;
let _ = render "tone" 8000.0 (sample tone 0s 500ms) ;;
let _ = render_vis "tone-wave" 8000.0 (sample tone 0s 500ms) ;;
)");
  tp.write("build.json", projectManifest("vis", {"v.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 2);

  const TargetInfo* wav = nullptr;
  const TargetInfo* svg = nullptr;
  for (auto& t : r.targets) {
    if (t.name == "tone") wav = &t;
    if (t.name == "tone-wave") svg = &t;
  }
  CHECK(wav && wav->kind == "audio");
  CHECK(svg && svg->kind == "visual");
  CHECK(svg->artifact == "_build/artifacts/tone-wave.svg");
  CHECK(svg->frames == 4000);

  std::string content = slurp(tp.dir / svg->artifact);
  CHECK(content.find("<svg") == 0);
  CHECK(content.find("tone-wave") != std::string::npos);
  CHECK(content.find("<path") != std::string::npos);
  CHECK(content.find("0.500s @ 8000 Hz, 1 channel") != std::string::npos);

  // Metadata carries the kind so the dev app can tell them apart.
  std::string meta = slurp(tp.dir / "_build" / "metadata.json");
  CHECK(meta.find("\"kind\": \"visual\"") != std::string::npos);
  CHECK(meta.find("\"kind\": \"audio\"") != std::string::npos);
}

TEST(build_render_vis_multichannel_lanes) {
  TempDir tp;
  tp.write("st.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = render_vis "stereo-wave" 4000.0
  (sample (channels [sine 220.0; sine 224.0]) 0s 1s) ;;
)");
  tp.write("build.json", projectManifest("visst", {"st.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(r.ok);
  std::string content = slurp(tp.dir / r.targets[0].artifact);
  CHECK(content.find("2 channels") != std::string::npos);
  // Two waveform lanes -> two path elements.
  size_t first = content.find("<path");
  CHECK(first != std::string::npos);
  CHECK(content.find("<path", first + 1) != std::string::npos);
}

TEST(build_render_and_render_vis_share_namespace) {
  TempDir tp;
  tp.write("dup.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = render "same" 8000.0 (sample (sine 440.0) 0s 100ms) ;;
let _ = render_vis "same" 8000.0 (sample (sine 440.0) 0s 100ms) ;;
)");
  tp.write("build.json", projectManifest("visdup", {"dup.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  CHECK(r.diags.hasErrors());
}

TEST(build_distortion_end_to_end) {
  TempDir tp;
  tp.write("dist.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hot : Scalar Signal = (sine 220.0) *. 3.0 ;;
let _ = render "hard" 8000.0 (sample (hard_clip 0.5 hot) 0s 250ms) ;;
let _ = render "soft" 8000.0 (sample (soft_clip 0.5 hot) 0s 250ms) ;;
)");
  tp.write("build.json", projectManifest("dist", {"dist.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData hard = readWav((tp.dir / "_build" / "artifacts" / "hard.wav").string());
  WavData soft = readWav((tp.dir / "_build" / "artifacts" / "soft.wav").string());
  double hardPeak = 0, softPeak = 0;
  int flatHard = 0;
  for (int64_t i = 0; i < hard.frames(); i++) {
    double h = std::fabs(hard.channels[0][(size_t)i]);
    hardPeak = std::max(hardPeak, h);
    if (h > 0.499) flatHard++;
    softPeak = std::max(softPeak, std::fabs(soft.channels[0][(size_t)i]));
  }
  CHECK_NEAR(hardPeak, 0.5, 0.01);
  CHECK(softPeak <= 0.5 + 0.01);
  // A heavily hard-clipped sine spends a large share of its period flat
  // against the rails; tanh soft clip does not sit flat.
  CHECK(flatHard > hard.frames() / 4);
}

TEST(build_place_multi_matches_mixed_places) {
  // place_multi must be exactly mix_all of individual placements -
  // byte-identical artifacts.
  auto write = [](TempDir& tp, const char* body) {
    tp.write("p.synth", body);
    tp.write("build.json", projectManifest("pm", {"p.synth"}));
  };
  TempDir multi, manual;
  write(multi, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sample ((sine 660.0) *. (exp_decay 15.0)) 0s 150ms ;;
let _ = render "out" 8000.0
  (sample (place_multi hit [0s; 200ms; 400ms]) 0s 700ms) ;;
)");
  write(manual, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sample ((sine 660.0) *. (exp_decay 15.0)) 0s 150ms ;;
let _ = render "out" 8000.0
  (sample (mix_all [place hit 0s; place hit 200ms; place hit 400ms])
   0s 700ms) ;;
)");
  CHECK(buildProject(multi.dir.string()).ok);
  CHECK(buildProject(manual.dir.string()).ok);
  std::string a = slurp(multi.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(manual.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_timestamp_conversions_match_literals) {
  // to_sec/to_ms/to_min must land on exactly the same instants as the
  // lexer's unit suffixes, so a render driven by computed timestamps is
  // byte-identical to the same render written with literals.
  auto write = [](TempDir& tp, const char* body) {
    tp.write("t.synth", body);
    tp.write("build.json", projectManifest("ts", {"t.synth"}));
  };
  TempDir computed, literal;
  write(computed, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sample (sine 660.0 *. exp_decay 20.0) 0s (to_ms 200.0) ;;
let _ = place_multi hit (time_steps ~start:(to_ms 250.0)
                                    ~step:(to_min (1.0 /. 120.0)) ~count:3)
  |> sample ~from:(to_sec 0.0) ~to:(to_sec 2.0)
  |> render ~name:"out" ~rate:8000.0 ;;
)");
  write(literal, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sample (sine 660.0 *. exp_decay 20.0) 0s 200ms ;;
let _ = place_multi hit (time_steps ~start:250ms ~step:500ms ~count:3)
  |> sample ~from:0s ~to:2s
  |> render ~name:"out" ~rate:8000.0 ;;
)");
  CHECK(buildProject(computed.dir.string()).ok);
  CHECK(buildProject(literal.dir.string()).ok);
  std::string a = slurp(computed.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(literal.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_timestamp_arithmetic_matches_literals) {
  // Timestamp arithmetic must land on exactly the same instants as the
  // literals it replaces, so a render whose grid is derived from a tempo
  // is byte-identical to the same render written out by hand. This is
  // the whole point of the rule: musical time expressed as musical time.
  auto write = [](TempDir& tp, const char* body) {
    tp.write("t.synth", body);
    tp.write("build.json", projectManifest("ta", {"t.synth"}));
  };
  TempDir derived, literal;
  write(derived, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let beat : Timestamp = to_min (1.0 /. 120.0) ;;
let bar : Timestamp = beat *. 4.0 ;;
let hit : Scalar Sample =
  sample (sine 660.0 *. exp_decay 20.0) 0s (beat /. 2.0) ;;
let _ = place_multi hit (time_steps ~start:(bar -. beat) ~step:beat ~count:3)
  |> sample ~from:0s ~to:(bar +. bar)
  |> render ~name:"out" ~rate:8000.0 ;;
)");
  write(literal, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sample (sine 660.0 *. exp_decay 20.0) 0s 250ms ;;
let _ = place_multi hit (time_steps ~start:1500ms ~step:500ms ~count:3)
  |> sample ~from:0s ~to:4s
  |> render ~name:"out" ~rate:8000.0 ;;
)");
  CHECK(buildProject(derived.dir.string()).ok);
  CHECK(buildProject(literal.dir.string()).ok);
  std::string a = slurp(derived.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(literal.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_timestamp_subtraction_clamps_at_the_epoch) {
  // The timeline starts at 0s: subtracting past it clamps rather than
  // producing a negative instant, so an early-shifted placement lands on
  // the epoch instead of failing or wrapping.
  auto write = [](TempDir& tp, const char* body) {
    tp.write("t.synth", body);
    tp.write("build.json", projectManifest("tc", {"t.synth"}));
  };
  TempDir clamped, epoch;
  write(clamped, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sample (sine 440.0 *. exp_decay 12.0) 0s 200ms ;;
let _ = place hit (100ms -. 900ms)
  |> sample ~from:0s ~to:500ms
  |> render ~name:"out" ~rate:8000.0 ;;
)");
  write(epoch, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sample (sine 440.0 *. exp_decay 12.0) 0s 200ms ;;
let _ = place hit 0s
  |> sample ~from:0s ~to:500ms
  |> render ~name:"out" ~rate:8000.0 ;;
)");
  CHECK(buildProject(clamped.dir.string()).ok);
  CHECK(buildProject(epoch.dir.string()).ok);
  std::string a = slurp(clamped.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(epoch.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_list_combinators_compute_the_right_values) {
  // The combinators are written in SynthGraph, so nothing but a render
  // pins their *values*. Every result is folded into one frequency and
  // one placement grid: a wrong length, a wrong index, a zip that did
  // not truncate, or a rev that did not reverse all move the artifact.
  auto write = [](TempDir& tp, const char* body) {
    tp.write("l.synth", body);
    tp.write("build.json", projectManifest("lc", {"l.synth"}));
  };
  TempDir derived, literal;
  write(derived, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let big : Scalar list =
  List.filter ~f:(fun x:Scalar -> x >. 0.5) ~xs:[0.0; 1.0; 0.25; 2.0; 3.0] ;;
let n : Int = List.length ~xs:big ;;
let total : Scalar = List.sum ~xs:big ;;
let top : Scalar = List.maximum ~xs:big ~least:0.0 ;;
let pick : Scalar = List.nth ~xs:big ~i:1 ~default:0.0 ;;
let miss : Scalar = List.nth ~xs:big ~i:99 ~default:0.0 ;;
let pairs : (Int, Scalar) list =
  List.zip ~xs:(List.range ~from:0 ~count:4) ~ys:big ;;
let doubled : Scalar list =
  List.flat_map ~f:(fun x:Scalar -> [x; x]) ~xs:big ;;
(* 600 + 30 + 2 + 0 + 3 + 3 + 6 = 644 *)
let freq : Scalar =
  total *. 100.0 +. top *. 10.0 +. pick +. miss +. to_scalar n
    +. to_scalar (List.length ~xs:pairs)
    +. to_scalar (List.length ~xs:doubled) ;;
let grid : Timestamp list =
  List.concat ~xss:[List.append ~xs:[0s] ~ys:[250ms];
                    List.rev ~xs:[750ms; 500ms]] ;;
let hit : Scalar Sample = sample (sine freq *. exp_decay 20.0) 0s 100ms ;;
let _ = place_multi hit grid
  |> sample ~from:0s ~to:1s |> render ~name:"out" ~rate:8000.0 ;;
)");
  write(literal, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sample (sine 644.0 *. exp_decay 20.0) 0s 100ms ;;
let _ = place_multi hit [0s; 250ms; 500ms; 750ms]
  |> sample ~from:0s ~to:1s |> render ~name:"out" ~rate:8000.0 ;;
)");
  CHECK(buildProject(derived.dir.string()).ok);
  CHECK(buildProject(literal.dir.string()).ok);
  std::string a = slurp(derived.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(literal.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_list_combinators_handle_empty_lists) {
  // Every combinator has to answer for the empty list without a special
  // case at the call site: that is what makes them safe to fold into a
  // score builder that may legitimately produce nothing. A *negative*
  // index counts as out of range too, and must answer with `default`
  // rather than the head - the extra nth below contributes 0.0 only if
  // it does.
  TempDir tp;
  tp.write("l.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let none : Scalar list = List.filter ~f:(fun x:Scalar -> x >. 9.0) ~xs:[1.0] ;;
(* 0 + 0 + (-1) + 0 + 5 + 0 + 0 + 0 + 440 = 444 *)
let freq : Scalar =
  to_scalar (List.length ~xs:none) +. List.sum ~xs:none
    +. List.nth ~xs:none ~i:0 ~default:(-1.0)
    +. List.nth ~xs:[1.0; 2.0] ~i:(-3) ~default:0.0
    +. List.maximum ~xs:none ~least:5.0
    +. to_scalar (List.length ~xs:(List.rev ~xs:none))
    +. to_scalar (List.length ~xs:(List.concat ~xss:[none; none]))
    +. to_scalar (List.length ~xs:(List.zip ~xs:none ~ys:[1.0]))
    +. 440.0 ;;
let _ = sine freq *. exp_decay 8.0
  |> sample ~from:0s ~to:200ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  tp.write("build.json", projectManifest("le", {"l.synth"}));
  TempDir ref;
  ref.write("l.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = sine 444.0 *. exp_decay 8.0
  |> sample ~from:0s ~to:200ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  ref.write("build.json", projectManifest("le", {"l.synth"}));
  CHECK(buildProject(tp.dir.string()).ok);
  CHECK(buildProject(ref.dir.string()).ok);
  std::string a = slurp(tp.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(ref.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

// Pitch is written in SynthGraph, so only a render pins its values.
// Rather than compare against decimal literals - `pow`/`log` results are
// libm-dependent and would make these brittle across machines - each
// claim is a build-time Bool, and the render frequency is 440 only if
// every one of them holds. A single wrong value moves the artifact.
namespace {
void checkPitchClaims(const char* claims) {
  TempDir derived, expected;
  std::string src = std::string(R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Core.Pitch
let e : Tuning = et12 ~ref_hz:440.0 ;;
let j : Tuning = just ~root:0 ~ref_hz:440.0 ;;
let p : Tuning = pyth ~root:0 ~ref_hz:440.0 ;;
let a4 : Note = { pc = A; oct = 4 } ;;
let ok : Bool =
)") + claims + R"( ;;
let _ = sine (if ok then 440.0 else 1.0)
  |> sample ~from:0s ~to:100ms |> render ~name:"out" ~rate:8000.0 ;;
)";
  derived.write("p.synth", src);
  derived.write("build.json", projectManifest("pv", {"p.synth"}));
  expected.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = sine 440.0
  |> sample ~from:0s ~to:100ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  expected.write("build.json", projectManifest("pv", {"p.synth"}));
  CHECK(buildProject(derived.dir.string()).ok);
  CHECK(buildProject(expected.dir.string()).ok);
  std::string a = slurp(derived.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(expected.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}
}  // namespace

TEST(build_pitch_reference_is_exact_in_every_temperament) {
  // Dividing by raw(ref_step) is what anchors a tuning, so the reference
  // pitch must come back bit-exact no matter which ratios are in play.
  checkPitchClaims(
      "a440 ~note:a4 ==. 440.0\n"
      "  && hz ~t:e ~note:a4 ==. 440.0\n"
      "  && hz ~t:j ~note:a4 ==. 440.0\n"
      "  && hz ~t:p ~note:a4 ==. 440.0\n"
      "  && hz ~t:(et12 ~ref_hz:432.0) ~note:a4 ==. 432.0\n"
      "  && step_hz ~t:(et ~n:19 ~ref_hz:440.0 ~ref_step:57) ~step:57 ==. 440.0");
}

TEST(build_pitch_equal_temperament_intervals) {
  // Octaves are exact in equal temperament; the tempered third is not a
  // round number, so it is pinned by a band rather than a literal.
  checkPitchClaims(
      "hz ~t:e ~note:{ pc = A; oct = 3 } ==. 220.0\n"
      "  && hz ~t:e ~note:{ pc = A; oct = 5 } ==. 880.0\n"
      "  && hz ~t:e ~note:{ pc = A; oct = 0 } ==. 27.5\n"
      "  && hz ~t:e ~note:{ pc = C; oct = 5 } >. 523.2511\n"
      "  && hz ~t:e ~note:{ pc = C; oct = 5 } <. 523.2512\n"
      "  && hz ~t:e ~note:{ pc = E; oct = 4 } >. 329.6275\n"
      "  && hz ~t:e ~note:{ pc = E; oct = 4 } <. 329.6276");
}

TEST(build_pitch_just_intonation_is_rational) {
  // The point of just intonation: the intervals are exact small ratios,
  // which equal temperament can only approximate.
  checkPitchClaims(
      "hz ~t:j ~note:{ pc = C; oct = 5 } ==. 528.0\n"
      "  && hz ~t:j ~note:{ pc = E; oct = 4 } ==. 330.0\n"
      "  && hz ~t:j ~note:{ pc = C; oct = 4 } ==. 264.0\n"
      "  && hz ~t:j ~note:{ pc = G; oct = 4 } ==. 396.0\n"
      "  && hz ~t:j ~note:{ pc = G; oct = 4 }\n"
      "       ==. hz ~t:j ~note:{ pc = C; oct = 4 } *. ratio ~num:3 ~den:2");
}

TEST(build_pitch_temperaments_actually_differ) {
  // A guard against the whole tuning model collapsing into 12-TET: away
  // from the reference the three temperaments must disagree, and the
  // key centre must matter for the unequal ones.
  // Not every note separates every pair - Pythagorean and just both put
  // E4 at exactly 3/4 of the reference, and several just roots agree on
  // C5 - so each claim names a note that genuinely does.
  checkPitchClaims(
      "hz ~t:j ~note:{ pc = C; oct = 5 } !=. hz ~t:e ~note:{ pc = C; oct = 5 }\n"
      "  && hz ~t:p ~note:{ pc = C; oct = 5 } !=. hz ~t:e ~note:{ pc = C; oct = 5 }\n"
      "  && hz ~t:p ~note:{ pc = D; oct = 4 } !=. hz ~t:j ~note:{ pc = D; oct = 4 }\n"
      "  && hz ~t:p ~note:{ pc = D; oct = 4 } ==. 880.0 /. 3.0\n"
      "  && hz ~t:j ~note:{ pc = D; oct = 4 } ==. 297.0\n"
      "  && hz ~t:(just ~root:7 ~ref_hz:440.0) ~note:{ pc = C; oct = 5 }\n"
      "       !=. hz ~t:j ~note:{ pc = C; oct = 5 }\n"
      "  && hz ~t:(just ~root:7 ~ref_hz:440.0) ~note:a4 ==. 440.0");
}

TEST(build_pitch_steps_round_trip) {
  // C0 = 0, A4 = 57, and Math.floor being a true floor is what keeps
  // octaves below C0 correct.
  checkPitchClaims(
      "step ~note:{ pc = C; oct = 0 } == 0\n"
      "  && step ~note:a4 == 57\n"
      "  && step ~note:(of_step ~step:57) == 57\n"
      "  && step ~note:(of_step ~step:0) == 0\n"
      "  && step ~note:(of_step ~step:(-1)) == -1\n"
      "  && step ~note:(of_step ~step:(-13)) == -13\n"
      "  && step ~note:(shift ~note:a4 ~by:7) == 64\n"
      "  && step ~note:(shift ~note:a4 ~by:(-12)) == 45\n"
      "  && step ~note:(flat ~note:a4) == 56");
}

TEST(build_pitch_cents_are_temperament_independent) {
  checkPitchClaims(
      "cents ~n:1200.0 ==. 2.0\n"
      "  && cents ~n:0.0 ==. 1.0\n"
      "  && cents ~n:(-1200.0) ==. 0.5\n"
      "  && detune ~freq:440.0 ~cents:1200.0 ==. 880.0\n"
      "  && detune ~freq:440.0 ~cents:0.0 ==. 440.0\n"
      "  && to_cents ~ratio:2.0 ==. 1200.0\n"
      "  && to_cents ~ratio:1.0 ==. 0.0\n"
      "  && ratio ~num:3 ~den:2 ==. 1.5\n"
      "  && to_cents ~ratio:(cents ~n:10.4) >. 10.39\n"
      "  && to_cents ~ratio:(cents ~n:10.4) <. 10.41");
}

TEST(build_pitch_supports_non_octave_tunings) {
  // Bohlen-Pierce: 13 steps to a tritave. If `octave` were secretly 2.0
  // this would not land on 3x the reference.
  checkPitchClaims(
      "step_hz ~t:{ ref_hz = 440.0; ref_step = 0; root = 0;\n"
      "             ratios = List.init ~n:13\n"
      "                        ~f:(fun i:Int ->\n"
      "                              Math.pow ~x:3.0\n"
      "                                       ~y:(to_scalar i /. 13.0));\n"
      "             octave = 3.0 } ~step:13 ==. 1320.0\n"
      "  && step_hz ~t:{ ref_hz = 440.0; ref_step = 0; root = 0;\n"
      "                  ratios = []; octave = 2.0 } ~step:99 ==. 440.0");
}

// Tempo is written in SynthGraph too, so the same trick pins its values:
// each claim is a build-time Bool over Timestamps, and the render is 440
// only if all of them hold. `same` compares two Timestamp lists
// element-wise - the length guard is what stops two out-of-range `nth`
// defaults from agreeing vacuously.
namespace {
void checkTempoClaims(const char* claims) {
  TempDir derived, expected;
  std::string src = std::string(R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Core.Tempo
let t : Tempo = common ~bpm:120.0 ;;
let six8 : Tempo = { bpm = 120.0; meter = { beats = 6; unit = 8 } } ;;
let same xs:Timestamp list ys:Timestamp list : Bool =
  List.length ~xs:xs == List.length ~xs:ys
    && List.fold
         ~f:(fun acc:Bool i:Int ->
               acc && List.nth ~xs:xs ~i:i ~default:0s
                        ==. List.nth ~xs:ys ~i:i ~default:1s)
         ~init:true
         ~xs:(List.range ~from:0 ~count:(List.length ~xs:xs)) ;;
let ok : Bool =
)") + claims + R"( ;;
let _ = sine (if ok then 440.0 else 1.0)
  |> sample ~from:0s ~to:100ms |> render ~name:"out" ~rate:8000.0 ;;
)";
  derived.write("t.synth", src);
  derived.write("build.json", projectManifest("tv", {"t.synth"}));
  expected.write("t.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = sine 440.0
  |> sample ~from:0s ~to:100ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  expected.write("build.json", projectManifest("tv", {"t.synth"}));
  CHECK(buildProject(derived.dir.string()).ok);
  CHECK(buildProject(expected.dir.string()).ok);
  std::string a = slurp(derived.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(expected.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}
}  // namespace

TEST(build_tempo_pulse_derives_from_bpm) {
  // Everything hangs off `beat = to_min (1 / bpm)`; 120 and 90 both
  // divide 60 cleanly, so these are exact rather than approximate.
  checkTempoClaims(
      "beat ~t:t ==. 500ms\n"
      "  && bar ~t:t ==. 2s\n"
      "  && beats ~t:t ~n:1.5 ==. 750ms\n"
      "  && beats ~t:t ~n:0.0 ==. 0s\n"
      "  && beats ~t:t ~n:4.0 ==. bar ~t:t\n"
      "  && beat ~t:(common ~bpm:60.0) ==. 1s\n"
      "  && bar ~t:six8 ==. 3s");
}

TEST(build_tempo_note_values_scale_with_the_meter_unit) {
  // A whole note is `unit` beats, not four: in 6/8 the beat is an eighth,
  // so a whole note is eight of them. This is the claim that catches the
  // tempting-but-wrong `whole = 4 beats`.
  checkTempoClaims(
      "value ~t:t ~v:Quarter ==. beat ~t:t\n"
      "  && value ~t:t ~v:Whole ==. 2s\n"
      "  && value ~t:t ~v:Half ==. 1s\n"
      "  && value ~t:t ~v:Eighth ==. 250ms\n"
      "  && value ~t:t ~v:Sixteenth ==. 125ms\n"
      "  && value ~t:t ~v:ThirtySecond ==. 62500us\n"
      "  && value ~t:six8 ~v:Eighth ==. beat ~t:six8\n"
      "  && value ~t:six8 ~v:Whole ==. 4s\n"
      "  && value ~t:six8 ~v:Quarter ==. 1s");
}

TEST(build_tempo_values_nest) {
  // The recursive constructors: a dot is x1.5 and composes, and
  // `Tuplet (n, m, v)` is n of v in the time of m - which is why three
  // eighth-triplets make a quarter, and why the identity tuplet is a
  // no-op. Pinned against `value Quarter` rather than a decimal so a
  // flipped n/m cannot pass.
  checkTempoClaims(
      "value ~t:t ~v:(Dotted Quarter) ==. 750ms\n"
      "  && value ~t:t ~v:(Dotted (Dotted Quarter)) ==. 1125ms\n"
      "  && value ~t:t ~v:(Dotted Quarter)\n"
      "       ==. value ~t:t ~v:Quarter +. value ~t:t ~v:Eighth\n"
      "  && value ~t:t ~v:(Tuplet (3, 2, Eighth)) *. 3.0\n"
      "       ==. value ~t:t ~v:Quarter\n"
      "  && value ~t:t ~v:(Tuplet (1, 1, Eighth)) ==. value ~t:t ~v:Eighth\n"
      "  && value ~t:t ~v:(Tuplet (3, 2, Quarter)) *. 3.0\n"
      "       ==. value ~t:t ~v:Half\n"
      "  && value ~t:six8 ~v:(Dotted Quarter) ==. bar ~t:six8 /. 2.0");
}

TEST(build_tempo_at_counts_bars_and_beats_from_zero) {
  // `at` is an offset, not a ruler label: bar 0 beat 0 is the origin and
  // the two arguments simply add.
  checkTempoClaims(
      "at ~t:t ~bar:0 ~beat:0.0 ==. 0s\n"
      "  && at ~t:t ~bar:1 ~beat:0.0 ==. bar ~t:t\n"
      "  && at ~t:t ~bar:4 ~beat:2.0 ==. 9s\n"
      "  && at ~t:t ~bar:0 ~beat:4.0 ==. at ~t:t ~bar:1 ~beat:0.0\n"
      "  && at ~t:t ~bar:2 ~beat:1.5 ==. 4750ms\n"
      "  && at ~t:six8 ~bar:1 ~beat:0.0 ==. 3s");
}

TEST(build_tempo_grid_matches_the_literals_it_replaces) {
  // A migration rehearsal: this is the exact call in
  // examples/song/song.synth, written the way Tempo will write it.
  checkTempoClaims(
      "same (grid ~t:t ~from:2s ~step:Quarter ~count:28)\n"
      "     (time_steps ~start:2s ~step:500ms ~count:28)\n"
      "  && same (grid ~t:t ~from:4s ~step:Eighth ~count:48)\n"
      "          (time_steps ~start:4s ~step:250ms ~count:48)\n"
      "  && same (grid ~t:t ~from:0s ~step:Quarter ~count:0) []\n"
      "  && List.length ~xs:(grid ~t:t ~from:0s ~step:Whole ~count:3) == 3\n"
      "  && List.nth ~xs:(grid ~t:t ~from:0s ~step:Whole ~count:3) ~i:2\n"
      "             ~default:0s ==. 4s");
}

TEST(build_tempo_swing_displaces_alternate_entries) {
  // Entries 1 and 3 move later by step*amount; 0 and 2 do not. Amount
  // 0.0 is the identity, and a one-element grid has no offbeat to move.
  checkTempoClaims(
      "same (swing ~amount:0.0 ~step:500ms\n"
      "            ~steps:(grid ~t:t ~from:0s ~step:Quarter ~count:4))\n"
      "     (grid ~t:t ~from:0s ~step:Quarter ~count:4)\n"
      "  && same (swing ~amount:0.5 ~step:500ms\n"
      "                 ~steps:(grid ~t:t ~from:0s ~step:Quarter ~count:4))\n"
      "          [0s; 750ms; 1s; 1750ms]\n"
      "  && same (swing ~amount:0.5 ~step:500ms ~steps:[0s]) [0s]\n"
      "  && same (swing ~amount:0.5 ~step:500ms ~steps:[]) []\n"
      "  && List.nth ~xs:(swing ~amount:(1.0 /. 3.0) ~step:500ms\n"
      "                         ~steps:(grid ~t:t ~from:0s ~step:Quarter\n"
      "                                      ~count:4))\n"
      "              ~i:1 ~default:0s ==. 500ms +. 500ms /. 3.0");
}

// Scale trades in Pitch.Note lists, so the claims below compare step
// ladders: `st` turns a note list into its chromatic steps and `same`
// compares two Int lists element-wise, with the length guard that stops
// two out-of-range `nth` defaults from agreeing vacuously.
namespace {
void checkScaleClaims(const char* claims) {
  TempDir derived, expected;
  std::string src = std::string(R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Core.Pitch
open Core.Scale
let cmaj : Scale = { tonic = { pc = C; oct = 4 }; quality = Major } ;;
let amin : Scale = { tonic = { pc = A; oct = 3 }; quality = Minor } ;;
let cpent : Scale = { tonic = { pc = C; oct = 4 }; quality = PentMajor } ;;
let am : Chord = { root = { pc = A; oct = 3 }; quality = Min } ;;
let st ns:Note list : Int list =
  List.map ~f:(fun n:Note -> step ~note:n) ~xs:ns ;;
let same xs:Int list ys:Int list : Bool =
  List.length ~xs:xs == List.length ~xs:ys
    && List.fold
         ~f:(fun acc:Bool i:Int ->
               acc && List.nth ~xs:xs ~i:i ~default:0
                        == List.nth ~xs:ys ~i:i ~default:1)
         ~init:true ~xs:(List.range ~from:0 ~count:(List.length ~xs:xs)) ;;
let d s:Scale n:Int : Int = step ~note:(degree ~s:s ~n:n) ;;
let sn s:Scale k:Int : Int = step ~note:(snap ~s:s ~note:(of_step ~step:k)) ;;
let ok : Bool =
)") + claims + R"( ;;
let _ = sine (if ok then 440.0 else 1.0)
  |> sample ~from:0s ~to:100ms |> render ~name:"out" ~rate:8000.0 ;;
)";
  derived.write("s.synth", src);
  derived.write("build.json", projectManifest("sv", {"s.synth"}));
  expected.write("s.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = sine 440.0
  |> sample ~from:0s ~to:100ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  expected.write("build.json", projectManifest("sv", {"s.synth"}));
  CHECK(buildProject(derived.dir.string()).ok);
  CHECK(buildProject(expected.dir.string()).ok);
  std::string a = slurp(derived.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(expected.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}
}  // namespace

TEST(build_scale_degrees_count_from_zero) {
  // Degree 0 is the tonic, degree 7 of a seven-note scale is the tonic
  // an octave up, and negative degrees descend - the case a truncating
  // divide would get wrong. C4 is step 48.
  checkScaleClaims(
      "d cmaj 0 == 48\n"
      "  && d cmaj 4 == 55\n"
      "  && d cmaj 7 == 60\n"
      "  && d cmaj 8 == 62\n"
      "  && d cmaj (-1) == 47\n"
      "  && d cmaj (-7) == 36\n"
      "  && same (st (notes ~s:cmaj ~from:0 ~count:8))\n"
      "          [48; 50; 52; 53; 55; 57; 59; 60]");
}

TEST(build_scale_degrees_wrap_at_the_ladder_not_at_seven) {
  // A five-note scale wraps after five degrees, so the octave lands on
  // degree 5. A hardcoded 7 would put C an augmented fourth out.
  checkScaleClaims(
      "d cpent 5 == 60\n"
      "  && d cpent 3 == 55\n"
      "  && d cpent (-1) == 45\n"
      "  && same (st (notes ~s:cpent ~from:0 ~count:6))\n"
      "          [48; 50; 52; 55; 57; 60]\n"
      "  && List.length ~xs:(offsets ~q:Chromatic) == 12\n"
      "  && d { tonic = { pc = C; oct = 4 }; quality = Chromatic } 12 == 60");
}

TEST(build_scale_diatonic_stacks_take_quality_from_the_key) {
  // Every other degree: i on A minor is minor, iv is minor, v7 is a
  // minor seventh - none of it named, all of it falling out of the
  // ladder.
  checkScaleClaims(
      "same (st (triad ~s:amin ~degree:0)) [45; 48; 52]\n"
      "  && same (st (triad ~s:amin ~degree:3)) [50; 53; 57]\n"
      "  && same (st (seventh ~s:amin ~degree:4)) [52; 55; 59; 62]\n"
      "  && same (st (stack ~s:amin ~from:0 ~count:5)) [45; 48; 52; 55; 59]\n"
      "  && same (st (stack ~s:amin ~from:0 ~count:0)) []");
}

TEST(build_scale_named_chords_are_a_root_plus_a_shape) {
  checkScaleClaims(
      "same (st (tones ~c:am)) [45; 48; 52]\n"
      "  && same (st (tones ~c:{ root = { pc = G; oct = 3 };\n"
      "                          quality = Dom7 })) [43; 47; 50; 53]\n"
      "  && same (st (tones ~c:{ root = { pc = C; oct = 4 };\n"
      "                          quality = Add9 })) [48; 52; 55; 62]\n"
      "  && same (st (tones ~c:{ root = { pc = B; oct = 3 };\n"
      "                          quality = HalfDim7 })) [47; 50; 53; 57]\n"
      "  && same (shape ~q:Sus4) [0; 5; 7]");
}

TEST(build_scale_inversions_rotate_up_and_drop_down) {
  // n:1 lifts the bottom note an octave; n:3 on a triad is the whole
  // chord an octave up; a negative n drops the top note instead.
  checkScaleClaims(
      "same (st (invert ~notes:(tones ~c:am) ~n:1)) [48; 52; 57]\n"
      "  && same (st (invert ~notes:(tones ~c:am) ~n:2)) [52; 57; 60]\n"
      "  && same (st (invert ~notes:(tones ~c:am) ~n:3)) [57; 60; 64]\n"
      "  && same (st (invert ~notes:(tones ~c:am) ~n:(-1))) [40; 45; 48]\n"
      "  && same (st (invert ~notes:(tones ~c:am) ~n:0)) [45; 48; 52]\n"
      "  && same (st (invert ~notes:Nil ~n:1)) []");
}

TEST(build_scale_voicing_spreads_from_a_floor) {
  // Four parts out of a three-note chord, starting at the lowest octave
  // at or above `low`: root, third, fifth, root again.
  checkScaleClaims(
      "same (st (voicing ~notes:(tones ~c:am)\n"
      "                  ~low:{ pc = A; oct = 2 } ~count:4))\n"
      "     [33; 36; 40; 45]\n"
      "  && same (st (voicing ~notes:(tones ~c:am)\n"
      "                       ~low:{ pc = A; oct = 3 } ~count:3))\n"
      "          [45; 48; 52]\n"
      "  && same (st (voicing ~notes:(tones ~c:am)\n"
      "                       ~low:{ pc = A; oct = 3 } ~count:0)) []\n"
      "  && same (st (voicing ~notes:Nil ~low:{ pc = A; oct = 3 }\n"
      "                       ~count:4)) []");
}

TEST(build_scale_snap_keeps_a_line_in_the_key) {
  // In-key notes are left alone, above and below the tonic alike; the
  // black keys fall to the nearest white one, and a note exactly
  // between two of them takes the lower.
  checkScaleClaims(
      "sn cmaj 59 == 59\n"
      "  && sn cmaj 45 == 45\n"
      "  && sn cmaj 48 == 48\n"
      "  && sn cmaj 54 == 53\n"
      "  && sn cmaj 44 == 43\n"
      "  && sn cmaj 49 == 48\n"
      "  && sn cmaj 61 == 60\n"
      "  && sn amin 46 == 45");
}

TEST(build_scale_freqs_names_its_temperament) {
  // The one exit to Scalars, and it goes through a Tuning like Pitch.hz
  // does - so a just-intonation chord really is rational.
  checkScaleClaims(
      "List.nth ~xs:(freqs ~t:(et12 ~ref_hz:440.0)\n"
      "                    ~notes:(triad ~s:amin ~degree:0))\n"
      "         ~i:0 ~default:0.0 ==. 220.0\n"
      "  && List.length ~xs:(freqs ~t:(et12 ~ref_hz:440.0)\n"
      "                            ~notes:(tones ~c:am)) == 3\n"
      "  && List.nth ~xs:(freqs ~t:(just ~root:0 ~ref_hz:440.0)\n"
      "                         ~notes:(tones ~c:{ root = { pc = C; oct = 4 };\n"
      "                                            quality = Maj }))\n"
      "              ~i:2 ~default:0.0\n"
      "       ==. List.nth ~xs:(freqs ~t:(just ~root:0 ~ref_hz:440.0)\n"
      "                              ~notes:(tones ~c:{ root = { pc = C;\n"
      "                                                          oct = 4 };\n"
      "                                                 quality = Maj }))\n"
      "                   ~i:0 ~default:0.0 *. ratio ~num:3 ~den:2");
}

// Score is symbolic until `realize`, so most claims compare beat
// positions rather than Timestamps. `near` gives the Scalar comparisons
// a tolerance - the dynamics table is a `pow`, and libm is not
// bit-reproducible across machines.
namespace {
void checkScoreClaims(const char* claims) {
  TempDir derived, expected;
  std::string src = std::string(R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
open Core.Pitch
open Core.Tempo
open Core.Scale
open Core.Score
let t : Tempo = common ~bpm:120.0 ;;
let tn : Tuning = et12 ~ref_hz:440.0 ;;
let cmaj : Scale = { tonic = { pc = C; oct = 4 }; quality = Major } ;;
let a4 : Note = { pc = A; oct = 4 } ;;
let c5 : Note = { pc = C; oct = 5 } ;;
let ats p:Phrase : Scalar list =
  List.map ~f:(fun s:Step -> s.at) ~xs:p.steps ;;
let lens p:Phrase : Scalar list =
  List.map ~f:(fun s:Step -> s.len) ~xs:p.steps ;;
let vels p:Phrase : Scalar list =
  List.map ~f:(fun s:Step -> s.vel) ~xs:p.steps ;;
let stps p:Phrase : Int list =
  List.map ~f:(fun s:Step -> step ~note:s.note) ~xs:p.steps ;;
let near a:Scalar b:Scalar : Bool =
  let d : Scalar = a -. b in (if d <. 0.0 then 0.0 -. d else d) <. 0.00001 ;;
let sameS xs:Scalar list ys:Scalar list : Bool =
  List.length ~xs:xs == List.length ~xs:ys
    && List.fold ~f:(fun acc:Bool i:Int ->
                       acc && near (List.nth ~xs:xs ~i:i ~default:0.0)
                                   (List.nth ~xs:ys ~i:i ~default:1.0))
                 ~init:true
                 ~xs:(List.range ~from:0 ~count:(List.length ~xs:xs)) ;;
let sameI xs:Int list ys:Int list : Bool =
  List.length ~xs:xs == List.length ~xs:ys
    && List.fold ~f:(fun acc:Bool i:Int ->
                       acc && List.nth ~xs:xs ~i:i ~default:0
                                == List.nth ~xs:ys ~i:i ~default:1)
                 ~init:true
                 ~xs:(List.range ~from:0 ~count:(List.length ~xs:xs)) ;;
let l1 : Phrase = line ~items:[Play (a4, 1.0); Rest 0.5; Play (c5, 1.0)] ;;
let m1 : Phrase = melody ~notes:[a4; c5] ~len:0.5 ;;
let ok : Bool =
)") + claims + R"( ;;
let _ = sine (if ok then 440.0 else 1.0)
  |> sample ~from:0s ~to:100ms |> render ~name:"out" ~rate:8000.0 ;;
)";
  derived.write("p.synth", src);
  derived.write("build.json", projectManifest("scv", {"p.synth"}));
  expected.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = sine 440.0
  |> sample ~from:0s ~to:100ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  expected.write("build.json", projectManifest("scv", {"p.synth"}));
  CHECK(buildProject(derived.dir.string()).ok);
  CHECK(buildProject(expected.dir.string()).ok);
  std::string a = slurp(derived.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(expected.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}
}  // namespace

TEST(build_score_line_lays_items_end_to_end) {
  // A Rest advances the cursor without emitting a step, so the second
  // note starts at 1.5 and the phrase spans 2.5 beats.
  checkScoreClaims(
      "sameS (ats l1) [0.0; 1.5]\n"
      "  && sameS (lens l1) [1.0; 1.0]\n"
      "  && List.length ~xs:l1.steps == 2\n"
      "  && near (span ~p:l1) 2.5\n"
      "  && sameS (ats (line ~items:[])) []\n"
      "  && near (span ~p:(line ~items:[Rest 4.0])) 0.0");
}

TEST(build_score_builders_place_their_notes) {
  // melody is a line of equal notes; chord puts them all at one beat;
  // arpeggio cycles through the notes it was given. A4 is step 57.
  checkScoreClaims(
      "sameS (ats m1) [0.0; 0.5] && near (span ~p:m1) 1.0\n"
      "  && sameS (ats (chord ~notes:(triad ~s:cmaj ~degree:0)\n"
      "                       ~at:2.0 ~len:1.0)) [2.0; 2.0; 2.0]\n"
      "  && sameI (stps (chord ~notes:(triad ~s:cmaj ~degree:0)\n"
      "                        ~at:2.0 ~len:1.0)) [48; 52; 55]\n"
      "  && near (span ~p:(chord ~notes:(triad ~s:cmaj ~degree:0)\n"
      "                          ~at:2.0 ~len:1.0)) 3.0\n"
      "  && sameS (ats (arpeggio ~notes:[a4; c5] ~step:0.25 ~count:5))\n"
      "           [0.0; 0.25; 0.5; 0.75; 1.0]\n"
      "  && sameI (stps (arpeggio ~notes:[a4; c5] ~step:0.25 ~count:5))\n"
      "           [57; 60; 57; 60; 57]\n"
      "  && sameS (ats (arpeggio ~notes:[] ~step:0.25 ~count:4)) []");
}

TEST(build_score_seq_and_layer_are_different_compositions) {
  // seq starts each phrase where the last ended; layer leaves their
  // positions alone. That difference is the whole point of both.
  checkScoreClaims(
      "sameS (ats (seq ~ps:[m1; m1])) [0.0; 0.5; 1.0; 1.5]\n"
      "  && sameS (ats (layer ~ps:[m1; m1])) [0.0; 0.5; 0.0; 0.5]\n"
      "  && near (span ~p:(seq ~ps:[m1; m1])) 2.0\n"
      "  && near (span ~p:(layer ~ps:[m1; m1])) 1.0\n"
      "  && near (span ~p:(loop ~p:m1 ~n:3)) 3.0\n"
      "  && sameS (ats (loop ~p:m1 ~n:0)) []\n"
      "  && sameS (ats (seq ~ps:[])) []\n"
      "  && sameS (ats (layer ~ps:[])) []");
}

TEST(build_score_edits_are_pure_and_compose) {
  checkScoreClaims(
      "sameS (ats (move ~p:m1 ~beats:2.0)) [2.0; 2.5]\n"
      "  && sameI (stps (transpose ~p:m1 ~semitones:12)) [69; 72]\n"
      "  && sameI (stps (transpose ~p:m1 ~semitones:0)) [57; 60]\n"
      "  && sameI (stps (in_key ~p:(melody ~notes:[{ pc = Cs; oct = 4 }]\n"
      "                                    ~len:1.0) ~s:cmaj)) [48]\n"
      "  && sameS (lens (staccato ~p:m1 ~ratio:0.5)) [0.25; 0.25]\n"
      "  && sameS (vels (velocity ~p:m1 ~f:(fun v:Scalar -> v *. 0.25)))\n"
      "           [0.25; 0.25]\n"
      "  (* the original is untouched: every edit is a copy *)\n"
      "  && sameS (ats m1) [0.0; 0.5] && sameS (lens m1) [0.5; 0.5]");
}

TEST(build_score_legato_stretches_to_the_next_attack) {
  // l1 has a 0.5-beat gap after its first note; legato closes it and
  // leaves the last note as written.
  checkScoreClaims(
      "sameS (lens (legato ~p:l1)) [1.5; 1.0]\n"
      "  && sameS (ats (legato ~p:l1)) [0.0; 1.5]\n"
      "  && sameS (lens (legato ~p:m1)) [0.5; 0.5]\n"
      "  && sameS (lens (legato ~p:(staccato ~p:l1 ~ratio:0.1)))\n"
      "           [1.5; 0.1]");
}

TEST(build_score_dynamics_are_a_decibel_ladder) {
  // Fff is unity by construction, Piano exactly a tenth of it, and the
  // eight levels are strictly increasing. The rest is a pow, so it gets
  // a tolerance rather than bit-equality.
  checkScoreClaims(
      "amp ~l:Fff ==. 1.0 && db ~x:0.0 ==. 1.0\n"
      "  && near (amp ~l:Piano) 0.1\n"
      "  && near (amp ~l:Mf) 0.251188643\n"
      "  && near (db ~x:20.0) 10.0\n"
      "  && near (db ~x:(-6.0)) 0.501187233\n"
      "  && amp ~l:Ppp <. amp ~l:Pp && amp ~l:Pp <. amp ~l:Piano\n"
      "  && amp ~l:Piano <. amp ~l:Mp && amp ~l:Mp <. amp ~l:Mf\n"
      "  && amp ~l:Mf <. amp ~l:Forte && amp ~l:Forte <. amp ~l:Ff\n"
      "  && amp ~l:Ff <. amp ~l:Fff");
}

TEST(build_score_ramp_interpolates_in_decibels) {
  // A crescendo is even in dB, not in amplitude: the midpoint of
  // Piano -> Fff is 0.316, not 0.55.
  checkScoreClaims(
      "sameS (ramp ~from:Piano ~to:Fff ~n:5)\n"
      "     [0.1; 0.177827941; 0.316227766; 0.562341325; 1.0]\n"
      "  && sameS (ramp ~from:Piano ~to:Fff ~n:1) [1.0]\n"
      "  && sameS (ramp ~from:Piano ~to:Fff ~n:0) []\n"
      "  && sameS (ramp ~from:Fff ~to:Fff ~n:3) [1.0; 1.0; 1.0]\n"
      "  && near (List.nth ~xs:(ramp ~from:Fff ~to:Piano ~n:5) ~i:0\n"
      "                    ~default:0.0) 1.0");
}

TEST(build_score_realize_resolves_tempo_and_tuning) {
  // The one bridge: beats become Timestamps at the given tempo, notes
  // become frequencies in the given temperament.
  checkScoreClaims(
      "List.nth ~xs:(List.map ~f:(fun e:Event -> e.at)\n"
      "                       ~xs:(realize ~tempo:t ~tuning:tn ~p:l1))\n"
      "         ~i:1 ~default:0s ==. 750ms\n"
      "  && List.nth ~xs:(List.map ~f:(fun e:Event -> e.dur)\n"
      "                            ~xs:(realize ~tempo:t ~tuning:tn ~p:l1))\n"
      "              ~i:0 ~default:0s ==. 500ms\n"
      "  && List.nth ~xs:(List.map ~f:(fun e:Event -> e.freq)\n"
      "                            ~xs:(realize ~tempo:t ~tuning:tn ~p:l1))\n"
      "              ~i:0 ~default:0.0 ==. 440.0\n"
      "  && near (List.nth ~xs:(List.map ~f:(fun e:Event -> e.freq)\n"
      "                     ~xs:(realize ~tempo:t ~tuning:(just ~root:0\n"
      "                                                         ~ref_hz:440.0)\n"
      "                                  ~p:(melody ~notes:[c5] ~len:1.0)))\n"
      "                    ~i:0 ~default:0.0) 528.0\n"
      "  && List.length ~xs:(realize ~tempo:t ~tuning:tn\n"
      "                              ~p:(line ~items:[])) == 0");
}

TEST(build_resample_identity_matches_the_input) {
  // End-to-end: `|> resample ~f:(fun t -> 1.0)` reads one source frame per
  // output frame, so the artifact must be byte-identical to the un-warped
  // render. Also pins the pipe wiring - `input` is the primitive's
  // unlabeled slot.
  auto write = [](TempDir& tp, const char* body) {
    tp.write("r.synth", body);
    tp.write("build.json", projectManifest("rs", {"r.synth"}));
  };
  TempDir warped, plain;
  write(warped, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = (sine 440.0) *. (exp_decay 4.0)
  |> resample ~f:(fun t:Scalar -> 1.0)
  |> sample ~from:0s ~to:300ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  write(plain, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = (sine 440.0) *. (exp_decay 4.0)
  |> sample ~from:0s ~to:300ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  CHECK(buildProject(warped.dir.string()).ok);
  CHECK(buildProject(plain.dir.string()).ok);
  std::string a = slurp(warped.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(plain.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_resample_runaway_rate_is_a_diagnostic) {
  TempDir tp;
  tp.write("r.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = sine 440.0 |> resample ~f:(fun t:Scalar -> 1000.0)
  |> sample ~from:0s ~to:100ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  tp.write("build.json", projectManifest("runaway", {"r.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  bool mentioned = false;
  for (auto& d : r.diags.items)
    if (d.message.find("resample: rate") != std::string::npos) mentioned = true;
  CHECK(mentioned);
}

TEST(build_place_multi_overlaps_sum) {
  // Two placements of a constant-ish sample overlapping halfway: the
  // overlap region carries double amplitude.
  TempDir tp;
  tp.write("o.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let level : Scalar Sample = sample (exp_decay 0.0) 0s 200ms ;;
let _ = render "out" 8000.0
  (sample ((place_multi level [0s; 100ms]) *. 0.4) 0s 400ms) ;;
)");
  tp.write("build.json", projectManifest("overlap", {"o.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "out.wav").string());
  auto at = [&](double t) { return w.channels[0][(size_t)(t * 8000.0)]; };
  CHECK_NEAR(at(0.05), 0.4, 0.01);   // one placement
  CHECK_NEAR(at(0.15), 0.8, 0.01);   // overlap: both sum
  CHECK_NEAR(at(0.25), 0.4, 0.01);   // only the second remains
  CHECK_NEAR(at(0.35), 0.0, 0.01);   // both finished
}

TEST(build_computed_callee_matches_direct_call) {
  // Applying a parenthesized partial application must render exactly the
  // same artifact as the direct call.
  auto write = [](TempDir& tp, const char* body) {
    tp.write("c.synth", body);
    tp.write("build.json", projectManifest("cc", {"c.synth"}));
  };
  TempDir computed, direct;
  write(computed, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = render "out" 8000.0
  (sample ((lowpass ~cutoff:600.0) (saw 220.0)) 0s 500ms) ;;
)");
  write(direct, R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = render "out" 8000.0
  (sample (lowpass 600.0 (saw 220.0)) 0s 500ms) ;;
)");
  BuildResult rc = buildProject(computed.dir.string());
  for (auto& d : rc.diags.items) std::cerr << d.message << "\n";
  CHECK(rc.ok);
  CHECK(buildProject(direct.dir.string()).ok);
  std::string a = slurp(computed.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(direct.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_place_multi_with_stateful_sample) {
  // A reverb-carrying sample placed at several timestamps: every
  // placement replays the same content (state isolation end to end).
  TempDir tp;
  tp.write("s.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let wet : Scalar Sample =
  sample (reverb 100ms 0.3 0.5 ((sine 440.0) *. (exp_decay 30.0))) 0s 150ms ;;
let _ = render "out" 8000.0 (sample (place_multi wet [0s; 300ms]) 0s 600ms) ;;
)");
  tp.write("build.json", projectManifest("statepm", {"s.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "out.wav").string());
  int64_t shift = 2400;  // 300ms
  int64_t len = 1200;    // 150ms
  for (int64_t i = 0; i < len; i++)
    CHECK_NEAR(w.channels[0][(size_t)i], w.channels[0][(size_t)(i + shift)],
               2.0 / 32768.0);  // identical up to 16-bit quantization
}

TEST(build_pipes_and_labels_match_classic_style) {
  // The same voice written classic-style and pipe/label-style must
  // produce byte-identical artifacts.
  TempDir classic, piped;
  classic.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let voice : Scalar Signal =
  soft_clip 0.8 (lowpass 900.0 ((saw 220.0) *. 2.0)) ;;
let _ = render "out" 8000.0 (sample voice 0s 300ms) ;;
)");
  classic.write("build.json", projectManifest("c", {"p.synth"}));
  piped.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let voice : Scalar Signal =
  saw 220.0 *. 2.0 |> lowpass ~cutoff:900.0 |> soft_clip ~threshold:0.8 ;;
let _ = sample voice ~from:0s ~to:300ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  piped.write("build.json", projectManifest("p", {"p.synth"}));
  BuildResult rc = buildProject(classic.dir.string());
  BuildResult rp = buildProject(piped.dir.string());
  for (auto& d : rc.diags.items) std::cerr << d.message << "\n";
  for (auto& d : rp.diags.items) std::cerr << d.message << "\n";
  CHECK(rc.ok);
  CHECK(rp.ok);
  std::string a = slurp(classic.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(piped.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_labeled_partial_application_evaluates) {
  // A user function partially applied by label, bound, then finished -
  // and a primitive passed bare to map.
  TempDir tp;
  tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let voice ~amp:Scalar ~freq:Scalar : Scalar Signal = (sine freq) *. amp ;;
let quiet : Scalar -> Scalar Signal = voice ~amp:0.25 ;;
let tones : Scalar Signal list = List.map sine [220.0; 330.0] ;;
let sum : Scalar Signal = (mix_all tones) *. 0.2 +. quiet 440.0 ;;
let _ = render "out" 8000.0 (sample sum 0s 200ms) ;;
)");
  tp.write("build.json", projectManifest("partial", {"p.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "out.wav").string());
  double peak = 0;
  for (double v : w.channels[0]) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.3);  // all three tones present
}

TEST(build_list_init_harmonic_stack) {
  // list_init driving additive synthesis: five harmonics of 110 Hz.
  TempDir tp;
  tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let harmonic i:Int : Scalar Signal =
  sine (110.0 *. (to_scalar i +. 1.0)) *. (1.0 /. (to_scalar i +. 1.0)) ;;
let stack : Scalar Signal = mix_all (List.init 5 harmonic) *. 0.3 ;;
let _ = stack |> sample ~from:0s ~to:200ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  tp.write("build.json", projectManifest("li", {"p.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "out.wav").string());
  // Goertzel power at each expected harmonic: all five present.
  auto power = [&](double freq) {
    double wc = 2.0 * M_PI * freq / 8000.0, c = 2.0 * std::cos(wc);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int64_t i = 0; i < w.frames(); i++) {
      s0 = w.channels[0][(size_t)i] + c * s1 - s2;
      s2 = s1;
      s1 = s0;
    }
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) /
           (double)w.frames();
  };
  for (double h : {110.0, 220.0, 330.0, 440.0, 550.0})
    CHECK(power(h) > 0.005);
  CHECK(power(660.0) < 0.001);  // sixth harmonic absent
}

TEST(build_time_steps_matches_manual_list) {
  TempDir stepped, manual;
  stepped.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sine 660.0 *. exp_decay 20.0 |> sample ~from:0s ~to:100ms ;;
let _ = place_multi hit (time_steps ~start:0s ~step:150ms ~count:4)
        |> sample ~from:0s ~to:600ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  stepped.write("build.json", projectManifest("ts", {"p.synth"}));
  manual.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sine 660.0 *. exp_decay 20.0 |> sample ~from:0s ~to:100ms ;;
let _ = place_multi hit [0s; 150ms; 300ms; 450ms]
        |> sample ~from:0s ~to:600ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  manual.write("build.json", projectManifest("tm", {"p.synth"}));
  CHECK(buildProject(stepped.dir.string()).ok);
  CHECK(buildProject(manual.dir.string()).ok);
  std::string a = slurp(stepped.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(manual.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_repeat_and_count_validation) {
  TempDir tp;
  tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let layers : Scalar Signal = mix_all (List.repeat 3 (sine 220.0)) *. 0.2 ;;
let _ = layers |> sample ~from:0s ~to:100ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  tp.write("build.json", projectManifest("rep", {"p.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(r.ok);
  // Three identical layers at 0.2 gain -> amplitude 0.6.
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "out.wav").string());
  double peak = 0;
  for (double v : w.channels[0]) peak = std::max(peak, std::fabs(v));
  CHECK_NEAR(peak, 0.6, 0.01);

  // A fractional count no longer needs a build-time check: counts are
  // Ints, so 2.5 is a *type* error now.
  TempDir bad;
  bad.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let xs : Scalar list = List.repeat 2.5 1.0 ;;
let _ = mix_all (List.repeat 1 (sine 1.0)) |> sample ~from:0s ~to:10ms
        |> render ~name:"x" ~rate:8000.0 ;;
)");
  bad.write("build.json", projectManifest("badrep", {"p.synth"}));
  BuildResult rb = buildProject(bad.dir.string());
  CHECK(!rb.ok);
  CHECK(rb.diags.hasErrors());

  // A negative count types fine and yields the empty list (List.init
  // counts up from 0, so nothing below n=1 produces an element).
  TempDir neg;
  neg.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let xs : Scalar list = List.repeat (0 - 2) 1.0 ;;
let n : Int = List.fold ~f:(fun acc:Int x:Scalar -> acc + 1) ~init:0 ~xs:xs ;;
let _ = mix_all (List.repeat (n + 1) (sine 1.0)) |> sample ~from:0s ~to:10ms
        |> render ~name:"x" ~rate:8000.0 ;;
)");
  neg.write("build.json", projectManifest("negrep", {"p.synth"}));
  BuildResult rn = buildProject(neg.dir.string());
  for (auto& d : rn.diags.items) std::cerr << d.message << "\n";
  CHECK(rn.ok);
}

TEST(build_let_in_matches_flat_version) {
  // The sub-let version and the top-level-lets version of the same
  // program must produce byte-identical artifacts.
  TempDir nested, flat;
  nested.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let song : Scalar Signal =
  let hit : Scalar Sample = sine 440.0 *. exp_decay 12.0
                            |> sample ~from:0s ~to:150ms in
  let beats : Timestamp list = time_steps ~start:0s ~step:200ms ~count:5 in
  place_multi hit beats
;;
let _ = song |> sample ~from:0s ~to:1s |> render ~name:"out" ~rate:8000.0 ;;
)");
  nested.write("build.json", projectManifest("n", {"p.synth"}));
  flat.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sine 440.0 *. exp_decay 12.0
                          |> sample ~from:0s ~to:150ms ;;
let beats : Timestamp list = time_steps ~start:0s ~step:200ms ~count:5 ;;
let song : Scalar Signal = place_multi hit beats ;;
let _ = song |> sample ~from:0s ~to:1s |> render ~name:"out" ~rate:8000.0 ;;
)");
  flat.write("build.json", projectManifest("f", {"p.synth"}));
  BuildResult rn = buildProject(nested.dir.string());
  BuildResult rf = buildProject(flat.dir.string());
  for (auto& d : rn.diags.items) std::cerr << d.message << "\n";
  CHECK(rn.ok);
  CHECK(rf.ok);
  std::string a = slurp(nested.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(flat.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_let_in_shadowing_cache_precision) {
  // A local binding shadowing a module definition must not create a
  // dependency on it: editing the (shadowed, unused) top-level `gain`
  // leaves the target cached.
  TempDir tp;
  tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let gain : Scalar = 0.9 ;;
let voice : Scalar Signal =
  let gain : Scalar = 0.5 in
  sine 440.0 *. gain
;;
let _ = voice |> sample ~from:0s ~to:50ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  tp.write("build.json", projectManifest("shadow", {"p.synth"}));
  BuildCache cache;
  CHECK(buildProject(tp.dir.string(), &cache).ok);
  tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let gain : Scalar = 0.1 ;;
let voice : Scalar Signal =
  let gain : Scalar = 0.5 in
  sine 440.0 *. gain
;;
let _ = voice |> sample ~from:0s ~to:50ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  BuildResult r = buildProject(tp.dir.string(), &cache);
  CHECK(r.ok);
  CHECK(r.targets[0].cached);  // the edit touched only the shadowed def
}

TEST(build_let_in_function_matches_flat_version) {
  // A local function definition and the same function written as a
  // top-level `let` must produce byte-identical artifacts.
  TempDir nested, flat;
  nested.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let song : Scalar Signal =
  let pluck freq:Scalar ~gain:Scalar : Scalar Signal =
    (sine freq) *. (exp_decay 12.0) *. gain in
  let hit : Scalar -> Scalar Sample =
    fun f:Scalar -> pluck f ~gain:0.8 |> sample ~from:0s ~to:150ms in
  mix_all [place (hit 440.0) 0s; place (hit 660.0) 300ms]
;;
let _ = song |> sample ~from:0s ~to:600ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  nested.write("build.json", projectManifest("n", {"p.synth"}));
  flat.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let pluck freq:Scalar ~gain:Scalar : Scalar Signal =
  (sine freq) *. (exp_decay 12.0) *. gain ;;
let hit f:Scalar : Scalar Sample =
  pluck f ~gain:0.8 |> sample ~from:0s ~to:150ms ;;
let song : Scalar Signal =
  mix_all [place (hit 440.0) 0s; place (hit 660.0) 300ms] ;;
let _ = song |> sample ~from:0s ~to:600ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  flat.write("build.json", projectManifest("f", {"p.synth"}));
  BuildResult rn = buildProject(nested.dir.string());
  BuildResult rf = buildProject(flat.dir.string());
  for (auto& d : rn.diags.items) std::cerr << d.message << "\n";
  CHECK(rn.ok);
  CHECK(rf.ok);
  std::string a = slurp(nested.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(flat.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_let_in_function_captures_enclosing_locals) {
  // A local function's body captures earlier locals and the enclosing
  // definition's parameters, like the lambda it desugars to.
  TempDir captured, inlined;
  captured.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let stack detune:Scalar : Scalar Signal =
  let base : Scalar = 220.0 in
  let partial i:Scalar : Scalar Signal = sine (base +. i *. detune) in
  mix_all (List.map partial [0.0; 1.0; 2.0]) ;;
let _ = stack 3.0 |> sample ~from:0s ~to:200ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  captured.write("build.json", projectManifest("cap", {"p.synth"}));
  inlined.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let stack : Scalar Signal =
  mix_all [sine 220.0; sine 223.0; sine 226.0] ;;
let _ = stack |> sample ~from:0s ~to:200ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  inlined.write("build.json", projectManifest("inl", {"p.synth"}));
  BuildResult rc = buildProject(captured.dir.string());
  for (auto& d : rc.diags.items) std::cerr << d.message << "\n";
  CHECK(rc.ok);
  CHECK(buildProject(inlined.dir.string()).ok);
  std::string a = slurp(captured.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(inlined.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_lambda_place_equivalence) {
  // place_multi, an explicit lambda, and a positionally-curried `place`
  // must all render byte-identical artifacts.
  auto write = [](TempDir& tp, const char* song) {
    std::string src = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sample ((sine 660.0) *. (exp_decay 15.0)) 0s 150ms ;;
let song : Scalar Signal = )" + std::string(song) + R"( ;;
let _ = render "out" 8000.0 (sample song 0s 900ms) ;;
)";
    tp.write("p.synth", src);
    tp.write("build.json", projectManifest("eq", {"p.synth"}));
  };
  TempDir multi, lambda, curried;
  write(multi, "place_multi hit [0s; 300ms; 600ms]");
  write(lambda,
        "mix_all (List.map (fun t:Timestamp -> place hit t) [0s; 300ms; 600ms])");
  write(curried, "mix_all (List.map (place hit) [0s; 300ms; 600ms])");
  BuildResult rm = buildProject(multi.dir.string());
  BuildResult rl = buildProject(lambda.dir.string());
  BuildResult rc = buildProject(curried.dir.string());
  for (auto& d : rl.diags.items) std::cerr << d.message << "\n";
  for (auto& d : rc.diags.items) std::cerr << d.message << "\n";
  CHECK(rm.ok);
  CHECK(rl.ok);
  CHECK(rc.ok);
  std::string a = slurp(multi.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(lambda.dir / "_build" / "artifacts" / "out.wav");
  std::string c = slurp(curried.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
  CHECK(a == c);
}

TEST(build_lambda_captures_local) {
  // A lambda capturing a let...in local and a def parameter renders the
  // same artifact as the version with the constant inlined.
  TempDir captured, inlined;
  captured.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let stack detune:Scalar : Scalar Signal =
  let base : Scalar = 220.0 in
  mix_all (List.map (fun i:Scalar -> sine (base +. i *. detune)) [0.0; 1.0; 2.0]) ;;
let _ = stack 3.0 |> sample ~from:0s ~to:200ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  captured.write("build.json", projectManifest("cap", {"p.synth"}));
  inlined.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let stack : Scalar Signal =
  mix_all [sine 220.0; sine 223.0; sine 226.0] ;;
let _ = stack |> sample ~from:0s ~to:200ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  inlined.write("build.json", projectManifest("inl", {"p.synth"}));
  BuildResult rc = buildProject(captured.dir.string());
  for (auto& d : rc.diags.items) std::cerr << d.message << "\n";
  CHECK(rc.ok);
  CHECK(buildProject(inlined.dir.string()).ok);
  std::string a = slurp(captured.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(inlined.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_lambda_shadowing_cache_precision) {
  // A lambda param shadowing a module definition must not create a
  // dependency on it (editing the shadowed def leaves the target cached),
  // while a def genuinely referenced inside a lambda body must.
  const char* fmt = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let gain : Scalar = %s ;;
let level : Scalar = %s ;;
let voice : Scalar Signal =
  mix_all (List.map (fun gain:Scalar -> sine (440.0 *. gain) *. level) [1.0; 2.0]) ;;
let _ = voice |> sample ~from:0s ~to:50ms |> render ~name:"out" ~rate:8000.0 ;;
)";
  auto src = [&](const char* g, const char* l) {
    char buf[512];
    snprintf(buf, sizeof buf, fmt, g, l);
    return std::string(buf);
  };
  TempDir tp;
  tp.write("p.synth", src("0.9", "0.5"));
  tp.write("build.json", projectManifest("lshadow", {"p.synth"}));
  BuildCache cache;
  CHECK(buildProject(tp.dir.string(), &cache).ok);
  // Edit only the shadowed, unused top-level `gain`: still cached.
  tp.write("p.synth", src("0.1", "0.5"));
  BuildResult r1 = buildProject(tp.dir.string(), &cache);
  CHECK(r1.ok);
  CHECK(r1.targets[0].cached);
  // Edit `level`, which the lambda body really references: rebuilt.
  tp.write("p.synth", src("0.1", "0.25"));
  BuildResult r2 = buildProject(tp.dir.string(), &cache);
  CHECK(r2.ok);
  CHECK(!r2.targets[0].cached);
}

TEST(build_open_matches_qualified_output) {
  // open + unqualified access renders byte-identically to import +
  // qualified access.
  TempDir opened, qualified;
  const char* instr =
      "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\n"
      "let tone freq:Scalar : Scalar Signal = sine freq *. exp_decay 8.0 ;;\n";
  opened.write("instr.synth", instr);
  opened.write("song.synth",
               "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nopen Instr\n"
               "let _ = tone 440.0 |> sample ~from:0s ~to:300ms\n"
               "        |> render ~name:\"out\" ~rate:8000.0 ;;\n");
  opened.write("build.json", projectManifest("o", {"song.synth"}));
  qualified.write("instr.synth", instr);
  qualified.write("song.synth",
                  "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Instr\n"
                  "let _ = Instr.tone 440.0 |> sample ~from:0s ~to:300ms\n"
                  "        |> render ~name:\"out\" ~rate:8000.0 ;;\n");
  qualified.write("build.json", projectManifest("q", {"song.synth"}));
  BuildResult ro = buildProject(opened.dir.string());
  for (auto& d : ro.diags.items) std::cerr << d.message << "\n";
  CHECK(ro.ok);
  CHECK(buildProject(qualified.dir.string()).ok);
  std::string a = slurp(opened.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(qualified.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

namespace {

// Build one source and hand back the bytes of the artifact it renders.
std::string renderSource(const std::string& body, bool* ok = nullptr) {
  TempDir d;
  d.write("s.synth",
          "open Core\nopen Core.Dsp\n" + body +
              "\nlet _ = Core.Arrange.sample ~signal:out ~from:0ms ~to:1500ms\n"
              "        |> Core.Render.render ~name:\"o\" ~rate:8000.0 ;;\n");
  d.write("build.json", projectManifest("sw", {"s.synth"}));
  BuildResult r = buildProject(d.dir.string());
  if (ok) *ok = r.ok;
  if (!r.ok && !ok)
    for (auto& x : r.diags.items) std::cerr << x.message << "\n";
  return r.ok ? slurp(d.dir / "_build" / "artifacts" / "o.wav")
              : std::string();
}

}  // namespace

// `signal ~f` applies f once to a *symbolic* time signal, so a comparison
// and an `if` inside f have no single answer: they become sample-wise
// graph nodes. The lambda must render exactly the `select` it stands for.
TEST(build_signal_lambda_comparison_and_if_equal_select) {
  std::string lambda = renderSource(
      "let out : Scalar Signal =\n"
      "  signal ~f:(fun x:Scalar -> if x >=. 1.0 then x /. 2.0 else x) ;;");
  std::string sel = renderSource(
      "let out : Scalar Signal =\n"
      "  select ~gate:time ~threshold:1.0 ~above:(time /. 2.0) ~below:time ;;");
  CHECK(!lambda.empty());
  CHECK(lambda == sel);
}

// `select` tests `gate >= threshold`, so the strict and non-strict forms
// must part company at exactly the boundary sample (t == 1.0 is exact at
// frame 8000 of an 8 kHz render).
TEST(build_signal_lambda_respects_strict_comparisons) {
  std::string ge = renderSource(
      "let out : Scalar Signal =\n"
      "  signal ~f:(fun x:Scalar -> if x >=. 1.0 then 0.5 else 0.0) ;;");
  std::string gt = renderSource(
      "let out : Scalar Signal =\n"
      "  signal ~f:(fun x:Scalar -> if x >. 1.0 then 0.5 else 0.0) ;;");
  CHECK(!ge.empty());
  CHECK(ge != gt);
  // ... and each strict form is the other's complement about the same
  // boundary: (x >= 1) is exactly not (x < 1).
  std::string lt = renderSource(
      "let out : Scalar Signal =\n"
      "  signal ~f:(fun x:Scalar -> if not (x <. 1.0) then 0.5 else 0.0) ;;");
  CHECK(ge == lt);
}

// `&&` and `||` over sample-wise conditions must obey the same algebra
// they do over Bools: `a && b` is `if a then (if b ...) else else-branch`.
TEST(build_signal_lambda_boolean_operators_compose) {
  std::string andForm = renderSource(
      "let out : Scalar Signal =\n"
      "  signal ~f:(fun x:Scalar ->\n"
      "    if x >=. 0.5 && x <. 1.0 then 0.5 else 0.0) ;;");
  std::string nested = renderSource(
      "let out : Scalar Signal =\n"
      "  signal ~f:(fun x:Scalar ->\n"
      "    if x >=. 0.5 then (if x <. 1.0 then 0.5 else 0.0) else 0.0) ;;");
  CHECK(!andForm.empty());
  CHECK(andForm == nested);

  std::string orForm = renderSource(
      "let out : Scalar Signal =\n"
      "  signal ~f:(fun x:Scalar ->\n"
      "    if x <. 0.5 || x >=. 1.0 then 0.5 else 0.0) ;;");
  std::string deMorgan = renderSource(
      "let out : Scalar Signal =\n"
      "  signal ~f:(fun x:Scalar ->\n"
      "    if not (x >=. 0.5 && x <. 1.0) then 0.5 else 0.0) ;;");
  CHECK(!orForm.empty());
  CHECK(orForm == deMorgan);
}

// Core functions written with `if` (Math.min/max/clamp) now work inside
// the lambda too - they are ordinary SynthGraph, not primitives.
TEST(build_signal_lambda_reaches_core_functions_written_with_if) {
  std::string viaMin = renderSource(
      "let out : Scalar Signal =\n"
      "  signal ~f:(fun x:Scalar -> Core.Math.min ~a:x ~b:0.5) ;;");
  std::string byHand = renderSource(
      "let out : Scalar Signal =\n"
      "  signal ~f:(fun x:Scalar -> if x <. 0.5 then x else 0.5) ;;");
  CHECK(!viaMin.empty());
  CHECK(viaMin == byHand);
}

// A sample-wise `if` builds both branches, so a non-numeric branch has
// nowhere to go and says so.
TEST(build_signal_lambda_rejects_non_numeric_branches) {
  TempDir d;
  d.write("s.synth",
          "open Core\nopen Core.Dsp\n"
          "let pick x:Scalar : String = if x >. 1.0 then \"a\" else \"b\" ;;\n"
          "let out : Scalar Signal =\n"
          "  signal ~f:(fun x:Scalar ->\n"
          "    if Core.Str.cat ~a:(pick x) ~b:\"\" ==. \"\" then 0.0 else 1.0) ;;\n");
  d.write("build.json", projectManifest("sw", {"s.synth"}));
  BuildResult r = buildProject(d.dir.string());
  CHECK(!r.ok);
}

TEST(build_open_stale_cache_invalidation) {
  // Editing a def reached through `open` must invalidate the cache - the
  // opened reference is rewritten to a qualified one, so the dependency
  // hasher sees the cross-module edge.
  TempDir tp;
  auto write = [&](const char* freq) {
    tp.write("instr.synth", std::string("open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet tone : Scalar Signal = sine ") +
                                freq + " *. exp_decay 8.0 ;;\n");
  };
  write("440.0");
  tp.write("song.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nopen Instr\n"
           "let _ = tone |> sample ~from:0s ~to:200ms\n"
           "        |> render ~name:\"out\" ~rate:8000.0 ;;\n");
  tp.write("build.json", projectManifest("oc", {"song.synth"}));
  BuildCache cache;
  CHECK(buildProject(tp.dir.string(), &cache).ok);
  std::string before = slurp(tp.dir / "_build" / "artifacts" / "out.wav");
  // Unchanged rebuild: cached.
  BuildResult r1 = buildProject(tp.dir.string(), &cache);
  CHECK(r1.ok);
  CHECK(r1.targets[0].cached);
  // Edit the opened def: must re-render and the artifact must change.
  write("220.0");
  BuildResult r2 = buildProject(tp.dir.string(), &cache);
  CHECK(r2.ok);
  CHECK(!r2.targets[0].cached);
  std::string after = slurp(tp.dir / "_build" / "artifacts" / "out.wav");
  CHECK(before != after);
}

TEST(build_module_alias_stale_cache_invalidation) {
  // Same guarantee for references through a module alias.
  TempDir tp;
  auto write = [&](const char* freq) {
    tp.write("instr.synth", std::string("open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet tone : Scalar Signal = sine ") +
                                freq + " *. exp_decay 8.0 ;;\n");
  };
  write("440.0");
  tp.write("song.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Instr\n"
           "module I = Instr\n"
           "let _ = I.tone |> sample ~from:0s ~to:200ms\n"
           "        |> render ~name:\"out\" ~rate:8000.0 ;;\n");
  tp.write("build.json", projectManifest("ac", {"song.synth"}));
  BuildCache cache;
  CHECK(buildProject(tp.dir.string(), &cache).ok);
  BuildResult r1 = buildProject(tp.dir.string(), &cache);
  CHECK(r1.ok);
  CHECK(r1.targets[0].cached);
  write("220.0");
  BuildResult r2 = buildProject(tp.dir.string(), &cache);
  CHECK(r2.ok);
  CHECK(!r2.targets[0].cached);
}

namespace {

// A root tree: a `Basic` library (with its own render target and an
// internal module) and a `tunes` project consuming it via `dep Basic`.
void writeRootTree(TempTree& tp) {
  tp.write("build.json", rootManifest("demo", {"lib/basic", "tunes"}));
  tp.write("lib/basic/build.json", libraryManifest("Basic"));
  tp.write("lib/basic/lib.synth", libraryInterface({"Keys"}));
  tp.write("lib/basic/keys.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Internal\n"
           "let strike freq:Scalar : Scalar Signal =\n"
           "  sine freq *. exp_decay 8.0 *. Internal.base ;;\n"
           "let _ = strike 660.0 |> sample ~from:0s ~to:100ms\n"
           "        |> render ~name:\"keys-demo\" ~rate:8000.0 ;;\n");
  tp.write("lib/basic/internal.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet base : Scalar = 0.5 ;;\n");
  tp.write("tunes/build.json", projectManifest("tunes", {"song.synth"}, {"Basic"}));
  tp.write("tunes/song.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Basic\n"
           "let _ = Basic.Keys.strike 440.0 |> sample ~from:0s ~to:200ms\n"
           "        |> render ~name:\"song\" ~rate:8000.0 ;;\n");
}

}  // namespace

TEST(build_root_builds_all_rules) {
  TempTree tp;
  writeRootTree(tp);
  RootBuildResult rr = buildRoot(tp.dir.string(), BuildOptions{});
  for (auto& d : rr.diags.items) std::cerr << d.message << "\n";
  for (auto& [rule, br] : rr.rules)
    for (auto& d : br.diags.items) std::cerr << rule << ": " << d.message << "\n";
  CHECK(rr.ok);
  CHECK(rr.rules.size() == 2);
  // Each rule renders into its own directory under the root's _build/.
  CHECK(fs::exists(tp.dir / "_build" / "lib" / "basic" / "artifacts" /
                   "keys-demo.wav"));
  CHECK(fs::exists(tp.dir / "_build" / "tunes" / "artifacts" / "song.wav"));
  CHECK(rr.registry.find("Basic") != nullptr);
}

TEST(build_dep_library_renders_suppressed) {
  TempTree tp;
  writeRootTree(tp);
  RootBuildResult rr = buildRoot(tp.dir.string(), BuildOptions{});
  CHECK(rr.ok);
  // The consumer's build renders only its own target - the library's
  // `keys-demo` must not leak into _build/tunes.
  const BuildResult* tunes = nullptr;
  for (auto& [rule, br] : rr.rules)
    if (rule == "tunes") tunes = &br;
  CHECK(tunes != nullptr);
  CHECK(tunes->targets.size() == 1);
  CHECK(tunes->targets[0].name == "song");
  CHECK(!fs::exists(tp.dir / "_build" / "tunes" / "artifacts" /
                    "keys-demo.wav"));
}

TEST(build_root_file_rule) {
  TempTree tp;
  tp.write("build.json", rootManifest("demo", {"tone.synth"}));
  tp.write("tone.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet _ = sine 440.0 |> sample ~from:0s ~to:100ms\n"
           "        |> render ~name:\"tone\" ~rate:8000.0 ;;\n");
  RootBuildResult rr = buildRoot(tp.dir.string(), BuildOptions{});
  for (auto& [rule, br] : rr.rules)
    for (auto& d : br.diags.items) std::cerr << d.message << "\n";
  CHECK(rr.ok);
  // A file rule's outputs mirror the rule path minus its extension.
  CHECK(fs::exists(tp.dir / "_build" / "tone" / "artifacts" / "tone.wav"));
  // An unknown rule fails.
  TempTree bad;
  bad.write("build.json", rootManifest("demo", {"nope"}));
  RootBuildResult rb = buildRoot(bad.dir.string(), BuildOptions{});
  CHECK(!rb.ok);
}

TEST(build_library_standalone) {
  // A dependency-free library builds on its own (no root needed).
  TempTree tp;
  writeRootTree(tp);
  BuildResult r = buildProject((tp.dir / "lib" / "basic").string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.manifest.projectName == "Basic");
  // Built directly, the unit still lands under the enclosing root's
  // _build/ - same place a root build would put it.
  CHECK(fs::exists(tp.dir / "_build" / "lib" / "basic" / "artifacts" /
                   "keys-demo.wav"));
}

TEST(build_library_without_interface_fails) {
  // No lib.synth means no declared public surface: not a buildable library.
  TempTree tp;
  tp.write("build.json", libraryManifest("Bare"));
  tp.write("thing.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = 1.0 ;;\n");
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  bool hinted = false;
  for (auto& d : r.diags.items)
    if (d.message.find("lib.synth") != std::string::npos) hinted = true;
  CHECK(hinted);
}

TEST(build_library_interface_reexports_to_consumer) {
  // lib.synth's own defs are part of the library's surface, and it can
  // rename a member module on the way out.
  TempTree tp;
  tp.write("build.json", rootManifest("demo", {"lib/fx", "tunes"}));
  tp.write("lib/fx/build.json", libraryManifest("Fx"));
  tp.write("lib/fx/lib.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Shape\n"
           "module Tone = Shape ;;\n"
           "let bright freq:Scalar : Scalar Signal = Shape.body freq *. 1.5 ;;\n");
  tp.write("lib/fx/shape.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\n"
           "let body freq:Scalar : Scalar Signal = sine freq *. exp_decay 8.0 ;;\n");
  tp.write("tunes/build.json", projectManifest("tunes", {"song.synth"}, {"Fx"}));
  tp.write("tunes/song.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nimport Fx\n"
           "let _ = (Fx.bright 440.0 +. Fx.Tone.body 220.0)\n"
           "        |> sample ~from:0s ~to:100ms\n"
           "        |> render ~name:\"song\" ~rate:8000.0 ;;\n");
  RootBuildResult rr = buildRoot(tp.dir.string(), BuildOptions{});
  for (auto& d : rr.diags.items) std::cerr << d.message << "\n";
  for (auto& [rule, br] : rr.rules)
    for (auto& d : br.diags.items) std::cerr << rule << ": " << d.message << "\n";
  CHECK(rr.ok);
  CHECK(fs::exists(tp.dir / "_build" / "tunes" / "artifacts" / "song.wav"));
}

TEST(build_subdir_build_resolves_deps_via_root) {
  // Building a dep-carrying project directly resolves libraries through
  // the enclosing root.
  TempTree tp;
  writeRootTree(tp);
  BuildResult r = buildProject((tp.dir / "tunes").string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(fs::exists(tp.dir / "_build" / "tunes" / "artifacts" / "song.wav"));
  // Without an enclosing root, deps are an error.
  TempTree lone;
  lone.write("tunes/build.json", projectManifest("tunes", {"s.synth"}, {"Basic"}));
  lone.write("tunes/s.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = 1.0 ;;\n");
  BuildResult r2 = buildProject((lone.dir / "tunes").string());
  CHECK(!r2.ok);
}

TEST(build_cross_library_byte_identity) {
  // The same defs consumed via a library render byte-identically to an
  // inlined standalone version.
  TempTree tp;
  writeRootTree(tp);
  RootBuildResult rr = buildRoot(tp.dir.string(), BuildOptions{});
  CHECK(rr.ok);
  TempDir inl;
  inl.write("song.synth",
            "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet base : Scalar = 0.5 ;;\n"
            "let strike freq:Scalar : Scalar Signal =\n"
            "  sine freq *. exp_decay 8.0 *. base ;;\n"
            "let _ = strike 440.0 |> sample ~from:0s ~to:200ms\n"
            "        |> render ~name:\"song\" ~rate:8000.0 ;;\n");
  inl.write("build.json", projectManifest("inline", {"song.synth"}));
  CHECK(buildProject(inl.dir.string()).ok);
  std::string a =
      slurp(tp.dir / "_build" / "tunes" / "artifacts" / "song.wav");
  std::string b = slurp(inl.dir / "_build" / "artifacts" / "song.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_root_duplicate_library_names_fail) {
  TempTree tp;
  tp.write("build.json", rootManifest("demo", {"a"}));
  tp.write("a/build.json", libraryManifest("Same"));
  tp.write("a/lib.synth", libraryInterface({"X"}));
  tp.write("a/x.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : Scalar = 1.0 ;;\n");
  tp.write("b/build.json", libraryManifest("Same"));
  tp.write("b/lib.synth", libraryInterface({"Y"}));
  tp.write("b/y.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet y : Scalar = 1.0 ;;\n");
  RootBuildResult rr = buildRoot(tp.dir.string(), BuildOptions{});
  CHECK(!rr.ok);
  CHECK(rr.diags.hasErrors());
}

TEST(build_lint_resolves_libraries_via_root) {
  TempTree tp;
  writeRootTree(tp);
  // A consumer file with a library import lints via the enclosing root,
  // and a library member lints as part of its library.
  DiagnosticBag d1 = lintFiles({(tp.dir / "tunes" / "song.synth").string()});
  for (auto& d : d1.items) std::cerr << d.message << "\n";
  CHECK(!d1.hasErrors());
  DiagnosticBag d2 =
      lintFiles({(tp.dir / "lib" / "basic" / "keys.synth").string()});
  for (auto& d : d2.items) std::cerr << d.message << "\n";
  CHECK(!d2.hasErrors());
}

TEST(build_watch_root_rebuilds_on_library_change) {
  TempTree tp;
  writeRootTree(tp);
  int builds = 0;
  bool edited = false;
  watchRoot(
      tp.dir.string(),
      [&](const RootBuildResult& rr) {
        builds++;
        CHECK(rr.ok);
      },
      [&]() -> bool {
        if (builds == 1 && !edited) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          tp.write("lib/basic/internal.synth",
                   "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet base : Scalar = 0.25 ;;\n");
          fs::last_write_time(tp.dir / "lib" / "basic" / "internal.synth",
                              fs::file_time_type::clock::now() +
                                  std::chrono::seconds(2));
          edited = true;
          return true;
        }
        return builds < 2;
      },
      10);
  CHECK(builds == 2);
}

TEST(build_core_qualified_end_to_end) {
  // Fully qualified Core access renders byte-identically to open Core.
  TempDir qualified, opened;
  qualified.write("q.synth",
                  "import Core\n"
                  "let _ = Core.Render.render \"out\" 8000.0\n"
                  "  (Core.Arrange.sample (Core.Arrange.mix_all (Core.List.init ~n:3\n"
                  "     ~f:(fun i:Int -> Core.Osc.sine (110.0 *. (Core.Math.to_scalar i +. 1.0)))))\n"
                  "   0s 300ms) ;;\n");
  qualified.write("build.json", projectManifest("cq", {"q.synth"}));
  opened.write("o.synth",
               "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\n"
               "let _ = render \"out\" 8000.0\n"
               "  (sample (mix_all (List.init ~n:3\n"
               "     ~f:(fun i:Int -> sine (110.0 *. (to_scalar i +. 1.0)))))\n"
               "   0s 300ms) ;;\n");
  opened.write("build.json", projectManifest("co", {"o.synth"}));
  BuildResult rq = buildProject(qualified.dir.string());
  for (auto& d : rq.diags.items) std::cerr << d.message << "\n";
  CHECK(rq.ok);
  CHECK(buildProject(opened.dir.string()).ok);
  std::string a = slurp(qualified.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(opened.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_render_stems_produces_named_targets) {
  // Each stem must byte-match the same sample rendered individually.
  TempDir stems, solo;
  stems.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let lead : Scalar Sample = sine 440.0 *. 0.5 |> sample ~from:0s ~to:200ms ;;
let bass : Scalar Sample = sine 110.0 *. 0.5 |> sample ~from:0s ~to:200ms ;;
let _ = render_stems ~name:"mix" ~rate:8000.0
                     ~stems:[("lead", lead); ("bass", bass)] ;;
)");
  stems.write("build.json", projectManifest("st", {"p.synth"}));
  solo.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let lead : Scalar Sample = sine 440.0 *. 0.5 |> sample ~from:0s ~to:200ms ;;
let bass : Scalar Sample = sine 110.0 *. 0.5 |> sample ~from:0s ~to:200ms ;;
let _ = lead |> render ~name:"mix-lead" ~rate:8000.0 ;;
let _ = bass |> render ~name:"mix-bass" ~rate:8000.0 ;;
)");
  solo.write("build.json", projectManifest("so", {"p.synth"}));
  BuildResult rs = buildProject(stems.dir.string());
  BuildResult ro = buildProject(solo.dir.string());
  for (auto& d : rs.diags.items) std::cerr << d.message << "\n";
  CHECK(rs.ok);
  CHECK(ro.ok);
  CHECK(rs.targets.size() == 2);
  for (const char* nm : {"mix-lead.wav", "mix-bass.wav"}) {
    std::string a = slurp(stems.dir / "_build" / "artifacts" / nm);
    std::string b = slurp(solo.dir / "_build" / "artifacts" / nm);
    CHECK(!a.empty());
    CHECK(a == b);
  }
  // Both stems appear in metadata as ordinary audio targets.
  std::string meta = slurp(stems.dir / "_build" / "metadata.json");
  CHECK(meta.find("\"name\": \"mix-lead\"") != std::string::npos);
  CHECK(meta.find("\"name\": \"mix-bass\"") != std::string::npos);
}

TEST(build_render_stems_duplicate_labels_fail) {
  TempDir tp;
  tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let s : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:50ms ;;
let _ = render_stems ~name:"mix" ~rate:8000.0
                     ~stems:[("x", s); ("x", s)] ;;
)");
  tp.write("build.json", projectManifest("dup", {"p.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  CHECK(r.diags.hasErrors());
}

TEST(build_render_vis_stems_single_stacked_svg) {
  TempDir tp;
  tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let a : Scalar Sample = sine 440.0 *. 0.5 |> sample ~from:0s ~to:200ms ;;
let b : Scalar Sample = saw 110.0 *. 0.5 |> sample ~from:0s ~to:100ms ;;
let mix : Scalar Sample = sine 440.0 *. 0.25 +. saw 110.0 *. 0.25
                          |> sample ~from:0s ~to:200ms ;;
let _ = render_vis_stems ~name:"stack" ~rate:8000.0
                         ~stems:[("mix", mix); ("lead", a); ("bass", b)] ;;
)");
  tp.write("build.json", projectManifest("vs", {"p.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);  // ONE artifact, not one per lane
  CHECK(r.targets[0].kind == "visual");
  CHECK(r.targets[0].artifact == "_build/artifacts/stack.svg");
  CHECK(r.targets[0].frames == 1600);  // the longest lane (200ms @ 8k)

  std::string svg = slurp(tp.dir / r.targets[0].artifact);
  CHECK(svg.find("<svg") == 0);
  for (const char* label : {"mix", "lead", "bass"})
    CHECK(svg.find(label) != std::string::npos);
  CHECK(svg.find("3 lanes") != std::string::npos);
  // One waveform path per lane.
  size_t paths = 0;
  for (size_t pos = svg.find("<path"); pos != std::string::npos;
       pos = svg.find("<path", pos + 1))
    paths++;
  CHECK(paths == 3);
}

TEST(build_verbose_log_covers_phases_and_targets) {
  TempDir tp;
  tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let base : Scalar Signal = sine 440.0 ;;
let half : Scalar Signal = base *. 0.5 ;;
let _ = half |> sample ~from:0s ~to:50ms |> render ~name:"one" ~rate:8000.0 ;;
let _ = base |> sample ~from:0s ~to:50ms |> render ~name:"two" ~rate:8000.0 ;;
)");
  tp.write("build.json", projectManifest("logs", {"p.synth"}));

  std::vector<std::string> log;
  std::mutex mu;
  BuildOptions options;
  options.log = [&](const std::string& line) {
    std::lock_guard<std::mutex> lock(mu);
    log.push_back(line);
  };
  BuildResult r = buildProject(tp.dir.string(), options);
  CHECK(r.ok);

  auto has = [&](const std::string& needle) {
    for (auto& l : log)
      if (l.find(needle) != std::string::npos) return true;
    return false;
  };
  // Phases.
  CHECK(has("manifest: project 'logs', 1 source(s)"));
  CHECK(has("front-end: 1 module(s), 4 definition(s)"));
  CHECK(has("evaluate: 2 target(s), 0 audio input(s)"));
  // The bundled Core implementations used by this program are external
  // sources, tracked like any other build input.
  CHECK(has(" external source(s)"));
  CHECK(has("render: 2 target(s) across"));
  CHECK(has("done: 2 target(s) (2 rendered, 0 cached, 0 failed)"));
  // Per-target worker lines with timings.
  CHECK(has("[worker "));
  CHECK(has("target 'one' (audio)"));
  CHECK(has("target 'two' (audio)"));
  CHECK(has("(discretize "));
  // Dependency stats: target "one" is declared by a def referencing
  // `half` (1 direct dep) whose closure is _ -> half -> base = 3 defs.
  CHECK(has("deps: 'one'"));
  CHECK(has("1 direct dependency, 0 dependent(s), closure 3 definition(s)"));
  // Target "two" references base directly: closure _ -> base = 2 defs.
  CHECK(has("deps: 'two'"));
  CHECK(has("closure 2 definition(s)"));
  // Every timestamped line carries the elapsed-time header.
  for (auto& l : log) CHECK(l.find("s] ") != std::string::npos);
}

TEST(build_verbose_log_reports_cache_hits) {
  TempDir tp;
  tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = sine 440.0 |> sample ~from:0s ~to:20ms |> render ~name:"t" ~rate:8000.0 ;;
)");
  tp.write("build.json", projectManifest("chlog", {"p.synth"}));

  BuildCache cache;
  CHECK(buildProject(tp.dir.string(), &cache).ok);

  std::vector<std::string> log;
  BuildOptions options;
  options.cache = &cache;
  options.log = [&](const std::string& line) { log.push_back(line); };
  BuildResult r = buildProject(tp.dir.string(), options);
  CHECK(r.ok);
  auto has = [&](const std::string& needle) {
    for (auto& l : log)
      if (l.find(needle) != std::string::npos) return true;
    return false;
  };
  CHECK(has("target 't': cached, artifact reused"));
  CHECK(has("cache: 1/1 target(s) fresh, 0 to render"));
  CHECK(has("done: 1 target(s) (0 rendered, 1 cached, 0 failed)"));
}

TEST(build_def_graph_stats) {
  TempDir tp;
  tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let base : Scalar Signal = sine 440.0 ;;
let a : Scalar Signal = base *. 0.5 ;;
let b : Scalar Signal = base +. a ;;
let _ = b |> sample ~from:0s ~to:10ms |> render ~name:"t" ~rate:8000.0 ;;
)");
  tp.write("build.json", projectManifest("stats", {"p.synth"}));
  DiagnosticBag diags;
  Program prog = checkProject({(tp.dir / "p.synth").string()}, diags);
  CHECK(!diags.hasErrors());
  auto stats = defGraphStats(prog);
  auto find = [&](const std::string& name) -> const DefStats* {
    for (auto& [def, st] : stats)
      if (def->name == name) return &st;
    return nullptr;
  };
  const DefStats* base = find("base");
  const DefStats* a = find("a");
  const DefStats* b = find("b");
  CHECK(base && base->directDeps == 0 && base->dependents == 2);
  CHECK(base->closureSize == 1);
  CHECK(a && a->directDeps == 1 && a->dependents == 1);
  CHECK(b && b->directDeps == 2 && b->dependents == 1);  // the `_` target
  CHECK(b->closureSize == 3);  // b, a, base
}

TEST(build_metadata_includes_render_ms) {
  TempDir tp;
  tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = sine 440.0 |> sample ~from:0s ~to:50ms |> render ~name:"t" ~rate:8000.0 ;;
)");
  tp.write("build.json", projectManifest("ms", {"p.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(r.ok);
  CHECK(r.targets[0].renderMillis > 0);
  std::string meta = slurp(tp.dir / "_build" / "metadata.json");
  CHECK(meta.find("\"render_ms\": ") != std::string::npos);
}

TEST(build_jitter_deterministic_and_distinct) {
  // Humanized timing is pseudo-random but pure: two identical projects
  // produce byte-identical artifacts, a different seed produces a
  // different (but valid) one, and the unjittered grid differs from
  // both.
  const char* jittered = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sine 660.0 *. exp_decay 20.0 |> sample ~from:0s ~to:100ms ;;
let beats : Timestamp list =
  time_steps ~start:100ms ~step:200ms ~count:5 |> jitter ~seed:7.0 ~spread:10ms ;;
let _ = place_multi hit beats |> sample ~from:0s ~to:1200ms
        |> render ~name:"out" ~rate:8000.0 ;;
)";
  TempDir a, b, c, d;
  a.write("p.synth", jittered);
  a.write("build.json", projectManifest("ja", {"p.synth"}));
  b.write("p.synth", jittered);
  b.write("build.json", projectManifest("jb", {"p.synth"}));
  c.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sine 660.0 *. exp_decay 20.0 |> sample ~from:0s ~to:100ms ;;
let beats : Timestamp list =
  time_steps ~start:100ms ~step:200ms ~count:5 |> jitter ~seed:8.0 ~spread:10ms ;;
let _ = place_multi hit beats |> sample ~from:0s ~to:1200ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  c.write("build.json", projectManifest("jc", {"p.synth"}));
  d.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let hit : Scalar Sample = sine 660.0 *. exp_decay 20.0 |> sample ~from:0s ~to:100ms ;;
let beats : Timestamp list = time_steps ~start:100ms ~step:200ms ~count:5 ;;
let _ = place_multi hit beats |> sample ~from:0s ~to:1200ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  d.write("build.json", projectManifest("jd", {"p.synth"}));
  CHECK(buildProject(a.dir.string()).ok);
  CHECK(buildProject(b.dir.string()).ok);
  CHECK(buildProject(c.dir.string()).ok);
  CHECK(buildProject(d.dir.string()).ok);
  std::string wa = slurp(a.dir / "_build" / "artifacts" / "out.wav");
  std::string wb = slurp(b.dir / "_build" / "artifacts" / "out.wav");
  std::string wc = slurp(c.dir / "_build" / "artifacts" / "out.wav");
  std::string wd = slurp(d.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!wa.empty());
  CHECK(wa == wb);  // same seed: reproducible
  CHECK(wa != wc);  // different seed: different feel
  CHECK(wa != wd);  // and not the robotic grid
}

TEST(build_jitter_rejects_negative_spread) {
  TempDir tp;
  tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let beats : Timestamp list = [0s] |> jitter ~seed:1.0 ~spread:0ms - 5ms ;;
let _ = sine 220.0 |> sample ~from:0s ~to:10ms |> render ~name:"x" ~rate:8000.0 ;;
)");
  tp.write("build.json", projectManifest("jn", {"p.synth"}));
  // Negative spread must be rejected; whether it parses as a checker or
  // eval error, the build fails with diagnostics.
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
}

TEST(build_signal_fn_matches_exp_decay) {
  // A hand-built exponential envelope via `signal ~f` must render
  // byte-identically to the exp_decay primitive.
  TempDir a;
  a.write("build.json", projectManifest("a", {"song.synth"}));
  a.write("song.synth",
          "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\n"
          "let _ = exp_decay 3.0 |> sample ~from:0s ~to:200ms\n"
          "        |> render ~name:\"out\" ~rate:8000.0 ;;\n");
  BuildResult ra = buildProject(a.dir.string());
  for (auto& d : ra.diags.items) std::cerr << d.message << "\n";
  CHECK(ra.ok);

  TempDir b;
  b.write("build.json", projectManifest("b", {"song.synth"}));
  b.write("song.synth",
          "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\n"
          "let _ = signal ~f:(fun t:Scalar -> exp (0.0 -. 3.0 *. t))\n"
          "        |> sample ~from:0s ~to:200ms\n"
          "        |> render ~name:\"out\" ~rate:8000.0 ;;\n");
  BuildResult rb = buildProject(b.dir.string());
  for (auto& d : rb.diags.items) std::cerr << d.message << "\n";
  CHECK(rb.ok);

  std::string wa = slurp(a.dir / "_build" / "artifacts" / "out.wav");
  std::string wb = slurp(b.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!wa.empty());
  CHECK(wa == wb);
}

TEST(build_constant_multi_stereo) {
  TempDir tp;
  tp.write("build.json", projectManifest("levels", {"a.synth"}));
  tp.write("a.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\n"
           "let _ = constant_multi [0.25; 0.5] |> sample ~from:0s ~to:100ms\n"
           "        |> render ~name:\"lv\" ~rate:8000.0 ;;\n");
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData out = readWav((tp.dir / "_build" / "artifacts" / "lv.wav").string());
  CHECK(out.channels.size() == 2);
  CHECK(out.frames() == 800);
  CHECK_NEAR(out.channels[0][10], 0.25, 1e-3);
  CHECK_NEAR(out.channels[1][10], 0.5, 1e-3);
}

TEST(build_time_and_pow_end_to_end) {
  // sqrt time as a fade-in gain, pow as a waveshaper: spot-check values.
  TempDir tp;
  tp.write("build.json", projectManifest("mathy", {"a.synth"}));
  tp.write("a.synth",
           "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\n"
           "let _ = sqrt time |> sample ~from:0s ~to:1s\n"
           "        |> render ~name:\"fade\" ~rate:100.0 ;;\n"
           "let _ = pow (sine 1.0) 3.0 |> sample ~from:0s ~to:1s\n"
           "        |> render ~name:\"shaped\" ~rate:8.0 ;;\n");
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData fade =
      readWav((tp.dir / "_build" / "artifacts" / "fade.wav").string());
  CHECK_NEAR(fade.channels[0][25], std::sqrt(0.25), 1e-3);
  CHECK_NEAR(fade.channels[0][81], std::sqrt(0.81), 1e-3);
  WavData shaped =
      readWav((tp.dir / "_build" / "artifacts" / "shaped.wav").string());
  // sine(2*pi*t) at t=1/8 is sqrt(0.5); cubed ~ 0.35355.
  CHECK_NEAR(shaped.channels[0][1], 0.35355, 2e-3);
}

TEST(build_math_domain_error_is_diagnostic) {
  // 'a in the math signatures admits any type at check time; the eval
  // guard must surface as a build diagnostic, not a crash.
  TempDir tp;
  tp.write("build.json", projectManifest("bad", {"a.synth"}));
  tp.write("a.synth", "open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math\nlet x : String = exp \"nope\" ;;");
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  bool found = false;
  for (auto& d : r.diags.items)
    if (d.message.find("exp: expected") != std::string::npos) found = true;
  CHECK(found);
}

TEST(build_inline_modules_end_to_end_and_cache) {
  TempDir tp;
  const char* src = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
module Voice = struct
  let tone freq:Scalar : Scalar Signal = (sine freq) *. (exp_decay 6.0) ;;
  module Fx = struct
    let damp ~input:'a Signal : 'a Signal = lowpass ~cutoff:900.0 input ;;
  end
end ;;
let _ = render "modular" 8000.0
  (sample (Voice.Fx.damp (Voice.tone 440.0)) 0s 100ms) ;;
let _ = render "standalone" 8000.0 (sample ((saw 220.0) *. 0.5) 0s 100ms) ;;
)";
  tp.write("song.synth", src);
  tp.write("build.json", projectManifest("modtest", {"song.synth"}));

  BuildCache cache;
  BuildResult first = buildProject(tp.dir.string(), &cache);
  for (auto& d : first.diags.items) std::cerr << d.message << "\n";
  CHECK(first.ok);
  WavData w =
      readWav((tp.dir / "_build" / "artifacts" / "modular.wav").string());
  double peak = 0;
  for (double s : w.channels[0]) peak = std::max(peak, std::fabs(s));
  CHECK(peak > 0.05);  // the module-built voice actually sounds

  // No edits: everything reused.
  BuildResult second = buildProject(tp.dir.string(), &cache);
  CHECK(second.ok);
  for (auto& t : second.targets) CHECK(t.cached);

  // Edit a definition *inside the nested module*: the dependency hash
  // must reach through the dotted name, re-rendering only its dependent.
  std::string edited = src;
  size_t pos = edited.find("~cutoff:900.0");
  CHECK(pos != std::string::npos);
  edited.replace(pos, 13, "~cutoff:400.0");
  tp.write("song.synth", edited);
  BuildResult third = buildProject(tp.dir.string(), &cache);
  CHECK(third.ok);
  for (auto& t : third.targets) {
    if (t.name == "modular") CHECK(!t.cached);
    if (t.name == "standalone") CHECK(t.cached);
  }
}

TEST(build_if_selects_render_and_branches_are_lazy) {
  TempDir tp;
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let tempo : Scalar = 128.0 ;;
let fast : Bool = tempo >=. 120.0 ;;
let s : Scalar Signal = (sine (if fast then 440.0 else 220.0)) *. 0.5 ;;
let xs : Scalar list =
  if true then [1.0]
  else List.init (0 - 1) (fun i:Int -> to_scalar i) ;;
let guard : Bool =
  false && (List.fold (fun a:Bool x:Scalar -> a) true
              (List.init (0 - 1) (fun i:Int -> to_scalar i))) ;;
let _ =
  if fast && not guard then render "fast" 8000.0 (sample s 0s 100ms)
  else render "slow" 8000.0 (sample s 0s 100ms) ;;
)");
  tp.write("build.json", projectManifest("booltest", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  // The untaken else-branch and the short-circuited right operand both
  // contain List.init with a negative count, which throws at evaluation:
  // the build succeeding proves they never ran.
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  CHECK(r.targets[0].name == "fast");
  CHECK(fs::exists(tp.dir / "_build" / "artifacts" / "fast.wav"));
  CHECK(!fs::exists(tp.dir / "_build" / "artifacts" / "slow.wav"));
}

TEST(build_user_external_end_to_end) {
  // The headline feature: a function implemented in C++ next to the
  // synth file, compiled and loaded at build time.
  TempDir tp;
  tp.write("succ.cpp", R"(
#include <synth/external.hpp>

SYNTH_EXTERNAL(succ) {
  (void)argc; (void)error;
  *result = synth::ext::Value::scalar(args[0].asScalar() + 1.0);
  return true;
}

SYNTH_EXTERNAL(halves) {
  (void)argc; (void)error;
  std::vector<synth::ext::Value> out;
  for (auto& v : args[0].asList())
    out.push_back(synth::ext::Value::scalar(v.asScalar() / 2.0));
  *result = synth::ext::Value::list(std::move(out));
  return true;
}
)");
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let succ a:Scalar : Scalar = external "succ.cpp" ;;
let halves ~xs:Scalar list : Scalar list = external "succ.cpp" ;;
let freqs : Scalar list = halves [880.0; 884.0] ;;
let tone : Scalar Signal =
  (mix_all (List.map (fun f:Scalar -> sine (succ f)) freqs)) *. 0.4 ;;
let _ = render "ext" 8000.0 (sample tone 0s 100ms) ;;
)");
  tp.write("build.json", projectManifest("exttest", {"song.synth"}));

  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  // The C++ file is a tracked build input (daemon watch, cache key).
  bool tracked = false;
  for (auto& in : r.inputs)
    if (in.find("succ.cpp") != std::string::npos) tracked = true;
  CHECK(tracked);
  // One compiled object, cached under the build dir.
  int soCount = 0;
  for (auto& e : fs::directory_iterator(tp.dir / "_build" / "externals"))
    if (e.path().extension() == ".so") soCount++;
  CHECK(soCount == 1);

  // The render is the sum of sines at succ(halves(...)) = 441 and 443 Hz
  // - just check it is alive.
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "ext.wav").string());
  double peak = 0;
  for (double s : w.channels[0]) peak = std::max(peak, std::fabs(s));
  CHECK(peak > 0.1);

  // Rebuild: the cached object is reused (same content hash, same file).
  auto mtimeBefore =
      fs::last_write_time(*fs::directory_iterator(
          tp.dir / "_build" / "externals"));
  BuildResult r2 = buildProject(tp.dir.string());
  CHECK(r2.ok);
  int soAfter = 0;
  for (auto& e : fs::directory_iterator(tp.dir / "_build" / "externals"))
    if (e.path().extension() == ".so") soAfter++;
  CHECK(soAfter == 1);
  (void)mtimeBefore;
}

TEST(build_user_external_compile_error_is_diagnosed) {
  TempDir tp;
  tp.write("bad.cpp", "this is not C++\n");
  tp.write("song.synth",
           "let f ~x:Scalar : Scalar = external \"bad.cpp\" ;;\n"
           "let y : Scalar = f 1.0 ;;");
  tp.write("build.json", projectManifest("extbad", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  bool mentioned = false;
  for (auto& d : r.diags.items)
    if (d.message.find("compiling") != std::string::npos) mentioned = true;
  CHECK(mentioned);
}

TEST(build_user_external_missing_symbol_is_diagnosed) {
  TempDir tp;
  tp.write("other.cpp", R"(
#include <synth/external.hpp>
SYNTH_EXTERNAL(wrong_name) {
  (void)args; (void)argc; (void)error;
  *result = synth::ext::Value::scalar(0.0);
  return true;
}
)");
  tp.write("song.synth",
           "let f ~x:Scalar : Scalar = external \"other.cpp\" ;;\n"
           "let y : Scalar = f 1.0 ;;");
  tp.write("build.json", projectManifest("extsym", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  bool mentioned = false;
  for (auto& d : r.diags.items)
    if (d.message.find("SYNTH_EXTERNAL(f)") != std::string::npos)
      mentioned = true;
  CHECK(mentioned);
}

TEST(build_int_arithmetic_matches_literals) {
  // Int expressions feeding to_scalar land on exactly the value the
  // equivalent Scalar literal spells; / truncates towards zero.
  TempDir computed, literal;
  computed.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let n : Int = 110 * (7 / 2 + 1) ;;
let toward_zero : Int = (0 - 7) / 2 ;;
let freq : Scalar = to_scalar n -. to_scalar (toward_zero + 3) ;;
let _ = sine freq |> sample ~from:0s ~to:100ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  computed.write("build.json", projectManifest("ia", {"p.synth"}));
  literal.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = sine 440.0 |> sample ~from:0s ~to:100ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  literal.write("build.json", projectManifest("ib", {"p.synth"}));
  BuildResult rc = buildProject(computed.dir.string());
  for (auto& d : rc.diags.items) std::cerr << d.message << "\n";
  CHECK(rc.ok);
  CHECK(buildProject(literal.dir.string()).ok);
  std::string a = slurp(computed.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(literal.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_int_division_by_zero_is_build_error) {
  TempDir tp;
  tp.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let zero : Int = 0 ;;
let n : Int = 1 / zero ;;
let _ = sine 440.0 |> sample ~from:0s ~to:10ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  tp.write("build.json", projectManifest("idz", {"p.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  bool found = false;
  for (auto& d : r.diags.items)
    if (d.message.find("division by zero") != std::string::npos) found = true;
  CHECK(found);
}

TEST(build_int_conversions_round_floor_ceil) {
  // round/floor/ceil all land on 440 here, byte-identical to the
  // literal render.
  TempDir computed, literal;
  computed.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = sine (to_scalar (round 439.6)) |> sample ~from:0s ~to:50ms
        |> render ~name:"r" ~rate:8000.0 ;;
let _ = sine (to_scalar (floor 440.9)) |> sample ~from:0s ~to:50ms
        |> render ~name:"f" ~rate:8000.0 ;;
let _ = sine (to_scalar (ceil 439.1)) |> sample ~from:0s ~to:50ms
        |> render ~name:"c" ~rate:8000.0 ;;
)");
  computed.write("build.json", projectManifest("ca", {"p.synth"}));
  literal.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let _ = sine 440.0 |> sample ~from:0s ~to:50ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  literal.write("build.json", projectManifest("cb", {"p.synth"}));
  BuildResult rc = buildProject(computed.dir.string());
  for (auto& d : rc.diags.items) std::cerr << d.message << "\n";
  CHECK(rc.ok);
  CHECK(buildProject(literal.dir.string()).ok);
  std::string want = slurp(literal.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!want.empty());
  for (const char* name : {"r.wav", "f.wav", "c.wav"})
    CHECK(slurp(computed.dir / "_build" / "artifacts" / name) == want);
}

TEST(build_list_init_int_indices_match_manual) {
  TempDir built, manual;
  built.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let stack : Scalar Signal =
  mix_all (List.init 3 (fun i:Int -> sine (110.0 *. to_scalar (i + 1)))) ;;
let _ = stack *. 0.3 |> sample ~from:0s ~to:100ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  built.write("build.json", projectManifest("ii", {"p.synth"}));
  manual.write("p.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let stack : Scalar Signal = mix_all [sine 110.0; sine 220.0; sine 330.0] ;;
let _ = stack *. 0.3 |> sample ~from:0s ~to:100ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  manual.write("build.json", projectManifest("im", {"p.synth"}));
  BuildResult rb = buildProject(built.dir.string());
  for (auto& d : rb.diags.items) std::cerr << d.message << "\n";
  CHECK(rb.ok);
  CHECK(buildProject(manual.dir.string()).ok);
  std::string a = slurp(built.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(manual.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_user_external_int_roundtrip) {
  // Ints cross the user-external boundary in both directions.
  TempDir tp;
  tp.write("twice.cpp", R"(
#include <synth/external.hpp>

SYNTH_EXTERNAL(twice) {
  (void)argc; (void)error;
  *result = synth::ext::Value::integer(args[0].asInt() * 2);
  return true;
}
)");
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let twice n:Int : Int = external "twice.cpp" ;;
let layers : Scalar Signal =
  mix_all (List.repeat (twice 2) (sine 220.0)) *. 0.1 ;;
let _ = layers |> sample ~from:0s ~to:100ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  tp.write("build.json", projectManifest("extint", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  // Four identical layers at 0.1 gain -> amplitude 0.4.
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "out.wav").string());
  double peak = 0;
  for (double v : w.channels[0]) peak = std::max(peak, std::fabs(v));
  CHECK_NEAR(peak, 0.4, 0.01);
}

TEST(build_records_evaluate_and_render) {
  // Records built, updated, projected and passed through an external
  // (mix_all's list elements come out of record fields).
  TempDir tp;
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render
type Voice = { osc : Scalar Signal; vel : Scalar } ;;
let v : Voice = { osc = sine 440.0; vel = 0.5 } ;;
let quiet : Voice = { v with vel = 0.25 } ;;
let mixed : Scalar Signal = v.osc *. v.vel +. quiet.osc *. quiet.vel ;;
let _ = render "rec" 48000.0 (sample mixed 0s 100ms) ;;
)");
  tp.write("build.json", projectManifest("rec-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  CHECK(r.targets[0].ok);
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "rec.wav").string());
  CHECK(w.frames() == 4800);
  // 0.75 * sine(440) peaks around 0.75.
  double peak = 0;
  for (auto s : w.channels[0]) peak = std::max(peak, std::fabs(s));
  CHECK(peak > 0.7);
  CHECK(peak < 0.8);
}

TEST(build_record_through_external_boundary) {
  // A record round-trips opaquely through a polymorphic external:
  // List.map carries Voice values through C++ and back.
  TempDir tp;
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Arrange open Core.Render
type Voice = { osc : Scalar Signal; vel : Scalar } ;;
let mk f:Scalar : Voice = { osc = sine f; vel = 0.5 } ;;
let flatten v:Voice : Scalar Signal = v.osc *. v.vel ;;
let voices : Voice list = List.map ~f:mk ~xs:[220.0; 440.0] ;;
let out : Scalar Signal = mix_all (List.map ~f:flatten ~xs:voices) ;;
let _ = render "vox" 48000.0 (sample out 0s 50ms) ;;
)");
  tp.write("build.json", projectManifest("vox-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets[0].ok);
}

TEST(build_type_decl_edit_invalidates_dependents) {
  // Changing a type declaration changes the closure hash of every
  // definition typed by it - even transitively, and even when the
  // dependent definitions' own source text is unchanged.
  auto check = [](const char* innerField, uint64_t* outHash) {
    TempDir tp;
    tp.write("song.synth", std::string(R"(
type Inner = { x : )") + innerField + R"( } ;;
type Box = { v : Inner } ;;
let use b:Box : Inner = b.v ;;
)");
    DiagnosticBag diags;
    Program prog = checkProject({(tp.dir / "song.synth").string()}, diags);
    CHECK(!diags.hasErrors());
    const CheckedModule* m = nullptr;
    for (auto& mm : prog.modules)
      if (mm.libName != "Core") m = &mm;
    const TopDef* use = nullptr;
    for (auto& d : m->parsed.defs)
      if (d.kind == TopDef::Kind::Let && d.name == "use") use = &d;
    // Stable across repeated hashing of one program.
    CHECK(defClosureHash(prog, *m, *use) == defClosureHash(prog, *m, *use));
    *outHash = defClosureHash(prog, *m, *use);
  };
  uint64_t hScalar = 0, hScalar2 = 0, hVector = 0;
  check("Scalar", &hScalar);
  check("Scalar", &hScalar2);
  check("Vector", &hVector);
  CHECK(hScalar == hScalar2);  // same sources -> same hash across runs
  // `use` and `Box` texts are identical in both programs; only Inner
  // changed, two hops away in the type-dependency chain.
  CHECK(hScalar != hVector);
}

TEST(build_variants_and_match_evaluate) {
  TempDir tp;
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Arrange open Core.Render
type Wave = | Sine | Pulse of Scalar ;;
let osc w:Wave ~freq:Scalar : Scalar Signal =
  match w with
  | Sine -> sine ~freq:freq
  | Pulse duty -> square ~freq:freq *. duty ;;
let out : Scalar Signal = osc Sine ~freq:440.0 +. osc (Pulse 0.25) ~freq:220.0 ;;
let _ = render "waves" 48000.0 (sample out 0s 50ms) ;;
)");
  tp.write("build.json", projectManifest("waves-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  CHECK(r.targets[0].ok);
}

TEST(build_untaken_match_arm_does_not_render) {
  // Render effects in untaken arms never fire (same rule as `if`).
  TempDir tp;
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Arrange open Core.Render
type Mode = | On | Off ;;
let mode : Mode = On ;;
let _ =
  match mode with
  | On -> render "on" 48000.0 (sample (sine 440.0) 0s 10ms)
  | Off -> render "off" 48000.0 (sample (sine 220.0) 0s 10ms) ;;
)");
  tp.write("build.json", projectManifest("mode-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  CHECK(r.targets[0].name == "on");
}

TEST(build_variant_through_external_boundary) {
  // Variants round-trip opaquely through polymorphic externals.
  TempDir tp;
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Arrange open Core.Render
type Wave = | Sine | Pulse of Scalar ;;
let waves : Wave list = List.map ~f:(fun d:Scalar -> Pulse d) ~xs:[0.25; 0.5] ;;
let toSig w:Wave : Scalar Signal =
  match w with
  | Sine -> sine 440.0
  | Pulse d -> square ~freq:110.0 *. d ;;
let out : Scalar Signal = mix_all (List.map ~f:toSig ~xs:waves) ;;
let _ = render "vw" 48000.0 (sample out 0s 20ms) ;;
)");
  tp.write("build.json", projectManifest("vw-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets[0].ok);
}

TEST(build_destructuring_let_evaluates) {
  TempDir tp;
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Arrange open Core.Render
type Env = { freq : Scalar; gain : Scalar } ;;
let e : Env = { freq = 440.0; gain = 0.5 } ;;
let out : Scalar Signal =
  let { freq; gain = g } : Env = e in sine freq *. g ;;
let _ = render "de" 48000.0 (sample out 0s 10ms) ;;
)");
  tp.write("build.json", projectManifest("de-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets[0].ok);
}

TEST(build_let_rec_evaluates) {
  TempDir tp;
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Arrange open Core.Render
let rec fact n:Int : Int = if n <= 1 then 1 else n * fact (n - 1) ;;
type Chain = | End | Link of (Scalar, Chain) ;;
let rec total c:Chain : Scalar =
  match c with
  | End -> 0.0
  | Link (x, rest) -> x +. total rest ;;
let gain : Scalar =
  let rec go n:Int ~acc:Scalar : Scalar =
    if n <= 0 then acc else go (n - 1) ~acc:(acc /. 2.0) in
  go (fact 3) ~acc:32.0 ;;
let t : Scalar = total (Link (0.125, Link (0.125, End))) ;;
let out : Scalar Signal = sine 440.0 *. (gain /. 2.0 +. t) ;;
let _ = render "rec" 48000.0 (sample out 0s 10ms) ;;
)");
  tp.write("build.json", projectManifest("rec-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  // fact 3 = 6 halvings of 32.0 -> 0.5; total = 0.25; amp = 0.5.
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "rec.wav").string());
  double peak = 0;
  for (auto s : w.channels[0]) peak = std::max(peak, std::fabs(s));
  CHECK(peak > 0.45);
  CHECK(peak < 0.55);
}

TEST(build_unbounded_recursion_is_diagnosed) {
  // Non-tail runaway recursion trips the depth guard. (A *tail*-position
  // runaway no longer grows the stack - tail calls are eliminated - and
  // is caught by the tail loop's own, much larger iteration brake; that
  // path is too slow to exercise in a unit test.)
  TempDir tp;
  tp.write("song.synth", R"(
let rec loop n:Int : Int = 1 + loop (n + 1) ;;
let x : Int = loop 0 ;;
)");
  tp.write("build.json", projectManifest("loop-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  bool found = false;
  for (auto& d : r.diags.items)
    if (d.message.find("recursion limit") != std::string::npos)
      found = true;
  CHECK(found);
}

TEST(build_tail_recursion_runs_at_constant_depth) {
  // Tail-call elimination: an accumulator recursion far past the 4096
  // frame guard evaluates fine - depth no longer scales with length.
  TempDir tp;
  tp.write("song.synth", R"(
let rec count n:Int ~acc:Int : Int =
  if n <= 0 then acc else count (n - 1) ~acc:(acc + 1) ;;
let x : Int = count 50000 ~acc:0 ;;
)");
  tp.write("build.json", projectManifest("tail-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
}

TEST(build_deep_recursion_within_limit) {
  // A recursion depth in the thousands (a large musical list) works.
  TempDir tp;
  tp.write("song.synth", R"(
let rec count n:Int ~acc:Int : Int =
  if n <= 0 then acc else count (n - 1) ~acc:(acc + 1) ;;
let x : Int = count 3000 ~acc:0 ;;
)");
  tp.write("build.json", projectManifest("deep-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
}

TEST(build_recursive_def_hash_is_stable) {
  TempDir tp;
  tp.write("song.synth", R"(
let rec fact n:Int : Int = if n <= 1 then 1 else n * fact (n - 1) ;;
let use : Int = fact 5 ;;
)");
  DiagnosticBag diags;
  Program prog = checkProject({(tp.dir / "song.synth").string()}, diags);
  CHECK(!diags.hasErrors());
  const CheckedModule* m = nullptr;
  for (auto& mm : prog.modules)
    if (mm.libName != "Core") m = &mm;
  const TopDef *fact = nullptr, *use = nullptr;
  for (auto& d : m->parsed.defs) {
    if (d.name == "fact") fact = &d;
    if (d.name == "use") use = &d;
  }
  uint64_t h1 = defClosureHash(prog, *m, *fact);
  uint64_t h2 = defClosureHash(prog, *m, *fact);
  CHECK(h1 == h2);  // terminates, deterministically
  CHECK(defClosureHash(prog, *m, *use) != h1);
}

TEST(build_synthgraph_list_functions) {
  // The List module is written in SynthGraph; map/fold/init/repeat all
  // evaluate, lists cross the external boundary both ways, and match
  // takes them apart.
  TempDir tp;
  tp.write("song.synth", R"(
open Core open Core.Osc open Core.Arrange open Core.Render open Core.Time open Core.Math
let rec sum xs:Scalar list : Scalar =
  match xs with
  | Nil -> 0.0
  | Cons (x, rest) -> x +. sum rest ;;
let doubled : Scalar list = List.map ~f:(fun x:Scalar -> x *. 2.0) ~xs:[1.0; 2.0; 3.0] ;;
let total : Scalar = sum doubled ;;
let folded : Scalar = List.fold ~f:(fun a:Scalar x:Scalar -> a +. x) ~init:0.0 ~xs:doubled ;;
let harmonics : Scalar Signal list =
  List.init ~n:3 ~f:(fun i:Int -> sine (110.0 *. (to_scalar i +. 1.0))) ;;
let steps : Timestamp list = time_steps ~start:0s ~step:100ms ~count:3 ;;
let first_step : Timestamp =
  match steps with
  | Nil -> 0s
  | Cons (t, _) -> t ;;
let amp : Scalar = (total +. folded) /. 24.0 ;;
let out : Scalar Signal = mix_all harmonics *. amp ;;
let _ = render "lists" 48000.0 (sample out first_step 50ms) ;;
)");
  tp.write("build.json", projectManifest("lists-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  CHECK(r.targets[0].ok);
  // total = 12, folded = 12 -> amp = 1.0; three sines peak near 3.0
  // scaled by 1.0 via mix_all's normalization behavior - just require
  // non-silence.
  WavData w =
      readWav((tp.dir / "_build" / "artifacts" / "lists.wav").string());
  double peak = 0;
  for (auto s : w.channels[0]) peak = std::max(peak, std::fabs(s));
  CHECK(peak > 0.1);
}

TEST(build_long_list_map_within_recursion_limit) {
  // A list in the thousands maps fine (each element is one recursion
  // level in the SynthGraph List.map).
  TempDir tp;
  tp.write("song.synth", R"(
open Core open Core.Time
let steps : Timestamp list = time_steps ~start:0s ~step:1ms ~count:2000 ;;
let same : Timestamp list = List.map ~f:(fun t:Timestamp -> t) ~xs:steps ;;
let n : Int = List.fold ~f:(fun a:Int x:Timestamp -> a + 1) ~init:0 ~xs:same ;;
)");
  tp.write("build.json", projectManifest("long-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
}

TEST(build_sample_record_projection_and_update) {
  // Cutting a window by record update renders byte-identically to
  // cutting it with Arrange.sample directly.
  TempDir viaUpdate, direct;
  viaUpdate.write("p.synth", R"(
open Core open Core.Osc open Core.Arrange open Core.Render
let whole : Scalar Sample = sample (sine 440.0) 0s 200ms ;;
let cut : Scalar Sample = { whole with from = 50ms; to = 150ms } ;;
let start : Timestamp = cut.from ;;
let _ = render "w" 48000.0 (sample (place cut start) 0s 200ms) ;;
)");
  viaUpdate.write("build.json", projectManifest("upd", {"p.synth"}));
  direct.write("p.synth", R"(
open Core open Core.Osc open Core.Arrange open Core.Render
let cut : Scalar Sample = sample (sine 440.0) 50ms 150ms ;;
let _ = render "w" 48000.0 (sample (place cut 50ms) 0s 200ms) ;;
)");
  direct.write("build.json", projectManifest("dir", {"p.synth"}));
  BuildResult r1 = buildProject(viaUpdate.dir.string());
  for (auto& d : r1.diags.items) std::cerr << d.message << "\n";
  BuildResult r2 = buildProject(direct.dir.string());
  for (auto& d : r2.diags.items) std::cerr << d.message << "\n";
  CHECK(r1.ok);
  CHECK(r2.ok);
  std::string a = slurp(viaUpdate.dir / "_build" / "artifacts" / "w.wav");
  std::string b = slurp(direct.dir / "_build" / "artifacts" / "w.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

// --- The core-library review round: value pins for the new surface ---------

namespace {
// The claims fixture: `ok` folds every claim into one Bool, and the
// artifact matches the 440 Hz reference only if all of them hold.
void checkClaims(const std::string& prelude, const std::string& claims) {
  TempDir derived, expected;
  std::string src = prelude + "\nlet ok : Bool =\n" + claims + R"( ;;
let _ = Core.Osc.sine (if ok then 440.0 else 1.0)
  |> Core.Arrange.sample ~from:0s ~to:100ms
  |> Core.Render.render ~name:"out" ~rate:8000.0 ;;
)";
  derived.write("c.synth", src);
  derived.write("build.json", projectManifest("claims", {"c.synth"}));
  expected.write("c.synth", R"(
open Core
let _ = Core.Osc.sine 440.0
  |> Core.Arrange.sample ~from:0s ~to:100ms
  |> Core.Render.render ~name:"out" ~rate:8000.0 ;;
)");
  expected.write("build.json", projectManifest("claims", {"c.synth"}));
  BuildResult r = buildProject(derived.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(buildProject(expected.dir.string()).ok);
  std::string a = slurp(derived.dir / "_build" / "artifacts" / "out.wav");
  std::string b = slurp(expected.dir / "_build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

// Two projects whose renders must be byte-identical.
void checkSameBytes(const std::string& srcA, const std::string& srcB,
                    const char* artifact) {
  TempDir a, b;
  a.write("x.synth", srcA);
  a.write("build.json", projectManifest("same", {"x.synth"}));
  b.write("x.synth", srcB);
  b.write("build.json", projectManifest("same", {"x.synth"}));
  BuildResult ra = buildProject(a.dir.string());
  for (auto& d : ra.diags.items) std::cerr << d.message << "\n";
  CHECK(ra.ok);
  BuildResult rb = buildProject(b.dir.string());
  for (auto& d : rb.diags.items) std::cerr << d.message << "\n";
  CHECK(rb.ok);
  std::string wa = slurp(a.dir / "_build" / "artifacts" / artifact);
  std::string wb = slurp(b.dir / "_build" / "artifacts" / artifact);
  CHECK(!wa.empty());
  CHECK(wa == wb);
}
}  // namespace

TEST(build_math_hash_is_jitter_in_the_value_domain) {
  // hash and jitter share one splitmix64: a jittered step equals the
  // step plus the hash-derived delta computed on the Scalar side, bit
  // for bit. hash is uniform in [0, 1) and varies with the index.
  checkClaims(R"(
open Core open Core.Math open Core.Time
let u0 : Scalar = hash ~seed:5.0 ~i:0 ;;
let u1 : Scalar = hash ~seed:5.0 ~i:1 ;;
let got : Timestamp =
  List.nth ~xs:(jitter ~seed:5.0 ~spread:(to_sec 0.1) ~steps:[1s]) ~i:0
           ~default:0s ;;
)",
              R"(got ==. to_sec (1.0 +. (u0 *. 2.0 -. 1.0) *. 0.1)
  && u0 >=. 0.0 && u0 <. 1.0 && u1 >=. 0.0 && u1 <. 1.0
  && u0 !=. u1
  && hash ~seed:5.0 ~i:0 ==. u0
  && hash ~seed:6.0 ~i:0 !=. u0)");
}

TEST(build_time_div_rem_values) {
  checkClaims(R"(
open Core open Core.Time
)",
              R"(div ~num:1s ~den:250ms == 4
  && div ~num:(to_sec 0.9) ~den:250ms == 3
  && rem ~num:(to_sec 0.9) ~den:250ms ==. to_sec (0.9 -. 0.25 *. 3.0)
  && div ~num:0s ~den:1s == 0
  && rem ~num:1s ~den:250ms ==. 0s)");
}

TEST(build_time_of_unit_values) {
  // of_sec/of_ms/of_min read a Timestamp back out in a named unit, and
  // are exact inverses of to_sec/to_ms/to_min.
  checkClaims(R"(
open Core open Core.Time
)",
              R"(of_sec ~x:1s ==. 1.0
  && of_sec ~x:1500ms ==. 1.5
  && of_ms ~x:250ms ==. 250.0
  && of_ms ~x:1s ==. 1000.0
  && of_min ~x:90s ==. 1.5
  && of_min ~x:1m ==. 1.0
  && of_sec ~x:0s ==. 0.0
  && of_ms ~x:0s ==. 0.0)");
}

TEST(build_time_of_unit_round_trips_to_unit) {
  // Both directions: to_X then of_X is the identity on the number, and
  // of_X then to_X is the identity on the duration.
  checkClaims(R"(
open Core open Core.Time
)",
              R"(of_sec ~x:(to_sec 0.75) ==. 0.75
  && of_ms ~x:(to_ms 250.0) ==. 250.0
  && of_min ~x:(to_min 2.5) ==. 2.5
  && to_ms (of_ms ~x:1500ms) ==. 1500ms
  && to_min (of_min ~x:90s) ==. 90s
  && of_min ~x:(to_sec 90.0) ==. 1.5)");
}

TEST(build_time_of_unit_reaches_the_scalar_domain) {
  // The point of leaving the time domain: a duration can now drive the
  // ordinary Scalar arithmetic that Timestamps deliberately refuse,
  // including the ratio `1s /. 500ms` is not allowed to be.
  checkClaims(R"(
open Core open Core.Time
let beat : Timestamp = to_min (1.0 /. 120.0) ;;
)",
              R"(of_sec ~x:1s /. of_sec ~x:500ms ==. 2.0
  && of_sec ~x:beat ==. 0.5
  && of_sec ~x:beat *. of_sec ~x:beat ==. 0.25
  && Math.sqrt ~x:(of_sec ~x:(to_sec 4.0)) ==. 2.0
  && of_sec ~x:beat >. 0.4 && of_sec ~x:beat <. 0.6)");
}

TEST(build_time_div_by_zero_is_a_diagnostic) {
  TempDir tp;
  tp.write("t.synth", R"(
open Core open Core.Time
let n : Int = div ~num:1s ~den:0s ;;
)");
  tp.write("build.json", projectManifest("divzero", {"t.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  bool found = false;
  for (auto& d : r.diags.items)
    if (d.message.find("division by 0s") != std::string::npos) found = true;
  CHECK(found);
}

TEST(build_tempo_additions_match_literals) {
  // per_bar is meter-correct (compound included), bars/bar_beats are
  // the derivations they replace, marks equals the hand-summed section
  // starts, and swung_grid is exactly grid |> swing.
  checkClaims(R"(
open Core open Core.Tempo
let t : Tempo = common ~bpm:120.0 ;;
let c68 : Tempo = { bpm = 120.0; meter = { beats = 6; unit = 8 } } ;;
let lm : Timestamp list = marks ~t:t ~bars:[8; 8; 12] ;;
let sg : Timestamp list =
  swung_grid ~t:t ~from:0s ~step:Eighth ~count:8 ~amount:0.33 ;;
let byhand : Timestamp list =
  grid ~t:t ~from:0s ~step:Eighth ~count:8
    |> swing ~amount:0.33 ~step:(value ~t:t ~v:Eighth) ;;
let nth_t xs:Timestamp list i:Int : Timestamp =
  List.nth ~xs:xs ~i:i ~default:99s ;;
)",
              R"(per_bar ~t:t ~v:Sixteenth == 16
  && per_bar ~t:t ~v:Quarter == 4
  && per_bar ~t:c68 ~v:(Dotted Quarter) == 2
  && bars ~t:t ~n:2.5 ==. bar ~t:t *. 2.5
  && bar_beats ~t:t ~n:8 ==. 32.0
  && List.length ~xs:lm == 4
  && nth_t lm 0 ==. 0s
  && nth_t lm 1 ==. at ~t:t ~bar:8 ~beat:0.0
  && nth_t lm 2 ==. at ~t:t ~bar:16 ~beat:0.0
  && nth_t lm 3 ==. at ~t:t ~bar:28 ~beat:0.0
  && nth_t sg 1 ==. nth_t byhand 1
  && nth_t sg 2 ==. nth_t byhand 2
  && nth_t sg 7 ==. nth_t byhand 7)");
}

TEST(build_groove_pattern_matches_place_multi) {
  const char* derived = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Time
let hit : Scalar Sample = sample (sine 440.0 *. exp_decay 20.0) 0s 100ms ;;
let steps : Timestamp list = time_steps ~start:0s ~step:250ms ~count:4 ;;
let a : Scalar Signal = Groove.pattern ~hit:hit ~steps:steps ;;
let b : Scalar Signal =
  Groove.humanized ~hit:hit ~steps:steps ~seed:3.0 ~spread:10ms ;;
let _ = a +. b |> sample ~from:0s ~to:2s |> render ~name:"out" ~rate:8000.0 ;;
)";
  const char* literal = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Time
let hit : Scalar Sample = sample (sine 440.0 *. exp_decay 20.0) 0s 100ms ;;
let steps : Timestamp list = time_steps ~start:0s ~step:250ms ~count:4 ;;
let a : Scalar Signal = place_multi hit steps ;;
let b : Scalar Signal =
  place_multi hit (jitter ~seed:3.0 ~spread:10ms ~steps:steps) ;;
let _ = a +. b |> sample ~from:0s ~to:2s |> render ~name:"out" ~rate:8000.0 ;;
)";
  checkSameBytes(derived, literal, "out.wav");
}

TEST(build_groove_mask_and_euclid_select_the_right_steps) {
  checkClaims(R"(
open Core open Core.Time
let g8 : Timestamp list = time_steps ~start:0s ~step:100ms ~count:8 ;;
let g16 : Timestamp list = time_steps ~start:0s ~step:125ms ~count:16 ;;
let row : Timestamp list =
  Groove.mask ~keep:[true; false; false; true] ~steps:g8 ;;
let e : Timestamp list = Groove.euclid ~hits:5 ~steps:g16 ;;
let nth_t xs:Timestamp list i:Int : Timestamp =
  List.nth ~xs:xs ~i:i ~default:99s ;;
)",
              R"(List.length ~xs:row == 4
  && nth_t row 0 ==. nth_t g8 0
  && nth_t row 1 ==. nth_t g8 3
  && nth_t row 2 ==. nth_t g8 4
  && nth_t row 3 ==. nth_t g8 7
  && List.length ~xs:e == 5
  && nth_t e 0 ==. nth_t g16 0
  && nth_t e 1 ==. nth_t g16 4
  && nth_t e 2 ==. nth_t g16 7
  && nth_t e 3 ==. nth_t g16 10
  && nth_t e 4 ==. nth_t g16 13
  && List.length ~xs:(Groove.euclid ~hits:0 ~steps:g8) == 0
  && List.length ~xs:(Groove.euclid ~hits:9 ~steps:g8) == 8
  && List.length ~xs:(Groove.mask ~keep:[] ~steps:g8) == 8)");
}

TEST(build_scale_open_enums_and_prog_values) {
  checkClaims(R"(
open Core open Core.Pitch open Core.Scale
let hm : Scale = { tonic = { pc = C; oct = 4 };
                   quality = CustomQ [0; 2; 4; 5; 7; 8; 11] } ;;
let th13 : Note list =
  tones ~c:{ root = { pc = C; oct = 3 };
             quality = Shape [0; 4; 7; 10; 14; 17; 21] } ;;
let mm7 : Note list =
  tones ~c:{ root = { pc = C; oct = 3 }; quality = MinMaj7 } ;;
let p : Prog = { key = hm; degrees = [0; 5; 3; 4] } ;;
let s n:Note : Int = step ~note:n ;;
let nth_n xs:Note list i:Int : Note =
  List.nth ~xs:xs ~i:i ~default:(of_step ~step:0) ;;
let c4 : Int = s { pc = C; oct = 4 } ;;
)",
              R"(s (degree ~s:hm ~n:5) == c4 + 8
  && s (degree ~s:hm ~n:7) == c4 + 12
  && s (degree ~s:hm ~n:(-1)) == c4 - 1
  && List.length ~xs:th13 == 7
  && s (nth_n th13 6) - s (nth_n th13 0) == 21
  && s (nth_n mm7 3) - s (nth_n mm7 0) == 11
  && prog_len ~p:p == 4
  && s (prog_root ~p:p ~i:5) == s (prog_root ~p:p ~i:1)
  && s (prog_root ~p:p ~i:1) == s (degree ~s:hm ~n:5)
  && List.length ~xs:(prog_chord ~p:p ~i:2) == 3
  && List.length ~xs:(prog_stack ~p:p ~i:0 ~count:4) == 4
  && s (wrap_to ~note:{ pc = A; oct = 6 } ~low:{ pc = C; oct = 3 }) == 45
  && s (wrap_to ~note:{ pc = C; oct = 1 } ~low:{ pc = C; oct = 3 }) == 36)");
}

TEST(build_score_rhythm_dynamics_and_feel_values) {
  checkClaims(R"(
open Core open Core.Pitch open Core.Tempo open Core.Score
let t : Tempo = common ~bpm:120.0 ;;
let d : Phrase = hits ~n:4 ~len:0.5 ;;
let pat : Phrase = rhythm ~lens:[1.0; 0.5; 2.0] ;;
let accented : Phrase = vels ~p:d ~vs:[1.0; 0.5] ;;
let rising : Phrase = crescendo ~p:d ~from:Piano ~to:Fff ;;
let loose : Phrase = humanize ~p:d ~seed:9.0 ~spread:0.05 ;;
let shuffled : Phrase = shuffle ~p:d ~grid:0.5 ~amount:0.2 ;;
let blue : Event list =
  realize_with ~tempo:t ~pitch:(fun n:Note -> 220.0)
               ~p:(bend ~p:pat ~f:(fun i:Int -> 1200.0)) ;;
let plain : Event list =
  realize_with ~tempo:t ~pitch:(fun n:Note -> 220.0) ~p:pat ;;
let dflt : Step = { note = of_step ~step:0; at = 99.0; len = 0.0;
                    vel = 0.0; bend = 0.0 } ;;
let stp p:Phrase i:Int : Step = List.nth ~xs:p.steps ~i:i ~default:dflt ;;
let edflt : Event = { freq = 0.0; at = 99s; dur = 0s; vel = 0.0 } ;;
let ev es:Event list i:Int : Event =
  List.nth ~xs:es ~i:i ~default:edflt ;;
let u1 : Scalar = Core.Math.hash ~seed:9.0 ~i:1 ;;
)",
              R"(span ~p:d ==. 2.0
  && Pitch.step ~note:(stp d 0).note == 0
  && (stp pat 2).at ==. 1.5 && (stp pat 2).len ==. 2.0
  && (stp accented 0).vel ==. 1.0 && (stp accented 1).vel ==. 0.5
  && (stp accented 2).vel ==. 1.0
  && (stp rising 0).vel ==. amp ~l:Piano
  && (stp rising 3).vel ==. amp ~l:Fff
  && (stp loose 1).at ==. 0.5 +. (u1 *. 2.0 -. 1.0) *. 0.05
  && (stp shuffled 0).at ==. 0.0
  && (stp shuffled 1).at ==. 0.5 +. 0.5 *. 0.2
  && (stp shuffled 2).at ==. 1.0
  && (stp shuffled 3).at ==. 1.5 +. 0.5 *. 0.2
  && (ev blue 0).freq ==. 440.0
  && (ev plain 0).freq ==. 220.0
  && (ev plain 1).at ==. beats ~t:t ~n:1.0
  && (ev plain 2).dur ==. beats ~t:t ~n:2.0)");
}

TEST(build_realize_stays_sugar_for_realize_with) {
  const char* derived = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render
open Core.Pitch open Core.Score
let t : Tempo.Tempo = Tempo.common ~bpm:100.0 ;;
let tune : Tuning = et12 ~ref_hz:440.0 ;;
let p : Phrase = melody ~notes:[{ pc = A; oct = 4 }; { pc = E; oct = 4 }]
                        ~len:1.0 ;;
let voice freq:Scalar dur:Timestamp vel:Scalar : Scalar Sample =
  sine freq *. vel |> sample ~from:0s ~to:dur ;;
let _ = play ~voice:voice ~events:(realize ~tempo:t ~tuning:tune ~p:p)
  |> sample ~from:0s ~to:2s |> render ~name:"out" ~rate:8000.0 ;;
)";
  const char* explicitly = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render
open Core.Pitch open Core.Score
let t : Tempo.Tempo = Tempo.common ~bpm:100.0 ;;
let tune : Tuning = et12 ~ref_hz:440.0 ;;
let p : Phrase = melody ~notes:[{ pc = A; oct = 4 }; { pc = E; oct = 4 }]
                        ~len:1.0 ;;
let voice freq:Scalar dur:Timestamp vel:Scalar : Scalar Sample =
  sine freq *. vel |> sample ~from:0s ~to:dur ;;
let _ = play ~voice:voice
             ~events:(realize_with ~tempo:t ~pitch:(hz ~t:tune) ~p:p)
  |> sample ~from:0s ~to:2s |> render ~name:"out" ~rate:8000.0 ;;
)";
  checkSameBytes(derived, explicitly, "out.wav");
}

TEST(build_fx_sugar_matches_hand_rolled) {
  const char* derived = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render
let x : Scalar Signal = sine 330.0 *. exp_decay 4.0 ;;
let e : Scalar Signal = echoes ~by:100ms ~gain:0.5 ~n:2 ~input:x ;;
let v : Scalar Sample =
  gated ~attack:3ms ~decay:110ms ~sustain:0.5 ~release:60ms ~hold:400ms
        ~input:(sine 220.0) ;;
let _ = e +. place v 1s
  |> sample ~from:0s ~to:2s |> render ~name:"out" ~rate:8000.0 ;;
)";
  const char* literal = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render
let x : Scalar Signal = sine 330.0 *. exp_decay 4.0 ;;
let e : Scalar Signal =
  mix_all [x;
           delay ~by:100ms ~signal:x *. 0.5;
           delay ~by:200ms ~signal:x *. 0.25] ;;
let v : Scalar Sample =
  sine 220.0 *. adsr ~attack:3ms ~decay:110ms ~sustain:0.5 ~release:60ms
                    ~hold:400ms
    |> sample ~from:0s ~to:460ms ;;
let _ = e +. place v 1s
  |> sample ~from:0s ~to:2s |> render ~name:"out" ~rate:8000.0 ;;
)";
  checkSameBytes(derived, literal, "out.wav");
}

TEST(build_mix_matches_hand_rolled) {
  const char* derived = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Time
let a : Scalar Signal = sine 220.0 *. exp_decay 1.0 ;;
let b : Scalar Signal = sine 331.0 *. exp_decay 2.0 ;;
let wide : Vector Signal = Mix.pan ~pos:0.4 ~input:a ;;
let bus : Vector Signal =
  Mix.mix ~parts:[(Mix.db (-6.0), wide);
                  (0.25, Mix.pan ~pos:(-0.5) ~input:b)] ;;
let out : Vector Signal =
  Mix.duck ~ats:[0s; 500ms] ~depth:0.6 ~dip:60ms ~recover:200ms
           ~input:(Mix.gain_db ~x:(-2.0) ~input:bus) ;;
let _ = out |> sample ~from:0s ~to:1s |> render ~name:"out" ~rate:8000.0 ;;
)";
  const char* literal = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Time
open Core.Math
let a : Scalar Signal = sine 220.0 *. exp_decay 1.0 ;;
let b : Scalar Signal = sine 331.0 *. exp_decay 2.0 ;;
let wide : Vector Signal =
  channels [a *. sqrt ((1.0 -. 0.4) *. 0.5); a *. sqrt ((1.0 +. 0.4) *. 0.5)] ;;
let narrow : Vector Signal =
  channels [b *. sqrt ((1.0 -. (-0.5)) *. 0.5);
            b *. sqrt ((1.0 +. (-0.5)) *. 0.5)] ;;
let bus : Vector Signal =
  mix_all [wide *. Score.db ~x:(-6.0); narrow *. 0.25] ;;
let dips : Scalar Signal =
  place_multi (adsr ~attack:0s ~decay:0s ~sustain:1.0 ~release:200ms
                    ~hold:60ms
                 |> sample ~from:0s ~to:260ms)
              [0s; 500ms] ;;
let out : Vector Signal =
  bus *. Score.db ~x:(-2.0) *. (1.0 -. dips *. 0.6) ;;
let _ = out |> sample ~from:0s ~to:1s |> render ~name:"out" ~rate:8000.0 ;;
)";
  checkSameBytes(derived, literal, "out.wav");
}

TEST(build_modulated_filters_end_to_end) {
  // A constant cutoff signal reproduces the fixed filter bit for bit;
  // the resonant/follow/feedback/select round renders and is audible.
  const char* modded = R"(
open Core open Core.Osc open Core.Fx open Core.Sig open Core.Arrange open Core.Render
let x : Scalar Signal = saw 110.0 ;;
let _ = lowpass_mod ~cutoff:(constant 800.0) ~input:x
  |> sample ~from:0s ~to:500ms |> render ~name:"out" ~rate:8000.0 ;;
)";
  const char* fixed = R"(
open Core open Core.Osc open Core.Fx open Core.Sig open Core.Arrange open Core.Render
let x : Scalar Signal = saw 110.0 ;;
let _ = lowpass ~cutoff:800.0 ~input:x
  |> sample ~from:0s ~to:500ms |> render ~name:"out" ~rate:8000.0 ;;
)";
  checkSameBytes(modded, fixed, "out.wav");

  TempDir tp;
  tp.write("s.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Sig open Core.Arrange
open Core.Render
let riser : Scalar Signal =
  resonant ~cutoff:(constant 100.0 +. time *. 900.0) ~q:4.0
           ~input:(saw 55.0) ;;
let env : Scalar Signal = follow ~attack:5ms ~release:50ms ~input:riser ;;
let gated2 : Scalar Signal =
  select ~gate:env ~threshold:0.05 ~above:riser ~below:(constant 0.0) ;;
let dub : Scalar Signal = feedback ~by:150ms ~gain:0.5 ~input:gated2 ;;
let _ = render "out" 8000.0 (sample dub 0s 1s) ;;
)");
  tp.write("build.json", projectManifest("fxr", {"s.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "out.wav").string());
  double peak = 0;
  for (auto s : w.channels[0]) peak = std::max(peak, std::fabs(s));
  CHECK(peak > 0.01);
}

TEST(build_new_fx_validation_diagnostics) {
  auto expectError = [](const char* src, const char* needle) {
    TempDir tp;
    tp.write("s.synth", src);
    tp.write("build.json", projectManifest("bad", {"s.synth"}));
    BuildResult r = buildProject(tp.dir.string());
    CHECK(!r.ok);
    bool found = false;
    for (auto& d : r.diags.items)
      if (d.message.find(needle) != std::string::npos) found = true;
    if (!found)
      for (auto& d : r.diags.items) std::cerr << d.message << "\n";
    CHECK(found);
  };
  expectError(R"(
open Core open Core.Osc open Core.Fx
let x : Scalar Signal = feedback ~by:100ms ~gain:1.0 ~input:(sine 440.0) ;;
)",
              "feedback");
  expectError(R"(
open Core open Core.Osc open Core.Fx open Core.Sig
let x : Scalar Signal =
  resonant ~cutoff:(constant 500.0) ~q:0.0 ~input:(sine 440.0) ;;
)",
              "resonant");
  expectError(R"(
open Core open Core.Osc open Core.Fx open Core.Arrange
let x : Scalar Signal =
  follow ~attack:5ms ~release:50ms
         ~input:(channels [sine 220.0; sine 330.0] *. 1.0) ;;
)",
              "follow");
  expectError(R"(
open Core open Core.Osc open Core.Arrange
let x : Scalar Signal =
  channel ~n:5 ~input:(channels [sine 220.0; sine 330.0]) ;;
)",
              "channel");
}

TEST(build_str_iter_renders_computed_names) {
  TempDir tp;
  tp.write("s.synth", R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render
let section i:Int : Scalar Sample =
  sine (220.0 +. Core.Math.to_scalar i *. 110.0) *. exp_decay 8.0
    |> sample ~from:0s ~to:200ms ;;
let _ = List.iter
  ~f:(fun i:Int ->
        render ~name:(Str.cat ~a:"section-" ~b:(Str.of_int ~n:i))
               ~rate:8000.0 ~sample:(section i))
  ~xs:(List.range ~from:0 ~count:3) ;;
)");
  tp.write("build.json", projectManifest("sections", {"s.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 3);
  for (int i = 0; i < 3; i++) {
    std::string name = "section-" + std::to_string(i) + ".wav";
    CHECK(!slurp(tp.dir / "_build" / "artifacts" / name).empty());
  }
}

TEST(build_dsp_prelude_matches_classic_opens) {
  const char* viaDsp = R"(
open Core
open Core.Dsp
let pluck freq:Scalar : Scalar Signal = sine freq *. exp_decay 6.0 ;;
let win : Scalar Sample = sample (pluck 440.0) 0s 800ms ;;
let _ = mix_all [place win 0s; place win 500ms]
  |> sample ~from:0s ~to:2s |> render ~name:"out" ~rate:8000.0 ;;
)";
  const char* classic = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render open Core.Io open Core.Time open Core.Sig open Core.Math
let pluck freq:Scalar : Scalar Signal = sine freq *. exp_decay 6.0 ;;
let win : Scalar Sample = sample (pluck 440.0) 0s 800ms ;;
let _ = mix_all [place win 0s; place win 500ms]
  |> sample ~from:0s ~to:2s |> render ~name:"out" ~rate:8000.0 ;;
)";
  checkSameBytes(viaDsp, classic, "out.wav");
}

TEST(build_local_inference_matches_annotated) {
  const char* inferred = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render
let freq = 220.0 *. 2.0 ;;
let pluck f:Scalar = sine f *. exp_decay 6.0 ;;
let song =
  let win = sample (pluck freq) 0s 800ms in
  mix_all [place win 0s; place win 500ms] ;;
let _ = render "out" 8000.0 (sample song 0s 2s) ;;
)";
  const char* annotated = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render
let freq : Scalar = 220.0 *. 2.0 ;;
let pluck f:Scalar : Scalar Signal = sine f *. exp_decay 6.0 ;;
let song : Scalar Signal =
  let win : Scalar Sample = sample (pluck freq) 0s 800ms in
  mix_all [place win 0s; place win 500ms] ;;
let _ = render "out" 8000.0 (sample song 0s 2s) ;;
)";
  checkSameBytes(inferred, annotated, "out.wav");
}

TEST(build_broadcast_row_fades_a_stereo_bus) {
  const char* broadcast = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render
let bus : Vector Signal = channels [sine 220.0; sine 331.0] ;;
let out : Vector Signal = bus *. exp_decay 2.0 ;;
let _ = out |> sample ~from:0s ~to:1s |> render ~name:"out" ~rate:8000.0 ;;
)";
  const char* perChannel = R"(
open Core open Core.Osc open Core.Fx open Core.Arrange open Core.Render
let env : Scalar Signal = exp_decay 2.0 ;;
let out : Vector Signal = channels [sine 220.0 *. env; sine 331.0 *. env] ;;
let _ = out |> sample ~from:0s ~to:1s |> render ~name:"out" ~rate:8000.0 ;;
)";
  checkSameBytes(broadcast, perChannel, "out.wav");
}

// --- Live controls (Core.Control) ---------------------------------------

TEST(build_controls_declared_with_defaults) {
  TempDir tp;
  tp.write("a.synth", R"(
open Core open Core.Arrange open Core.Render open Core.Sig
let gain : Scalar = Control.knob ~name:"gain" ~min:0.0 ~max:1.0 ~default:0.25 ;;
let cutoff : Scalar = Control.slider ~name:"cutoff" ~min:100.0 ~max:2000.0 ~default:700.0 ;;
let _ = render "demo" 8000.0 (sample (constant gain) 0s 100ms) ;;
)");
  tp.write("build.json", projectManifest("controls", {"a.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.controls.size() == 2);
  CHECK(r.controls[0].name == "gain");
  CHECK(r.controls[0].kind == "knob");
  CHECK_NEAR(r.controls[0].min, 0.0, 1e-12);
  CHECK_NEAR(r.controls[0].max, 1.0, 1e-12);
  CHECK_NEAR(r.controls[0].defaultValue, 0.25, 1e-12);
  CHECK_NEAR(r.controls[0].value, 0.25, 1e-12);  // no override active
  CHECK(r.controls[1].name == "cutoff");
  CHECK(r.controls[1].kind == "slider");
  CHECK_NEAR(r.controls[1].value, 700.0, 1e-12);

  // The default drives the render...
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "demo.wav").string());
  CHECK_NEAR(w.channels[0][100], 0.25, 0.01);
  // ...and the metadata carries the controls for the dev app.
  std::string meta = slurp(tp.dir / "_build" / "metadata.json");
  CHECK(meta.find("\"controls\": [") != std::string::npos);
  CHECK(meta.find("\"name\": \"cutoff\"") != std::string::npos);
  CHECK(meta.find("\"kind\": \"knob\"") != std::string::npos);
  // The overrides file is a build input, so a daemon watches it.
  bool hasControlsInput = false;
  for (auto& i : r.inputs)
    if (i == r.controlsPath) hasControlsInput = true;
  CHECK(hasControlsInput);
}

TEST(build_controls_overrides_apply_and_clamp) {
  TempDir tp;
  tp.write("a.synth", R"(
open Core open Core.Arrange open Core.Render open Core.Sig
let gain : Scalar = Control.slider ~name:"gain" ~min:0.0 ~max:1.0 ~default:0.25 ;;
let _ = render "demo" 8000.0 (sample (constant gain) 0s 100ms) ;;
)");
  tp.write("build.json", projectManifest("overrides", {"a.synth"}));
  fs::create_directories(tp.dir / "_build");
  {
    std::ofstream out(tp.dir / "_build" / "controls.json");
    out << R"({"overrides": {"gain": 0.75, "unknown": 3.0}})";
  }
  BuildResult r = buildProject(tp.dir.string());
  CHECK(r.ok);
  CHECK(r.controls.size() == 1);
  CHECK_NEAR(r.controls[0].value, 0.75, 1e-12);
  CHECK_NEAR(r.controls[0].defaultValue, 0.25, 1e-12);  // default unchanged
  WavData w = readWav((tp.dir / "_build" / "artifacts" / "demo.wav").string());
  CHECK_NEAR(w.channels[0][100], 0.75, 0.01);

  // An out-of-range override clamps to the declared range.
  {
    std::ofstream out(tp.dir / "_build" / "controls.json");
    out << R"({"overrides": {"gain": 42.0}})";
  }
  BuildResult r2 = buildProject(tp.dir.string());
  CHECK(r2.ok);
  CHECK_NEAR(r2.controls[0].value, 1.0, 1e-12);

  // Malformed overrides fall back to defaults instead of failing.
  {
    std::ofstream out(tp.dir / "_build" / "controls.json");
    out << "{not json";
  }
  BuildResult r3 = buildProject(tp.dir.string());
  CHECK(r3.ok);
  CHECK_NEAR(r3.controls[0].value, 0.25, 1e-12);
}

TEST(build_controls_redeclaration_rules) {
  // The same name with the same kind and range is fine (same value back);
  // a conflicting redeclaration is a build error.
  TempDir tp;
  tp.write("ok.synth", R"(
open Core open Core.Arrange open Core.Render open Core.Sig
let a : Scalar = Control.slider ~name:"amt" ~min:0.0 ~max:1.0 ~default:0.5 ;;
let b : Scalar = Control.slider ~name:"amt" ~min:0.0 ~max:1.0 ~default:0.5 ;;
let _ = render "ok" 8000.0 (sample (constant (a +. b)) 0s 10ms) ;;
)");
  tp.write("build.json", projectManifest("redecl", {"ok.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(r.ok);
  CHECK(r.controls.size() == 1);

  tp.write("ok.synth", R"(
open Core open Core.Arrange open Core.Render open Core.Sig
let a : Scalar = Control.slider ~name:"amt" ~min:0.0 ~max:1.0 ~default:0.5 ;;
let b : Scalar = Control.knob ~name:"amt" ~min:0.0 ~max:2.0 ~default:0.5 ;;
let _ = render "ok" 8000.0 (sample (constant (a +. b)) 0s 10ms) ;;
)");
  BuildResult r2 = buildProject(tp.dir.string());
  CHECK(!r2.ok);
  CHECK(r2.diags.hasErrors());
}

TEST(build_controls_validation_errors) {
  TempDir tp;
  // max must exceed min.
  tp.write("a.synth", R"(
open Core open Core.Arrange open Core.Render open Core.Sig
let x : Scalar = Control.slider ~name:"x" ~min:1.0 ~max:1.0 ~default:1.0 ;;
let _ = render "t" 8000.0 (sample (constant x) 0s 10ms) ;;
)");
  tp.write("build.json", projectManifest("badrange", {"a.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);

  // The default must sit inside [min, max].
  tp.write("a.synth", R"(
open Core open Core.Arrange open Core.Render open Core.Sig
let x : Scalar = Control.slider ~name:"x" ~min:0.0 ~max:1.0 ~default:2.0 ;;
let _ = render "t" 8000.0 (sample (constant x) 0s 10ms) ;;
)");
  BuildResult r2 = buildProject(tp.dir.string());
  CHECK(!r2.ok);
}

TEST(build_controls_cache_invalidates_on_override_change) {
  TempDir tp;
  tp.write("a.synth", R"(
open Core open Core.Arrange open Core.Render open Core.Sig open Core.Osc
let gain : Scalar = Control.slider ~name:"gain" ~min:0.0 ~max:1.0 ~default:0.25 ;;
let _ = render "uses_control" 8000.0 (sample (constant gain) 0s 50ms) ;;
)");
  tp.write("build.json", projectManifest("ctlcache", {"a.synth"}));

  BuildCache cache;
  BuildResult first = buildProject(tp.dir.string(), &cache);
  CHECK(first.ok);
  CHECK(!first.targets[0].cached);
  BuildResult second = buildProject(tp.dir.string(), &cache);
  CHECK(second.targets[0].cached);

  // A moved slider re-renders; the artifact reflects the new value.
  {
    std::ofstream out(tp.dir / "_build" / "controls.json");
    out << R"({"overrides": {"gain": 0.5}})";
  }
  BuildResult third = buildProject(tp.dir.string(), &cache);
  CHECK(third.ok);
  CHECK(!third.targets[0].cached);
  WavData w =
      readWav((tp.dir / "_build" / "artifacts" / "uses_control.wav").string());
  CHECK_NEAR(w.channels[0][100], 0.5, 0.01);

  // Unchanged overrides stay cached (the value, not the file stamp, is
  // what salts the key).
  BuildResult fourth = buildProject(tp.dir.string(), &cache);
  CHECK(fourth.targets[0].cached);
}

TEST(build_watch_rebuilds_on_override_change) {
  // The dev app "attaches" to a watch instance by writing the unit's
  // controls.json; the daemon treats it as an input and rebuilds.
  TempDir tp;
  tp.write("a.synth", R"(
open Core open Core.Arrange open Core.Render open Core.Sig
let gain : Scalar = Control.slider ~name:"gain" ~min:0.0 ~max:1.0 ~default:0.25 ;;
let _ = render "t" 8000.0 (sample (constant gain) 0s 10ms) ;;
)");
  tp.write("build.json", projectManifest("ctlwatch", {"a.synth"}));

  int builds = 0;
  bool changed = false;
  double lastValue = -1;
  watchProject(
      tp.dir.string(),
      [&](const BuildResult& r) {
        builds++;
        CHECK(r.ok);
        CHECK(r.controls.size() == 1);
        lastValue = r.controls[0].value;
      },
      [&] {
        if (builds == 1 && !changed) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          std::ofstream out(tp.dir / "_build" / "controls.json");
          out << R"({"overrides": {"gain": 0.9}})";
          out.close();
          fs::last_write_time(tp.dir / "_build" / "controls.json",
                              fs::file_time_type::clock::now() +
                                  std::chrono::seconds(2));
          changed = true;
        }
        return builds < 2;
      },
      10);
  CHECK(builds == 2);
  CHECK_NEAR(lastValue, 0.9, 1e-12);
}
