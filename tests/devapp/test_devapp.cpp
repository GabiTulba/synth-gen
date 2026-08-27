#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "build.hpp"
#include "manifest_helpers.hpp"
#include "json.hpp"
#include "metadata.hpp"
#include "player.hpp"
#include "test_framework.hpp"
#include "projectstate.hpp"
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
let _ = render "beep" 8000.0 (sample ((sine 440.0) *. 0.5) 0s 250ms) ;;
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

TEST(player_looping_reload_picks_up_rewritten_artifact) {
  // A rebuild rewrites the artifact while it loop-plays: reloadIfLooping
  // must re-read the file (here visibly shorter) rather than keep
  // replaying its stale in-memory copy. The dummy audio driver stands in
  // for a real device.
  ::setenv("SDL_AUDIODRIVER", "dummy", 1);
  TempDir tp;
  std::string path = (tp.dir / "a.wav").string();
  writeWav(path, 8000.0, 1, std::vector<double>(800, 0.5));
  AudioPlayer p;
  std::string err;
  CHECK(p.playRange(path, 0, -1, err, true));
  CHECK(p.playing());
  CHECK_NEAR(p.rangeEndSeconds(), 0.1, 1e-9);

  writeWav(path, 8000.0, 1, std::vector<double>(400, -0.5));
  p.reloadIfLooping();
  CHECK(p.playing());
  CHECK(p.looping());
  CHECK_NEAR(p.rangeEndSeconds(), 0.05, 1e-9);

  // A rate/channel change can't reuse the open device; playback restarts
  // on a fresh one, still looping the same (clamped) range.
  writeWav(path, 16000.0, 1, std::vector<double>(400, 0.25));
  p.reloadIfLooping();
  CHECK(p.playing());
  CHECK(p.looping());
  CHECK_NEAR(p.rangeEndSeconds(), 0.025, 1e-9);
  p.stop();
}

TEST(metadata_parses_controls) {
  TempDir tp;
  tp.write("metadata.json", R"({
  "project": "p", "status": "ok", "diagnostics": [], "targets": [],
  "controls": [
    {"name": "cutoff", "kind": "slider", "min": 100, "max": 2000,
     "default": 700, "value": 900},
    {"name": "gain", "kind": "knob", "min": 0, "max": 1, "default": 0.25,
     "value": 0.25},
    {"kind": "slider", "min": 0, "max": 1, "default": 0, "value": 0}
  ]
})");
  MetadataLoadResult m =
      loadProjectMetadata((tp.dir / "metadata.json").string());
  CHECK(m.ok);
  CHECK(m.meta.controls.size() == 2);  // the nameless one is dropped
  CHECK(m.meta.controls[0].name == "cutoff");
  CHECK(m.meta.controls[0].kind == "slider");
  CHECK_NEAR(m.meta.controls[0].min, 100.0, 1e-9);
  CHECK_NEAR(m.meta.controls[0].max, 2000.0, 1e-9);
  CHECK_NEAR(m.meta.controls[0].def, 700.0, 1e-9);
  CHECK_NEAR(m.meta.controls[0].value, 900.0, 1e-9);
  CHECK(m.meta.controls[1].kind == "knob");
  CHECK(m.meta.controls[0].group.empty());  // ungrouped: no group fields
  CHECK(m.meta.controls[0].groupIndex == -1);
}

