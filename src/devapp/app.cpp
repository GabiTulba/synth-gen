// synth-dev — the SynthGraph dev app (design doc §9, §10).
//
// A pure consumer of build outputs: it reads the project's metadata.json
// from <root>/_build/<project>/ (where <root> is the enclosing project
// root, or the project dir itself when standalone), lists render targets
// with their basic facts, plays artifacts, and live-refreshes when the
// metadata changes (i.e. whenever the daemon or a one-shot build rewrites
// it). Pointed at a root, it shows every `build` rule's metadata as its
// own section. Each audio target opens an interactive waveform panel:
// per-channel min/max envelopes (the same picture the .svg vis renders),
// wheel zoom about the cursor, right-drag pan, left-drag range selection,
// and range playback with a playhead. Beyond root/manifest resolution it
// never talks to compiler internals.

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "metadata.hpp"
#include "player.hpp"
#include "wav.hpp"
#include "waveform.hpp"

namespace fs = std::filesystem;
using namespace synth::devapp;

namespace {

struct UnitState {
  MetadataUnit unit;
  MetadataLoadResult loaded;
  FileStamp stamp;
};

// One open waveform panel; any number can be open at once, each with its
// own zoom window and selection. Holds the decoded WAV plus per-channel
// peak bins; reloaded automatically when a rebuild rewrites the artifact.
struct WavePanel {
  std::string artifactPath;  // empty = closed
  std::string targetName;
  FileStamp stamp;
  std::string error;  // decode failure, shown in place of the canvas
  synth::WavData wav;
  std::vector<PeakBins> bins;  // per channel
  WaveView view;
  double selStart = -1, selEnd = -1;  // selection in frames; -1 = none
  double dragAnchor = -1;             // frame where a selection drag began

  void open(const std::string& path, const std::string& name) {
    artifactPath = path;
    targetName = name;
    stamp = stampFile(path);
    load();
  }

  void load() {
    error.clear();
    bins.clear();
    selStart = selEnd = -1;
    dragAnchor = -1;
    try {
      wav = synth::readWav(artifactPath);
    } catch (const std::exception& e) {
      wav = synth::WavData{};
      error = e.what();
      view.reset(0);
      return;
    }
    for (auto& ch : wav.channels) bins.push_back(buildPeakBins(ch));
    view.reset(wav.frames());
  }

  // A rebuild rewrote (or removed) the artifact: reload, keeping the
  // current zoom window and selection clamped to the new length.
  void reloadIfChanged() {
    if (artifactPath.empty()) return;
    FileStamp now = stampFile(artifactPath);
    if (now == stamp) return;
    stamp = now;
    WaveView old = view;
    double oldSelStart = selStart, oldSelEnd = selEnd;
    load();
    if (error.empty() && old.frames > 0) {
      view = old;
      view.frames = wav.frames();
      view.clamp();
      if (oldSelStart >= 0) {
        selStart = std::min(oldSelStart, (double)wav.frames());
        selEnd = std::min(oldSelEnd, (double)wav.frames());
        if (selEnd - selStart < 1) selStart = selEnd = -1;
      }
    }
  }

  bool hasSelection() const { return selStart >= 0 && selEnd > selStart; }
};

struct AppState {
  std::string projectDir;
  std::string rootDir;  // enclosing root; artifact paths are relative to it
  std::string manifestPath;
  FileStamp manifestStamp;
  std::vector<UnitState> units;
  double sinceStatMs = 1e9;  // force an immediate first load
  AudioPlayer player;
  std::string playError;
  std::vector<WavePanel> waves;

  WavePanel* findWave(const std::string& path) {
    for (auto& w : waves)
      if (w.artifactPath == path) return &w;
    return nullptr;
  }

  void toggleWave(const std::string& path, const std::string& name) {
    for (size_t i = 0; i < waves.size(); i++) {
      if (waves[i].artifactPath == path) {
        waves.erase(waves.begin() + i);
        return;
      }
    }
    WavePanel w;
    w.open(path, name);
    waves.push_back(std::move(w));
  }

  void resolveLayout() {
    MetadataLayout layout = resolveMetadataLayout(projectDir);
    rootDir = layout.rootDir;
    manifestPath = layout.manifestPath;
    std::vector<UnitState> next;
    for (auto& u : layout.units) {
      UnitState s;
      for (auto& prev : units)
        if (prev.unit.metadataPath == u.metadataPath) s = std::move(prev);
      s.unit = u;
      next.push_back(std::move(s));
    }
    units = std::move(next);
  }

