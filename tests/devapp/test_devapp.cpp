#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "build.hpp"
#include "manifest_helpers.hpp"
#include "json.hpp"
#include "metadata.hpp"
#include "player.hpp"
#include "test_framework.hpp"
#include "wav.hpp"
#include "waveform.hpp"

using namespace synth;
using namespace synth::devapp;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path dir;
  TempDir() {
    static int counter = 0;
    dir = fs::temp_directory_path() /
          ("synthgraph-devapp-test-" + std::to_string(::getpid()) + "-" +
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

}  // namespace

TEST(json_parses_scalars_and_structures) {
  json::Value v;
  std::string err;
  CHECK(json::parse(R"({"a": 1.5, "b": "x", "c": [true, false, null],
                        "d": {"nested": -2e3}})",
                    v, err));
  CHECK(v.kind == json::Value::Kind::Object);
  CHECK_NEAR(v.getNumber("a"), 1.5, 1e-12);
  CHECK(v.getString("b") == "x");
  const json::Value* c = v.get("c");
  CHECK(c && c->array.size() == 3);
  CHECK(c->array[0].boolean == true);
  CHECK(c->array[2].kind == json::Value::Kind::Null);
  CHECK_NEAR(v.get("d")->getNumber("nested"), -2000.0, 1e-9);
}

TEST(json_parses_escapes) {
  json::Value v;
  std::string err;
  CHECK(json::parse(R"({"s": "a\"b\\c\ndAé"})", v, err));
  CHECK(v.getString("s") == "a\"b\\c\nd" "A" "\xc3\xa9");
}

TEST(json_rejects_garbage) {
  json::Value v;
  std::string err;
  CHECK(!json::parse("{", v, err));
  CHECK(!json::parse("{\"a\": }", v, err));
  CHECK(!json::parse("[1, 2,]", v, err));
  CHECK(!json::parse("{} trailing", v, err));
  CHECK(!err.empty());
}

