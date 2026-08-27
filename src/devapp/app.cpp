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
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "metadata.hpp"
#include "player.hpp"
#include "projectstate.hpp"
#include "wav.hpp"
#include "waveform.hpp"

namespace fs = std::filesystem;
using namespace synth::devapp;

namespace {

// UI scale factor (--scale). ImGui's style/font scaling covers most of the
// layout; this covers the few sizes the app picks in raw pixels (knob
// diameter, slider width, wave-window defaults).
float gUiScale = 1.0f;

// The app's editing state for one live control: `value` is what the UI
// shows, `editing` is true while the user is actively changing it, and
// `dirty` is true from the first edit until the daemon's rebuild echoes
// the value back through the metadata (shown as a pending marker).
struct ControlUi {
  float value = 0;
  bool editing = false;
  bool dirty = false;
};

struct UnitState {
  MetadataUnit unit;
  MetadataLoadResult loaded;
  FileStamp stamp;
  std::map<std::string, ControlUi> controlUi;  // by control name
  // What the project's project.json records for this unit. It seeds the
  // UI the first time a control is seen, so a knob keeps its position
  // across restarts whether or not a build ever picked the value up.
  std::map<std::string, double> savedControls;
  std::string controlsError;  // last overrides-write failure, if any
  double lastControlWriteSec = 0;  // throttles mid-drag override writes
};

// How a unit is named in the project state file: the rule path under a
// root, and a fixed stand-in for the single-unit case (whose label is
// empty).
std::string unitKey(const UnitState& u) {
  return u.unit.label.empty() ? std::string(".") : u.unit.label;
}

// The control values worth recording: everything that differs from its
// declaration's default. The same set goes to the project state file and
// to the build's controls.json, so the two can be compared directly.
std::map<std::string, double> unitOverrides(const UnitState& u) {
  std::map<std::string, double> overrides;
  for (auto& c : u.loaded.meta.controls) {
    auto it = u.controlUi.find(c.name);
    if (it == u.controlUi.end()) continue;
    // The UI value is a float; record the shortest decimal that reads
    // back as that same float, so the file says 0.42 and not
    // 0.41999998688697815.
    if (std::fabs(it->second.value - c.def) > 1e-6) {
      char buf[40];
      double v = it->second.value;
      for (int prec = 3; prec < 9; prec++) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (std::strtof(buf, nullptr) == it->second.value) {
          v = std::strtod(buf, nullptr);
          break;
        }
      }
      overrides[c.name] = v;
    }
  }
  return overrides;
}

