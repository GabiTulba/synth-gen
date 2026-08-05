#include "build.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include <atomic>

#include "checker.hpp"
#include "eval.hpp"
#include "incremental.hpp"
#include "vis.hpp"
#include "wav.hpp"

namespace fs = std::filesystem;

namespace synth {

namespace {

// Bump when engine semantics change so stale cached artifacts are not
// mistaken for up-to-date ones.
constexpr uint64_t kEngineVersion = 1;

// Snapshot of a file's identity for change detection. Missing files get a
// distinct marker so appearing/disappearing counts as a change.
struct Stamp {
  bool exists = false;
  int64_t size = 0;
  int64_t mtime = 0;
  bool operator==(const Stamp&) const = default;
};

Stamp stampOf(const fs::path& p) {
  Stamp s;
  std::error_code ec;
  auto status = fs::status(p, ec);
  if (ec || !fs::exists(status)) return s;
  s.exists = true;
  if (fs::is_regular_file(status)) s.size = (int64_t)fs::file_size(p, ec);
  auto t = fs::last_write_time(p, ec);
  if (!ec) s.mtime = t.time_since_epoch().count();
  return s;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}

std::string jsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string formatDouble(double v) {
  std::ostringstream ss;
  ss << v;
  return ss.str();
}

// Build metadata (§8.2 step 5): the machine-readable index the dev app
// consumes. Written by hand to keep the core dependency-free.
void writeMetadata(const std::string& path, const BuildResult& r,
                   const std::map<std::string, std::string>& sourcesByPath) {
  std::ostringstream j;
  j << "{\n";
  j << "  \"project\": \"" << jsonEscape(r.manifest.projectName) << "\",\n";
  j << "  \"status\": \"" << (r.ok ? "ok" : "error") << "\",\n";
  j << "  \"diagnostics\": [\n";
  for (size_t i = 0; i < r.diags.items.size(); i++) {
    const Diagnostic& d = r.diags.items[i];
    std::string rendered;
    auto it = sourcesByPath.find(d.file);
    rendered = renderDiagnostic(
        d, it != sourcesByPath.end() ? it->second : std::string{});
    if (!rendered.empty() && rendered.back() == '\n') rendered.pop_back();
    j << "    {\"severity\": \""
      << (d.severity == Severity::Error ? "error" : "warning")
      << "\", \"file\": \"" << jsonEscape(d.file) << "\", \"message\": \""
      << jsonEscape(d.message) << "\", \"rendered\": \""
      << jsonEscape(rendered) << "\"}"
      << (i + 1 < r.diags.items.size() ? "," : "") << "\n";
  }
  j << "  ],\n";
  j << "  \"targets\": [\n";
  for (size_t i = 0; i < r.targets.size(); i++) {
    const TargetInfo& t = r.targets[i];
    j << "    {\"name\": \"" << jsonEscape(t.name) << "\", \"kind\": \""
      << jsonEscape(t.kind) << "\", \"status\": \""
      << (t.ok ? "ok" : "error") << "\", \"cached\": "
      << (t.cached ? "true" : "false") << ", \"artifact\": \""
      << jsonEscape(t.artifact) << "\", \"rate\": " << formatDouble(t.rate)
      << ", \"channels\": " << t.channelCount << ", \"frames\": " << t.frames
      << ", \"duration_seconds\": " << formatDouble(t.durationSeconds)
      << ", \"error\": \"" << jsonEscape(t.error) << "\"}"
      << (i + 1 < r.targets.size() ? "," : "") << "\n";
  }
  j << "  ]\n";
  j << "}\n";
  std::ofstream out(path, std::ios::trunc);
  out << j.str();
}

}  // namespace

bool parseManifest(const std::string& text, const std::string& file,
                   Manifest& out, DiagnosticBag& diags) {
  std::istringstream in(text);
  std::string line;
  uint32_t offset = 0;
  bool ok = true;
  while (std::getline(in, line)) {
    uint32_t lineLo = offset;
    offset += (uint32_t)line.size() + 1;
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    Span span{lineLo, lineLo + (uint32_t)line.size()};
    size_t sp = t.find_first_of(" \t");
    std::string key = sp == std::string::npos ? t : t.substr(0, sp);
    std::string rest = sp == std::string::npos ? "" : trim(t.substr(sp));
    if (key == "project") {
      if (rest.empty()) {
        diags.error(file, span, "manifest: 'project' needs a name");
        ok = false;
      } else if (!out.projectName.empty()) {
        diags.error(file, span, "manifest: duplicate 'project' line");
        ok = false;
      } else {
        out.projectName = rest;
      }
    } else if (key == "source") {
      if (rest.empty()) {
        diags.error(file, span, "manifest: 'source' needs a file path");
        ok = false;
      } else {
        out.sources.push_back(rest);
      }
    } else {
      diags.error(file, span, "manifest: unknown directive '" + key + "'");
      ok = false;
    }
  }
  if (out.projectName.empty()) {
    diags.projectError("manifest: missing 'project <name>'");
    ok = false;
  }
  if (out.sources.empty()) {
    diags.projectError("manifest: no 'source' entries");
    ok = false;
  }
  return ok;
}

BuildResult buildProject(const std::string& projectDir, BuildCache* cache) {
  BuildResult r;
  fs::path dir(projectDir);
  fs::path manifestPath = dir / ".build";

  std::map<std::string, std::string> sourcesByPath;  // for diag rendering

  // 1. Manifest.
  std::ifstream mf(manifestPath);
  if (!mf) {
    r.diags.projectError("no .build manifest found in '" + dir.string() + "'");
    return r;
  }
  std::ostringstream mss;
  mss << mf.rdbuf();
  std::string manifestText = mss.str();
  sourcesByPath[manifestPath.string()] = manifestText;
  r.inputs.push_back(manifestPath.string());
  if (!parseManifest(manifestText, manifestPath.string(), r.manifest,
                     r.diags))
    return r;

  // 2. Parse & type-check every source file (plus imports).
  std::vector<std::string> roots;
  for (auto& s : r.manifest.sources) roots.push_back((dir / s).string());
  for (auto& s : roots) r.inputs.push_back(s);
  Program prog = checkProject(roots, r.diags);
  for (auto& m : prog.modules) {
    sourcesByPath[m.parsed.path] = m.parsed.source;
    if (std::find(roots.begin(), roots.end(), m.parsed.path) == roots.end())
      r.inputs.push_back(m.parsed.path);
  }

  fs::path buildDir = dir / "build";
  fs::path artifactDir = buildDir / "artifacts";
  std::error_code ec;
  fs::create_directories(artifactDir, ec);
  r.metadataPath = (buildDir / "metadata.json").string();

  if (r.diags.hasErrors()) {
    writeMetadata(r.metadataPath, r, sourcesByPath);
    return r;
  }

  // 3. Evaluate: enumerate render targets (and run load_* validation).
  std::vector<RenderTarget> targets;
  std::vector<std::string> audioInputs;
  bool evalOk = evaluateProgram(prog, targets, r.diags, &audioInputs);
  for (auto& a : audioInputs) r.inputs.push_back(a);

  // Project-level rule: duplicate render names are a build error (§5.2).
  std::map<std::string, const RenderTarget*> byName;
  for (auto& t : targets) {
    auto [it, inserted] = byName.emplace(t.name, &t);
    if (!inserted) {
      r.diags.error(t.file, t.span,
                    "duplicate render target name '" + t.name +
                        "' (also declared in " + it->second->file + ")");
      evalOk = false;
    }
  }
  if (!evalOk) {
    writeMetadata(r.metadataPath, r, sourcesByPath);
    return r;
  }

  // Content keys for incremental rebuilds (Epic 8): the target's
  // dependency-closure hash, salted with the engine version and the stamps
  // of every audio input (audio files are build inputs; a changed file
  // invalidates conservatively).
  uint64_t audioSalt = fnvCombine(kFnvOffset, kEngineVersion);
  {
    std::vector<std::string> sorted = audioInputs;
    std::sort(sorted.begin(), sorted.end());
    for (auto& a : sorted) {
      Stamp s = stampOf(a);
      audioSalt = fnv1a(a.data(), a.size(), audioSalt);
      audioSalt = fnvCombine(audioSalt, (uint64_t)s.size);
      audioSalt = fnvCombine(audioSalt, (uint64_t)s.mtime);
    }
  }
  std::vector<uint64_t> keys(targets.size(), 0);
  for (size_t i = 0; i < targets.size(); i++) {
    const RenderTarget& t = targets[i];
    uint64_t k = t.declModule && t.declDef
                     ? defClosureHash(prog, *t.declModule, *t.declDef)
                     : 0;
    keys[i] = fnvCombine(k, audioSalt);
  }

  // 4. Discretize targets and write artifacts. Cache-fresh targets are
  // reused without re-rendering; the rest render in parallel across a
  // thread pool (Epic 9) - safe because signal graphs are immutable and
  // all per-render state lives in each render's own context. A failing
  // target is recorded in metadata but does not stop the others (§6.3).
  r.targets.resize(targets.size());
  std::vector<size_t> pending;
  auto extensionFor = [](const RenderTarget& t) {
    return t.kind == RenderTarget::Kind::Visual ? ".svg" : ".wav";
  };
  for (size_t i = 0; i < targets.size(); i++) {
    const RenderTarget& t = targets[i];
    fs::path artifactPath = artifactDir / (t.name + extensionFor(t));
    const BuildCache::Entry* hit = nullptr;
    if (cache) {
      auto it = cache->targets.find(t.name);
      if (it != cache->targets.end() && it->second.key == keys[i] &&
          it->second.info.ok && fs::exists(artifactPath))
        hit = &it->second;
    }
    if (hit) {
      r.targets[i] = hit->info;
      r.targets[i].cached = true;
    } else {
      pending.push_back(i);
    }
  }

  std::vector<std::string> renderErrors(targets.size());
  {
    std::atomic<size_t> next{0};
    auto worker = [&] {
      for (;;) {
        size_t slot = next.fetch_add(1);
        if (slot >= pending.size()) return;
        size_t i = pending[slot];
        const RenderTarget& t = targets[i];
        TargetInfo info;
        info.name = t.name;
        info.kind = t.kind == RenderTarget::Kind::Visual ? "visual" : "audio";
        info.rate = t.rate;
        try {
          Rendered rendered =
              renderWindow(t.sample.sig, t.sample.from, t.sample.to, t.rate);
          std::string fileName = t.name + extensionFor(t);
          fs::path artifactPath = artifactDir / fileName;
          if (t.kind == RenderTarget::Kind::Visual) {
            std::ofstream out(artifactPath, std::ios::trunc);
            if (!out) throw std::runtime_error("cannot write artifact file");
            out << renderWaveformSvg(t.name, rendered, t.rate);
          } else {
            writeWav(artifactPath.string(), t.rate, rendered.channels,
                     rendered.interleaved);
          }
          info.artifact =
              (fs::path("build") / "artifacts" / fileName).generic_string();
          info.channelCount = rendered.channels;
          info.frames = rendered.frames;
          info.durationSeconds =
              t.rate > 0 ? (double)rendered.frames / t.rate : 0;
          info.ok = true;
        } catch (const std::exception& e) {
          info.error = e.what();
          renderErrors[i] = e.what();
        }
        r.targets[i] = std::move(info);
      }
    };
    size_t threadCount = std::min<size_t>(
        pending.size(), std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (size_t i = 1; i < threadCount; i++) pool.emplace_back(worker);
    if (!pending.empty()) worker();
    for (auto& th : pool) th.join();
  }

  bool allOk = true;
  for (size_t i = 0; i < targets.size(); i++) {
    if (!renderErrors[i].empty()) {
      r.diags.error(targets[i].file, targets[i].span,
                    "render target '" + targets[i].name +
                        "': " + renderErrors[i]);
      allOk = false;
    }
  }

  if (cache) {
    cache->targets.clear();  // prune removed targets; one entry per current
    for (size_t i = 0; i < targets.size(); i++) {
      if (!r.targets[i].ok) continue;
      TargetInfo stored = r.targets[i];
      stored.cached = false;
      cache->targets[targets[i].name] = {keys[i], std::move(stored)};
    }
  }

  r.ok = allOk;
  // 5. Emit build metadata.
  writeMetadata(r.metadataPath, r, sourcesByPath);
  return r;
}

DiagnosticBag lintFiles(const std::vector<std::string>& files) {
  DiagnosticBag diags;
  checkProject(files, diags);
  return diags;
}

namespace {

std::map<std::string, Stamp> snapshot(const std::string& projectDir,
                                      const std::vector<std::string>& inputs) {
  std::map<std::string, Stamp> snap;
  // Watching the directory itself catches newly added source files that a
  // manifest already references (or fixed unresolved imports).
  snap[projectDir] = stampOf(projectDir);
  for (auto& f : inputs) snap[f] = stampOf(f);
  return snap;
}

}  // namespace

void watchProject(const std::string& projectDir,
                  const std::function<void(const BuildResult&)>& onBuild,
                  const std::function<bool()>& keepRunning, int pollMillis) {
  // The daemon owns the incremental cache: rebuilds triggered by an edit
  // only re-render targets whose dependency closure actually changed.
  BuildCache cache;
  BuildResult r = buildProject(projectDir, &cache);
  onBuild(r);
  auto snap = snapshot(projectDir, r.inputs);
  while (keepRunning()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(pollMillis));
    auto now = snapshot(projectDir, r.inputs);
    if (now == snap) continue;
    r = buildProject(projectDir, &cache);
    onBuild(r);
    snap = snapshot(projectDir, r.inputs);
  }
}

}  // namespace synth