TEST(metadata_loads_real_build_output) {
  // End-to-end contract check: what buildProject writes, the dev app reads.
  TempDir tp;
  tp.write("song.synth", R"(
open Core.Osc open Core.Arrange open Core.Render
let _ = render "beep" 8000.0 (sample ((sine 440.0) * 0.5) 0s 250ms) ;;
)");
  tp.write("build.json", projectManifest("devapp-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(r.ok);

  MetadataLoadResult m = loadProjectMetadata(r.metadataPath);
  CHECK(m.ok);
  CHECK(m.meta.project == "devapp-demo");
  CHECK(m.meta.status == "ok");
  CHECK(m.meta.targets.size() == 1);
  const TargetMeta& t = m.meta.targets[0];
  CHECK(t.name == "beep");
  CHECK(t.status == "ok");
  CHECK_NEAR(t.rate, 8000.0, 1e-9);
  CHECK_NEAR(t.durationSeconds, 0.25, 1e-9);
  CHECK(t.channels == 1);
  CHECK(t.frames == 2000);
  CHECK(fs::exists(tp.dir / t.artifact));
}

TEST(metadata_surfaces_failed_builds) {
  TempDir tp;
  tp.write("bad.synth", "open Core\nlet x : Scalar = sine 440.0 ;;");
  tp.write("build.json", projectManifest("broken", {"bad.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(!r.ok);

  MetadataLoadResult m = loadProjectMetadata(r.metadataPath);
  CHECK(m.ok);  // the file itself is valid
  CHECK(m.meta.status == "error");
  CHECK(!m.meta.diagnostics.empty());
  CHECK(m.meta.diagnostics[0].severity == "error");
  CHECK(!m.meta.diagnostics[0].rendered.empty());
}

TEST(layout_standalone_project_is_one_unit) {
  TempDir tp;
  tp.write("build.json", projectManifest("solo", {"a.synth"}));
  MetadataLayout l = resolveMetadataLayout(tp.dir.string());
  CHECK(l.units.size() == 1);
  CHECK(l.rootDir == tp.dir.string());
  CHECK(l.units[0].label.empty());
  CHECK(l.units[0].metadataPath ==
        (tp.dir / "_build" / "metadata.json").string());
}

TEST(layout_root_yields_one_unit_per_rule) {
  // A root manifest builds each rule into its own _build/<rule>/ dir; the
  // dev app must watch every rule's metadata, not <root>/_build/ itself
  // (which no build ever writes).
  TempDir tp;
  fs::create_directories(tp.dir / "sub");
  tp.write("sub/build.json", projectManifest("sub", {"s.synth"}));
  tp.write("sub/s.synth", R"(
open Core.Osc open Core.Arrange open Core.Render
let _ = render "sub_beep" 8000.0 (sample (sine 440.0) 0s 100ms) ;;
)");
  tp.write("tune.synth", R"(
open Core.Osc open Core.Arrange open Core.Render
let _ = render "tune_beep" 8000.0 (sample (sine 220.0) 0s 100ms) ;;
)");
  tp.write("build.json", rootManifest("multi", {"sub", "tune.synth"}));

  MetadataLayout l = resolveMetadataLayout(tp.dir.string());
  CHECK(l.rootDir == tp.dir.string());
  CHECK(l.manifestPath == (tp.dir / "build.json").string());
  CHECK(l.units.size() == 2);
  CHECK(l.units[0].label == "sub");
  CHECK(l.units[0].metadataPath ==
        (tp.dir / "_build" / "sub" / "metadata.json").string());
  // File rules drop their extension, mirroring buildRoot's layout.
  CHECK(l.units[1].label == "tune.synth");
  CHECK(l.units[1].metadataPath ==
        (tp.dir / "_build" / "tune" / "metadata.json").string());

  // End-to-end: buildRoot writes exactly where the layout points.
  RootBuildResult rr = buildRoot(tp.dir.string(), BuildOptions{});
  CHECK(rr.ok);
  for (auto& u : l.units) {
    MetadataLoadResult m = loadProjectMetadata(u.metadataPath);
    CHECK(m.ok);
    CHECK(m.meta.status == "ok");
    CHECK(m.meta.targets.size() == 1);
  }
}

TEST(layout_project_inside_root_is_one_unit) {
  TempDir tp;
  fs::create_directories(tp.dir / "sub");
  tp.write("sub/build.json", projectManifest("sub", {"s.synth"}));
  tp.write("build.json", rootManifest("multi", {"sub"}));
  MetadataLayout l = resolveMetadataLayout((tp.dir / "sub").string());
  CHECK(l.units.size() == 1);
  CHECK(l.rootDir == tp.dir.string());
  CHECK(l.units[0].metadataPath ==
        (tp.dir / "_build" / "sub" / "metadata.json").string());
}

TEST(metadata_missing_file_reports_error) {
  MetadataLoadResult m = loadProjectMetadata("/nonexistent/metadata.json");
  CHECK(!m.ok);
  CHECK(!m.error.empty());
}

TEST(stamp_detects_changes) {
  TempDir tp;
  FileStamp missing = stampFile((tp.dir / "f.json").string());
  CHECK(!missing.exists);
  tp.write("f.json", "{}");
  FileStamp first = stampFile((tp.dir / "f.json").string());
  CHECK(first.exists);
  CHECK(!(first == missing));
  tp.write("f.json", "{\"longer\": true}");
  FileStamp second = stampFile((tp.dir / "f.json").string());
  CHECK(!(second == first));  // size differs even if mtime is coarse
}

TEST(waveview_zoom_keeps_anchor_and_clamps) {
  WaveView v;
  v.reset(48000);
  // Zoom in about 25% of the view: the anchor frame stays put.
  double anchor = v.start + 0.25 * v.span();
  v.zoomAt(0.25, 0.5);
  CHECK_NEAR(v.span(), 24000.0, 1e-6);
  CHECK_NEAR(v.start + 0.25 * v.span(), anchor, 1e-6);
  // Zooming out beyond the file clamps to the whole file.
  v.zoomAt(0.5, 100.0);
  CHECK_NEAR(v.start, 0.0, 1e-6);
  CHECK_NEAR(v.end, 48000.0, 1e-6);
  // Zooming in never goes below the minimum span.
  for (int i = 0; i < 100; i++) v.zoomAt(0.5, 0.5);
  CHECK_NEAR(v.span(), WaveView::kMinSpan, 1e-6);
}

TEST(waveview_pan_clamps_to_file) {
  WaveView v;
  v.reset(1000);
  v.zoomAt(0.5, 0.1);  // span 100, centered
  v.pan(-1e9);
  CHECK_NEAR(v.start, 0.0, 1e-6);
  CHECK_NEAR(v.span(), 100.0, 1e-6);
  v.pan(1e9);
  CHECK_NEAR(v.end, 1000.0, 1e-6);
  CHECK_NEAR(v.span(), 100.0, 1e-6);
}

TEST(minmax_columns_raw_and_binned_agree) {
  // A ramp from -1 to 1: each column's envelope is its slice's endpoints,
  // and the binned path must agree with the raw scan.
  std::vector<double> ch(8192);
  for (size_t i = 0; i < ch.size(); i++)
    ch[i] = -1.0 + 2.0 * (double)i / (double)(ch.size() - 1);
  PeakBins bins = buildPeakBins(ch, 16);

  auto raw = minMaxColumns(ch, PeakBins{}, 0, (double)ch.size(), 8);
  CHECK(raw.size() == 8);
  CHECK_NEAR(raw[0].first, -1.0, 1e-6);
  CHECK_NEAR(raw[7].second, 1.0, 1e-6);
  for (int c = 0; c + 1 < 8; c++) CHECK(raw[c].second <= raw[c + 1].second);

  // 8 columns over 8192 frames = 1024 frames/column >= 2 bins of 16.
  auto binned = minMaxColumns(ch, bins, 0, (double)ch.size(), 8);
  for (int c = 0; c < 8; c++) {
    CHECK_NEAR(binned[c].first, raw[c].first, 1e-6);
    CHECK_NEAR(binned[c].second, raw[c].second, 1e-6);
  }
}

TEST(minmax_columns_zoomed_in_subsample) {
  // Fewer frames than columns: every column still gets a valid value
  // from its nearest sample.
  std::vector<double> ch = {0.5, -0.5};
  auto cols = minMaxColumns(ch, PeakBins{}, 0, 2, 8);
  CHECK(cols.size() == 8);
  CHECK_NEAR(cols[0].first, 0.5, 1e-6);
  CHECK_NEAR(cols[7].second, -0.5, 1e-6);
}

TEST(interleave_slices_frame_range) {
  WavData w;
  w.rate = 8000;
  w.channels = {{0.1, 0.2, 0.3, 0.4}, {-0.1, -0.2, -0.3, -0.4}};
  std::vector<float> out = interleaveToFloat(w, 1, 3);
  CHECK(out.size() == 4);
  CHECK_NEAR(out[0], 0.2, 1e-6);
  CHECK_NEAR(out[1], -0.2, 1e-6);
  CHECK_NEAR(out[2], 0.3, 1e-6);
  CHECK_NEAR(out[3], -0.3, 1e-6);
  // Out-of-range bounds clamp instead of crashing.
  CHECK(interleaveToFloat(w, -5, 100).size() == 8);
  CHECK(interleaveToFloat(w, 3, 1).empty());
}

TEST(player_interleaves_channels) {
  WavData w;
  w.rate = 8000;
  w.channels = {{0.1, 0.2, 0.3}, {-0.1, -0.2, -0.3}};
  std::vector<float> out = interleaveToFloat(w);
  CHECK(out.size() == 6);
  CHECK_NEAR(out[0], 0.1, 1e-6);
  CHECK_NEAR(out[1], -0.1, 1e-6);
  CHECK_NEAR(out[2], 0.2, 1e-6);
  CHECK_NEAR(out[3], -0.2, 1e-6);
  CHECK_NEAR(out[4], 0.3, 1e-6);
  CHECK_NEAR(out[5], -0.3, 1e-6);
}

TEST(player_reports_missing_file) {
  AudioPlayer p;
  std::string err;
  CHECK(!p.play("/nonexistent/x.wav", err));
  CHECK(!err.empty());
  CHECK(!p.playing());
}