// One artifact's waveform: the decoded WAV, per-channel peak bins, and
// the view state (zoom, selection, loop) laid over them. Drawn inside
// whichever panel names its target - there is one of these per artifact,
// not per panel - and reloaded automatically when a rebuild rewrites the
// file.
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
  bool loop = false;   // replay the played range indefinitely

  void open(const std::string& path, const std::string& name) {
    artifactPath = path;
    targetName = name;
    stamp = stampFile(path);
    load();
  }

  // Reopens a panel from saved UI state: the artifact is loaded as
  // usual, then the saved zoom window and selection are put back,
  // clamped to whatever length the file has now. A range that no longer
  // makes sense (the artifact was re-rendered shorter, say) falls back
  // to the fitted view with no selection.
  void restore(const std::string& path, const WavePanelState& st) {
    open(path, st.target);
    loop = st.loop;
    if (!error.empty() || wav.frames() == 0) return;
    if (st.viewEnd > st.viewStart) {
      view.start = st.viewStart;
      view.end = st.viewEnd;
      view.clamp();
    }
    if (st.selStart >= 0 && st.selEnd > st.selStart) {
      selStart = std::min(st.selStart, (double)wav.frames());
      selEnd = std::min(st.selEnd, (double)wav.frames());
      if (selEnd - selStart < 1) selStart = selEnd = -1;
    }
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
  // current zoom window and selection clamped to the new length. `force`
  // reloads even when the stamp looks unchanged - a rebuild that rewrites
  // a same-sized artifact within the filesystem's mtime granularity is
  // invisible to the stamp, so metadata changes force the panels fresh.
  void reloadIfChanged(bool force) {
    if (artifactPath.empty()) return;
    FileStamp now = stampFile(artifactPath);
    if (!force && now == stamp) return;
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
  // Decoded audio and view state per artifact. Waveforms live inside
  // panels now, so there is exactly one view per artifact however many
  // panels name it, and it is dropped when no open panel is showing it -
  // a stem of a full-length song costs tens of megabytes decoded.
  std::map<std::string, WavePanel> waves;

  // The project's settings file (projectstate.hpp). `state` is the last
  // snapshot written to disk; the live state lives in the members it
  // mirrors (the wave panels, `sections`, `windowGeom`, each unit's
  // controlUi) plus ImGui's own settings, which we only re-serialize when
  // ImGui says they changed. Saving compares a freshly captured snapshot
  // against `state`, so nothing is written while the user is only looking
  // at the window.
  std::string statePath;
  bool persist = true;  // writes are off under --self-test; reads are not
  ProjectState state;
  std::map<std::string, bool> sections;  // collapsing headers we own
  // Which panel windows are open, by "<unit>/<panel>", and whether each
  // unit's flat lists hide what a panel already covers. Where a panel
  // window sits is ImGui's business, carried in its ini blob; only
  // whether it is on screen at all is ours to remember.
  std::map<std::string, bool> panelsOpen;
  WindowGeometry windowGeom;
  bool imguiIniDirty = false;
  double sinceSaveMs = 0;
  std::string stateError;  // last save failure, if any

  std::string panelKey(const UnitState& u, const std::string& panel) {
    return unitKey(u) + "/" + panel;
  }

  // A panel a project bothered to declare is worth showing, so one the
  // user has never touched opens by default; closing it persists.
  bool& panelOpen(const UnitState& u, const std::string& panel) {
    return panelsOpen.try_emplace(panelKey(u, panel), true).first->second;
  }

  std::vector<PanelMeta> panelsFor(const UnitState& u) {
    return resolvePanels(u.loaded.meta);
  }

  // The view for one artifact, decoded on first sight. Panels ask for it
  // by path, so two panels naming the same target share one view.
  WavePanel& wave(const std::string& path, const std::string& target) {
    auto [it, fresh] = waves.try_emplace(path);
    if (fresh) {
      auto saved = savedWaves.find(path);
      if (saved != savedWaves.end())
        it->second.restore(path, saved->second);
      else
        it->second.open(path, target);
    }
    return it->second;
  }

  void resolveLayout() {
    MetadataLayout layout = resolveMetadataLayout(projectDir);
    rootDir = layout.rootDir;
    manifestPath = layout.manifestPath;
    statePath = layout.projectStatePath;
    std::vector<UnitState> next;
    for (auto& u : layout.units) {
      UnitState s;
      bool seen = false;
      for (auto& prev : units)
        if (prev.unit.metadataPath == u.metadataPath) {
          s = std::move(prev);
          seen = true;
        }
      s.unit = u;
      // A unit that just appeared (first resolve, or a rule added to the
      // root while running) takes its control values from the project
      // file; `state` is empty on the very first pass, and loadState
      // fills these in once it has been read.
      if (!seen) {
        auto it = state.controls.find(unitKey(s));
        if (it != state.controls.end()) s.savedControls = it->second;
      }
      next.push_back(std::move(s));
    }
    units = std::move(next);
  }

  // View state (zoom, selection, loop) a previous run recorded, by
  // artifact path. Artifacts are stored relative to the layout root, so
  // the state survives the tree moving. Seeded into a view the first
  // time a panel asks for that artifact; an entry whose artifact is gone
  // simply never gets used.
  std::map<std::string, WavePanelState> savedWaves;

  void adoptSavedWaves(const std::vector<WavePanelState>& saved) {
    for (const WavePanelState& st : saved)
      savedWaves[(fs::path(rootDir) / st.artifact).string()] = st;
  }

  // Reads the project's settings and adopts them. Called once, after the
  // first layout resolve. Reading always happens - `persist` only gates
  // the writes - so even the self-test reports the values the real app
  // would show.
  void loadState() {
    if (statePath.empty()) return;
    ProjectStateLoad r = loadProjectState(statePath);
    state = std::move(r.state);
    sections = state.ui.sections;
    panelsOpen = state.ui.panels;
    adoptSavedWaves(state.ui.waves);
    // Carry the saved placement forward even when this run never touches
    // it (--fullscreen, or a window nobody moves): capturing the state
    // would otherwise write the geometry back out empty.
    windowGeom = state.ui.window;
    adoptSavedControls(r.found);
  }

  // Pushes the project's recorded control values into the units, and
  // makes the build agree: controls.json is what a `synthc watch` daemon
  // actually reads, so a value the project remembers has to be written
  // there too or it would never take effect. Only a real difference is
  // written - launching the app must not kick off a rebuild on its own.
  //
  // `found` false means this project has no settings file yet (or an
  // unreadable one). Then the direction reverses: whatever controls.json
  // already holds is the truth, and the first save records it. Adopting
  // an empty state instead would silently wipe the user's overrides.
  void adoptSavedControls(bool found) {
    for (UnitState& u : units) {
      std::string controlsPath = controlsPathFor(u.unit.metadataPath);
      std::map<std::string, double> onDisk = readControlOverrides(controlsPath);
      if (!found) {
        u.savedControls = std::move(onDisk);
        continue;
      }
      auto it = state.controls.find(unitKey(u));
      u.savedControls =
          it == state.controls.end() ? std::map<std::string, double>{}
                                     : it->second;
      if (onDisk == u.savedControls) continue;
      if (!persist) continue;  // the self-test never writes into a build
      std::string err;
      if (!writeControlOverrides(controlsPath, u.savedControls, err))
        u.controlsError = err;
    }
  }

  // The project as it stands right now, in the form that gets written
  // out. Control values come straight from the live UI, so a knob the
  // user just moved is recorded even before a build has echoed it back.
  ProjectState capture() const {
    ProjectState s;
    for (const UnitState& u : units) {
      std::map<std::string, double> overrides = unitOverrides(u);
      // A unit whose metadata has not loaded yet declares no controls, so
      // capturing it would read as "no overrides" and drop what the file
      // already records. Keep the saved values until we know better.
      if (!u.loaded.ok) overrides = u.savedControls;
      if (!overrides.empty()) s.controls[unitKey(u)] = std::move(overrides);
    }
    s.ui.window = windowGeom;
    s.ui.imguiIni = state.ui.imguiIni;  // refreshed by save() when dirty
    s.ui.sections = sections;
    s.ui.panels = panelsOpen;
    // Every view currently decoded, plus the ones recorded earlier whose
    // panel is closed right now: closing a panel must not throw away the
    // zoom you set inside it.
    std::map<std::string, WavePanelState> keep = savedWaves;
    for (const auto& [path, w] : waves) {
      WavePanelState p;
      std::error_code ec;
      fs::path rel = fs::relative(w.artifactPath, rootDir, ec);
      p.artifact = (ec || rel.empty()) ? w.artifactPath : rel.string();
      p.target = w.targetName;
      p.viewStart = w.view.start;
      p.viewEnd = w.view.end;
      p.selStart = w.selStart;
      p.selEnd = w.selEnd;
      p.loop = w.loop;
      keep[path] = std::move(p);
    }
    for (auto& [path, p] : keep) s.ui.waves.push_back(p);
    return s;
  }

  // Writes the state, but only when it actually changed. Called on a
  // timer rather than per edit: dragging a window, a zoom or a knob would
  // otherwise rewrite the file every frame.
  void save() {
    if (!persist || statePath.empty()) return;
    if (imguiIniDirty) {
      size_t n = 0;
      const char* ini = ImGui::SaveIniSettingsToMemory(&n);
      state.ui.imguiIni.assign(ini, n);
      imguiIniDirty = false;
    }
    ProjectState cur = capture();
    if (cur == state) return;
    state = std::move(cur);
    stateError.clear();
    std::string err;
    if (!saveProjectState(statePath, state, err)) stateError = err;
  }

  void maybeSave(double dtMs) {
    sinceSaveMs += dtMs;
    if (sinceSaveMs < 750.0) return;
    sinceSaveMs = 0;
    save();
  }

  // Pulls the build's control values into the UI. A control the user is
  // editing (or whose write the daemon hasn't rebuilt with yet) keeps its
  // UI value; `dirty` clears once the metadata echoes the value back.
  void syncControls(UnitState& u) {
    std::map<std::string, ControlUi> next;
    for (auto& c : u.loaded.meta.controls) {
      auto it = u.controlUi.find(c.name);
      if (it == u.controlUi.end()) {
        // First sight of this control. The build's value is the truth,
        // except when the project remembers one no build has picked up
        // yet (set with no `synthc watch` attached, then restarted):
        // show that one, still marked pending.
        ControlUi fresh;
        fresh.value = (float)c.value;
        auto saved = u.savedControls.find(c.name);
        double seedTol = std::max(1e-9, 1e-4 * (c.max - c.min));
        if (saved != u.savedControls.end() &&
            std::fabs(saved->second - c.value) > seedTol) {
          fresh.value = (float)saved->second;
          fresh.dirty = true;
        }
        next[c.name] = fresh;
        continue;
      }
      ControlUi ui = it->second;
      // Range-relative tolerance: the echo went through float -> JSON
      // (~6 significant digits) -> double, so exact equality would leave
      // the pending marker stuck on wide-range controls.
      double tol = std::max(1e-9, 1e-4 * (c.max - c.min));
      if (ui.dirty && std::fabs(ui.value - c.value) <= tol) ui.dirty = false;
      if (!ui.dirty && !ui.editing) ui.value = (float)c.value;
      next[c.name] = ui;
    }
    u.controlUi = std::move(next);
  }

  void maybeRefresh(double dtMs) {
    sinceStatMs += dtMs;
    if (sinceStatMs < 100.0) return;  // stat ~10x/second, reload on change
    sinceStatMs = 0;
    // Adding or removing root build rules changes which metadata files
    // this app should be watching.
    FileStamp m = stampFile(manifestPath);
    if (!(m == manifestStamp)) {
      manifestStamp = m;
      resolveLayout();
    }
    bool rebuilt = false;
    for (auto& u : units) {
      FileStamp now = stampFile(u.unit.metadataPath);
      bool changed = !(now == u.stamp);
      if (!changed && u.loaded.ok) continue;
      u.stamp = now;
      u.loaded = loadProjectMetadata(u.unit.metadataPath);
      syncControls(u);
      if (changed) rebuilt = true;
    }
    // Metadata is written after the artifacts, so a metadata change means
    // a build just finished: force every open panel fresh - an artifact
    // rewritten with the same size within the mtime granularity would
    // otherwise be missed and leave the panel showing stale audio.
    // A knob you turned in a panel is exactly what triggered the
    // rebuild, so the waveform under it must show the new sound.
    for (auto& [path, w] : waves) w.reloadIfChanged(rebuilt);
    // Same for a looping playback: re-read the playing artifact so the
    // loop picks up the rebuilt audio instead of replaying its stale
    // in-memory copy forever.
    if (rebuilt) player.reloadIfLooping();
  }
};

std::string formatSeconds(double s) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3fs", s);
  return buf;
}