  void maybeRefresh(double dtMs) {
    sinceStatMs += dtMs;
    if (sinceStatMs < 250.0) return;  // stat ~4x/second, reload on change
    sinceStatMs = 0;
    // Adding or removing root build rules changes which metadata files
    // this app should be watching.
    FileStamp m = stampFile(manifestPath);
    if (!(m == manifestStamp)) {
      manifestStamp = m;
      resolveLayout();
    }
    for (auto& u : units) {
      FileStamp now = stampFile(u.unit.metadataPath);
      if (now == u.stamp && u.loaded.ok) continue;
      u.stamp = now;
      u.loaded = loadProjectMetadata(u.unit.metadataPath);
    }
    for (auto& w : waves) w.reloadIfChanged();
  }
};

std::string formatSeconds(double s) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3fs", s);
  return buf;
}

void drawDiagnostics(const ProjectMeta& meta) {
  if (meta.diagnostics.empty()) return;
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
  bool open = ImGui::CollapsingHeader(
      ("diagnostics (" + std::to_string(meta.diagnostics.size()) + ")###diags")
          .c_str(),
      ImGuiTreeNodeFlags_DefaultOpen);
  ImGui::PopStyleColor();
  if (!open) return;
  for (auto& d : meta.diagnostics) {
    const std::string& text = d.rendered.empty() ? d.message : d.rendered;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.55f, 1.0f));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
}

void drawTargets(AppState& app, UnitState& u) {
  const ProjectMeta& meta = u.loaded.meta;
  if (meta.targets.empty()) {
    ImGui::TextDisabled("no render targets in this build");
    return;
  }
  ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp;
  if (!ImGui::BeginTable("targets", 6, flags)) return;
  ImGui::TableSetupColumn("target", ImGuiTableColumnFlags_WidthStretch, 3.0f);
  ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthStretch, 1.2f);
  ImGui::TableSetupColumn("duration", ImGuiTableColumnFlags_WidthStretch, 1.4f);
  ImGui::TableSetupColumn("rate", ImGuiTableColumnFlags_WidthStretch, 1.4f);
  ImGui::TableSetupColumn("ch", ImGuiTableColumnFlags_WidthStretch, 0.7f);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.6f);
  ImGui::TableHeadersRow();

  for (auto& t : meta.targets) {
    ImGui::TableNextRow();
    ImGui::PushID(t.name.c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(t.name.c_str());

    ImGui::TableNextColumn();
    if (t.status == "ok")
      ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "ok");
    else
      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "error");

    ImGui::TableNextColumn();
    ImGui::Text("%.3fs", t.durationSeconds);
    ImGui::TableNextColumn();
    ImGui::Text("%.0f Hz", t.rate);
    ImGui::TableNextColumn();
    ImGui::Text("%d", t.channels);

    ImGui::TableNextColumn();
    std::string artifactPath =
        (fs::path(app.rootDir) / t.artifact).string();
    bool isPlaying =
        app.player.playing() && app.player.currentPath() == artifactPath;
    if (t.kind != "visual" && t.status == "ok" && !t.artifact.empty()) {
      bool waveOpen = app.findWave(artifactPath) != nullptr;
      if (ImGui::SmallButton(waveOpen ? "hide" : "wave"))
        app.toggleWave(artifactPath, t.name);
      ImGui::SameLine();
    }
    if (t.kind == "visual") {
      // Waveform images are viewed in any browser/image viewer; the dev
      // app just points at them.
      ImGui::TextDisabled("%s", t.status == "ok" ? "waveform svg" : "-");
    } else if (isPlaying) {
      if (ImGui::SmallButton("stop")) app.player.stop();
      ImGui::SameLine();
      ImGui::ProgressBar((float)app.player.progress(), ImVec2(-1, 0), "");
    } else if (t.status == "ok" && !t.artifact.empty()) {
      if (ImGui::SmallButton("play")) {
        app.playError.clear();
        if (!app.player.play(artifactPath, app.playError) &&
            !app.playError.empty())
          app.playError = t.name + ": " + app.playError;
      }
    } else {
      ImGui::TextDisabled("%s", t.error.empty() ? "-" : t.error.c_str());
    }
    ImGui::PopID();
  }
  ImGui::EndTable();
}

