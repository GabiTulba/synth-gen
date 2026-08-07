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
open Core
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
