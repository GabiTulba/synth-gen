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