// A section header whose open/closed state outlives the process. ImGui's
// ini carries window geometry but deliberately not tree state, so the few
// headers the app owns are tracked here and saved with the rest of the UI
// state. `key` must be stable across rebuilds; `defOpen` only applies the
// first time this run sees the header.
bool persistentHeader(AppState& app, const std::string& key,
                      const std::string& label, bool defOpen) {
  auto [it, inserted] = app.sections.try_emplace(key, defOpen);
  if (inserted) it->second = defOpen;
  ImGui::SetNextItemOpen(it->second, ImGuiCond_Once);
  bool open = ImGui::CollapsingHeader(label.c_str());
  it->second = open;
  return open;
}

// A unit's key prefix for `sections`: the rule label under a root, and a
// fixed stand-in for the single-unit case (where labels are empty).
std::string sectionKey(const UnitState& u, const char* section) {
  return (u.unit.label.empty() ? std::string(".") : u.unit.label) + "/" +
         section;
}

// The unit's panel switches: one checkbox per declared panel, plus the
// toggle that hides panel-covered controls and targets from the flat
// lists below. This row is the whole point of panels - it is what lets
// you show the two you are working on and put the rest away.
void drawPanelBar(AppState& app, UnitState& u) {
  const std::vector<PanelMeta>& panels = u.loaded.meta.panels;
  if (panels.empty()) return;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("panels:");
  for (const PanelMeta& p : panels) {
    ImGui::SameLine();
    bool open = app.panelOpen(u, p.name);
    if (ImGui::Checkbox(p.name.c_str(), &open))
      app.panelOpen(u, p.name) = open;
  }
  ImGui::Separator();
}

void drawDiagnostics(AppState& app, const UnitState& u) {
  const ProjectMeta& meta = u.loaded.meta;
  if (meta.diagnostics.empty()) return;
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
  bool open = persistentHeader(
      app, sectionKey(u, "diags"),
      "diagnostics (" + std::to_string(meta.diagnostics.size()) + ")###diags",
      true);
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


// A rotary knob: drag vertically to change the value (hold Shift for
// fine adjustment). Returns true while the drag is changing the value.
bool knobFloat(const char* id, float* v, float vmin, float vmax,
               float diameter) {
  ImGui::PushID(id);
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("knob", ImVec2(diameter, diameter));
  bool edited = false;
  ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsItemActive() && io.MouseDelta.y != 0.0f) {
    // Full range over ~200 px of travel; 10x finer with Shift.
    float perPixel = (vmax - vmin) / 200.0f;
    if (io.KeyShift) perPixel *= 0.1f;
    *v = std::clamp(*v - io.MouseDelta.y * perPixel, vmin, vmax);
    edited = true;
  }

  // The dial: a 270-degree arc from lower-left to lower-right, with the
  // indicator line at the value's angle.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 center(pos.x + diameter * 0.5f, pos.y + diameter * 0.5f);
  float radius = diameter * 0.5f - 2.0f;
  float t = vmax > vmin ? (*v - vmin) / (vmax - vmin) : 0.0f;
  const float start = 0.75f * 3.14159265f;  // lower-left
  const float sweep = 1.5f * 3.14159265f;   // 270 degrees
  bool hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
  dl->PathArcTo(center, radius, start, start + sweep, 24);
  dl->PathStroke(IM_COL32(70, 70, 88, 255), 0, 2.0f);
  dl->PathArcTo(center, radius, start, start + sweep * t, 24);
  dl->PathStroke(IM_COL32(110, 205, 160, 255), 0, 2.0f);
  float angle = start + sweep * t;
  dl->AddLine(center,
              ImVec2(center.x + std::cos(angle) * radius * 0.75f,
                     center.y + std::sin(angle) * radius * 0.75f),
              hovered ? IM_COL32(235, 235, 245, 255)
                      : IM_COL32(200, 200, 210, 255),
              2.0f);
  ImGui::PopID();
  return edited;
}

