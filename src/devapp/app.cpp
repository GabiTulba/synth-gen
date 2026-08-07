// synth-dev — the SynthGraph dev app (design doc §9, §10).
//
// A pure consumer of build outputs: it reads the project's metadata.json
// from <root>/_build/<project>/ (where <root> is the enclosing project
// root, or the project dir itself when standalone), lists render targets
// with their basic facts, plays artifacts, and live-refreshes when the
// metadata changes (i.e. whenever the daemon or a one-shot build rewrites
// it). Beyond root resolution it never talks to compiler internals.

#include <SDL.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "library.hpp"
#include "metadata.hpp"
#include "player.hpp"

namespace fs = std::filesystem;
using namespace synth::devapp;

namespace {

struct AppState {
  std::string projectDir;
  std::string rootDir;  // enclosing root; artifact paths are relative to it
  std::string metadataPath;
  MetadataLoadResult loaded;
  FileStamp stamp;
  double sinceStatMs = 1e9;  // force an immediate first load
  AudioPlayer player;
  std::string playError;

  void maybeRefresh(double dtMs) {
    sinceStatMs += dtMs;
    if (sinceStatMs < 250.0) return;  // stat ~4x/second, reload on change
    sinceStatMs = 0;
    FileStamp now = stampFile(metadataPath);
    if (now == stamp && loaded.ok) return;
    stamp = now;
    loaded = loadProjectMetadata(metadataPath);
  }
};

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

void drawTargets(AppState& app) {
  const ProjectMeta& meta = app.loaded.meta;
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

void drawFrame(AppState& app) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("synthgraph", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

  if (!app.loaded.ok) {
    ImGui::TextWrapped("no build metadata at %s", app.metadataPath.c_str());
    ImGui::TextDisabled("(%s)", app.loaded.error.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped(
        "run `synthc build %s` or leave `synthc watch %s` running; this "
        "window refreshes on its own.",
        app.projectDir.c_str(), app.projectDir.c_str());
    ImGui::End();
    return;
  }

  const ProjectMeta& meta = app.loaded.meta;
  ImGui::Text("project: %s", meta.project.c_str());
  ImGui::SameLine();
  if (meta.status == "ok")
    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "[build ok]");
  else
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "[build failed]");
  ImGui::SameLine();
  ImGui::TextDisabled("metadata: %s", app.metadataPath.c_str());
  ImGui::Separator();

  drawDiagnostics(meta);
  drawTargets(app);

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
  // Outputs live under the enclosing root's _build/, mirroring the source
  // tree; a project with no enclosing root is its own root.
  std::string root = synth::findEnclosingRoot(projectDir);
  fs::path base = root.empty() ? fs::path(projectDir) : fs::path(root);
  fs::path rel;
  if (!root.empty()) {
    std::error_code ec;
    rel = fs::relative(fs::absolute(projectDir), base, ec).lexically_normal();
    if (ec || rel == ".") rel = "";
  }
  app.rootDir = base.string();
  app.metadataPath =
      (base / "_build" / rel / "metadata.json").lexically_normal().string();

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
    if (!selfTest) SDL_Delay(10);
  }

  if (selfTest) {
    std::printf("self-test: metadata %s, %zu target(s), %zu diagnostic(s)\n",
                app.loaded.ok ? "loaded" : "absent",
                app.loaded.meta.targets.size(),
                app.loaded.meta.diagnostics.size());
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
