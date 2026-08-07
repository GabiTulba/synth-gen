#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>

#include "build.hpp"
#include "checker.hpp"
#include "incremental.hpp"
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

TEST(build_modulation_end_to_end) {
  TempDir tp;
  tp.write("modul.synth", R"(
let vibrato : Scalar Signal = fm 440.0 ((sine 5.0) * 20.0) ;;
let tremolo : Scalar Signal = am vibrato (sine 4.0) 0.5 ;;
let bell : Scalar Signal = pm 220.0 ((sine 110.0) * 2.0) ;;
let _ = render "voice" 48000.0 (sample (tremolo * 0.5 + bell * 0.3) 0s 250ms) ;;
)");
  tp.write(".build", "project modulation\nsource modul.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "build" / "artifacts" / "voice.wav").string());
  CHECK(w.frames() == 12000);
  double peak = 0;
  for (double v : w.channels[0]) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.3);  // audibly non-silent
}

TEST(build_delay_echo_end_to_end) {
  TempDir tp;
  tp.write("echo.synth", R"(
let hit : Scalar Signal =
  place (sample ((sine 660.0) * (exp_decay 30.0)) 0s 100ms) 0s ;;
let echoed : Scalar Signal =
  mix_all [hit; (delay 200ms hit) * 0.5; (delay 400ms hit) * 0.25] ;;
let _ = render "echo" 8000.0 (sample echoed 0s 600ms) ;;
)");
  tp.write(".build", "project echo\nsource echo.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "build" / "artifacts" / "echo.wav").string());
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
let hit : Scalar Signal =
  place (sample ((sine 660.0) * (exp_decay 40.0)) 0s 100ms) 0s ;;
let roomy : Scalar Signal = reverb 500ms 0.3 0.6 hit ;;
let _ = render "roomy" 8000.0 (sample roomy 0s 1s) ;;
)");
  tp.write(".build", "project verb\nsource verb.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "build" / "artifacts" / "roomy.wav").string());
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
let snare : Scalar Signal = (noise 1800.0) * (exp_decay 25.0) ;;
let _ = render "snare" 16000.0 (sample snare 0s 400ms) ;;
)");
  tp.write(".build", "project snare\nsource snare.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "build" / "artifacts" / "snare.wav").string());
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
           "let tone freq:Scalar : Scalar Signal = "
           "(sine freq) * (exp_decay 6.0) ;;");
  tp.write("song.synth", R"(
import Instr
let _ = render "uses_instr" 8000.0 (sample (Instr.tone 440.0) 0s 100ms) ;;
let _ = render "standalone" 8000.0 (sample ((saw 220.0) * 0.5) 0s 100ms) ;;
)");
  tp.write(".build", "project cachetest\nsource song.synth\n");

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
           "let tone freq:Scalar : Scalar Signal = "
           "(sine freq) * (exp_decay 9.0) ;;");
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
           "let _ = render \"fromfile\" 8000.0 "
           "(sample (load_mono \"in.wav\") 0s 40ms) ;;");
  tp.write(".build", "project audiocache\nsource a.synth\n");

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
      readWav((tp.dir / "build" / "artifacts" / "fromfile.wav").string());
  CHECK(std::fabs(w.channels[0][10] - 0.5) < 0.01);
}

TEST(build_parallel_targets_all_render) {
  TempDir tp;
  tp.write("many.synth", R"(
let _ = render "t1" 8000.0 (sample ((sine 220.0) * 0.5) 0s 200ms) ;;
let _ = render "t2" 8000.0 (sample ((saw 220.0) * 0.5) 0s 200ms) ;;
let _ = render "t3" 8000.0 (sample ((square 220.0) * 0.5) 0s 200ms) ;;
let _ = render "t4" 8000.0 (sample ((noise 1000.0) * 0.5) 0s 200ms) ;;
let _ = render "t5" 8000.0 (sample ((fm 110.0 ((sine 55.0) * 50.0)) * 0.5) 0s 200ms) ;;
let _ = render "t6" 8000.0 (sample ((reverb 200ms 0.4 0.5 ((sine 330.0) * (exp_decay 10.0)))) 0s 200ms) ;;
)");
  tp.write(".build", "project many\nsource many.synth\n");
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
let voice : Scalar Signal = fm 220.0 ((sine 3.0) * 12.0) ;;
let _ = render "a" 8000.0 (sample ((lowpass 900.0 voice) * 0.6) 0s 300ms) ;;
let _ = render "b" 8000.0 (sample ((delay 50ms voice) * 0.4) 0s 300ms) ;;
)");
    tp.write(".build", "project det\nsource p.synth\n");
  };
  TempDir one, two;
  makeProject(one);
  makeProject(two);
  CHECK(buildProject(one.dir.string()).ok);
  CHECK(buildProject(two.dir.string()).ok);
  for (const char* name : {"a.wav", "b.wav"}) {
    std::string x = slurp(one.dir / "build" / "artifacts" / name);
    std::string y = slurp(two.dir / "build" / "artifacts" / name);
    CHECK(!x.empty());
    CHECK(x == y);
  }
}