// One lane of a multi_slider group. The lane's own range spans the whole
// track, but the group's sum budget usually puts part of that range out
// of reach, so the track is drawn in three bands: what the lane has
// taken, the headroom it can still take, and the stretch the other lanes
// have spoken for. Dragging stops at [lo, hi] - no other lane ever moves
// on its own. Returns true while the drag is changing the value.
bool laneSliderFloat(const char* id, float* v, float vmin, float vmax,
                     float lo, float hi, float width, float height) {
  ImGui::PushID(id);
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("lane", ImVec2(width, height));
  bool edited = false;
  float span = vmax - vmin;
  if (ImGui::IsItemActive() && span > 0) {
    // Absolute positioning, like ImGui's own slider: the value follows
    // the cursor across the track, then clamps to the reachable band.
    float t = (ImGui::GetIO().MousePos.x - pos.x) / width;
    float want = vmin + std::clamp(t, 0.0f, 1.0f) * span;
    float next = std::clamp(want, lo, hi);
    if (next != *v) {
      *v = next;
      edited = true;
    }
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto atX = [&](float value) {
    float t = span > 0 ? (value - vmin) / span : 0.0f;
    return pos.x + std::clamp(t, 0.0f, 1.0f) * width;
  };
  float y0 = pos.y, y1 = pos.y + height;
  // Out of reach (the other lanes' share), then reachable, then taken.
  dl->AddRectFilled(pos, ImVec2(pos.x + width, y1), IM_COL32(34, 34, 44, 255));
  dl->AddRectFilled(ImVec2(atX(lo), y0), ImVec2(atX(hi), y1),
                    IM_COL32(70, 70, 88, 255));
  dl->AddRectFilled(ImVec2(atX(*v), y0), ImVec2(atX(hi), y1),
                    IM_COL32(74, 108, 92, 255));
  dl->AddRectFilled(ImVec2(pos.x, y0), ImVec2(atX(*v), y1),
                    IM_COL32(110, 205, 160, 255));
  // The limits themselves: where the sum budget stops this lane.
  if (hi < vmax - 1e-6f)
    dl->AddLine(ImVec2(atX(hi), y0), ImVec2(atX(hi), y1),
                IM_COL32(235, 200, 110, 230), 2.0f);
  if (lo > vmin + 1e-6f)
    dl->AddLine(ImVec2(atX(lo), y0), ImVec2(atX(lo), y1),
                IM_COL32(235, 200, 110, 230), 2.0f);
  bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  dl->AddLine(ImVec2(atX(*v), y0 - 1), ImVec2(atX(*v), y1 + 1),
              hot ? IM_COL32(245, 245, 250, 255) : IM_COL32(205, 205, 215, 255),
              2.0f);
  dl->AddRect(pos, ImVec2(pos.x + width, y1), IM_COL32(90, 90, 110, 255));
  ImGui::PopID();
  return edited;
}

// How much of the group's budget is spent, drawn as a bar: the filled
// part is the current sum against sum_max, and the tick (when there is
// one) is the sum_min the group must stay above.
void groupBudgetBar(double sum, double sumMin, double sumMax, float width,
                    float height) {
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(width, height));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 end(pos.x + width, pos.y + height);
  dl->AddRectFilled(pos, end, IM_COL32(34, 34, 44, 255));
  double t = sumMax > 0 ? std::clamp(sum / sumMax, 0.0, 1.0) : 0.0;
  bool full = sumMax > 0 && sum >= sumMax - 1e-6;
  dl->AddRectFilled(pos, ImVec2(pos.x + width * (float)t, end.y),
                    full ? IM_COL32(235, 200, 110, 255)
                         : IM_COL32(110, 205, 160, 255));
  if (sumMin > 0 && sumMax > 0) {
    float x = pos.x + width * (float)std::clamp(sumMin / sumMax, 0.0, 1.0);
    dl->AddLine(ImVec2(x, pos.y), ImVec2(x, end.y), IM_COL32(150, 170, 235, 255),
                2.0f);
  }
  dl->AddRect(pos, end, IM_COL32(90, 90, 110, 255));
}

// Writes the unit's override file from the UI values: every control that
// differs from its declared default. Also called with everything back at
// defaults - the (empty) overrides object still reaches the daemon and
// rebuilds. The write is atomic, so a mid-write daemon poll never parses
// a torn file.
void writeUnitOverrides(UnitState& u) {
  std::map<std::string, double> overrides = unitOverrides(u);
  u.controlsError.clear();
  std::string err;
  if (!writeControlOverrides(controlsPathFor(u.unit.metadataPath), overrides,
                             err))
    u.controlsError = err;
}

// Marks a control edited and pushes it to the daemon. Mid-drag writes
// make the sound track the drag; the throttle keeps a fast drag from
// flooding the daemon with rebuilds, and the release write always lands.
void noteControlEdit(UnitState& u, ControlUi& ui, bool released) {
  ui.dirty = true;
  if (released) {
    ui.editing = false;
    writeUnitOverrides(u);
    u.lastControlWriteSec = ImGui::GetTime();
    return;
  }
  ui.editing = true;
  if (ImGui::GetTime() - u.lastControlWriteSec > 0.1) {
    writeUnitOverrides(u);
    u.lastControlWriteSec = ImGui::GetTime();
  }
}

// The pending-rebuild marker and the per-control reset button, shared by
// plain controls and group lanes.
bool drawControlTail(UnitState& u, ControlUi& ui, const ControlMeta& c) {
  bool dirty = ui.dirty;
  if (dirty) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.5f, 1.0f), "*");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("waiting for a rebuild with this value\n"
                        "(leave `synthc watch` running)");
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("reset")) {
    ui.value = (float)c.def;
    ui.editing = false;
    ui.dirty = true;
    writeUnitOverrides(u);
  }
  return dirty;
}