// The interactive waveform pane: min/max envelope lanes per channel
// (matching the .svg vis), wheel zoom about the cursor, right-drag pan,
// left-drag selection, and playback of the selection or visible range.
// Returns false when the user closed the panel.
bool drawWavePanel(AppState& app, WavePanel& p) {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::Text("waveform: %s", p.targetName.c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("close")) return false;

  if (!p.error.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "cannot load %s: %s",
                       p.artifactPath.c_str(), p.error.c_str());
    return true;
  }
  if (p.wav.frames() == 0) {
    ImGui::TextDisabled("empty artifact");
    return true;
  }

  WaveView& v = p.view;
  double rate = p.wav.rate;

  bool isThisPlaying =
      app.player.playing() && app.player.currentPath() == p.artifactPath;
  if (isThisPlaying) {
    if (ImGui::SmallButton("stop")) app.player.stop();
  } else {
    if (ImGui::SmallButton(p.hasSelection() ? "play selection"
                                            : "play view")) {
      double from = p.hasSelection() ? p.selStart : v.start;
      double to = p.hasSelection() ? p.selEnd : v.end;
      app.playError.clear();
      if (!app.player.playRange(p.artifactPath, (int64_t)std::floor(from),
                                (int64_t)std::ceil(to), app.playError) &&
          !app.playError.empty())
        app.playError = p.targetName + ": " + app.playError;
    }
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("zoom in")) v.zoomAt(0.5, 1.0 / 1.5);
  ImGui::SameLine();
  if (ImGui::SmallButton("zoom out")) v.zoomAt(0.5, 1.5);
  ImGui::SameLine();
  if (ImGui::SmallButton("fit")) v.reset(v.frames);
  if (p.hasSelection()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("zoom to selection")) {
      v.start = p.selStart;
      v.end = p.selEnd;
      v.clamp();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("clear selection")) p.selStart = p.selEnd = -1;
  }
  ImGui::SameLine();
  if (rate > 0) {
    std::string info = "view " + formatSeconds(v.start / rate) + " - " +
                       formatSeconds(v.end / rate);
    if (p.hasSelection())
      info += " | selection " + formatSeconds(p.selStart / rate) + " - " +
              formatSeconds(p.selEnd / rate) + " (" +
              formatSeconds((p.selEnd - p.selStart) / rate) + ")";
    else
      info += " | drag: select, wheel: zoom, right-drag: pan";
    ImGui::TextDisabled("%s", info.c_str());
  }

  int channels = (int)p.wav.channels.size();
  float laneH = channels > 1 ? 80.0f : 140.0f;
  float laneGap = 4.0f;
  ImVec2 canvasSize(std::max(120.0f, ImGui::GetContentRegionAvail().x),
                    channels * laneH + (channels - 1) * laneGap);
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("wave_canvas", canvasSize);
  bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiIO& io = ImGui::GetIO();
  double width = canvasSize.x;
  auto frameToX = [&](double f) {
    return pos.x + (float)((f - v.start) / v.span() * width);
  };
  auto xToFrame = [&](double x) {
    return v.start + (x - pos.x) / width * v.span();
  };

  // An in-progress drag (right-button pan or left-button selection) takes
  // precedence over wheel zoom: applying both in one frame makes them
  // fight over the view.
  bool dragActive = ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                    ImGui::IsItemActive();
  if (hovered && io.MouseWheel != 0 && !dragActive) {
    double frac = std::clamp((double)(io.MousePos.x - pos.x) / width, 0.0, 1.0);
    v.zoomAt(frac, std::pow(1.3, (double)-io.MouseWheel));
  }
  if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0))
    v.pan(-io.MouseDelta.x * v.span() / width);

  if (ImGui::IsItemActivated())
    p.dragAnchor = std::clamp(xToFrame(io.MousePos.x), 0.0, (double)v.frames);
  if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      p.dragAnchor >= 0) {
    double cur = std::clamp(xToFrame(io.MousePos.x), 0.0, (double)v.frames);
    p.selStart = std::min(p.dragAnchor, cur);
    p.selEnd = std::max(p.dragAnchor, cur);
  }
  if (ImGui::IsItemDeactivated()) {
    // A click without a real drag (under ~2px of travel) clears the
    // selection instead of leaving a sliver selected.
    if (p.hasSelection() && p.selEnd - p.selStart < v.span() / width * 2.0)
      p.selStart = p.selEnd = -1;
    p.dragAnchor = -1;
  }

  for (int c = 0; c < channels; c++) {
    float laneTop = pos.y + c * (laneH + laneGap);
    float laneBot = laneTop + laneH;
    float mid = laneTop + laneH * 0.5f;
    float amp = laneH * 0.5f - 2.0f;
    dl->AddRectFilled(ImVec2(pos.x, laneTop),
                      ImVec2(pos.x + (float)width, laneBot),
                      IM_COL32(20, 20, 26, 255));
    dl->AddLine(ImVec2(pos.x, mid), ImVec2(pos.x + (float)width, mid),
                IM_COL32(60, 60, 75, 255));
    auto cols =
        minMaxColumns(p.wav.channels[c], p.bins[c], v.start, v.end,
                      (int)width);
    for (int x = 0; x < (int)cols.size(); x++) {
      float lo = std::clamp(cols[x].first, -1.0f, 1.0f);
      float hi = std::clamp(cols[x].second, -1.0f, 1.0f);
      dl->AddLine(ImVec2(pos.x + x + 0.5f, mid - hi * amp),
                  ImVec2(pos.x + x + 0.5f, mid - lo * amp + 1.0f),
                  IM_COL32(110, 205, 160, 255));
    }
  }

  if (p.hasSelection() && p.selEnd > v.start && p.selStart < v.end) {
    float x0 = std::max(frameToX(p.selStart), pos.x);
    float x1 = std::min(frameToX(p.selEnd), pos.x + (float)width);
    dl->AddRectFilled(ImVec2(x0, pos.y), ImVec2(x1, pos.y + canvasSize.y),
                      IM_COL32(120, 160, 255, 48));
    dl->AddLine(ImVec2(x0, pos.y), ImVec2(x0, pos.y + canvasSize.y),
                IM_COL32(150, 180, 255, 180));
    dl->AddLine(ImVec2(x1, pos.y), ImVec2(x1, pos.y + canvasSize.y),
                IM_COL32(150, 180, 255, 180));
  }

  if (isThisPlaying && rate > 0) {
    double f = app.player.positionSeconds() * rate;
    if (f >= v.start && f <= v.end) {
      float x = frameToX(f);
      dl->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + canvasSize.y),
                  IM_COL32(255, 230, 120, 220));
    }
  }
  return true;
}