TEST(metadata_parses_multi_slider_group_lanes) {
  TempDir tp;
  tp.write("metadata.json", R"({
  "project": "p", "status": "ok", "diagnostics": [], "targets": [],
  "controls": [
    {"name": "env.attack", "kind": "multi_slider", "min": 0, "max": 0.5,
     "default": 0.05, "value": 0.1, "group": "env", "group_index": 0,
     "sum_min": 0, "sum_max": 1},
    {"name": "env.decay", "kind": "multi_slider", "min": 0, "max": 0.5,
     "default": 0.15, "value": 0.2, "group": "env", "group_index": 1,
     "sum_min": 0, "sum_max": 1}
  ]
})");
  MetadataLoadResult m =
      loadProjectMetadata((tp.dir / "metadata.json").string());
  CHECK(m.ok);
  CHECK(m.meta.controls.size() == 2);
  CHECK(m.meta.controls[0].kind == "multi_slider");
  CHECK(m.meta.controls[0].group == "env");
  CHECK(m.meta.controls[0].groupIndex == 0);
  CHECK_NEAR(m.meta.controls[0].sumMax, 1.0, 1e-9);
  CHECK(m.meta.controls[1].groupIndex == 1);
  // Lanes of one group arrive consecutively - the app relies on that to
  // draw them as one linked block.
  CHECK(m.meta.controls[1].group == m.meta.controls[0].group);
}

TEST(control_lane_band_is_what_the_other_lanes_leave) {
  ControlMeta lane;
  lane.name = "env.attack";
  lane.kind = "multi_slider";
  lane.group = "env";
  lane.min = 0.0;
  lane.max = 0.5;
  lane.sumMin = 0.0;
  lane.sumMax = 1.0;

  // Plenty of budget left: the lane's own range is the whole band.
  ControlBand free_ = controlLaneBand(lane, 0.2);
  CHECK_NEAR(free_.lo, 0.0, 1e-12);
  CHECK_NEAR(free_.hi, 0.5, 1e-12);

  // The others have spent 0.7, so only 0.3 of the lane's 0.5 is reachable.
  ControlBand tight = controlLaneBand(lane, 0.7);
  CHECK_NEAR(tight.lo, 0.0, 1e-12);
  CHECK_NEAR(tight.hi, 0.3, 1e-12);

  // Budget exhausted: the lane is pinned, and the band is degenerate
  // rather than inverted.
  ControlBand pinned = controlLaneBand(lane, 1.2);
  CHECK_NEAR(pinned.lo, 0.0, 1e-12);
  CHECK_NEAR(pinned.hi, 0.0, 1e-12);
  CHECK(pinned.hi >= pinned.lo);

  // A sum_min floor pushes the *lower* limit up: with the others at 0.2
  // and a floor of 0.6, this lane may not go below 0.4.
  lane.sumMin = 0.6;
  ControlBand floored = controlLaneBand(lane, 0.2);
  CHECK_NEAR(floored.lo, 0.4, 1e-12);
  CHECK_NEAR(floored.hi, 0.5, 1e-12);

  // Anywhere inside the band keeps the group's sum inside its bounds -
  // which is the whole point of clamping the drag to it.
  double rest = 0.2;
  for (double v : {floored.lo, 0.45, floored.hi}) {
    CHECK(v + rest >= lane.sumMin - 1e-12);
    CHECK(v + rest <= lane.sumMax + 1e-12);
  }
}

