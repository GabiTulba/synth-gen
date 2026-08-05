#include "build.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "checker.hpp"
#include "eval.hpp"
#include "wav.hpp"

namespace fs = std::filesystem;

namespace synth {

namespace {

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
    j << "    {\"name\": \"" << jsonEscape(t.name) << "\", \"status\": \""
      << (t.ok ? "ok" : "error") << "\", \"artifact\": \""
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

BuildResult buildProject(const std::string& projectDir) {
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
  if (!parseManifest(manifestText, manifestPath.string(), r.manifest,
                     r.diags))
    return r;

  // 2. Parse & type-check every source file (plus imports).
  std::vector<std::string> roots;
  for (auto& s : r.manifest.sources) roots.push_back((dir / s).string());
  Program prog = checkProject(roots, r.diags);
  for (auto& m : prog.modules)
    sourcesByPath[m.parsed.path] = m.parsed.source;

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
  bool evalOk = evaluateProgram(prog, targets, r.diags);

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

  // 4. Discretize each target and write artifacts. A failing target is
  // recorded in metadata but does not stop the others (partial-failure
  // builds stay useful, §6.3).
  bool allOk = true;
  for (auto& t : targets) {
    TargetInfo info;
    info.name = t.name;
    info.rate = t.rate;
    try {
      Rendered rendered =
          renderWindow(t.sample.sig, t.sample.from, t.sample.to, t.rate);
      std::string fileName = t.name + ".wav";
      fs::path artifactPath = artifactDir / fileName;
      writeWav(artifactPath.string(), t.rate, rendered.channels,
               rendered.interleaved);
      info.artifact =
          (fs::path("build") / "artifacts" / fileName).generic_string();
      info.channelCount = rendered.channels;
      info.frames = rendered.frames;
      info.durationSeconds =
          t.rate > 0 ? (double)rendered.frames / t.rate : 0;
      info.ok = true;
    } catch (const std::exception& e) {
      info.error = e.what();
      r.diags.error(t.file, t.span,
                    "render target '" + t.name + "': " + e.what());
      allOk = false;
    }
    r.targets.push_back(std::move(info));
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

}  // namespace synth