// One multi_slider group: lanes that share a budget for their sum. Each
// lane is drawn against the band the other lanes leave it
// (controlLaneBand), and dragging clamps to that band - so the group
// invariant holds after every drag without any lane moving on its own.
// Returns true if any lane is waiting on a rebuild.
bool drawControlGroup(UnitState& u, const std::vector<ControlMeta>& controls,
                      size_t first, size_t count) {
  const ControlMeta& head = controls[first];
  double sum = 0;
  for (size_t k = 0; k < count; k++)
    sum += u.controlUi[controls[first + k].name].value;

  ImGui::PushID(head.group.c_str());
  float width = std::max(200.0f * gUiScale,
                         ImGui::GetContentRegionAvail().x * 0.45f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "%s", head.group.c_str());
  ImGui::SameLine();
  if (head.sumMin > 0)
    ImGui::TextDisabled("sum %.4g of %.4g (at least %.4g)", sum, head.sumMax,
                        head.sumMin);
  else
    ImGui::TextDisabled("sum %.4g of %.4g", sum, head.sumMax);
  groupBudgetBar(sum, head.sumMin, head.sumMax, width, 6.0f * gUiScale);

  bool anyDirty = false;
  for (size_t k = 0; k < count; k++) {
    const ControlMeta& c = controls[first + k];
    ControlUi& ui = u.controlUi[c.name];
    ControlBand band = controlLaneBand(c, sum - ui.value);
    float lo = (float)band.lo, hi = (float)band.hi;
    ImGui::PushID((int)k);
    bool edited = laneSliderFloat("##lane", &ui.value, (float)c.min,
                                  (float)c.max, lo, hi, width,
                                  16.0f * gUiScale);
    bool released = ImGui::IsItemDeactivated() && ui.editing;
    if (edited || released) {
      sum = 0;
      for (size_t j = 0; j < count; j++)
        sum += u.controlUi[controls[first + j].name].value;
      noteControlEdit(u, ui, released);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    // The lane's own name; the group already named itself above.
    const char* lane = c.name.c_str() + head.group.size() + 1;
    ImGui::Text("%s  %.4g", lane, (double)ui.value);
    if (hi < c.max - 1e-6) {
      ImGui::SameLine();
      ImGui::TextDisabled("(<= %.4g)", (double)hi);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("the other lanes have spoken for the rest of the "
                          "budget;\nlower one of them to take more here");
    }
    anyDirty |= drawControlTail(u, ui, c);
    ImGui::PopID();
  }

  if (ImGui::SmallButton("group defaults")) {
    for (size_t k = 0; k < count; k++) {
      ControlUi& ui = u.controlUi[controls[first + k].name];
      ui.value = (float)controls[first + k].def;
      ui.editing = false;
      ui.dirty = true;
    }
    writeUnitOverrides(u);
  }
  ImGui::PopID();
  return anyDirty;
}

// The live controls of one unit: a slider or knob per Core.Control
// declaration, and one linked block per multi_slider group. Edits write
// the unit's controls.json live while dragging (throttled to ~10
// writes/second) and once more on release; an attached `synthc watch`
// picks each write up and rebuilds, and the pending marker clears once
// the new metadata echoes the final value back.
// One ungrouped control: a knob or a slider, its name, and the pending
// marker. Split out so a panel can draw one control at a time.
// for a member it names.
bool drawOneControl(UnitState& u, const ControlMeta& c) {
  {
    ControlUi& ui = u.controlUi[c.name];
    ImGui::PushID(c.name.c_str());
    bool edited = false, released = false;
    if (c.kind == "knob") {
      edited = knobFloat("##knob", &ui.value, (float)c.min, (float)c.max,
                         36.0f * gUiScale);
      released = ImGui::IsItemDeactivated() && ui.editing;
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::Text("%s  %.4g", c.name.c_str(), (double)ui.value);
    } else {
      ImGui::SetNextItemWidth(
          std::max(160.0f * gUiScale,
                   ImGui::GetContentRegionAvail().x * 0.45f));
      edited = ImGui::SliderFloat("##slider", &ui.value, (float)c.min,
                                  (float)c.max, "%.4g");
      released = ImGui::IsItemDeactivatedAfterEdit();
      ImGui::SameLine();
      ImGui::TextUnformatted(c.name.c_str());
    }
    if (edited || released) noteControlEdit(u, ui, released);
    bool dirty = drawControlTail(u, ui, c);
    ImGui::PopID();
    return dirty;
  }
}

// The lanes of one multi_slider group, found by name. Lanes arrive
// consecutively in declaration order, so the group is the run of
// controls sharing this name; they draw together because each lane's
// limits depend on where the others sit. Returns false when no such
// group exists.
bool drawGroupByName(UnitState& u, const std::vector<ControlMeta>& controls,
                     const std::string& group, bool& anyDirty) {
  for (size_t i = 0; i < controls.size(); i++) {
    if (controls[i].group != group) continue;
    size_t n = 1;
    while (i + n < controls.size() && controls[i + n].group == group) n++;
    anyDirty |= drawControlGroup(u, controls, i, n);
    return true;
  }
  return false;
}

// The unit's whole control list, groups drawn linked. `skip`, when set,
// names the members some panel already covers, which the "grouped only"
// toggle hides from here.
void drawControlList(UnitState& u, const std::vector<ControlMeta>& controls,
                     const std::set<std::string>* skip, bool& anyDirty) {
  for (size_t i = 0; i < controls.size();) {
    if (!controls[i].group.empty()) {
      const std::string& group = controls[i].group;
      size_t n = 1;
      while (i + n < controls.size() && controls[i + n].group == group) n++;
      if (!skip || !skip->count(group))
        anyDirty |= drawControlGroup(u, controls, i, n);
      i += n;
      continue;
    }
    const ControlMeta& c = controls[i++];
    if (skip && skip->count(c.name)) continue;
    anyDirty |= drawOneControl(u, c);
  }
}


// The contents of one waveform view: min/max envelope lanes per channel
// (matching the .svg vis), wheel zoom about the cursor, right-drag pan,
// left-drag selection, and (optionally looped) playback of the selection
// or visible range.
//
// `availY` is the vertical room the canvas may take. A panel showing
// several targets divides its window between them and passes each one's
// share; pass -1 to fill whatever is left, which is what a view that
// owns its window wants.
void drawWaveContent(AppState& app, WavePanel& p, float availY = -1.0f) {
  if (!p.error.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "cannot load %s: %s",
                       p.artifactPath.c_str(), p.error.c_str());
    return;
  }
  if (p.wav.frames() == 0) {
    ImGui::TextDisabled("empty artifact");
    return;
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
                                (int64_t)std::ceil(to), app.playError,
                                p.loop) &&
          !app.playError.empty())
        app.playError = p.targetName + ": " + app.playError;
    }
  }
  ImGui::SameLine();
  // Replays the played range (the selection, usually) until stopped;
  // toggling mid-play applies to the running playback too.
  if (ImGui::Checkbox("loop", &p.loop) && isThisPlaying)
    app.player.setLooping(p.loop);
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
  float laneGap = 4.0f;
  // The canvas fills its share of the window, so resizing the window
  // resizes the lanes.
  if (availY < 0) availY = ImGui::GetContentRegionAvail().y;
  float laneH = std::max(28.0f * gUiScale,
                         (availY - (channels - 1) * laneGap) / channels);
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
}