TEST(control_overrides_roundtrip_through_a_build) {
  // The full attach loop, minus the daemon: build with defaults, write
  // overrides the way the UI does, rebuild, and see the new value both in
  // the metadata and in the artifact.
  TempDir tp;
  tp.write("song.synth", R"(
open Core open Core.Arrange open Core.Render open Core.Sig
let gain : Scalar = Control.slider ~name:"gain" ~min:0.0 ~max:1.0 ~default:0.25 ;;
let _ = render "beep" 8000.0 (sample (constant gain) 0s 100ms) ;;
)");
  tp.write("build.json", projectManifest("ctl-demo", {"song.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(r.ok);

  MetadataLoadResult m = loadProjectMetadata(r.metadataPath);
  CHECK(m.ok);
  CHECK(m.meta.controls.size() == 1);
  CHECK_NEAR(m.meta.controls[0].value, 0.25, 1e-9);

  std::string overridesPath = controlsPathFor(r.metadataPath);
  CHECK(overridesPath == r.controlsPath);
  std::string err;
  CHECK(writeControlOverrides(overridesPath, {{"gain", 0.75}}, err));

  BuildResult r2 = buildProject(tp.dir.string());
  CHECK(r2.ok);
  MetadataLoadResult m2 = loadProjectMetadata(r2.metadataPath);
  CHECK_NEAR(m2.meta.controls[0].value, 0.75, 1e-9);
  CHECK_NEAR(m2.meta.controls[0].def, 0.25, 1e-9);
  WavData w = readWav((tp.dir / m2.meta.targets[0].artifact).string());
  CHECK_NEAR(w.channels[0][100], 0.75, 0.01);

  // An empty overrides map is still a real write: back to defaults.
  CHECK(writeControlOverrides(overridesPath, {}, err));
  BuildResult r3 = buildProject(tp.dir.string());
  MetadataLoadResult m3 = loadProjectMetadata(r3.metadataPath);
  CHECK_NEAR(m3.meta.controls[0].value, 0.25, 1e-9);
}

TEST(control_overrides_write_is_atomic) {
  // The write goes through a temp file + rename; no .tmp litter remains
  // and the result parses.
  TempDir tp;
  std::string path = (tp.dir / "controls.json").string();
  std::string err;
  CHECK(writeControlOverrides(path, {{"a", 1.5}, {"b", -2.0}}, err));
  CHECK(!fs::exists(path + ".tmp"));
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  json::Value v;
  std::string jsonErr;
  CHECK(json::parse(ss.str(), v, jsonErr));
  const json::Value* ov = v.get("overrides");
  CHECK(ov != nullptr);
  CHECK_NEAR(ov->getNumber("a"), 1.5, 1e-12);
  CHECK_NEAR(ov->getNumber("b"), -2.0, 1e-12);
}

TEST(control_overrides_read_back) {
  // What the UI wrote is what a restarted UI reads, so a value edited
  // with no daemon attached is not lost on the next run.
  TempDir tp;
  std::string path = (tp.dir / "controls.json").string();
  std::string err;
  CHECK(writeControlOverrides(path, {{"gain", 0.75}, {"cutoff", 880.0}}, err));
  std::map<std::string, double> back = readControlOverrides(path);
  CHECK(back.size() == 2);
  CHECK_NEAR(back["gain"], 0.75, 1e-12);
  CHECK_NEAR(back["cutoff"], 880.0, 1e-12);

  // Missing and malformed files are "no overrides", never an error.
  CHECK(readControlOverrides((tp.dir / "nope.json").string()).empty());
  tp.write("junk.json", "{not json");
  CHECK(readControlOverrides((tp.dir / "junk.json").string()).empty());
  tp.write("wrong.json", R"({"overrides": 3})");
  CHECK(readControlOverrides((tp.dir / "wrong.json").string()).empty());
}

TEST(layout_points_at_a_project_state_file) {
  // Beside the build.json the app was pointed at - never the same path
  // as a unit's build metadata, which always lives under _build/.
  TempDir tp;
  tp.write("build.json", projectManifest("solo", {"a.synth"}));
  MetadataLayout l = resolveMetadataLayout(tp.dir.string());
  CHECK(l.projectStatePath == (tp.dir / "project.json").string());
  CHECK(l.units[0].metadataPath ==
        (tp.dir / "_build" / "metadata.json").string());

  TempDir tr;
  fs::create_directories(tr.dir / "sub");
  tr.write("sub/build.json", projectManifest("sub", {"s.synth"}));
  tr.write("build.json", rootManifest("multi", {"sub"}));
  // Pointed at the root: the root's own project.json.
  MetadataLayout root = resolveMetadataLayout(tr.dir.string());
  CHECK(root.projectStatePath == (tr.dir / "project.json").string());
  // Pointed at the subproject: the subproject's, not the root's.
  MetadataLayout inner = resolveMetadataLayout((tr.dir / "sub").string());
  CHECK(inner.projectStatePath == (tr.dir / "sub" / "project.json").string());
}

TEST(metadata_loads_panels_from_a_real_build) {
  // Same end-to-end contract as the targets above: what buildProject
  // writes for a panel, the dev app reads back.
  TempDir tp;
  tp.write("a.synth", R"(
open Core open Core.Control open Core.Arrange open Core.Render open Core.Sig
let gain : Scalar = Control.knob ~name:"gain" ~min:0.0 ~max:1.0 ~default:0.5 ;;
let env : Scalar list =
  Control.multi_slider ~name:"env" ~sum_min:0.0 ~sum_max:1.0
    ~lanes:[ { name = "attack"; min = 0.0; max = 0.5; default = 0.05 };
             { name = "decay";  min = 0.0; max = 0.5; default = 0.15 } ] ;;
let _ = render "demo" 8000.0 (sample (constant gain) 0s 50ms) ;;
let _ = Ui.panel ~name:"Voice" ~controls:["gain"; "env"] ~targets:["demo"] ;;
)");
  tp.write("build.json", projectManifest("panels-demo", {"a.synth"}));
  BuildResult r = buildProject(tp.dir.string());
  CHECK(r.ok);

  MetadataLoadResult m = loadProjectMetadata(r.metadataPath);
  CHECK(m.ok);
  CHECK(m.meta.panels.size() == 1);
  CHECK(m.meta.panels[0].name == "Voice");
  // "env" stays the group name; expanding it to lanes is the app's job
  // at draw time, so the group can be drawn as one linked widget.
  CHECK(m.meta.panels[0].controls.size() == 2);
  CHECK(m.meta.panels[0].controls[0] == "gain");
  CHECK(m.meta.panels[0].controls[1] == "env");
  CHECK(m.meta.panels[0].targets.size() == 1);
  CHECK(m.meta.panels[0].targets[0] == "demo");
}

TEST(metadata_without_panels_reports_none) {
  TempDir tp;
  tp.write("m.json", R"({"project": "p", "status": "ok", "targets": [],
                         "controls": []})");
  MetadataLoadResult m = loadProjectMetadata((tp.dir / "m.json").string());
  CHECK(m.ok);
  CHECK(m.meta.panels.empty());
}

TEST(metadata_malformed_panels_are_dropped_not_fatal) {
  // A panel is presentation: a broken one must cost you that panel, and
  // never the app's view of the build.
  TempDir tp;
  tp.write("m.json", R"({"project": "p", "status": "ok",
    "targets": [{"name": "t", "kind": "audio", "status": "ok"}],
    "controls": [],
    "panels": [
      {"name": "", "controls": ["a"], "targets": []},
      {"controls": ["a"]},
      {"name": "Bad", "controls": "not-an-array", "targets": 7},
      {"name": "Good", "controls": ["a", "", 5], "targets": ["t"]}
    ]})");
  MetadataLoadResult m = loadProjectMetadata((tp.dir / "m.json").string());
  CHECK(m.ok);
  CHECK(m.meta.targets.size() == 1);  // the rest of the file still loads
  // The nameless two are skipped; "Bad" survives with empty member lists
  // rather than taking the file down with it.
  CHECK(m.meta.panels.size() == 2);
  CHECK(m.meta.panels[0].name == "Bad");
  CHECK(m.meta.panels[0].controls.empty());
  CHECK(m.meta.panels[0].targets.empty());
  CHECK(m.meta.panels[1].name == "Good");
  // Non-string and empty members are dropped, the good one kept.
  CHECK(m.meta.panels[1].controls.size() == 1);
  CHECK(m.meta.panels[1].controls[0] == "a");
}

TEST(resolve_panels_covers_everything_a_project_declares) {
  ProjectMeta meta;
  meta.project = "demo";
  TargetMeta one, two;
  one.name = "one";
  two.name = "two";
  meta.targets.push_back(one);
  meta.targets.push_back(two);
  ControlMeta gain;
  gain.name = "gain";
  meta.controls.push_back(gain);
  ControlMeta lane0, lane1;
  lane0.name = "env.attack";
  lane0.group = "env";
  lane1.name = "env.decay";
  lane1.group = "env";
  meta.controls.push_back(lane0);
  meta.controls.push_back(lane1);

  // Nothing declared: one panel named for the project holds it all, and
  // the multi_slider group appears once, under its group name.
  {
    std::vector<PanelMeta> p = resolvePanels(meta);
    CHECK(p.size() == 1);
    CHECK(p[0].name == "demo");
    CHECK(p[0].controls.size() == 2);
    CHECK(p[0].controls[0] == "gain");
    CHECK(p[0].controls[1] == "env");
    CHECK(p[0].targets.size() == 2);
  }

  // A declared panel that covers everything leaves no remainder.
  {
    ProjectMeta full = meta;
    full.panels.push_back(
        PanelMeta{"All", {"gain", "env"}, {"one", "two"}});
    std::vector<PanelMeta> p = resolvePanels(full);
    CHECK(p.size() == 1);
    CHECK(p[0].name == "All");
  }

  // A partial one leaves the rest in an "ungrouped" panel.
  {
    ProjectMeta part = meta;
    part.panels.push_back(PanelMeta{"Some", {"gain"}, {"one"}});
    std::vector<PanelMeta> p = resolvePanels(part);
    CHECK(p.size() == 2);
    CHECK(p[0].name == "Some");
    CHECK(p[1].name == "ungrouped");
    CHECK(p[1].controls.size() == 1);
    CHECK(p[1].controls[0] == "env");
    CHECK(p[1].targets.size() == 1);
    CHECK(p[1].targets[0] == "two");
  }

  // Naming a lane individually still claims it, so the group does not
  // come back whole in the remainder.
  {
    ProjectMeta lane = meta;
    lane.panels.push_back(PanelMeta{"Lane", {"env.attack"}, {}});
    std::vector<PanelMeta> p = resolvePanels(lane);
    CHECK(p.size() == 2);
    CHECK(p[1].name == "ungrouped");
    // "gain" and the group (via its still-unclaimed decay lane).
    CHECK(p[1].controls.size() == 2);
    CHECK(p[1].controls[0] == "gain");
    CHECK(p[1].controls[1] == "env");
  }

  // An empty project produces no panels rather than an empty one.
  {
    ProjectMeta empty;
    CHECK(resolvePanels(empty).empty());
  }
}

TEST(project_state_roundtrips_panel_visibility) {
  TempDir tp;
  std::string path = (tp.dir / "project.json").string();
  ProjectState s;
  s.ui.panels["./Voice"] = false;
  s.ui.panels["./Drums"] = true;
  std::string err;
  CHECK(saveProjectState(path, s, err));
  ProjectStateLoad back = loadProjectState(path);
  CHECK(back.found);
  CHECK(back.state == s);
  CHECK(back.state.ui.panels.at("./Voice") == false);
  CHECK(back.state.ui.panels.at("./Drums") == true);
}

TEST(project_state_roundtrips) {
  TempDir tp;
  std::string path = (tp.dir / "project.json").string();

  ProjectState s;
  s.controls["."] = {{"cutoff", 2500.0}, {"gain", 0.42}};
  s.controls["sub"] = {{"env.attack", 0.125}};
  s.ui.window = WindowGeometry{true, 120, 64, 1280, 800};
  // ImGui's dump is multi-line text with quotes and brackets in it - it
  // has to survive JSON escaping byte for byte.
  s.ui.imguiIni = "[Window][synthgraph]\nPos=0,19\nSize=900,581\nCollapsed=0\n";
  WavePanelState p;
  p.artifact = "_build/pluck/artifacts/demo.wav";
  p.target = "demo";
  p.viewStart = 1024.5;
  p.viewEnd = 48000.25;
  p.selStart = 2000;
  p.selEnd = 3000;
  p.loop = true;
  s.ui.waves.push_back(p);
  s.ui.sections["./diags"] = false;
  s.ui.sections["./controls"] = true;

  std::string err;
  CHECK(saveProjectState(path, s, err));
  CHECK(err.empty());
  CHECK(!fs::exists(path + ".tmp"));  // temp + rename, no litter

  ProjectStateLoad back = loadProjectState(path);
  CHECK(back.found);
  CHECK(back.state == s);
}

TEST(project_state_is_written_for_people_to_read) {
  // It sits in the source tree next to build.json, so it is indented and
  // its numbers are the shortest form that reads back identically -
  // never the 0.41999999999999998 that %.17g would produce.
  TempDir tp;
  std::string path = (tp.dir / "project.json").string();
  ProjectState s;
  s.controls["."] = {{"gain", 0.42}, {"cutoff", 900.0}};
  std::string err;
  CHECK(saveProjectState(path, s, err));

  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  CHECK(text.find("\n  \"controls\": {") != std::string::npos);
  CHECK(text.find("\"gain\": 0.42") != std::string::npos);
  CHECK(text.find("\"cutoff\": 900") != std::string::npos);
  CHECK(text.find("0.4199") == std::string::npos);
  // Still valid JSON, and still exactly what we put in.
  json::Value v;
  std::string jsonErr;
  CHECK(json::parse(text, v, jsonErr));
  CHECK(loadProjectState(path).state == s);
}

TEST(project_state_empty_control_sets_are_omitted) {
  // "back to defaults" is the absence of an entry, so a project that has
  // been reset does not carry dead units around forever.
  TempDir tp;
  std::string path = (tp.dir / "project.json").string();
  ProjectState s;
  s.controls["."] = {};
  s.controls["sub"] = {{"gain", 0.5}};
  std::string err;
  CHECK(saveProjectState(path, s, err));
  ProjectState back = loadProjectState(path).state;
  CHECK(back.controls.size() == 1);
  CHECK(back.controls.count(".") == 0);
  CHECK_NEAR(back.controls.at("sub").at("gain"), 0.5, 1e-12);
}

TEST(project_state_missing_or_corrupt_is_not_fatal) {
  // The app must start even when the file is garbage - and `found` has to
  // stay false, because the app treats that as "this project has no
  // settings yet" and keeps whatever controls.json already holds rather
  // than overwriting it from an empty state.
  TempDir tp;
  ProjectStateLoad none = loadProjectState((tp.dir / "project.json").string());
  CHECK(!none.found);
  CHECK(none.state == ProjectState{});

  tp.write("bad.json", "{\"ui\": {\"waves\": [1, 2");
  CHECK(!loadProjectState((tp.dir / "bad.json").string()).found);
  tp.write("array.json", "[1, 2, 3]");
  CHECK(!loadProjectState((tp.dir / "array.json").string()).found);

  // Well-formed JSON with the wrong shapes: take what parses, drop the
  // rest, and never crash. A zero-size window is not a usable placement.
  tp.write("odd.json", R"({"controls": {"good": {"a": 1}, "bad": 7,
                                        "mixed": {"n": 2, "s": "no"}},
                           "ui": {"window": {"x": 1, "y": 2, "w": 0, "h": 0},
                                  "imgui": 7,
                                  "waves": [3, {"target": "no artifact"},
                                            {"artifact": "a.wav"}],
                                  "sections": {"a": true, "b": "not a bool"}}})");
  ProjectStateLoad odd = loadProjectState((tp.dir / "odd.json").string());
  CHECK(odd.found);
  CHECK(odd.state.controls.size() == 2);
  CHECK_NEAR(odd.state.controls.at("good").at("a"), 1.0, 1e-12);
  CHECK(odd.state.controls.at("mixed").size() == 1);
  CHECK(!odd.state.ui.window.valid);
  CHECK(odd.state.ui.imguiIni.empty());
  CHECK(odd.state.ui.waves.size() == 1);
  CHECK(odd.state.ui.waves[0].artifact == "a.wav");
  CHECK(odd.state.ui.waves[0].selStart == -1);
  CHECK(odd.state.ui.sections.size() == 1);
  CHECK(odd.state.ui.sections.at("a") == true);
}