void drawFrame(AppState& app) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("synthgraph", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

  bool anyMissing = false;
  for (size_t i = 0; i < app.units.size(); i++) {
    UnitState& u = app.units[i];
    if (i) {
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
    }
    ImGui::PushID((int)i);
    if (!u.unit.label.empty()) {
      ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "[%s]",
                         u.unit.label.c_str());
      ImGui::SameLine();
    }
    if (!u.loaded.ok) {
      anyMissing = true;
      ImGui::TextWrapped("no build metadata at %s",
                         u.unit.metadataPath.c_str());
      ImGui::TextDisabled("(%s)", u.loaded.error.c_str());
      ImGui::PopID();
      continue;
    }
    const ProjectMeta& meta = u.loaded.meta;
    ImGui::Text("project: %s", meta.project.c_str());
    ImGui::SameLine();
    if (meta.status == "ok")
      ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "[build ok]");
    else
      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "[build failed]");
    ImGui::SameLine();
    ImGui::TextDisabled("metadata: %s", u.unit.metadataPath.c_str());
    ImGui::Separator();

    drawDiagnostics(meta);
    drawTargets(app, u);
    ImGui::PopID();
  }
  if (anyMissing) {
    ImGui::Spacing();
    ImGui::TextWrapped(
        "run `synthc build %s` or leave `synthc watch %s` running; this "
        "window refreshes on its own.",
        app.projectDir.c_str(), app.projectDir.c_str());
  }

  for (size_t i = 0; i < app.waves.size();) {
    ImGui::PushID(app.waves[i].artifactPath.c_str());
    bool keep = drawWavePanel(app, app.waves[i]);
    ImGui::PopID();
    if (keep)
      i++;
    else
      app.waves.erase(app.waves.begin() + i);
  }

  if (!app.playError.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "playback: %s",
                       app.playError.c_str());
  }
  if (app.player.playing()) {
    ImGui::Spacing();
    ImGui::TextDisabled("playing %s", app.player.currentPath().c_str());
  }
  ImGui::End();
}

