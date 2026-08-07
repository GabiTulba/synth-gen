#include "build.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <atomic>

#include <set>

#include "checker.hpp"
#include "eval.hpp"
#include "incremental.hpp"
#include "library.hpp"
#include "vis.hpp"
#include "wav.hpp"

namespace fs = std::filesystem;

namespace synth {

namespace {

// Bump when engine semantics change so stale cached artifacts are not
// mistaken for up-to-date ones.
constexpr uint64_t kEngineVersion = 1;

// Streaming build log: every line is stamped with the time since the
// build started; calls are serialized so worker threads can log freely.
class BuildLog {
 public:
  explicit BuildLog(std::function<void(const std::string&)> sink)
      : sink_(std::move(sink)),
        t0_(std::chrono::steady_clock::now()) {}

  explicit operator bool() const { return (bool)sink_; }

  void line(const std::string& msg) {
    if (!sink_) return;
    double secs = secondsSince(t0_);
    char head[32];
    std::snprintf(head, sizeof head, "[%8.3fs] ", secs);
    std::lock_guard<std::mutex> lock(mu_);
    sink_(head + msg);
  }

  static double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start)
        .count();
  }

 private:
  std::function<void(const std::string&)> sink_;
  std::chrono::steady_clock::time_point t0_;
  std::mutex mu_;
};

std::string fmtSecs(double s) {
  char buf[32];
  if (s < 1.0)
    std::snprintf(buf, sizeof buf, "%.0f ms", s * 1000.0);
  else
    std::snprintf(buf, sizeof buf, "%.2f s", s);
  return buf;
}

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
      << ", \"render_ms\": " << formatDouble(t.renderMillis)
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
  auto isCapitalized = [](const std::string& s) {
    return !s.empty() && std::isupper((unsigned char)s[0]);
  };
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
    } else if (key == "library") {
      if (rest.empty()) {
        diags.error(file, span, "manifest: 'library' needs a name");
        ok = false;
      } else if (!isCapitalized(rest)) {
        diags.error(file, span,
                    "manifest: library names are capitalized ('" + rest +
                        "' is not)");
        ok = false;
      } else if (!out.libraryName.empty()) {
        diags.error(file, span, "manifest: duplicate 'library' line");
        ok = false;
      } else {
        out.libraryName = rest;
      }
    } else if (key == "source") {
      if (rest.empty()) {
        diags.error(file, span, "manifest: 'source' needs a file path");
        ok = false;
      } else {
        out.sources.push_back(rest);
      }
    } else if (key == "expose") {
      if (rest.empty()) {
        diags.error(file, span, "manifest: 'expose' needs a file path");
        ok = false;
      } else if (std::find(out.sources.begin(), out.sources.end(), rest) !=
                 out.sources.end()) {
        diags.error(file, span, "manifest: '" + rest +
                                    "' is listed as both 'source' and "
                                    "'expose' ('expose' implies 'source')");
        ok = false;
      } else if (std::find(out.exposed.begin(), out.exposed.end(), rest) !=
                 out.exposed.end()) {
        diags.error(file, span,
                    "manifest: duplicate 'expose' for '" + rest + "'");
        ok = false;
      } else {
        out.exposed.push_back(rest);
      }
    } else if (key == "dep") {
      if (rest.empty()) {
        diags.error(file, span, "manifest: 'dep' needs a library name");
        ok = false;
      } else if (!isCapitalized(rest)) {
        diags.error(file, span,
                    "manifest: library names are capitalized ('" + rest +
                        "' is not)");
        ok = false;
      } else {
        out.deps.push_back(rest);
      }
    } else if (key == "build") {
      if (rest.empty()) {
        diags.error(file, span, "manifest: 'build' needs a rule path");
        ok = false;
      } else {
        out.buildRules.push_back(rest);
      }
    } else {
      diags.error(file, span, "manifest: unknown directive '" + key + "'");
      ok = false;
    }
  }
  // 'expose' implies 'source' (the double-listing case erred above).
  bool anySourceLine = !out.sources.empty();
  for (auto& e : out.exposed) out.sources.push_back(e);
  // Manifest-kind validation. Exactly one of project/library; roots
  // ('build' rules) are orchestrators; libraries must expose something.
  if (!out.projectName.empty() && !out.libraryName.empty()) {
    diags.projectError(
        "manifest: 'project' and 'library' cannot both be declared");
    ok = false;
  } else if (out.isLibrary()) {
    if (!out.buildRules.empty()) {
      diags.projectError(
          "manifest: 'build' rules are only allowed in a root 'project' "
          "manifest");
      ok = false;
    }
    if (out.exposed.empty()) {
      diags.projectError(
          "manifest: a library must 'expose' at least one file");
      ok = false;
    }
  } else {
    if (out.projectName.empty()) {
      diags.projectError("manifest: missing 'project <name>'");
      ok = false;
    }
    if (!out.exposed.empty()) {
      diags.projectError(
          "manifest: 'expose' is only allowed in a 'library' manifest");
      ok = false;
    }
    if (out.isRoot()) {
      if (anySourceLine) {
        diags.projectError(
            "manifest: a root manifest ('build' rules) cannot also list "
            "'source' files");
        ok = false;
      }
    } else if (out.sources.empty()) {
      diags.projectError("manifest: no 'source' entries");
      ok = false;
    }
  }
  return ok;
}