// A panel's compact view of one target: the whole file's envelope in a
// short strip, a playhead while it is playing, and the few controls that
// matter next to a knob you are turning. Deliberately not the full
// waveform window - no zoom, no selection - because a panel's point is
// to fit several targets and their controls on screen at once. `detail`
// opens the real thing.
//
// Height is passed in rather than taken from the content region: unlike
// drawWaveContent, a strip shares its window with everything above it.
// One target inside a panel: the row of facts the old targets table
// carried - status, duration, rate, channels - and then the waveform
// itself, fully interactive. This is the merge: there is no separate
// waveform window any more, so everything that used to live in one is
// here, next to the controls that shape it.
void drawPanelTarget(AppState& app, UnitState& u, const TargetMeta& t,
                     float waveHeight, std::set<std::string>& wanted) {
  ImGui::PushID(t.name.c_str());
  ImGui::Spacing();
  ImGui::SeparatorText(t.name.c_str());

  if (t.status != "ok") {
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
                       t.error.empty() ? "not built" : t.error.c_str());
    ImGui::PopID();
    return;
  }
  if (t.kind == "visual") {
    // Visual targets are .svg files the app does not render; the build
    // wrote one, and saying where is the useful thing to show.
    ImGui::TextDisabled("waveform svg - %s", t.artifact.c_str());
    ImGui::PopID();
    return;
  }
  if (t.artifact.empty()) {
    ImGui::TextDisabled("no artifact");
    ImGui::PopID();
    return;
  }

  ImGui::TextDisabled("%s | %g Hz | %d ch | %lld frames",
                      formatSeconds(t.durationSeconds).c_str(), t.rate,
                      t.channels, (long long)t.frames);

  // Decoding is the expensive part - a couple of minutes of stereo costs
  // ~90 MB as doubles - so a target scrolled out of the panel is
  // reserved its space and skipped. Panels open by default, and a
  // project with a dozen targets would otherwise decode all of them the
  // moment you launched.
  float blockH = waveHeight > 0
                     ? waveHeight + ImGui::GetFrameHeightWithSpacing() +
                           ImGui::GetTextLineHeightWithSpacing()
                     : ImGui::GetContentRegionAvail().y;
  float blockW = std::max(16.0f, ImGui::GetContentRegionAvail().x);
  if (!ImGui::IsRectVisible(ImVec2(blockW, blockH))) {
    ImGui::Dummy(ImVec2(blockW, blockH));
    ImGui::PopID();
    return;
  }

  std::string path = (fs::path(app.rootDir) / t.artifact).string();
  wanted.insert(path);
  WavePanel& w = app.wave(path, t.name);
  drawWaveContent(app, w, waveHeight);
  ImGui::PopID();
}

// One panel: its controls, then each of its targets with a full
// waveform. The window is ordinary - drag, resize, close - and its
// `###` id is unit-qualified so ImGui's ini remembers where the user put
// it and how big they made it.
//
// `wanted` collects the artifact paths this panel is showing, so the
// caller can drop the decoded audio of panels that are closed.
void drawPanel(AppState& app, UnitState& u, const PanelMeta& panel,
               std::set<std::string>& wanted) {
  bool open = app.panelOpen(u, panel.name);
  if (!open) return;

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  float cascade = 30.0f * gUiScale *
                  (float)(std::hash<std::string>{}(panel.name) % 6);
  // Panels are the whole UI now, so they open big enough to work in: a
  // waveform you can actually read plus room for the controls above it.
  ImGui::SetNextWindowSize(ImVec2(720 * gUiScale, 560 * gUiScale),
                           ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 60 + cascade,
                                 vp->WorkPos.y + 70 + cascade),
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(320 * gUiScale, 160 * gUiScale),
                                      ImVec2(FLT_MAX, FLT_MAX));
  std::string title = panel.name + "###panel " + app.panelKey(u, panel.name);
  if (ImGui::Begin(title.c_str(), &open)) {
    const std::vector<ControlMeta>& controls = u.loaded.meta.controls;

    // Resolve the panel's members once: a control member names either
    // one control or a whole multi_slider group, and a target member
    // names one of the unit's targets.
    std::vector<const TargetMeta*> targets;
    for (const std::string& name : panel.targets)
      for (auto& m : u.loaded.meta.targets)
        if (m.name == name) targets.push_back(&m);

    if (!panel.controls.empty()) {
      bool anyDirty = false;
      for (const std::string& name : panel.controls) {
        const ControlMeta* c = nullptr;
        for (auto& m : controls)
          if (m.name == name && m.group.empty()) c = &m;
        if (c)
          anyDirty |= drawOneControl(u, *c);
        else
          drawGroupByName(u, controls, name, anyDirty);
      }
      if (ImGui::SmallButton("defaults")) {
        for (auto& m : controls) {
          bool mine = false;
          for (const std::string& n : panel.controls)
            if (n == m.name || n == m.group) mine = true;
          if (!mine) continue;
          ControlUi& ui = u.controlUi[m.name];
          ui.value = (float)m.def;
          ui.editing = false;
          ui.dirty = true;
        }
        writeUnitOverrides(u);
      }
      if (anyDirty) {
        ImGui::SameLine();
        ImGui::TextDisabled("* pending rebuild");
      }
      if (!u.controlsError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "controls: %s",
                           u.controlsError.c_str());
    }

    // A lone target fills the window, so a one-target panel uses all the
    // room you gave it. Several get a fixed, readable height each and
    // the panel scrolls - dividing the window between seven stems would
    // leave every one of them too short to read.
    size_t drawable = 0;
    for (const TargetMeta* t : targets)
      if (t->status == "ok" && t->kind != "visual" && !t->artifact.empty())
        drawable++;
    float perTarget = drawable > 1 ? 150.0f * gUiScale : -1.0f;
    for (const TargetMeta* t : targets)
      drawPanelTarget(app, u, *t, perTarget, wanted);
    if (targets.empty() && panel.controls.empty())
      ImGui::TextDisabled("this panel is empty");
  }
  ImGui::End();
  app.panelOpen(u, panel.name) = open;
}