int usage() {
  std::fprintf(stderr,
               "synth-dev - SynthGraph artifact browser/player\n\n"
               "Usage: synth-dev [PROJECT_DIR]   (defaults to '.')\n"
               "       synth-dev --self-test [PROJECT_DIR]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  bool selfTest = false;
  std::string projectDir = ".";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--self-test") selfTest = true;
    else if (a == "--help" || a[0] == '-') return usage();
    else projectDir = a;
  }

  if (selfTest) SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow(
      "SynthGraph", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 900, 600,
      SDL_WINDOW_RESIZABLE | (selfTest ? SDL_WINDOW_HIDDEN : 0));
  SDL_Renderer* renderer = SDL_CreateRenderer(
      window, -1,
      selfTest ? SDL_RENDERER_SOFTWARE
               : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer)
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini litter
  ImGui::StyleColorsDark();
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);

  AppState app;
  app.projectDir = projectDir;
  app.resolveLayout();
  app.manifestStamp = stampFile(app.manifestPath);

  // Pace the loop to the display's refresh rate: with vsync,
  // SDL_RenderPresent blocks until vblank and no sleep is needed; on the
  // software fallback (no vsync) we sleep out the remainder of each
  // frame ourselves.
  SDL_RendererInfo rinfo{};
  bool vsync = SDL_GetRendererInfo(renderer, &rinfo) == 0 &&
               (rinfo.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
  double refreshHz = 60.0;
  int displayIndex = SDL_GetWindowDisplayIndex(window);
  SDL_DisplayMode mode{};
  if (displayIndex >= 0 &&
      SDL_GetCurrentDisplayMode(displayIndex, &mode) == 0 &&
      mode.refresh_rate > 0)
    refreshHz = (double)mode.refresh_rate;
  double targetFrameMs = 1000.0 / refreshHz;

  bool done = false;
  int frames = 0;
  Uint64 last = SDL_GetPerformanceCounter();
  while (!done) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL2_ProcessEvent(&e);
      if (e.type == SDL_QUIT) done = true;
    }
    Uint64 now = SDL_GetPerformanceCounter();
    double dtMs =
        (double)(now - last) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    last = now;

    app.maybeRefresh(dtMs);
    app.player.update();

    // Self-test also exercises the waveform panes: open every ok audio
    // target once the metadata has loaded and draw a few frames.
    if (selfTest && frames == 1 && app.waves.empty()) {
      for (auto& u : app.units) {
        if (!u.loaded.ok) continue;
        for (auto& t : u.loaded.meta.targets) {
          if (t.kind == "visual" || t.status != "ok" || t.artifact.empty())
            continue;
          app.toggleWave((fs::path(app.rootDir) / t.artifact).string(),
                         t.name);
        }
      }
    }

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    drawFrame(app);
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 18, 18, 22, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);

    if (selfTest && ++frames >= 5) done = true;
    if (!selfTest && !vsync) {
      double frameMs = (double)(SDL_GetPerformanceCounter() - now) * 1000.0 /
                       (double)SDL_GetPerformanceFrequency();
      if (frameMs < targetFrameMs)
        SDL_Delay((Uint32)(targetFrameMs - frameMs));
    }
  }

  if (selfTest) {
    size_t loadedCount = 0, targetCount = 0, diagCount = 0;
    for (auto& u : app.units) {
      if (u.loaded.ok) loadedCount++;
      targetCount += u.loaded.meta.targets.size();
      diagCount += u.loaded.meta.diagnostics.size();
    }
    std::printf(
        "self-test: metadata %zu/%zu loaded, %zu target(s), "
        "%zu diagnostic(s)\n",
        loadedCount, app.units.size(), targetCount, diagCount);
    for (auto& w : app.waves) {
      if (w.error.empty())
        std::printf("self-test: waveform '%s' %lld frame(s), %zu channel(s)\n",
                    w.targetName.c_str(), (long long)w.wav.frames(),
                    w.wav.channels.size());
      else
        std::printf("self-test: waveform '%s' failed: %s\n",
                    w.targetName.c_str(), w.error.c_str());
    }
  }

  app.player.stop();
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