TEST(build_watch_uses_incremental_cache) {
  TempDir tp;
  tp.write("w.synth", R"(
let _ = render "one" 8000.0 (sample ((sine 440.0) * 0.5) 0s 50ms) ;;
let _ = render "two" 8000.0 (sample ((saw 110.0) * 0.5) 0s 50ms) ;;
)");
  tp.write(".build", "project watchcache\nsource w.synth\n");

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
          tp.write(".build",
                   "# touched\nproject watchcache\nsource w.synth\n");
          fs::last_write_time(tp.dir / ".build",
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
let tone : Scalar Signal = (sine 440.0) * (exp_decay 6.0) ;;
let _ = render "tone" 8000.0 (sample tone 0s 500ms) ;;
let _ = render_vis "tone-wave" 8000.0 (sample tone 0s 500ms) ;;
)");
  tp.write(".build", "project vis\nsource v.synth\n");
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
  CHECK(svg->artifact == "build/artifacts/tone-wave.svg");
  CHECK(svg->frames == 4000);

  std::string content = slurp(tp.dir / svg->artifact);
  CHECK(content.find("<svg") == 0);
  CHECK(content.find("tone-wave") != std::string::npos);
  CHECK(content.find("<path") != std::string::npos);
  CHECK(content.find("0.500s @ 8000 Hz, 1 channel") != std::string::npos);

  // Metadata carries the kind so the dev app can tell them apart.
  std::string meta = slurp(tp.dir / "build" / "metadata.json");
  CHECK(meta.find("\"kind\": \"visual\"") != std::string::npos);
  CHECK(meta.find("\"kind\": \"audio\"") != std::string::npos);
}

TEST(build_render_vis_multichannel_lanes) {
  TempDir tp;
  tp.write("st.synth", R"(
let _ = render_vis "stereo-wave" 4000.0
  (sample (channels [sine 220.0; sine 224.0]) 0s 1s) ;;
)");
  tp.write(".build", "project visst\nsource st.synth\n");
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
let _ = render "same" 8000.0 (sample (sine 440.0) 0s 100ms) ;;
let _ = render_vis "same" 8000.0 (sample (sine 440.0) 0s 100ms) ;;
)");
  tp.write(".build", "project visdup\nsource dup.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  CHECK(r.diags.hasErrors());
}

