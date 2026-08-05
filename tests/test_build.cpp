#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "build.hpp"
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

std::string slurp(const fs::path& p) {
  std::ifstream in(p);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

const char* kPluckSource = R"(
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
)";

}  // namespace

TEST(build_manifest_parsing) {
  Manifest m;
  DiagnosticBag diags;
  bool ok = parseManifest(
      "# a comment\nproject demo\nsource a.synth\nsource b.synth\n",
      ".build", m, diags);
  CHECK(ok);
  CHECK(m.projectName == "demo");
  CHECK(m.sources.size() == 2);
  CHECK(m.sources[1] == "b.synth");
}

TEST(build_manifest_errors) {
  Manifest m;
  DiagnosticBag diags;
  CHECK(!parseManifest("source a.synth\n", ".build", m, diags));
  Manifest m2;
  DiagnosticBag diags2;
  CHECK(!parseManifest("project x\nfrobnicate y\n", ".build", m2, diags2));
}

TEST(build_end_to_end_pluck) {
  // The full §3.4 example: two seconds of audio, four 440 Hz plucks.
  TempDir tp;
  tp.write("pluck.synth", kPluckSource);
  tp.write(".build", "project pluck-demo\nsource pluck.synth\n");

  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  CHECK(r.targets[0].name == "demo");
  CHECK(r.targets[0].ok);
  CHECK(r.targets[0].channelCount == 1);
  CHECK(r.targets[0].frames == 96000);  // 2s at 48 kHz
  CHECK_NEAR(r.targets[0].durationSeconds, 2.0, 1e-9);

  fs::path artifact = tp.dir / "build" / "artifacts" / "demo.wav";
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
  std::string meta = slurp(tp.dir / "build" / "metadata.json");
  CHECK(meta.find("\"project\": \"pluck-demo\"") != std::string::npos);
  CHECK(meta.find("\"name\": \"demo\"") != std::string::npos);
  CHECK(meta.find("\"status\": \"ok\"") != std::string::npos);
}

TEST(build_duplicate_render_names_fail) {
  TempDir tp;
  tp.write("a.synth", R"(
let _ = render "same" 48000.0 (sample (sine 440.0) 0s 100ms) ;;
let _ = render "same" 48000.0 (sample (sine 220.0) 0s 100ms) ;;
)");
  tp.write(".build", "project dup\nsource a.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  CHECK(r.diags.hasErrors());
}

TEST(build_type_error_fails_and_emits_metadata) {
  TempDir tp;
  tp.write("a.synth", "let x : Scalar = sine 440.0 ;;");
  tp.write(".build", "project broken\nsource a.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  std::string meta = slurp(tp.dir / "build" / "metadata.json");
  CHECK(meta.find("\"status\": \"error\"") != std::string::npos);
}

TEST(build_imports_across_files) {
  TempDir tp;
  tp.write("instr.synth", R"(
let tone freq:Scalar : Scalar Signal = (sine freq) * (exp_decay 3.0) ;;
)");
  tp.write("song.synth", R"(
import Instr
let _ = render "song" 44100.0 (sample (Instr.tone 330.0) 0s 500ms) ;;
)");
  tp.write(".build", "project imports\nsource song.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  CHECK(fs::exists(tp.dir / "build" / "artifacts" / "song.wav"));
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
           "let _ = render \"x\" 44100.0 "
           "(sample (load_mono \"stereo.wav\") 0s 10ms) ;;");
  tp.write(".build", "project loads\nsource a.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  CHECK(r.diags.hasErrors());

  // load_multi accepts it.
  TempDir tp2;
  writeWav((tp2.dir / "stereo.wav").string(), 44100.0, 2, interleaved);
  tp2.write("a.synth",
            "let _ = render \"x\" 44100.0 "
            "(sample (load_multi \"stereo.wav\") 0s 10ms) ;;");
  tp2.write(".build", "project loads2\nsource a.synth\n");
  BuildResult r2 = buildProject(tp2.dir.string());
  for (auto& d : r2.diags.items) std::cerr << d.message << "\n";
  CHECK(r2.ok);
  WavData out = readWav((tp2.dir / "build" / "artifacts" / "x.wav").string());
  CHECK(out.channels.size() == 2);
}

TEST(build_lint_mode) {
  TempDir tp;
  std::string good = (tp.dir / "good.synth").string();
  tp.write("good.synth", "let x : Scalar Signal = sine 440.0 ;;");
  DiagnosticBag ok = lintFiles({good});
  CHECK(!ok.hasErrors());

  tp.write("bad.synth", "let x : Scalar = sine 440.0 ;;");
  DiagnosticBag bad = lintFiles({(tp.dir / "bad.synth").string()});
  CHECK(bad.hasErrors());
}

TEST(build_inputs_are_tracked) {
  TempDir tp;
  tp.write("instr.synth",
           "let tone freq:Scalar : Scalar Signal = sine freq ;;");
  tp.write("song.synth", R"(
import Instr
let _ = render "song" 44100.0 (sample (Instr.tone 330.0) 0s 100ms) ;;
)");
  tp.write(".build", "project inputs\nsource song.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  CHECK(r.ok);
  auto has = [&](const std::string& suffix) {
    for (auto& i : r.inputs)
      if (i.size() >= suffix.size() &&
          i.compare(i.size() - suffix.size(), suffix.size(), suffix) == 0)
        return true;
    return false;
  };
  CHECK(has(".build"));
  CHECK(has("song.synth"));
  CHECK(has("instr.synth"));  // discovered via import
}

TEST(build_watch_rebuilds_on_change) {
  TempDir tp;
  tp.write("a.synth",
           "let _ = render \"t\" 8000.0 (sample (sine 440.0) 0s 10ms) ;;");
  tp.write(".build", "project watch\nsource a.synth\n");

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
                   "let _ = render \"t\" 8000.0 "
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