BuildResult buildProject(const std::string& projectDir, BuildCache* cache) {
  BuildOptions options;
  options.cache = cache;
  return buildProject(projectDir, options);
}

// The single-unit pipeline: a standalone project, a library, or a rule of
// a root build (which passes the discovered registry; single-file rules
// pass a synthesized manifest).
static BuildResult buildUnitImpl(const std::string& projectDir,
                                 const BuildOptions& options,
                                 const LibraryRegistry* preRegistry,
                                 const Manifest* overrideManifest) {
  BuildCache* cache = options.cache;
  BuildLog log(options.log);
  auto buildStart = std::chrono::steady_clock::now();

  BuildResult r;
  fs::path dir(projectDir);
  fs::path manifestPath = dir / ".build";

  std::map<std::string, std::string> sourcesByPath;  // for diag rendering

  // 1. Manifest (or a synthesized one for single-file rules).
  if (overrideManifest) {
    r.manifest = *overrideManifest;
  } else {
    std::ifstream mf(manifestPath);
    if (!mf) {
      r.diags.projectError("no .build manifest found in '" + dir.string() +
                           "'");
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
  }
  if (r.manifest.isRoot()) {
    r.diags.projectError("'" + dir.string() +
                         "' is a project root; its 'build' rules build "
                         "together (run the build at the root)");
    return r;
  }
  // A library builds like a project named after itself.
  if (r.manifest.isLibrary()) r.manifest.projectName = r.manifest.libraryName;
  log.line("manifest: project '" + r.manifest.projectName + "', " +
           std::to_string(r.manifest.sources.size()) + " source(s)");

  // Library context. The registry comes from the caller (a root build) or
  // from the enclosing root; a dependency-free library also builds
  // standalone against a registry of itself.
  LibraryRegistry localReg;
  const LibraryRegistry* registry = preRegistry;
  if (!registry && (r.manifest.isLibrary() || !r.manifest.deps.empty())) {
    std::string rootDir = findEnclosingRoot(dir.string());
    if (!rootDir.empty()) {
      localReg = discoverLibraries(rootDir, r.diags);
      registry = &localReg;
    } else if (r.manifest.isLibrary() && r.manifest.deps.empty()) {
      LibraryInfo self;
      self.name = r.manifest.libraryName;
      self.dir = dir.string();
      self.files = r.manifest.sources;
      self.exposedFiles.insert(r.manifest.exposed.begin(),
                               r.manifest.exposed.end());
      localReg.byName.emplace(self.name, std::move(self));
      registry = &localReg;
    } else {
      r.diags.projectError(
          "library dependencies require an enclosing project root (a "
          ".build with 'build' rules) to resolve them");
      return r;
    }
  }
  ModuleLoadContext ctx;
  ctx.registry = registry;
  ctx.deps = r.manifest.deps;
  if (r.manifest.isLibrary() && registry)
    ctx.currentLib = registry->find(r.manifest.libraryName);
  const ModuleLoadContext* ctxPtr = registry ? &ctx : nullptr;

  // 2. Parse & type-check every source file (plus imports).
  auto frontEndStart = std::chrono::steady_clock::now();
  std::vector<std::string> roots;
  for (auto& s : r.manifest.sources) roots.push_back((dir / s).string());
  for (auto& s : roots) r.inputs.push_back(s);
  Program prog = checkProject(roots, r.diags, ctxPtr);
  size_t defCount = 0;
  for (auto& m : prog.modules) {
    sourcesByPath[m.parsed.path] = m.parsed.source;
    if (std::find(roots.begin(), roots.end(), m.parsed.path) == roots.end())
      r.inputs.push_back(m.parsed.path);
    for (auto& d : m.parsed.defs)
      if (d.kind == TopDef::Kind::Let) defCount++;
  }
  // Watch the .build of every library that contributed modules: its
  // expose/dep lines shape this build.
  if (registry) {
    std::set<std::string> libsSeen;
    for (auto& m : prog.modules)
      if (!m.libName.empty() && libsSeen.insert(m.libName).second)
        if (const LibraryInfo* li = registry->find(m.libName))
          r.inputs.push_back((fs::path(li->dir) / ".build").string());
  }
  log.line("front-end: " + std::to_string(prog.modules.size()) +
           " module(s), " + std::to_string(defCount) +
           " definition(s) parsed + checked in " +
           fmtSecs(BuildLog::secondsSince(frontEndStart)));

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
  auto evalStart = std::chrono::steady_clock::now();
  std::vector<RenderTarget> targets;
  std::vector<std::string> audioInputs;
  bool evalOk = evaluateProgram(prog, targets, r.diags, &audioInputs);
  for (auto& a : audioInputs) r.inputs.push_back(a);
  log.line("evaluate: " + std::to_string(targets.size()) + " target(s), " +
           std::to_string(audioInputs.size()) + " audio input(s) in " +
           fmtSecs(BuildLog::secondsSince(evalStart)));

  // Dependency statistics per target (from the declaring definition's
  // spot in the definition graph).
  if (log && evalOk) {
    auto stats = defGraphStats(prog);
    for (auto& t : targets) {
      auto it = t.declDef ? stats.find(t.declDef) : stats.end();
      if (it == stats.end()) continue;
      const DefStats& st = it->second;
      log.line("deps: '" + t.name + "' (declared in " +
               st.mod->parsed.name + "): " +
               std::to_string(st.directDeps) + " direct dependenc" +
               (st.directDeps == 1 ? "y" : "ies") + ", " +
               std::to_string(st.dependents) + " dependent(s), closure " +
               std::to_string(st.closureSize) + " definition(s)");
    }
  }

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
  if (!cache) log.line("cache: none supplied (full rebuild)");

  // 4. Discretize targets and write artifacts. Cache-fresh targets are
  // reused without re-rendering; the rest render in parallel across a
  // thread pool (Epic 9) - safe because signal graphs are immutable and
  // all per-render state lives in each render's own context. A failing
  // target is recorded in metadata but does not stop the others (§6.3).
  r.targets.resize(targets.size());
  std::vector<size_t> pending;
  auto extensionFor = [](const RenderTarget& t) {
    return t.kind == RenderTarget::Kind::Audio ? ".wav" : ".svg";
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
      log.line("target '" + t.name + "': cached, artifact reused");
    } else {
      pending.push_back(i);
    }
  }
  if (cache)
    log.line("cache: " + std::to_string(targets.size() - pending.size()) +
             "/" + std::to_string(targets.size()) + " target(s) fresh, " +
             std::to_string(pending.size()) + " to render");

  // 4b. Shared bus rendering across targets: when one pending target's
  // signal occurs as a shared subtree of another pending target's graph
  // (the canonical case: a master summing the buses the stems render),
  // the containing target is scheduled after its providers and their
  // finished buffers are served to it block-by-block instead of being
  // re-discretized. Deps only form between targets with identical
  // windows and rates; renders are deterministic, so output is identical.
  std::vector<std::vector<size_t>> providersOf(targets.size());
  std::vector<std::vector<size_t>> consumersOf(targets.size());
  {
    auto shareable = [&](const RenderTarget& t) {
      return t.kind != RenderTarget::Kind::VisualStems &&
             t.sample.sig != nullptr;
    };
    for (size_t a : pending) {
      const RenderTarget& ta = targets[a];
      if (!shareable(ta)) continue;
      for (size_t b : pending) {
        if (a == b) continue;
        const RenderTarget& tb = targets[b];
        if (!shareable(tb)) continue;
        if (ta.rate != tb.rate || ta.sample.from != tb.sample.from ||
            ta.sample.to != tb.sample.to)
          continue;
        const SigNode* ra = ta.sample.sig.get();
        const SigNode* rb = tb.sample.sig.get();
        // Identical roots (duplicate targets) tie-break by index; the
        // containment order itself is acyclic on a DAG.
        bool dep = ra == rb ? b < a : graphContainsShared(ta.sample.sig, rb);
        if (dep) {
          providersOf[a].push_back(b);
          consumersOf[b].push_back(a);
        }
      }
      if (!providersOf[a].empty()) {
        std::string names;
        for (size_t b : providersOf[a])
          names += (names.empty() ? "'" : ", '") + targets[b].name + "'";
        log.line("share: target '" + ta.name + "' reuses " +
                 std::to_string(providersOf[a].size()) +
                 " shared bus render(s): " + names);
      }
    }
  }

  // Sample-window cache for the whole build: shared by all targets, and
  // - when the caller (the daemon) supplies one - by successive builds.
  // beginBuild starts a generation and prunes entries the previous build
  // did not place.
  std::shared_ptr<SampleCache> localSampleCache;
  SampleCache* sampleCache = options.sampleCache;
  if (!sampleCache) {
    localSampleCache = makeSampleCache();
    sampleCache = localSampleCache.get();
  }
  sampleCacheBeginBuild(*sampleCache);

  std::vector<std::string> renderErrors(targets.size());
  {
    std::mutex schedMutex;
    std::condition_variable schedCv;
    std::deque<size_t> ready;
    std::vector<size_t> waitCount(targets.size(), 0);
    std::vector<size_t> consumersLeft(targets.size(), 0);
    // Buffers of provider targets, kept only while consumers still need
    // them (a stem buffer is ~duration * rate * channels doubles).
    std::unordered_map<size_t, std::shared_ptr<Rendered>> busBuffers;
    size_t done = 0;
    for (size_t i : pending) {
      waitCount[i] = providersOf[i].size();
      consumersLeft[i] = consumersOf[i].size();
      if (waitCount[i] == 0) ready.push_back(i);
    }
    auto worker = [&](int workerId) {
      for (;;) {
        size_t i;
        PreRenderedMap pre;
        std::vector<std::shared_ptr<Rendered>> keepAlive;
        {
          std::unique_lock<std::mutex> lk(schedMutex);
          schedCv.wait(
              lk, [&] { return !ready.empty() || done == pending.size(); });
          if (ready.empty()) return;
          i = ready.front();
          ready.pop_front();
          for (size_t j : providersOf[i]) {
            auto it = busBuffers.find(j);
            if (it == busBuffers.end()) continue;  // provider failed
            keepAlive.push_back(it->second);
            pre[targets[j].sample.sig.get()] = PreRenderedWindow{
                (int64_t)llround(targets[j].sample.from * targets[j].rate),
                it->second.get()};
          }
        }
        std::shared_ptr<Rendered> forConsumers;
        const RenderTarget& t = targets[i];
        auto targetStart = std::chrono::steady_clock::now();
        double renderSecs = 0, writeSecs = 0;
        TargetInfo info;
        info.name = t.name;
        info.kind = t.kind == RenderTarget::Kind::Audio ? "audio" : "visual";
        info.rate = t.rate;
        try {
          std::string fileName = t.name + extensionFor(t);
          fs::path artifactPath = artifactDir / fileName;
          if (t.kind == RenderTarget::Kind::VisualStems) {
            // Lanes often contain one another (an overview stacks the
            // master above the buses it sums), so discretize them in
            // dependency order and serve finished lanes to the ones that
            // contain them - the intra-target version of shared bus
            // rendering.
            size_t laneCount = t.stems.size();
            std::vector<std::vector<size_t>> laneProviders(laneCount);
            for (size_t x = 0; x < laneCount; x++) {
              const SampleV& sx = t.stems[x].second;
              for (size_t y = 0; y < laneCount; y++) {
                if (x == y) continue;
                const SampleV& sy = t.stems[y].second;
                if (sx.from != sy.from || sx.to != sy.to) continue;
                const SigNode* rx = sx.sig.get();
                const SigNode* ry = sy.sig.get();
                bool dep =
                    rx == ry ? y < x : graphContainsShared(sx.sig, ry);
                if (dep) laneProviders[x].push_back(y);
              }
            }
            std::vector<Rendered> laneR(laneCount);
            std::vector<char> laneDone(laneCount, 0);
            for (size_t finished = 0; finished < laneCount; finished++) {
              size_t x = laneCount;
              for (size_t c = 0; c < laneCount && x == laneCount; c++) {
                if (laneDone[c]) continue;
                bool ok = true;
                for (size_t y : laneProviders[c]) ok = ok && laneDone[y];
                if (ok) x = c;
              }
              if (x == laneCount)  // unreachable: containment is acyclic
                throw std::runtime_error("internal: lane dependency cycle");
              const SampleV& s = t.stems[x].second;
              PreRenderedMap lanePre;
              for (size_t y : laneProviders[x])
                lanePre[t.stems[y].second.sig.get()] = PreRenderedWindow{
                    (int64_t)llround(t.stems[y].second.from * t.rate),
                    &laneR[y]};
              auto laneStart = std::chrono::steady_clock::now();
              laneR[x] =
                  renderWindow(s.sig, s.from, s.to, t.rate, 0,
                               lanePre.empty() ? nullptr : &lanePre,
                               sampleCache);
              double laneSecs = BuildLog::secondsSince(laneStart);
              renderSecs += laneSecs;
              log.line("[worker " + std::to_string(workerId) + "] '" +
                       t.name + "' lane '" + t.stems[x].first +
                       "' discretized in " + fmtSecs(laneSecs) +
                       (lanePre.empty()
                            ? std::string()
                            : " (reused " + std::to_string(lanePre.size()) +
                                  " lane render(s))"));
              laneDone[x] = 1;
            }
            std::vector<std::pair<std::string, Rendered>> lanes;
            for (size_t x = 0; x < laneCount; x++)
              lanes.emplace_back(t.stems[x].first, std::move(laneR[x]));
            auto writeStart = std::chrono::steady_clock::now();
            std::ofstream out(artifactPath, std::ios::trunc);
            if (!out) throw std::runtime_error("cannot write artifact file");
            out << renderStackedWaveformSvg(t.name, lanes, t.rate);
            writeSecs = BuildLog::secondsSince(writeStart);
            for (auto& [label, r] : lanes)
              info.frames = std::max(info.frames, r.frames);
            info.channelCount = lanes.empty() ? 0 : lanes[0].second.channels;
          } else {
            auto renderStart = std::chrono::steady_clock::now();
            auto rendered = std::make_shared<Rendered>(
                renderWindow(t.sample.sig, t.sample.from, t.sample.to, t.rate,
                             0, pre.empty() ? nullptr : &pre, sampleCache));
            renderSecs = BuildLog::secondsSince(renderStart);
            auto writeStart = std::chrono::steady_clock::now();
            if (t.kind == RenderTarget::Kind::Visual) {
              std::ofstream out(artifactPath, std::ios::trunc);
              if (!out) throw std::runtime_error("cannot write artifact file");
              out << renderWaveformSvg(t.name, *rendered, t.rate);
            } else {
              writeWav(artifactPath.string(), t.rate, rendered->channels,
                       rendered->interleaved);
            }
            writeSecs = BuildLog::secondsSince(writeStart);
            info.channelCount = rendered->channels;
            info.frames = rendered->frames;
            forConsumers = std::move(rendered);
          }
          info.artifact =
              (fs::path("build") / "artifacts" / fileName).generic_string();
          info.durationSeconds =
              t.rate > 0 ? (double)info.frames / t.rate : 0;
          info.renderMillis =
              BuildLog::secondsSince(targetStart) * 1000.0;
          info.ok = true;
          log.line("[worker " + std::to_string(workerId) + "] target '" +
                   t.name + "' (" + info.kind + "): " +
                   std::to_string(info.frames) + " frames x " +
                   std::to_string(info.channelCount) + " ch in " +
                   fmtSecs(BuildLog::secondsSince(targetStart)) +
                   " (discretize " + fmtSecs(renderSecs) + ", write " +
                   fmtSecs(writeSecs) +
                   (pre.empty() ? std::string()
                                : ", reused " + std::to_string(pre.size()) +
                                      " bus render(s)") +
                   ")");
        } catch (const std::exception& e) {
          info.error = e.what();
          renderErrors[i] = e.what();
          log.line("[worker " + std::to_string(workerId) + "] target '" +
                   t.name + "' FAILED after " +
                   fmtSecs(BuildLog::secondsSince(targetStart)) + ": " +
                   e.what());
        }
        r.targets[i] = std::move(info);
        {
          std::lock_guard<std::mutex> lk(schedMutex);
          if (forConsumers && consumersLeft[i] > 0 && renderErrors[i].empty())
            busBuffers[i] = std::move(forConsumers);
          for (size_t j : providersOf[i])
            if (consumersLeft[j] > 0 && --consumersLeft[j] == 0)
              busBuffers.erase(j);
          for (size_t k : consumersOf[i])
            if (--waitCount[k] == 0) ready.push_back(k);
          done++;
          schedCv.notify_all();
        }
      }
    };
    size_t threadCount = std::min<size_t>(
        pending.size(), std::max(1u, std::thread::hardware_concurrency()));
    if (!pending.empty())
      log.line("render: " + std::to_string(pending.size()) +
               " target(s) across " + std::to_string(threadCount) +
               " worker thread(s)");
    std::vector<std::thread> pool;
    for (size_t i = 1; i < threadCount; i++)
      pool.emplace_back(worker, (int)i);
    if (!pending.empty()) worker(0);
    for (auto& th : pool) th.join();
  }
  if (!pending.empty()) {
    SampleCacheStats scs = sampleCacheStats(*sampleCache);
    log.line("samples: " + std::to_string(scs.entries) + " window(s) cached (" +
             std::to_string(scs.bytes / 1024) + " KiB), " +
             std::to_string(scs.rendered) + " rendered this build, " +
             std::to_string(scs.reusedPrior) + " reused from previous builds");
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
  if (log) {
    size_t rendered = 0, cachedCount = 0, failed = 0;
    for (auto& t : r.targets) {
      if (!t.ok) failed++;
      else if (t.cached) cachedCount++;
      else rendered++;
    }
    log.line("done: " + std::to_string(r.targets.size()) + " target(s) (" +
             std::to_string(rendered) + " rendered, " +
             std::to_string(cachedCount) + " cached, " +
             std::to_string(failed) + " failed), wall " +
             fmtSecs(BuildLog::secondsSince(buildStart)));
  }
  return r;
}

BuildResult buildProject(const std::string& projectDir,
                         const BuildOptions& options) {
  return buildUnitImpl(projectDir, options, nullptr, nullptr);
}

RootBuildResult buildRoot(const std::string& rootDir,
                          const BuildOptions& options,
                          std::map<std::string, BuildCache>* ruleCaches) {
  RootBuildResult rr;
  fs::path dir(rootDir);
  fs::path manifestPath = dir / ".build";
  std::ifstream mf(manifestPath);
  if (!mf) {
    rr.diags.projectError("no .build manifest found in '" + dir.string() +
                          "'");
    return rr;
  }
  std::ostringstream mss;
  mss << mf.rdbuf();
  std::string text = mss.str();
  if (!parseManifest(text, manifestPath.string(), rr.manifest, rr.diags))
    return rr;
  if (!rr.manifest.isRoot()) {
    rr.diags.projectError("'" + manifestPath.string() +
                          "' is not a root manifest (no 'build' rules)");
    return rr;
  }
  // Libraries are discovered dynamically: every .build under the root
  // that declares one, wherever it lives in the tree.
  rr.registry = discoverLibraries(rootDir, rr.diags);
  if (rr.diags.hasErrors()) return rr;
  rr.ok = true;
  for (auto& rule : rr.manifest.buildRules) {
    fs::path rulePath = dir / rule;
    BuildOptions o = options;
    // Each rule keeps its own incremental cache: caches key targets by
    // render name, which is only unique per rule.
    o.cache = ruleCaches ? &(*ruleCaches)[rule] : nullptr;
    BuildResult br;
    std::error_code ec;
    if (fs::is_directory(rulePath, ec)) {
      br = buildUnitImpl(rulePath.string(), o, &rr.registry, nullptr);
    } else if (rulePath.extension() == ".synth" && fs::exists(rulePath, ec)) {
      Manifest m;
      m.projectName = rulePath.stem().string();
      m.sources = {rulePath.filename().string()};
      br = buildUnitImpl(rulePath.parent_path().string(), o, &rr.registry,
                         &m);
    } else {
      br.diags.projectError("build rule '" + rule +
                            "' is neither a directory with a .build nor a "
                            ".synth file");
    }
    rr.ok = rr.ok && br.ok;
    rr.rules.emplace_back(rule, std::move(br));
  }
  return rr;
}

DiagnosticBag lintFiles(const std::vector<std::string>& files) {
  DiagnosticBag diags;
  // Group files by their enclosing project root (if any) so library
  // imports resolve; rootless files check standalone. Lint is lenient
  // about `dep` declarations - every discovered library is in scope.
  std::map<std::string, std::vector<std::string>> byRoot;
  for (auto& f : files)
    byRoot[findEnclosingRoot(fs::path(f).parent_path().string())]
        .push_back(f);
  for (auto& [root, group] : byRoot) {
    if (root.empty()) {
      checkProject(group, diags);
      continue;
    }
    LibraryRegistry reg = discoverLibraries(root, diags);
    // Files inside a library lint as that library (sibling imports and
    // canonical ids resolve as they would in its build).
    std::map<std::string, std::vector<std::string>> byLib;
    for (auto& f : group) {
      std::string owner;
      std::error_code ec;
      fs::path fp = fs::absolute(f, ec);
      for (auto& [name, li] : reg.byName) {
        fs::path rel = fs::relative(fp, fs::absolute(li.dir, ec), ec);
        if (!rel.empty() && rel.string().rfind("..", 0) != 0) owner = name;
      }
      byLib[owner].push_back(f);
    }
    for (auto& [libName, libGroup] : byLib) {
      ModuleLoadContext ctx;
      ctx.registry = &reg;
      for (auto& [name, li] : reg.byName) ctx.deps.push_back(name);
      if (!libName.empty()) ctx.currentLib = reg.find(libName);
      checkProject(libGroup, diags, &ctx);
    }
  }
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

// Stamp every directory and .build file under `root` (skipping output and
// hidden dirs): directory stamps catch created/removed files, and .build
// stamps catch manifest edits - including manifests of libraries that no
// rule has loaded yet, so a new library appearing re-runs discovery.
void stampTree(const fs::path& root, std::map<std::string, Stamp>& snap) {
  std::error_code ec;
  snap[root.string()] = stampOf(root);
  fs::path manifest = root / ".build";
  if (fs::exists(manifest, ec)) snap[manifest.string()] = stampOf(manifest);
  for (auto& entry : fs::directory_iterator(
           root, fs::directory_options::skip_permission_denied, ec)) {
    if (!entry.is_directory(ec)) continue;
    std::string name = entry.path().filename().string();
    if (name == "build" || (!name.empty() && name[0] == '.')) continue;
    stampTree(entry.path(), snap);
  }
}

std::map<std::string, Stamp> snapshotRoot(const std::string& rootDir,
                                          const RootBuildResult& rr) {
  std::map<std::string, Stamp> snap;
  stampTree(fs::path(rootDir), snap);
  for (auto& [rule, br] : rr.rules)
    for (auto& f : br.inputs) snap[f] = stampOf(f);
  return snap;
}

}  // namespace

void watchProject(const std::string& projectDir,
                  const std::function<void(const BuildResult&)>& onBuild,
                  const std::function<bool()>& keepRunning, int pollMillis,
                  std::function<void(const std::string&)> log) {
  // The daemon owns the incremental cache: rebuilds triggered by an edit
  // only re-render targets whose dependency closure actually changed. It
  // also owns a cross-build sample-window cache keyed by structural
  // content hash, so even when a target IS dirty (an arrangement edit),
  // the samples it places re-render only if their own definitions
  // changed.
  BuildCache cache;
  std::shared_ptr<SampleCache> sampleCache = makeSampleCache();
  BuildOptions options;
  options.cache = &cache;
  options.sampleCache = sampleCache.get();
  options.log = log;
  BuildResult r = buildProject(projectDir, options);
  onBuild(r);
  auto snap = snapshot(projectDir, r.inputs);
  while (keepRunning()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(pollMillis));
    auto now = snapshot(projectDir, r.inputs);
    if (now == snap) continue;
    if (log) log("--- change detected, rebuilding ---");
    r = buildProject(projectDir, options);
    onBuild(r);
    snap = snapshot(projectDir, r.inputs);
  }
}

void watchRoot(const std::string& rootDir,
               const std::function<void(const RootBuildResult&)>& onBuild,
               const std::function<bool()>& keepRunning, int pollMillis,
               std::function<void(const std::string&)> log) {
  // Per-rule incremental caches (render names are unique per rule, not
  // project-wide) plus one shared structural sample cache - safe across
  // rules because it is keyed by content hash.
  std::map<std::string, BuildCache> caches;
  std::shared_ptr<SampleCache> sampleCache = makeSampleCache();
  BuildOptions options;
  options.sampleCache = sampleCache.get();
  options.log = log;
  RootBuildResult rr = buildRoot(rootDir, options, &caches);
  onBuild(rr);
  auto snap = snapshotRoot(rootDir, rr);
  while (keepRunning()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(pollMillis));
    auto now = snapshotRoot(rootDir, rr);
    if (now == snap) continue;
    if (log) log("--- change detected, rebuilding ---");
    rr = buildRoot(rootDir, options, &caches);
    onBuild(rr);
    snap = snapshotRoot(rootDir, rr);
  }
}

}  // namespace synth