TEST(build_distortion_end_to_end) {
  TempDir tp;
  tp.write("dist.synth", R"(
let hot : Scalar Signal = (sine 220.0) * 3.0 ;;
let _ = render "hard" 8000.0 (sample (hard_clip 0.5 hot) 0s 250ms) ;;
let _ = render "soft" 8000.0 (sample (soft_clip 0.5 hot) 0s 250ms) ;;
)");
  tp.write(".build", "project dist\nsource dist.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData hard = readWav((tp.dir / "build" / "artifacts" / "hard.wav").string());
  WavData soft = readWav((tp.dir / "build" / "artifacts" / "soft.wav").string());
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
    tp.write(".build", "project pm\nsource p.synth\n");
  };
  TempDir multi, manual;
  write(multi, R"(
let hit : Scalar Sample = sample ((sine 660.0) * (exp_decay 15.0)) 0s 150ms ;;
let _ = render "out" 8000.0
  (sample (place_multi hit [0s; 200ms; 400ms]) 0s 700ms) ;;
)");
  write(manual, R"(
let hit : Scalar Sample = sample ((sine 660.0) * (exp_decay 15.0)) 0s 150ms ;;
let _ = render "out" 8000.0
  (sample (mix_all [place hit 0s; place hit 200ms; place hit 400ms])
   0s 700ms) ;;
)");
  CHECK(buildProject(multi.dir.string()).ok);
  CHECK(buildProject(manual.dir.string()).ok);
  std::string a = slurp(multi.dir / "build" / "artifacts" / "out.wav");
  std::string b = slurp(manual.dir / "build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_place_multi_overlaps_sum) {
  // Two placements of a constant-ish sample overlapping halfway: the
  // overlap region carries double amplitude.
  TempDir tp;
  tp.write("o.synth", R"(
let level : Scalar Sample = sample (exp_decay 0.0) 0s 200ms ;;
let _ = render "out" 8000.0
  (sample ((place_multi level [0s; 100ms]) * 0.4) 0s 400ms) ;;
)");
  tp.write(".build", "project overlap\nsource o.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "build" / "artifacts" / "out.wav").string());
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
    tp.write(".build", "project cc\nsource c.synth\n");
  };
  TempDir computed, direct;
  write(computed, R"(
let _ = render "out" 8000.0
  (sample ((lowpass ~cutoff:600.0) (saw 220.0)) 0s 500ms) ;;
)");
  write(direct, R"(
let _ = render "out" 8000.0
  (sample (lowpass 600.0 (saw 220.0)) 0s 500ms) ;;
)");
  BuildResult rc = buildProject(computed.dir.string());
  for (auto& d : rc.diags.items) std::cerr << d.message << "\n";
  CHECK(rc.ok);
  CHECK(buildProject(direct.dir.string()).ok);
  std::string a = slurp(computed.dir / "build" / "artifacts" / "out.wav");
  std::string b = slurp(direct.dir / "build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_place_multi_with_stateful_sample) {
  // A reverb-carrying sample placed at several timestamps: every
  // placement replays the same content (state isolation end to end).
  TempDir tp;
  tp.write("s.synth", R"(
let wet : Scalar Sample =
  sample (reverb 100ms 0.3 0.5 ((sine 440.0) * (exp_decay 30.0))) 0s 150ms ;;
let _ = render "out" 8000.0 (sample (place_multi wet [0s; 300ms]) 0s 600ms) ;;
)");
  tp.write(".build", "project statepm\nsource s.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "build" / "artifacts" / "out.wav").string());
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
let voice : Scalar Signal =
  soft_clip 0.8 (lowpass 900.0 ((saw 220.0) * 2.0)) ;;
let _ = render "out" 8000.0 (sample voice 0s 300ms) ;;
)");
  classic.write(".build", "project c\nsource p.synth\n");
  piped.write("p.synth", R"(
let voice : Scalar Signal =
  saw 220.0 * 2.0 |> lowpass ~cutoff:900.0 |> soft_clip ~threshold:0.8 ;;
let _ = sample voice ~from:0s ~to:300ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  piped.write(".build", "project p\nsource p.synth\n");
  BuildResult rc = buildProject(classic.dir.string());
  BuildResult rp = buildProject(piped.dir.string());
  for (auto& d : rc.diags.items) std::cerr << d.message << "\n";
  for (auto& d : rp.diags.items) std::cerr << d.message << "\n";
  CHECK(rc.ok);
  CHECK(rp.ok);
  std::string a = slurp(classic.dir / "build" / "artifacts" / "out.wav");
  std::string b = slurp(piped.dir / "build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_labeled_partial_application_evaluates) {
  // A user function partially applied by label, bound, then finished -
  // and a primitive passed bare to map.
  TempDir tp;
  tp.write("p.synth", R"(
let voice ~amp:Scalar ~freq:Scalar : Scalar Signal = (sine freq) * amp ;;
let quiet : Scalar -> Scalar Signal = voice ~amp:0.25 ;;
let tones : Scalar Signal list = map sine [220.0; 330.0] ;;
let sum : Scalar Signal = (mix_all tones) * 0.2 + quiet 440.0 ;;
let _ = render "out" 8000.0 (sample sum 0s 200ms) ;;
)");
  tp.write(".build", "project partial\nsource p.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "build" / "artifacts" / "out.wav").string());
  double peak = 0;
  for (double v : w.channels[0]) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.3);  // all three tones present
}

TEST(build_list_init_harmonic_stack) {
  // list_init driving additive synthesis: five harmonics of 110 Hz.
  TempDir tp;
  tp.write("p.synth", R"(
let harmonic i:Scalar : Scalar Signal =
  sine (110.0 * (i + 1.0)) * (1.0 / (i + 1.0)) ;;
let stack : Scalar Signal = mix_all (list_init 5.0 harmonic) * 0.3 ;;
let _ = stack |> sample ~from:0s ~to:200ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  tp.write(".build", "project li\nsource p.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  WavData w = readWav((tp.dir / "build" / "artifacts" / "out.wav").string());
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
let hit : Scalar Sample = sine 660.0 * exp_decay 20.0 |> sample ~from:0s ~to:100ms ;;
let _ = place_multi hit (time_steps ~start:0s ~step:150ms ~count:4.0)
        |> sample ~from:0s ~to:600ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  stepped.write(".build", "project ts\nsource p.synth\n");
  manual.write("p.synth", R"(
let hit : Scalar Sample = sine 660.0 * exp_decay 20.0 |> sample ~from:0s ~to:100ms ;;
let _ = place_multi hit [0s; 150ms; 300ms; 450ms]
        |> sample ~from:0s ~to:600ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  manual.write(".build", "project tm\nsource p.synth\n");
  CHECK(buildProject(stepped.dir.string()).ok);
  CHECK(buildProject(manual.dir.string()).ok);
  std::string a = slurp(stepped.dir / "build" / "artifacts" / "out.wav");
  std::string b = slurp(manual.dir / "build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_repeat_and_count_validation) {
  TempDir tp;
  tp.write("p.synth", R"(
let layers : Scalar Signal = mix_all (repeat 3.0 (sine 220.0)) * 0.2 ;;
let _ = layers |> sample ~from:0s ~to:100ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  tp.write(".build", "project rep\nsource p.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  CHECK(r.ok);
  // Three identical layers at 0.2 gain -> amplitude 0.6.
  WavData w = readWav((tp.dir / "build" / "artifacts" / "out.wav").string());
  double peak = 0;
  for (double v : w.channels[0]) peak = std::max(peak, std::fabs(v));
  CHECK_NEAR(peak, 0.6, 0.01);

  // Fractional and negative counts are build errors.
  TempDir bad;
  bad.write("p.synth", R"(
let xs : Scalar list = repeat 2.5 1.0 ;;
let _ = mix_all (repeat 1.0 (sine 1.0)) |> sample ~from:0s ~to:10ms
        |> render ~name:"x" ~rate:8000.0 ;;
)");
  bad.write(".build", "project badrep\nsource p.synth\n");
  BuildResult rb = buildProject(bad.dir.string());
  CHECK(!rb.ok);
  CHECK(rb.diags.hasErrors());
}

TEST(build_let_in_matches_flat_version) {
  // The sub-let version and the top-level-lets version of the same
  // program must produce byte-identical artifacts.
  TempDir nested, flat;
  nested.write("p.synth", R"(
let song : Scalar Signal =
  let hit : Scalar Sample = sine 440.0 * exp_decay 12.0
                            |> sample ~from:0s ~to:150ms in
  let beats : Timestamp list = time_steps ~start:0s ~step:200ms ~count:5.0 in
  place_multi hit beats
;;
let _ = song |> sample ~from:0s ~to:1s |> render ~name:"out" ~rate:8000.0 ;;
)");
  nested.write(".build", "project n\nsource p.synth\n");
  flat.write("p.synth", R"(
let hit : Scalar Sample = sine 440.0 * exp_decay 12.0
                          |> sample ~from:0s ~to:150ms ;;
let beats : Timestamp list = time_steps ~start:0s ~step:200ms ~count:5.0 ;;
let song : Scalar Signal = place_multi hit beats ;;
let _ = song |> sample ~from:0s ~to:1s |> render ~name:"out" ~rate:8000.0 ;;
)");
  flat.write(".build", "project f\nsource p.synth\n");
  BuildResult rn = buildProject(nested.dir.string());
  BuildResult rf = buildProject(flat.dir.string());
  for (auto& d : rn.diags.items) std::cerr << d.message << "\n";
  CHECK(rn.ok);
  CHECK(rf.ok);
  std::string a = slurp(nested.dir / "build" / "artifacts" / "out.wav");
  std::string b = slurp(flat.dir / "build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_let_in_shadowing_cache_precision) {
  // A local binding shadowing a module definition must not create a
  // dependency on it: editing the (shadowed, unused) top-level `gain`
  // leaves the target cached.
  TempDir tp;
  tp.write("p.synth", R"(
let gain : Scalar = 0.9 ;;
let voice : Scalar Signal =
  let gain : Scalar = 0.5 in
  sine 440.0 * gain
;;
let _ = voice |> sample ~from:0s ~to:50ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  tp.write(".build", "project shadow\nsource p.synth\n");
  BuildCache cache;
  CHECK(buildProject(tp.dir.string(), &cache).ok);
  tp.write("p.synth", R"(
let gain : Scalar = 0.1 ;;
let voice : Scalar Signal =
  let gain : Scalar = 0.5 in
  sine 440.0 * gain
;;
let _ = voice |> sample ~from:0s ~to:50ms |> render ~name:"out" ~rate:8000.0 ;;
)");
  BuildResult r = buildProject(tp.dir.string(), &cache);
  CHECK(r.ok);
  CHECK(r.targets[0].cached);  // the edit touched only the shadowed def
}

TEST(build_lambda_place_equivalence) {
  // place_multi, an explicit lambda, and a positionally-curried `place`
  // must all render byte-identical artifacts.
  auto write = [](TempDir& tp, const char* song) {
    std::string src = R"(
let hit : Scalar Sample = sample ((sine 660.0) * (exp_decay 15.0)) 0s 150ms ;;
let song : Scalar Signal = )" + std::string(song) + R"( ;;
let _ = render "out" 8000.0 (sample song 0s 900ms) ;;
)";
    tp.write("p.synth", src);
    tp.write(".build", "project eq\nsource p.synth\n");
  };
  TempDir multi, lambda, curried;
  write(multi, "place_multi hit [0s; 300ms; 600ms]");
  write(lambda,
        "mix_all (map (fun t:Timestamp -> place hit t) [0s; 300ms; 600ms])");
  write(curried, "mix_all (map (place hit) [0s; 300ms; 600ms])");
  BuildResult rm = buildProject(multi.dir.string());
  BuildResult rl = buildProject(lambda.dir.string());
  BuildResult rc = buildProject(curried.dir.string());
  for (auto& d : rl.diags.items) std::cerr << d.message << "\n";
  for (auto& d : rc.diags.items) std::cerr << d.message << "\n";
  CHECK(rm.ok);
  CHECK(rl.ok);
  CHECK(rc.ok);
  std::string a = slurp(multi.dir / "build" / "artifacts" / "out.wav");
  std::string b = slurp(lambda.dir / "build" / "artifacts" / "out.wav");
  std::string c = slurp(curried.dir / "build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
  CHECK(a == c);
}

TEST(build_lambda_captures_local) {
  // A lambda capturing a let...in local and a def parameter renders the
  // same artifact as the version with the constant inlined.
  TempDir captured, inlined;
  captured.write("p.synth", R"(
let stack detune:Scalar : Scalar Signal =
  let base : Scalar = 220.0 in
  mix_all (map (fun i:Scalar -> sine (base + i * detune)) [0.0; 1.0; 2.0]) ;;
let _ = stack 3.0 |> sample ~from:0s ~to:200ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  captured.write(".build", "project cap\nsource p.synth\n");
  inlined.write("p.synth", R"(
let stack : Scalar Signal =
  mix_all [sine 220.0; sine 223.0; sine 226.0] ;;
let _ = stack |> sample ~from:0s ~to:200ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  inlined.write(".build", "project inl\nsource p.synth\n");
  BuildResult rc = buildProject(captured.dir.string());
  for (auto& d : rc.diags.items) std::cerr << d.message << "\n";
  CHECK(rc.ok);
  CHECK(buildProject(inlined.dir.string()).ok);
  std::string a = slurp(captured.dir / "build" / "artifacts" / "out.wav");
  std::string b = slurp(inlined.dir / "build" / "artifacts" / "out.wav");
  CHECK(!a.empty());
  CHECK(a == b);
}

TEST(build_lambda_shadowing_cache_precision) {
  // A lambda param shadowing a module definition must not create a
  // dependency on it (editing the shadowed def leaves the target cached),
  // while a def genuinely referenced inside a lambda body must.
  const char* fmt = R"(
let gain : Scalar = %s ;;
let level : Scalar = %s ;;
let voice : Scalar Signal =
  mix_all (map (fun gain:Scalar -> sine (440.0 * gain) * level) [1.0; 2.0]) ;;
let _ = voice |> sample ~from:0s ~to:50ms |> render ~name:"out" ~rate:8000.0 ;;
)";
  auto src = [&](const char* g, const char* l) {
    char buf[512];
    snprintf(buf, sizeof buf, fmt, g, l);
    return std::string(buf);
  };
  TempDir tp;
  tp.write("p.synth", src("0.9", "0.5"));
  tp.write(".build", "project lshadow\nsource p.synth\n");
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

TEST(build_render_stems_produces_named_targets) {
  // Each stem must byte-match the same sample rendered individually.
  TempDir stems, solo;
  stems.write("p.synth", R"(
let lead : Scalar Sample = sine 440.0 * 0.5 |> sample ~from:0s ~to:200ms ;;
let bass : Scalar Sample = sine 110.0 * 0.5 |> sample ~from:0s ~to:200ms ;;
let _ = render_stems ~name:"mix" ~rate:8000.0
                     ~stems:[("lead", lead); ("bass", bass)] ;;
)");
  stems.write(".build", "project st\nsource p.synth\n");
  solo.write("p.synth", R"(
let lead : Scalar Sample = sine 440.0 * 0.5 |> sample ~from:0s ~to:200ms ;;
let bass : Scalar Sample = sine 110.0 * 0.5 |> sample ~from:0s ~to:200ms ;;
let _ = lead |> render ~name:"mix-lead" ~rate:8000.0 ;;
let _ = bass |> render ~name:"mix-bass" ~rate:8000.0 ;;
)");
  solo.write(".build", "project so\nsource p.synth\n");
  BuildResult rs = buildProject(stems.dir.string());
  BuildResult ro = buildProject(solo.dir.string());
  for (auto& d : rs.diags.items) std::cerr << d.message << "\n";
  CHECK(rs.ok);
  CHECK(ro.ok);
  CHECK(rs.targets.size() == 2);
  for (const char* nm : {"mix-lead.wav", "mix-bass.wav"}) {
    std::string a = slurp(stems.dir / "build" / "artifacts" / nm);
    std::string b = slurp(solo.dir / "build" / "artifacts" / nm);
    CHECK(!a.empty());
    CHECK(a == b);
  }
  // Both stems appear in metadata as ordinary audio targets.
  std::string meta = slurp(stems.dir / "build" / "metadata.json");
  CHECK(meta.find("\"name\": \"mix-lead\"") != std::string::npos);
  CHECK(meta.find("\"name\": \"mix-bass\"") != std::string::npos);
}

TEST(build_render_stems_duplicate_labels_fail) {
  TempDir tp;
  tp.write("p.synth", R"(
let s : Scalar Sample = sine 440.0 |> sample ~from:0s ~to:50ms ;;
let _ = render_stems ~name:"mix" ~rate:8000.0
                     ~stems:[("x", s); ("x", s)] ;;
)");
  tp.write(".build", "project dup\nsource p.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
  CHECK(r.diags.hasErrors());
}

TEST(build_render_vis_stems_single_stacked_svg) {
  TempDir tp;
  tp.write("p.synth", R"(
let a : Scalar Sample = sine 440.0 * 0.5 |> sample ~from:0s ~to:200ms ;;
let b : Scalar Sample = saw 110.0 * 0.5 |> sample ~from:0s ~to:100ms ;;
let mix : Scalar Sample = sine 440.0 * 0.25 + saw 110.0 * 0.25
                          |> sample ~from:0s ~to:200ms ;;
let _ = render_vis_stems ~name:"stack" ~rate:8000.0
                         ~stems:[("mix", mix); ("lead", a); ("bass", b)] ;;
)");
  tp.write(".build", "project vs\nsource p.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  for (auto& d : r.diags.items) std::cerr << d.message << "\n";
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);  // ONE artifact, not one per lane
  CHECK(r.targets[0].kind == "visual");
  CHECK(r.targets[0].artifact == "build/artifacts/stack.svg");
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
let base : Scalar Signal = sine 440.0 ;;
let half : Scalar Signal = base * 0.5 ;;
let _ = half |> sample ~from:0s ~to:50ms |> render ~name:"one" ~rate:8000.0 ;;
let _ = base |> sample ~from:0s ~to:50ms |> render ~name:"two" ~rate:8000.0 ;;
)");
  tp.write(".build", "project logs\nsource p.synth\n");

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
let _ = sine 440.0 |> sample ~from:0s ~to:20ms |> render ~name:"t" ~rate:8000.0 ;;
)");
  tp.write(".build", "project chlog\nsource p.synth\n");

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
let base : Scalar Signal = sine 440.0 ;;
let a : Scalar Signal = base * 0.5 ;;
let b : Scalar Signal = base + a ;;
let _ = b |> sample ~from:0s ~to:10ms |> render ~name:"t" ~rate:8000.0 ;;
)");
  tp.write(".build", "project stats\nsource p.synth\n");
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
let _ = sine 440.0 |> sample ~from:0s ~to:50ms |> render ~name:"t" ~rate:8000.0 ;;
)");
  tp.write(".build", "project ms\nsource p.synth\n");
  BuildResult r = buildProject(tp.dir.string());
  CHECK(r.ok);
  CHECK(r.targets[0].renderMillis > 0);
  std::string meta = slurp(tp.dir / "build" / "metadata.json");
  CHECK(meta.find("\"render_ms\": ") != std::string::npos);
}

TEST(build_jitter_deterministic_and_distinct) {
  // Humanized timing is pseudo-random but pure: two identical projects
  // produce byte-identical artifacts, a different seed produces a
  // different (but valid) one, and the unjittered grid differs from
  // both.
  const char* jittered = R"(
let hit : Scalar Sample = sine 660.0 * exp_decay 20.0 |> sample ~from:0s ~to:100ms ;;
let beats : Timestamp list =
  time_steps ~start:100ms ~step:200ms ~count:5.0 |> jitter ~seed:7.0 ~spread:10ms ;;
let _ = place_multi hit beats |> sample ~from:0s ~to:1200ms
        |> render ~name:"out" ~rate:8000.0 ;;
)";
  TempDir a, b, c, d;
  a.write("p.synth", jittered);
  a.write(".build", "project ja\nsource p.synth\n");
  b.write("p.synth", jittered);
  b.write(".build", "project jb\nsource p.synth\n");
  c.write("p.synth", R"(
let hit : Scalar Sample = sine 660.0 * exp_decay 20.0 |> sample ~from:0s ~to:100ms ;;
let beats : Timestamp list =
  time_steps ~start:100ms ~step:200ms ~count:5.0 |> jitter ~seed:8.0 ~spread:10ms ;;
let _ = place_multi hit beats |> sample ~from:0s ~to:1200ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  c.write(".build", "project jc\nsource p.synth\n");
  d.write("p.synth", R"(
let hit : Scalar Sample = sine 660.0 * exp_decay 20.0 |> sample ~from:0s ~to:100ms ;;
let beats : Timestamp list = time_steps ~start:100ms ~step:200ms ~count:5.0 ;;
let _ = place_multi hit beats |> sample ~from:0s ~to:1200ms
        |> render ~name:"out" ~rate:8000.0 ;;
)");
  d.write(".build", "project jd\nsource p.synth\n");
  CHECK(buildProject(a.dir.string()).ok);
  CHECK(buildProject(b.dir.string()).ok);
  CHECK(buildProject(c.dir.string()).ok);
  CHECK(buildProject(d.dir.string()).ok);
  std::string wa = slurp(a.dir / "build" / "artifacts" / "out.wav");
  std::string wb = slurp(b.dir / "build" / "artifacts" / "out.wav");
  std::string wc = slurp(c.dir / "build" / "artifacts" / "out.wav");
  std::string wd = slurp(d.dir / "build" / "artifacts" / "out.wav");
  CHECK(!wa.empty());
  CHECK(wa == wb);  // same seed: reproducible
  CHECK(wa != wc);  // different seed: different feel
  CHECK(wa != wd);  // and not the robotic grid
}

TEST(build_jitter_rejects_negative_spread) {
  TempDir tp;
  tp.write("p.synth", R"(
let beats : Timestamp list = [0s] |> jitter ~seed:1.0 ~spread:0ms - 5ms ;;
let _ = sine 220.0 |> sample ~from:0s ~to:10ms |> render ~name:"x" ~rate:8000.0 ;;
)");
  tp.write(".build", "project jn\nsource p.synth\n");
  // Negative spread must be rejected; whether it parses as a checker or
  // eval error, the build fails with diagnostics.
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);
}