void drawFrame(AppState& app) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  // NoBringToFrontOnFocus/NoNavFocus keep this full-screen backdrop pinned
  // behind everything: clicking it must not bury the floating wave panels.
  ImGui::Begin("synthgraph", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoNavFocus);

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

    drawDiagnostics(app, u);
    drawPanelBar(app, u);
    ImGui::PopID();
  }
  if (anyMissing) {
    ImGui::Spacing();
    ImGui::TextWrapped(
        "run `synthc build %s` or leave `synthc watch %s` running; this "
        "window refreshes on its own.",
        app.projectDir.c_str(), app.projectDir.c_str());
  }

  if (!app.playError.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "playback: %s",
                       app.playError.c_str());
  }
  if (!app.stateError.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "project state: %s",
                       app.stateError.c_str());
  }
  if (app.player.playing()) {
    ImGui::Spacing();
    ImGui::TextDisabled("playing%s %s", app.player.looping() ? " (loop)" : "",
                        app.player.currentPath().c_str());
  }
  ImGui::End();

  // Panels are the UI: every control and every waveform lives in one.
  // `wanted` collects the artifacts the open panels are showing, so the
  // audio behind a panel the user closed can be released.
  std::set<std::string> wanted;
  for (size_t i = 0; i < app.units.size(); i++) {
    UnitState& u = app.units[i];
    if (!u.loaded.ok) continue;
    ImGui::PushID((int)i);
    for (const PanelMeta& panel : app.panelsFor(u))
      drawPanel(app, u, panel, wanted);
    ImGui::PopID();
  }

  // Drop the audio of anything no open panel is showing, remembering its
  // view first so reopening the panel comes back where you left it.
  for (auto it = app.waves.begin(); it != app.waves.end();) {
    if (wanted.count(it->first)) {
      ++it;
      continue;
    }
    const WavePanel& w = it->second;
    WavePanelState st;
    std::error_code ec;
    fs::path rel = fs::relative(w.artifactPath, app.rootDir, ec);
    st.artifact = (ec || rel.empty()) ? w.artifactPath : rel.string();
    st.target = w.targetName;
    st.viewStart = w.view.start;
    st.viewEnd = w.view.end;
    st.selStart = w.selStart;
    st.selEnd = w.selEnd;
    st.loop = w.loop;
    app.savedWaves[it->first] = std::move(st);
    it = app.waves.erase(it);
  }
}

// A saved window position is only reused when it still lands on a
// connected display: unplugging the monitor it was on would otherwise
// reopen the app somewhere no one can see it.
bool positionOnSomeDisplay(int x, int y) {
  for (int i = 0, n = SDL_GetNumVideoDisplays(); i < n; i++) {
    SDL_Rect b{};
    if (SDL_GetDisplayBounds(i, &b) != 0) continue;
    if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return true;
  }
  return false;
}

int usage() {
  std::fprintf(stderr,
               "synth-dev - SynthGraph artifact browser/player\n\n"
               "Usage: synth-dev [OPTIONS] [PROJECT_DIR]   (defaults to '.')\n"
               "       synth-dev --self-test [PROJECT_DIR]\n\n"
               "Options:\n"
               "  --fullscreen  fill the whole display (borderless)\n"
               "  --scale N     scale the UI by N, e.g. 2 on a small/dense "
               "screen\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  bool selfTest = false;
  bool fullscreen = false;
  std::string projectDir = ".";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--self-test") selfTest = true;
    else if (a == "--fullscreen") fullscreen = true;
    else if (a == "--scale" && i + 1 < argc) {
      gUiScale = std::strtof(argv[++i], nullptr);
      if (!(gUiScale >= 0.5f && gUiScale <= 8.0f)) return usage();
    }
    else if (a == "--help" || a[0] == '-') return usage();
    else projectDir = a;
  }

  if (selfTest) SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  AppState app;
  app.projectDir = projectDir;
  app.resolveLayout();
  app.manifestStamp = stampFile(app.manifestPath);

  // The settings have to be read before the window is created - they
  // carry the window's own placement. The self-test runs headless against
  // a dummy driver and auto-opens every panel, so it neither reads nor
  // writes the file: it must not leave a layout behind for the next real
  // run to restore, nor touch the project's control values.
  app.persist = !selfTest;
  app.loadState();

  Uint32 windowFlags = SDL_WINDOW_RESIZABLE | (selfTest ? SDL_WINDOW_HIDDEN : 0);
  int winX = SDL_WINDOWPOS_CENTERED, winY = SDL_WINDOWPOS_CENTERED;
  int winW = 900, winH = 600;
  // A saved size always applies; a saved position only when it is still
  // on-screen. --fullscreen wins over both.
  if (app.windowGeom.valid && !fullscreen && !selfTest) {
    winW = app.windowGeom.w;
    winH = app.windowGeom.h;
    if (positionOnSomeDisplay(app.windowGeom.x, app.windowGeom.y)) {
      winX = app.windowGeom.x;
      winY = app.windowGeom.y;
    }
  }
  if (fullscreen && !selfTest) {
    // SDL's fullscreen-desktop mode is an EWMH request that only a window
    // manager honors, and window-manager-less X servers (termux-x11) drop
    // it on the floor. A borderless window covering the display's bounds
    // is fullscreen with no one's cooperation, so prefer that and keep the
    // flag only as a fallback when the bounds can't be queried.
    SDL_Rect bounds{};
    if (SDL_GetDisplayBounds(0, &bounds) == 0) {
      winX = bounds.x;
      winY = bounds.y;
      winW = bounds.w;
      winH = bounds.h;
      windowFlags = SDL_WINDOW_BORDERLESS | (selfTest ? SDL_WINDOW_HIDDEN : 0);
    } else {
      windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
  }
  SDL_Window* window =
      SDL_CreateWindow("SynthGraph", winX, winY, winW, winH, windowFlags);
  SDL_Renderer* renderer = SDL_CreateRenderer(
      window, -1,
      selfTest ? SDL_RENDERER_SOFTWARE
               : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer)
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);

  if (app.persist && !fullscreen) {
    // Where the window actually landed - the window manager gets the
    // final say, and no move/resize event is guaranteed at creation.
    WindowGeometry g;
    SDL_GetWindowPosition(window, &g.x, &g.y);
    SDL_GetWindowSize(window, &g.w, &g.h);
    g.valid = g.w > 0 && g.h > 0;
    if (g.valid) app.windowGeom = g;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  // ImGui's own ini is disabled: its settings ride along in the project's
  // UI state file instead of an imgui.ini in whatever directory the app
  // happened to be started from. WantSaveIniSettings tells us when to
  // re-serialize them.
  ImGui::GetIO().IniFilename = nullptr;
  if (!app.state.ui.imguiIni.empty())
    ImGui::LoadIniSettingsFromMemory(app.state.ui.imguiIni.c_str(),
                                     app.state.ui.imguiIni.size());
  ImGui::StyleColorsDark();
  if (gUiScale != 1.0f) {
    ImGui::GetStyle().ScaleAllSizes(gUiScale);
    ImGui::GetIO().FontGlobalScale = gUiScale;
  }
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);


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
      // Only sample the OS window's geometry when it actually changed:
      // on X11 these are round trips, too costly to poll every frame.
      if (app.persist && !fullscreen && e.type == SDL_WINDOWEVENT &&
          (e.window.event == SDL_WINDOWEVENT_MOVED ||
           e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
        WindowGeometry g;
        SDL_GetWindowPosition(window, &g.x, &g.y);
        SDL_GetWindowSize(window, &g.w, &g.h);
        g.valid = g.w > 0 && g.h > 0;
        app.windowGeom = g;
      }
    }
    Uint64 now = SDL_GetPerformanceCounter();
    double dtMs =
        (double)(now - last) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    last = now;

    app.maybeRefresh(dtMs);
    app.player.update();

    // Self-test also exercises waveform decoding: load every ok audio
    // target once the metadata has loaded and draw a few frames. These
    // views are not tied to an open panel, so drawFrame's collector
    // would release them - the self-test reads them before that matters.
    if (selfTest && frames == 1 && app.waves.empty()) {
      for (auto& u : app.units) {
        if (!u.loaded.ok) continue;
        for (auto& t : u.loaded.meta.targets) {
          if (t.kind == "visual" || t.status != "ok" || t.artifact.empty())
            continue;
          app.wave((fs::path(app.rootDir) / t.artifact).string(), t.name);
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

    // ImGui batches its own settings changes behind this flag; everything
    // else the app persists is compared by value inside save().
    if (ImGui::GetIO().WantSaveIniSettings) {
      ImGui::GetIO().WantSaveIniSettings = false;
      app.imguiIniDirty = true;
    }
    app.maybeSave(dtMs);

    if (selfTest && ++frames >= 5) done = true;
    if (!selfTest && !vsync) {
      double frameMs = (double)(SDL_GetPerformanceCounter() - now) * 1000.0 /
                       (double)SDL_GetPerformanceFrequency();
      if (frameMs < targetFrameMs)
        SDL_Delay((Uint32)(targetFrameMs - frameMs));
    }
  }

  if (selfTest) {
    size_t loadedCount = 0, targetCount = 0, diagCount = 0, controlCount = 0;
    for (auto& u : app.units) {
      if (u.loaded.ok) loadedCount++;
      targetCount += u.loaded.meta.targets.size();
      diagCount += u.loaded.meta.diagnostics.size();
      controlCount += u.loaded.meta.controls.size();
    }
    std::printf(
        "self-test: metadata %zu/%zu loaded, %zu target(s), "
        "%zu diagnostic(s), %zu control(s)\n",
        loadedCount, app.units.size(), targetCount, diagCount, controlCount);
    for (auto& u : app.units)
      for (auto& c : u.loaded.meta.controls) {
        auto it = u.controlUi.find(c.name);
        bool pending = it != u.controlUi.end() && it->second.dirty;
        double shown =
            it == u.controlUi.end() ? c.value : (double)it->second.value;
        std::printf("self-test: control '%s' = %.6g%s\n", c.name.c_str(),
                    shown, pending ? " (pending rebuild)" : "");
      }
    // Panels are reported but never drawn: a headless smoke test must
    // stay deterministic.
    // What the app would actually show, so a project that declares no
    // panels reports the one holding everything rather than nothing.
    for (auto& u : app.units)
      for (auto& p : app.panelsFor(u))
        std::printf("self-test: panel '%s' (%zu control(s), %zu target(s))\n",
                    p.name.c_str(), p.controls.size(), p.targets.size());
    for (auto& [path, w] : app.waves) {
      if (w.error.empty())
        std::printf("self-test: waveform '%s' %lld frame(s), %zu channel(s)\n",
                    w.targetName.c_str(), (long long)w.wav.frames(),
                    w.wav.channels.size());
      else
        std::printf("self-test: waveform '%s' failed: %s\n",
                    w.targetName.c_str(), w.error.c_str());
    }
  }

  // Last chance to catch edits made since the previous tick; the ImGui
  // context has to outlive this, so it runs before the shutdown calls.
  // Nothing is written unless something actually changed.
  app.save();

  app.player.stop();
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
