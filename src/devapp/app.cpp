// synth-dev - the SynthGraph dev app (design doc §9, §10).
//
// A pure consumer of build outputs: it reads the project's metadata.json
// from <root>/_build/<project>/ (where <root> is the enclosing project
// root, or the project dir itself when standalone) and live-refreshes
// whenever the daemon or a one-shot build rewrites it. Pointed at a
// root, it shows every `build` rule's metadata at once. Beyond
// root/manifest resolution it never talks to compiler internals.
//
// The shell is a tiling window manager modelled on i3, and this file is
// all of it: numbered tabs, a tree of split containers per tab, keyboard
// focus and movement, search, hint labels, the `?` help overlay and the
// which-key pane. The tree lives in layout.hpp, the shortcut table and
// its state machine in keymap.hpp, the search index and hints in
// search.hpp - all headless and unit-tested. What goes *inside* a window
// (controls, waveforms) is widgets.cpp.

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
#include "keymap.hpp"
#include "layout.hpp"
#include "metadata.hpp"
#include "player.hpp"
#include "projectstate.hpp"
#include "search.hpp"
#include "wav.hpp"
#include "waveform.hpp"
#include "widgets.hpp"

namespace fs = std::filesystem;
using namespace synth::devapp;

namespace {

// The font atlas bakes one pixel size per font, so text drawn at any
// other size is a stretched bitmap - and shrinking an already small
// font is what turns it to mush. Baking a ladder of sizes lets a scaled
// window pick one near what it needs, leaving only a sliver for the
// window's own font scale to cover, so a window at 80% is about as
// sharp as one at 100%.
struct FontLadder {
  std::vector<std::pair<float, ImFont*>> steps;  // baked size -> font
  float base = 13.0f;                            // what 100% asks for

  void bake(ImGuiIO& io, float uiScale) {
    static const float kSteps[] = {6,  7,  8,  9,  10, 11, 12, 13, 14,
                                   16, 18, 20, 23, 26, 30, 34, 39};
    base = std::max(6.0f, std::floor(13.0f * uiScale));
    for (float px : kSteps) {
      ImFontConfig cfg;
      // Whole pixels: a pixel font is at its best on them.
      cfg.SizePixels = std::max(6.0f, std::floor(px * uiScale));
      if (!steps.empty() && steps.back().first == cfg.SizePixels) continue;
      if (ImFont* f = io.Fonts->AddFontDefault(&cfg))
        steps.push_back({cfg.SizePixels, f});
    }
    io.FontDefault = pick(base, nullptr);
  }

  int indexNear(float wanted) const {
    int best = -1;
    float bestErr = 1e9f;
    for (size_t i = 0; i < steps.size(); i++)
      if (std::fabs(steps[i].first - wanted) < bestErr) {
        bestErr = std::fabs(steps[i].first - wanted);
        best = (int)i;
      }
    return best;
  }

  int baseIndex() const { return indexNear(base); }
  int count() const { return (int)steps.size(); }
  float sizeAt(int i) const {
    return i >= 0 && i < count() ? steps[(size_t)i].first : base;
  }
  ImFont* fontAt(int i) const {
    return i >= 0 && i < count() ? steps[(size_t)i].second : nullptr;
  }
  // The scale a step of the ladder amounts to. A window is only ever at
  // one of these, so its glyphs are drawn at a size they were baked at:
  // one texel per pixel, and no resampling to soften them.
  float scaleAt(int i) const { return sizeAt(i) / base; }

  ImFont* pick(float wanted, float* residual) const {
    int i = indexNear(wanted);
    if (residual) *residual = 1.0f;
    return fontAt(i);
  }
};
FontLadder gFonts;

// What the keyboard has hold of inside the focused window. Named rather
// than indexed, so a rebuild that adds a control above it does not move
// the selection out from under you.
struct Selection {
  WindowRef window;
  std::string element;
  bool active() const { return !element.empty(); }
  void clear() { element.clear(); }
};

// An action that can only be carried out while the window it applies to
// is being drawn (it needs the ImGui window, or the decoded waveform).
struct Deferred {
  float scrollLines = 0;
  float scrollPages = 0;
  Action wave = Action::None;
  double waveStep = 0;
  void clear() { *this = Deferred{}; }
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
  // windows, so there is exactly one view per artifact however many
  // windows name it, and it is dropped when none is showing it - a stem
  // of a full-length song costs tens of megabytes decoded.
  std::map<std::string, WavePanel> waves;
  // What resolvePanels() made of each unit, refreshed when its metadata
  // is. Windows are named after these, and both the search index and
  // every draw walk them.
  std::map<std::string, std::vector<PanelMeta>> panels;

  // --- the shell ------------------------------------------------------
  std::vector<Tab> tabs;
  int activeTab = 1;
  bool outline = false;   // the tree, written out beside the layout
  bool whichKey = true;   // the shortcut pane that narrows as you type
  // How big each window draws its own contents. A window not in the map
  // is at 1; nothing else in the shell is affected by it.
  std::map<std::string, float> windowScales;
  KeyMachine keys;
  Selection sel;
  Deferred deferred;
  // Where each window's rows landed this frame, by element name, so the
  // hint badges and the focus ring can be drawn over them.
  std::map<std::string, std::map<std::string, Rect>> elementRects;
  std::vector<std::string> hintOrder;  // the focused window's elements
  std::string hintTyped;
  std::vector<SearchItem> index;
  std::vector<Match> matches;
  std::string searchQuery;
  int searchPick = 0;
  bool captureFocus = false;  // put the caret in the search/rename field
  char textBuf[160] = {};   // what the search and rename fields edit
  double pendingSince = 0;  // when the held prefix started, for which-key
  double altSince = 0;      // ...and when Alt went down, for the same reason
  bool altHeld = false;
  bool altArmed = false;    // Alt is down and nothing else has been pressed
  // A settings file written before tabs existed says only which panels
  // were open; the tabs are built from that once the metadata says what
  // the panels are.
  bool migratePending = false;

  // The project's settings file (projectstate.hpp). `state` is the last
  // snapshot written to disk; the live state lives in the members it
  // mirrors (the tabs, the wave views, `sections`, `windowGeom`, each
  // unit's controlUi) plus ImGui's own settings, which we only
  // re-serialize when ImGui says they changed. Saving compares a freshly
  // captured snapshot against `state`, so nothing is written while the
  // user is only looking at the window.
  std::string statePath;
  bool persist = true;  // writes are off under --self-test; reads are not
  ProjectState state;
  std::map<std::string, bool> sections;  // collapsing headers we own
  WindowGeometry windowGeom;
  bool imguiIniDirty = false;
  double sinceSaveMs = 0;
  std::string stateError;  // last save failure, if any

  // The tab on screen. Asking for it creates it: switching to a tab that
  // does not exist yet is how you get an empty one, as in i3.
  Tab& tab() { return ensureTab(tabs, activeTab); }

  UnitState* unitFor(const std::string& key) {
    for (UnitState& u : units)
      if (unitKey(u.unit) == key) return &u;
    return nullptr;
  }

  const PanelMeta* panelFor(const WindowRef& w) {
    auto it = panels.find(w.unit);
    if (it == panels.end()) return nullptr;
    for (const PanelMeta& p : it->second)
      if (p.name == w.panel) return &p;
    return nullptr;
  }

  std::vector<UnitIndex> unitIndexes() {
    std::vector<UnitIndex> out;
    for (UnitState& u : units) {
      UnitIndex ui;
      ui.unit = unitKey(u.unit);
      ui.meta = &u.loaded.meta;
      ui.panels = panels[ui.unit];
      out.push_back(std::move(ui));
    }
    return out;
  }

  // The rows of a window, in the order it draws them - the same list the
  // hints label, the search index carries and Tab steps through. The
  // overview's rows are the panel tickboxes, which are as selectable as
  // any control.
  std::vector<WindowElement> elementsOf(const WindowRef& w) {
    if (w.kind == WindowRef::Kind::Overview) return overviewElements(unitIndexes());
    UnitState* u = unitFor(w.unit);
    const PanelMeta* p = panelFor(w);
    if (!u || !p) return {};
    return windowElements(*p, u->loaded.meta);
  }

  // The panel a row of the overview ticks.
  std::optional<WindowRef> panelRowTarget(const std::string& id) {
    for (const WindowRef& w : allWindows())
      if (w.kind == WindowRef::Kind::Panel && windowId(w) == id) return w;
    return std::nullopt;
  }

  // Rebuilt whenever the metadata changes: the panel lists every other
  // part of the shell names windows from.
  void refreshPanels() {
    panels.clear();
    for (UnitState& u : units)
      panels[unitKey(u.unit)] = resolvePanels(u.loaded.meta);
  }

  // Every window a project could show, in a stable order.
  std::vector<WindowRef> allWindows() {
    std::vector<WindowRef> out{overviewWindow()};
    for (UnitState& u : units) {
      std::string key = unitKey(u.unit);
      for (const PanelMeta& p : panels[key]) {
        WindowRef w;
        w.kind = WindowRef::Kind::Panel;
        w.unit = key;
        w.panel = p.name;
        out.push_back(std::move(w));
      }
    }
    return out;
  }

  // The area the tiled windows are laid out in, kept from the last
  // frame so a window can be opened knowing how much room there is.
  Rect contentArea{0, 0, 1200, 800};

  // A window lives in exactly one tab, as in i3: opening it here takes
  // it out of wherever it was.
  void openWindow(const WindowRef& w, int intoTab = 0) {
    closeWindowEverywhere(tabs, w);
    insertWindowAuto(ensureTab(tabs, intoTab ? intoTab : activeTab), w,
                     contentArea);
  }

  // Showing or hiding a panel, from the tickbox or from the keyboard.
  // The overview keeps the focus either way: ticking is usually the
  // first of several, and the new window is on screen regardless.
  void togglePanel(const WindowRef& w) {
    Path was = tab().focused;
    if (tabHolding(tabs, w))
      closeWindowEverywhere(tabs, w);
    else
      openWindow(w);
    if (!focusWindow(tab(), overviewWindow()) && at(tab().root, was))
      tab().focused = was;
  }

  float windowScale(const std::string& id) const {
    auto it = windowScales.find(id);
    return it == windowScales.end() ? 1.0f : it->second;
  }

  // Ctrl+= / Ctrl+- / Ctrl+0 on the focused window. The steps are the
  // ladder's own sizes, so every one of them is a size the font was
  // actually rasterised at; `by` of 0 is back to the base step.
  void scaleFocusedWindow(int by) {
    auto w = focusedWindow(tab());
    if (!w) return;
    std::string id = windowId(*w);
    int base = gFonts.baseIndex();
    int at = by == 0 ? base
                     : std::clamp(gFonts.indexNear(gFonts.base *
                                                   windowScale(id)) + by,
                                  0, gFonts.count() - 1);
    if (at == base)
      windowScales.erase(id);
    else
      windowScales[id] = gFonts.scaleAt(at);
  }

  void gotoTab(int index) {
    if (index < 1) return;
    ensureTab(tabs, index);
    activeTab = index;
    sel.clear();
  }

  // Windows whose panel the build no longer declares have nothing to
  // draw, so they go; the saved tree keeps naming one until then, which
  // is what brings a panel back where it was when its declaration
  // returns.
  void pruneMissingWindows() {
    if (units.empty()) return;
    // Only a build that succeeded says what the panels are. A failed one
    // declares none - it never got that far - and taking it at its word
    // would throw the layout away over a typo.
    for (UnitState& u : units)
      if (!u.loaded.ok || u.loaded.meta.status != "ok") return;
    std::set<std::string> live;
    for (const WindowRef& w : allWindows()) live.insert(windowId(w));
    for (Tab& t : tabs)
      for (const WindowRef& w : windowsIn(t))
        if (!live.count(windowId(w))) removeWindow(t, w);
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
        auto it = state.controls.find(unitKey(s.unit));
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
    adoptSavedWaves(state.ui.waves);
    adoptTabs();
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
      auto it = state.controls.find(unitKey(u.unit));
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
      if (!overrides.empty()) s.controls[unitKey(u.unit)] = std::move(overrides);
    }
    s.ui.window = windowGeom;
    s.ui.imguiIni = state.ui.imguiIni;  // refreshed by save() when dirty
    s.ui.sections = sections;
    s.ui.tabs = tabs;
    s.ui.activeTab = activeTab;
    s.ui.outline = outline;
    s.ui.whichKey = whichKey;
    for (auto& [id, v] : windowScales) s.ui.windowScales[id] = v;
    // Which panels are on screen is now a fact about the tabs. It is
    // written out all the same: it is what an older build of the app,
    // and the migration below, read.
    for (const WindowRef& w : windowsIn(tabs)) s.ui.panels[windowId(w)] = true;
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
    if (rebuilt || panels.empty()) {
      refreshPanels();
      maybeMigrateTabs();
      pruneMissingWindows();
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

  // The tabs a previous run left. A project whose settings predate them
  // gets one tab, and its windows are filled in by maybeMigrateTabs()
  // as soon as the build says which panels there are.
  void adoptTabs() {
    tabs = state.ui.tabs;
    activeTab = state.ui.activeTab > 0 ? state.ui.activeTab : 1;
    outline = state.ui.outline;
    whichKey = state.ui.whichKey;
    windowScales.clear();
    for (auto& [id, v] : state.ui.windowScales)
      windowScales[id] = (float)v;
    if (!tabs.empty()) {
      ensureTab(tabs, activeTab);
      return;
    }
    insertWindow(ensureTab(tabs, 1), overviewWindow());
    migratePending = true;
  }

  // The old shell kept a flat row of panel checkboxes and a map of which
  // were open; a panel it had never heard of opened by default. Tab 1
  // starts out as that same set of windows, tiled.
  void maybeMigrateTabs() {
    if (!migratePending) return;
    bool anyLoaded = false;
    for (UnitState& u : units) anyLoaded |= u.loaded.ok;
    if (!anyLoaded) return;
    migratePending = false;
    Tab& t = ensureTab(tabs, 1);
    t = migratedTab(allWindows(), state.ui.panels, contentArea);
    t.index = 1;
  }
};

// ---------------------------------------------------------------------
// Drawing: the tab bar, the tiled windows, and what goes inside them.
// ---------------------------------------------------------------------

const ImVec4 kBlue(0.70f, 0.80f, 1.00f, 1.0f);
const ImVec4 kRed(1.00f, 0.45f, 0.45f, 1.0f);
const ImVec4 kGreen(0.50f, 0.90f, 0.50f, 1.0f);
const ImU32 kAccent = IM_COL32(110, 205, 160, 255);

ImVec2 v2(const Rect& r) { return ImVec2(r.x, r.y); }
ImVec2 v2end(const Rect& r) { return ImVec2(r.x + r.w, r.y + r.h); }

// One tone per level of nesting, so the borders say how deep a container
// sits without anyone having to open the outline.
ImU32 depthColor(int depth, int alpha) {
  static const ImU32 rgb[] = {IM_COL32(90, 110, 150, 255),
                              IM_COL32(150, 120, 90, 255),
                              IM_COL32(110, 150, 110, 255),
                              IM_COL32(140, 100, 150, 255)};
  ImU32 c = rgb[depth % 4];
  return (c & 0x00FFFFFF) | ((ImU32)alpha << IM_COL32_A_SHIFT);
}

// A section header whose open/closed state outlives the process. ImGui's
// ini carries window geometry but deliberately not tree state, so the few
// headers the app owns are tracked here and saved with the rest of the UI
// state.
bool persistentHeader(AppState& app, const std::string& key,
                      const std::string& label, bool defOpen) {
  auto [it, inserted] = app.sections.try_emplace(key, defOpen);
  if (inserted) it->second = defOpen;
  ImGui::SetNextItemOpen(it->second, ImGuiCond_Once);
  bool open = ImGui::CollapsingHeader(label.c_str());
  it->second = open;
  return open;
}

std::string sectionKey(const UnitState& u, const char* section) {
  return unitKey(u.unit) + "/" + section;
}

void drawDiagnostics(AppState& app, const UnitState& u) {
  const ProjectMeta& meta = u.loaded.meta;
  if (meta.diagnostics.empty()) return;
  ImGui::PushStyleColor(ImGuiCol_Text, kRed);
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

// The bar along the top: one entry per tab, the one on screen
// highlighted, then what mode the keyboard is in. Clicking a tab is the
// mouse's Alt+<n>.
float tabBarHeight() {
  return ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2;
}

void drawTabBar(AppState& app, float width) {
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(width, tabBarHeight()));
  ImGui::Begin("###tabbar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoNavFocus);
  for (const Tab& t : app.tabs) {
    bool active = t.index == app.activeTab;
    ImGui::PushStyleColor(ImGuiCol_Button,
                          active ? ImVec4(0.20f, 0.34f, 0.28f, 1.0f)
                                 : ImVec4(0.16f, 0.16f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          active ? ImVec4(0.85f, 1.0f, 0.90f, 1.0f)
                                 : ImVec4(0.70f, 0.70f, 0.76f, 1.0f));
    std::string label = tabLabel(t);
    if (t.empty()) label += " (empty)";
    if (ImGui::Button((label + "###tab" + std::to_string(t.index)).c_str()))
      app.gotoTab(t.index);
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
  }
  if (ImGui::Button("+")) app.gotoTab(nextFreeTabIndex(app.tabs));
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("new tab (Alt+n)");

  // The right-hand end: what mode we are in, and how to find the rest.
  std::string right = app.keys.mode == Mode::Normal
                          ? std::string("? for shortcuts")
                          : std::string("-- ") + modeName(app.keys.mode) +
                                " -- (Esc)";
  if (!app.keys.prefixLabel().empty())
    right = app.keys.prefixLabel() + "  (Esc cancels)";
  if (app.keys.mode == Mode::Select) {
    Run r = focusedRun(app.tab());
    int n = r.valid ? r.count() : 1;
    right = "-- select: " + std::to_string(n) +
            (n == 1 ? " window -- (Esc)" : " windows -- (Esc)");
    if (focusIsContainer(app.tab())) right = "-- select: container -- (Esc)";
  }
  float w = ImGui::CalcTextSize(right.c_str()).x;
  ImGui::SameLine(std::max(ImGui::GetCursorPosX(),
                           width - w - ImGui::GetStyle().WindowPadding.x * 2));
  ImGui::AlignTextToFramePadding();
  if (app.keys.mode == Mode::Normal)
    ImGui::TextDisabled("%s", right.c_str());
  else
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAccent), "%s",
                       right.c_str());
  ImGui::End();
}

// One target inside a window: the row of facts - status, duration, rate,
// channels - and then the waveform itself, fully interactive.
void drawTargetElement(AppState& app, const TargetMeta& t, float waveHeight,
                       std::set<std::string>& wanted, WavePanel** shown) {
  ImGui::PushID(t.name.c_str());
  ImGui::Spacing();
  ImGui::SeparatorText(t.name.c_str());

  if (t.status != "ok") {
    ImGui::TextColored(kRed, "%s", t.error.empty() ? "not built"
                                                   : t.error.c_str());
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
  // ~90 MB as doubles - so a target scrolled out of the window is
  // reserved its space and skipped.
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
  if (shown) *shown = &w;
  drawWaveContent(app.player, app.playError, w, waveHeight);
  ImGui::PopID();
}

// Carries out a wave action the keyboard asked for on the waveform the
// selection is pointing at, now that it has been decoded.
void applyWaveRequest(AppState& app, WavePanel& w) {
  switch (app.deferred.wave) {
    case Action::WavePlay:
      if (app.player.playing() && app.player.currentPath() == w.artifactPath) {
        app.player.stop();
      } else {
        double from = w.hasSelection() ? w.selStart : w.view.start;
        double to = w.hasSelection() ? w.selEnd : w.view.end;
        app.playError.clear();
        app.player.playRange(w.artifactPath, (int64_t)std::floor(from),
                             (int64_t)std::ceil(to), app.playError, w.loop);
      }
      break;
    case Action::WaveZoom: w.view.zoomAt(0.5, app.deferred.waveStep); break;
    case Action::WaveFit: w.view.reset(w.view.frames); break;
    case Action::WaveLoop:
      w.loop = !w.loop;
      if (app.player.playing() && app.player.currentPath() == w.artifactPath)
        app.player.setLooping(w.loop);
      break;
    default: break;
  }
  app.deferred.wave = Action::None;
}

// The overview: what the backdrop used to show. Per unit the project,
// its build status and diagnostics, then the tick list of its panels -
// which is how a window is brought into existence, and where it says
// which tab is holding it.
void drawOverviewBody(AppState& app, const WindowRef& self) {
  std::map<std::string, Rect>& rects = app.elementRects[windowId(self)];
  rects.clear();
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
      ImGui::TextColored(kBlue, "[%s]", u.unit.label.c_str());
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
      ImGui::TextColored(kGreen, "[build ok]");
    else
      ImGui::TextColored(kRed, "[build failed]");
    ImGui::Separator();
    drawDiagnostics(app, u);

    ImGui::TextDisabled("panels");
    for (const PanelMeta& p : app.panels[unitKey(u.unit)]) {
      WindowRef ref;
      ref.kind = WindowRef::Kind::Panel;
      ref.unit = unitKey(u.unit);
      ref.panel = p.name;
      int in = tabHolding(app.tabs, ref);
      bool open = in != 0;
      ImVec2 top = ImGui::GetCursorScreenPos();
      float rowW = std::max(16.0f, ImGui::GetContentRegionAvail().x);
      if (ImGui::Checkbox((p.name + "###p" + p.name).c_str(), &open))
        app.togglePanel(ref);
      if (in) {
        ImGui::SameLine();
        // Where it went, so a window you ticked on and cannot see is
        // one tab away rather than lost.
        ImGui::TextDisabled("tab %d%s", in,
                            in == app.activeTab ? "" : " (elsewhere)");
      }
      ImVec2 after = ImGui::GetCursorScreenPos();
      // Every row is a selectable element, keyed the way the
      // enumeration keys it: hints label these, Tab steps through them
      // and Enter ticks them.
      rects[windowId(ref)] =
          Rect{top.x, top.y, rowW, std::max(2.0f, after.y - top.y)};
    }
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
    ImGui::TextColored(kRed, "playback: %s", app.playError.c_str());
  }
  if (!app.stateError.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(kRed, "project state: %s", app.stateError.c_str());
  }
  if (app.player.playing()) {
    ImGui::Spacing();
    ImGui::TextDisabled("playing%s %s", app.player.looping() ? " (loop)" : "",
                        app.player.currentPath().c_str());
  }
}

// One panel window's contents: its controls in the order the panel names
// them - a component's parts indented under it - then its targets with
// their waveforms. Every row's rectangle is recorded as it is drawn, so
// the hint badges and the focus ring land on it.
void drawPanelBody(AppState& app, UnitState& u, const PanelMeta& panel,
                   const WindowRef& ref, std::set<std::string>& wanted) {
  const std::vector<ControlMeta>& controls = u.loaded.meta.controls;
  std::vector<WindowElement> els = windowElements(panel, u.loaded.meta);
  std::map<std::string, Rect>& rects = app.elementRects[windowId(ref)];
  rects.clear();

  size_t drawable = 0;
  for (const WindowElement& e : els) {
    if (e.kind != WindowElement::Kind::Target) continue;
    for (const TargetMeta& t : u.loaded.meta.targets)
      if (t.name == e.name && t.status == "ok" && t.kind != "visual" &&
          !t.artifact.empty())
        drawable++;
  }
  // A lone target fills the window; several get a readable fixed height
  // each and the window scrolls.
  float perTarget = drawable > 1 ? 150.0f * gUiScale : -1.0f;

  bool anyDirty = false;
  int indented = 0;
  for (const WindowElement& e : els) {
    while (indented < e.depth) {
      ImGui::Indent(12.0f * gUiScale);
      indented++;
    }
    while (indented > e.depth) {
      ImGui::Unindent(12.0f * gUiScale);
      indented--;
    }
    ImVec2 top = ImGui::GetCursorScreenPos();
    float rowW = std::max(16.0f, ImGui::GetContentRegionAvail().x);
    bool selected = app.sel.window == ref && app.sel.element == e.name;
    WavePanel* wave = nullptr;
    switch (e.kind) {
      case WindowElement::Kind::Control:
        for (const ControlMeta& c : controls)
          if (c.name == e.name && c.group.empty())
            anyDirty |= drawOneControl(u, c);
        break;
      case WindowElement::Kind::Group:
        drawGroupByName(u, controls, e.name, anyDirty);
        break;
      case WindowElement::Kind::Target:
        for (const TargetMeta& t : u.loaded.meta.targets)
          if (t.name == e.name)
            drawTargetElement(app, t, perTarget, wanted, &wave);
        break;
      case WindowElement::Kind::Panel:
        break;  // the overview's rows, which this window does not have
    }
    if (selected && wave && app.deferred.wave != Action::None)
      applyWaveRequest(app, *wave);
    ImVec2 after = ImGui::GetCursorScreenPos();
    rects[e.name] = Rect{top.x, top.y, rowW, std::max(2.0f, after.y - top.y)};
  }
  while (indented > 0) {
    ImGui::Unindent(12.0f * gUiScale);
    indented--;
  }

  if (!els.empty()) {
    if (ImGui::SmallButton("defaults")) {
      for (const ControlMeta& m : controls) {
        bool mine = false;
        for (const WindowElement& e : els)
          if (e.name == m.name || e.name == m.group) mine = true;
        if (mine) resetControl(u, m);
      }
    }
    if (anyDirty) {
      ImGui::SameLine();
      ImGui::TextDisabled("* pending rebuild");
    }
  } else {
    ImGui::TextDisabled("this panel is empty");
  }
  if (!u.controlsError.empty())
    ImGui::TextColored(kRed, "controls: %s", u.controlsError.c_str());
}

// Draws one window's contents at its own scale, and puts the style back
// afterwards. Scaling has to reach three separate things or it is not a
// smaller *rendering*, only smaller text in full-size frames:
//
//   - ImGui's style metrics (padding, spacing, scrollbars, rounding,
//     grab sizes), which is what ScaleAllSizes does and what --scale
//     already does globally;
//   - the font, through the window's own font scale;
//   - the sizes this app picks in raw pixels - knob diameters, slider
//     widths, wave heights - which is exactly what gUiScale multiplies.
//
// Everything is then laid out and drawn at the target size. The one
// thing that is a true raster scale is the glyphs themselves: they come
// from an atlas baked at one size, so large factors soften the text.
struct ScopedWindowScale {
  ImGuiStyle saved;
  float outerUiScale;
  bool on;

  explicit ScopedWindowScale(float scale)
      : saved(ImGui::GetStyle()), outerUiScale(gUiScale), on(scale != 1.0f) {
    if (!on) return;
    ImGui::GetStyle().ScaleAllSizes(scale);
    gUiScale *= scale;
  }
  // A style left scaled would compound on the next frame, so this is
  // never conditional on how the drawing went.
  ~ScopedWindowScale() {
    if (!on) return;
    ImGui::GetStyle() = saved;
    gUiScale = outerUiScale;
  }
};

// One leaf of the tree: a window with our own title row (the tiling
// owns the geometry, so ImGui's own bar and its ini have no say) and a
// scrolling body.
void drawLeaf(AppState& app, const PlacedWindow& pw,
              std::set<std::string>& wanted) {
  const WindowRef& ref = pw.window;
  std::string id = windowId(ref);
  Tab& tab = app.tab();

  // Before Begin: the window's own padding is read there. The scale is
  // taken from the ladder step nearest what the window asks for, so the
  // style and the glyphs are always the same size and that size is
  // always one the font was baked at.
  int step = gFonts.indexNear(gFonts.base * app.windowScale(id));
  float ownScale = gFonts.scaleAt(step);
  ScopedWindowScale scaled(ownScale);

  ImGui::SetNextWindowPos(v2(pw.rect));
  ImGui::SetNextWindowSize(ImVec2(pw.rect.w, pw.rect.h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
  ImGui::PushStyleColor(ImGuiCol_Border,
                        pw.focused ? ImGui::ColorConvertU32ToFloat4(kAccent)
                                   : ImVec4(0.28f, 0.28f, 0.34f, 1.0f));
  ImGui::Begin(("###win " + id).c_str(), nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoScrollbar);
  // Both of these are per-window state ImGui keeps between frames, so
  // they are set every frame whatever the scale is: leaving a window's
  // font scale behind is what made one that had been zoomed and reset
  // stay small and blurry at "100%". The scale is always exactly 1 now
  // - the size comes from the font, not from stretching it.
  ImFont* font = gFonts.fontAt(step);
  if (font) ImGui::PushFont(font);
  ImGui::SetWindowFontScale(1.0f);

  // Clicking anywhere in a window focuses it, the way clicking a
  // terminal does.
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                             ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !pw.focused) {
    focusWindow(tab, ref);
    app.sel.clear();
  }

  // A window inside the focused container is not itself focused, but it
  // is about to be moved or closed with it, so it does not read as
  // inactive either.
  ImGui::PushStyleColor(ImGuiCol_Text,
                        pw.focused || pw.inFocus
                            ? ImVec4(0.85f, 1.0f, 0.90f, 1.0f)
                            : ImVec4(0.72f, 0.72f, 0.78f, 1.0f));
  ImGui::TextUnformatted(windowTitle(ref).c_str());
  ImGui::PopStyleColor();
  if (app.windowScale(id) != 1.0f) {
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f%%", app.windowScale(id) * 100.0f);
  }
  float closeW = ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SameLine(std::max(ImGui::GetCursorPosX(),
                           ImGui::GetWindowWidth() - closeW -
                               ImGui::GetStyle().WindowPadding.x));
  bool closed = ImGui::SmallButton("x");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("close (Alt+q)");
  ImGui::Separator();

  ImGui::BeginChild("body", ImVec2(0, 0), false);
  // A scroll the keyboard asked for lands on the focused window.
  if (pw.focused && app.deferred.scrollLines != 0)
    ImGui::SetScrollY(ImGui::GetScrollY() +
                      app.deferred.scrollLines *
                          ImGui::GetTextLineHeightWithSpacing());
  if (pw.focused && app.deferred.scrollPages != 0)
    ImGui::SetScrollY(ImGui::GetScrollY() +
                      app.deferred.scrollPages * ImGui::GetWindowHeight());

  if (ref.kind == WindowRef::Kind::Overview) {
    drawOverviewBody(app, ref);
  } else {
    UnitState* u = app.unitFor(ref.unit);
    const PanelMeta* p = app.panelFor(ref);
    if (u && p)
      drawPanelBody(app, *u, *p, ref, wanted);
    else
      ImGui::TextDisabled("this panel is not in the build any more");
  }
  ImGui::EndChild();
  if (font) ImGui::PopFont();
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();

  if (closed) {
    removeWindow(tab, ref);
    app.sel.clear();
  }
}

// The containers themselves: a thin border per level, so the shape of
// the tree is visible in the layout and not only in the outline. The
// focused window gets a heavier one on top of everything - which window
// has the keyboard has to be readable at a glance.
void drawContainerFrames(const Placement& p) {
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  ImDrawList* fg = ImGui::GetForegroundDrawList();
  auto ring = [fg](const Rect& r, ImU32 col, float grow, float thick) {
    fg->AddRect(ImVec2(r.x - grow, r.y - grow),
                ImVec2(r.x + r.w + grow, r.y + r.h + grow), col, 4.0f, 0,
                thick);
  };
  // One ring, one meaning: this is what the next command acts on -
  // whether that is a window, a container, or a gathered run of them.
  for (const PlacedSplit& s : p.splits) {
    bg->AddRect(v2(s.rect), v2end(s.rect), depthColor(s.depth, 150), 4.0f, 0,
                1.0f);
    if (s.focused || s.selected) ring(s.rect, kAccent, 2, 2.0f);
  }
  for (const PlacedWindow& w : p.windows)
    if (w.focused || w.selected) ring(w.rect, kAccent, 1, 2.0f);
}

// The gutters, dragged with the mouse: the same boundaries Alt+r moves
// with the keyboard.
void dragDividers(AppState& app, const Placement& p, const Rect& area) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  static int held = -1;
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) held = -1;
  for (size_t i = 0; i < p.dividers.size(); i++) {
    const Divider& d = p.dividers[i];
    bool over = io.MousePos.x >= d.rect.x &&
                io.MousePos.x <= d.rect.x + d.rect.w &&
                io.MousePos.y >= d.rect.y && io.MousePos.y <= d.rect.y + d.rect.h;
    if (over && !ImGui::IsAnyItemHovered()) {
      ImGui::SetMouseCursor(d.split == Split::H ? ImGuiMouseCursor_ResizeEW
                                                : ImGuiMouseCursor_ResizeNS);
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) held = (int)i;
    }
    if (held == (int)i) {
      dl->AddRectFilled(v2(d.rect), v2end(d.rect), kAccent);
      float span = d.split == Split::H ? area.w : area.h;
      float move = d.split == Split::H ? io.MouseDelta.x : io.MouseDelta.y;
      if (span > 0 && move != 0)
        resizeSplit(app.tab(), d.path, d.boundary, move / span);
    }
  }
}

// ---------------------------------------------------------------------
// The overlays: the tree outline, hint labels, search, help, which-key.
// ---------------------------------------------------------------------

void outlineLines(const Node& n, const Tab& t, Path path,
                  const std::string& prefix, bool last,
                  std::vector<std::string>& out) {
  std::string head = path.empty() ? "" : prefix + (last ? "`-- " : "|-- ");
  // The outline is where a focused *container* is easiest to see, so it
  // marks the focus and the gathered run wherever they sit.
  Run run = focusedRun(t);
  std::string mark = path == t.focused  ? "   <- focused"
                     : inRun(run, path) ? "   <- selected"
                                        : "";
  if (n.kind == Node::Kind::Leaf) {
    out.push_back(head + windowTitle(n.window) + mark);
    return;
  }
  if (n.kind == Node::Kind::Empty) return;
  out.push_back(head + (n.split == Split::H ? "splith" : "splitv") + mark);
  std::string next = path.empty() ? "" : prefix + (last ? "    " : "|   ");
  for (size_t i = 0; i < n.children.size(); i++) {
    Path cp = path;
    cp.push_back((int)i);
    outlineLines(n.children[i], t, cp, next, i + 1 == n.children.size(), out);
  }
}

// The focused tab's tree, written out. The layout already says the shape
// through its nested borders; this says it in words, which is what you
// want when a window is somewhere you cannot see.
void drawOutline(AppState& app, const Rect& area) {
  Tab& t = app.tab();
  std::vector<std::string> lines;
  outlineLines(t.root, t, {}, "", true, lines);
  float w = 240.0f * gUiScale;
  ImGui::SetNextWindowPos(ImVec2(area.x + area.w - w - 10 * gUiScale,
                                 area.y + 10 * gUiScale));
  ImGui::SetNextWindowSize(ImVec2(w, 0));
  ImGui::SetNextWindowBgAlpha(0.92f);
  ImGui::Begin("###outline", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::TextColored(kBlue, "tab %s", tabLabel(t).c_str());
  ImGui::Separator();
  if (lines.empty()) ImGui::TextDisabled("(empty)");
  for (const std::string& l : lines) ImGui::TextUnformatted(l.c_str());
  ImGui::Separator();
  ImGui::TextDisabled("Alt+t to close");
  ImGui::End();
}

// The focused window's rows, each with the key that jumps to it.
void drawHints(AppState& app, const Placement& p) {
  const PlacedWindow* focused = nullptr;
  for (const PlacedWindow& w : p.windows)
    if (w.focused) focused = &w;
  if (!focused) return;
  auto rects = app.elementRects.find(windowId(focused->window));
  if (rects == app.elementRects.end()) return;
  std::vector<std::string> labels = hintLabels(app.hintOrder.size());
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  for (size_t i = 0; i < app.hintOrder.size(); i++) {
    auto r = rects->second.find(app.hintOrder[i]);
    if (r == rects->second.end()) continue;
    // A row scrolled out of its window keeps its rectangle from when it
    // was last drawn; labelling it would put a badge outside the window.
    if (r->second.y < focused->rect.y ||
        r->second.y > focused->rect.y + focused->rect.h)
      continue;
    const std::string& label = labels[i];
    // A label the typing has ruled out fades rather than vanishing, so
    // the rows do not jump about under a half-typed hint.
    bool live = label.rfind(app.hintTyped, 0) == 0;
    ImVec2 at(r->second.x + 2, r->second.y + 1);
    ImVec2 size = ImGui::CalcTextSize(label.c_str());
    dl->AddRectFilled(at, ImVec2(at.x + size.x + 8, at.y + size.y + 4),
                      live ? IM_COL32(235, 200, 110, 235)
                           : IM_COL32(90, 90, 100, 160),
                      3.0f);
    dl->AddText(ImVec2(at.x + 4, at.y + 2),
                live ? IM_COL32(20, 20, 20, 255) : IM_COL32(60, 60, 60, 255),
                label.c_str());
  }
}

// A ring around the row the keyboard has hold of. The focused *window*
// says so with its own border and title, so this is only ever about
// what is selected inside one.
void drawFocusRing(AppState& app) {
  if (!app.sel.active()) return;
  auto rects = app.elementRects.find(windowId(app.sel.window));
  if (rects == app.elementRects.end()) return;
  auto r = rects->second.find(app.sel.element);
  if (r == rects->second.end()) return;
  ImGui::GetForegroundDrawList()->AddRect(
      ImVec2(r->second.x - 2, r->second.y - 1),
      ImVec2(r->second.x + r->second.w, r->second.y + r->second.h + 1),
      kAccent, 3.0f, 0, 1.5f);
}

void rebuildIndex(AppState& app) {
  app.index = buildSearchIndex(app.tabs, app.unitIndexes());
  app.matches = searchItems(app.index, app.searchQuery);
}

void acceptSearch(AppState& app) {
  if (app.matches.empty()) return;
  int pick = std::clamp(app.searchPick, 0, (int)app.matches.size() - 1);
  const SearchItem& it = app.index[app.matches[(size_t)pick].item];
  app.sel.clear();
  if (it.kind == SearchItem::Kind::Tab) {
    app.gotoTab(it.tab);
    return;
  }
  // A window no tab is holding is opened here; one that is already
  // somewhere is where we go.
  if (it.tab)
    app.activeTab = it.tab;
  else
    app.openWindow(it.window);
  focusWindow(app.tab(), it.window);
  if (it.kind == SearchItem::Kind::Element) {
    app.sel.window = it.window;
    app.sel.element = it.element;
  }
}

// Search: everything the project can show, narrowed as you type. Tabs
// first, then windows, then the things inside them.
void drawSearch(AppState& app, const Rect& area) {
  float w = std::min(area.w - 40 * gUiScale, 560.0f * gUiScale);
  ImGui::SetNextWindowPos(ImVec2(area.x + (area.w - w) * 0.5f,
                                 area.y + 40 * gUiScale));
  ImGui::SetNextWindowSize(ImVec2(w, 0));
  ImGui::Begin("###search", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::SetNextItemWidth(-1);
  if (app.captureFocus) {
    ImGui::SetKeyboardFocusHere();
    app.captureFocus = false;
  }
  if (ImGui::InputTextWithHint("##query", "tab, window, control, waveform...",
                               app.textBuf, sizeof app.textBuf)) {
    app.searchQuery = app.textBuf;
    app.searchPick = 0;
    app.matches = searchItems(app.index, app.searchQuery);
  }
  ImGui::Separator();
  if (app.matches.empty()) ImGui::TextDisabled("nothing matches");
  int shown = 0;
  if (!app.matches.empty())
    app.searchPick = std::clamp(app.searchPick, 0, (int)app.matches.size() - 1);
  for (size_t i = 0; i < app.matches.size() && shown < 12; i++, shown++) {
    const SearchItem& it = app.index[app.matches[i].item];
    bool picked = (int)i == app.searchPick;
    const char* what = it.kind == SearchItem::Kind::Tab      ? "tab"
                       : it.kind == SearchItem::Kind::Window ? "window"
                                                             : "in";
    std::string row = it.label + "##r" + std::to_string(i);
    if (ImGui::Selectable(row.c_str(), picked)) {
      app.searchPick = (int)i;
      acceptSearch(app);
      app.keys.mode = Mode::Normal;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s %s", what, it.detail.c_str());
  }
  ImGui::Separator();
  ImGui::TextDisabled("Enter go  -  Up/Down pick  -  Esc close");
  ImGui::End();
}

void drawRename(AppState& app, const Rect& area) {
  float w = 320.0f * gUiScale;
  ImGui::SetNextWindowPos(ImVec2(area.x + (area.w - w) * 0.5f,
                                 area.y + 40 * gUiScale));
  ImGui::SetNextWindowSize(ImVec2(w, 0));
  ImGui::Begin("###rename", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::Text("name for tab %d", app.activeTab);
  ImGui::SetNextItemWidth(-1);
  if (app.captureFocus) {
    ImGui::SetKeyboardFocusHere();
    app.captureFocus = false;
  }
  ImGui::InputText("##name", app.textBuf, sizeof app.textBuf);
  ImGui::TextDisabled("Enter to keep it, Esc to leave it alone");
  ImGui::End();
}

// Every shortcut that applies right here: the mode's own map, filtered
// by what is selected. Read from the same table the keys are, so it
// cannot drift out of date.
void drawHelp(AppState& app, const Rect& area, unsigned ctx) {
  Mode showing = app.keys.helpFrom;
  float w = std::min(area.w - 40 * gUiScale, 700.0f * gUiScale);
  ImGui::SetNextWindowPos(ImVec2(area.x + (area.w - w) * 0.5f,
                                 area.y + 30 * gUiScale));
  ImGui::SetNextWindowSize(ImVec2(w, 0));
  ImGui::SetNextWindowBgAlpha(0.96f);
  ImGui::Begin("###help", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::TextColored(kBlue, "shortcuts - %s mode", modeName(showing));
  ImGui::TextDisabled(
      "global first, then what applies to the window and to whatever is "
      "selected in it");
  ImGui::Separator();
  std::string group;
  if (ImGui::BeginTable("help", 2,
                        ImGuiTableFlags_SizingFixedFit |
                            ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed,
                            130.0f * gUiScale);
    ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
    for (const Binding* b : bindingsFor(showing, ctx)) {
      if (!b->listed) continue;
      if (group != b->group) {
        group = b->group;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextColored(kBlue, "%s", group.c_str());
      }
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(sequenceName(b->sequence).c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(b->help);
    }
    ImGui::EndTable();
  }
  ImGui::Separator();
  ImGui::TextDisabled("any key closes");
  ImGui::End();
}

// The which-key pane: what can come next, while you are in the middle of
// typing it. Holding Alt lists the whole Alt map; a held prefix narrows
// to what completes it; a mode lists that mode's keys.
std::vector<const Binding*> whichKeyRows(AppState& app, unsigned ctx) {
  std::vector<const Binding*> out;
  bool bare = app.keys.pending.empty();
  for (const Binding* b : app.keys.completions(ctx)) {
    if (!b->listed) continue;
    // With Alt down and nothing typed yet, only the Alt bindings are
    // reachable, so only those are worth showing.
    if (bare && app.keys.mode == Mode::Normal &&
        b->sequence[0].alt != (app.altHeld || app.keys.sticky))
      continue;
    out.push_back(b);
  }
  return out;
}

bool whichKeyShowing(AppState& app, size_t rows) {
  if (!app.whichKey || rows == 0) return false;
  if (app.keys.mode == Mode::Search || app.keys.mode == Mode::Rename ||
      app.keys.mode == Mode::Help)
    return false;
  // A tapped Alt asked for it, so it appears at once. A held one waits,
  // so a chord typed at speed never makes the pane flash.
  if (app.keys.sticky) return true;
  if (!app.keys.pending.empty() || app.keys.mode != Mode::Normal)
    return ImGui::GetTime() - app.pendingSince > 0.25;
  return app.altHeld && ImGui::GetTime() - app.altSince > 0.35;
}

// How the pane is laid out: as many columns as fit the widest entry it
// actually has, and no more rows than the height allows. Everything the
// drawing needs to size the window before it opens it.
struct WhichKeyLayout {
  int columns = 1;
  int rows = 1;
  float cell = 200;
  float keyWidth = 60;
  size_t shown = 0;   // entries that fit
  size_t hidden = 0;  // and the ones that did not
  float height = 0;
};

WhichKeyLayout layOutWhichKey(const std::vector<const Binding*>& rows,
                              float width, float maxHeight) {
  WhichKeyLayout l;
  const ImGuiStyle& st = ImGui::GetStyle();
  float line = ImGui::GetTextLineHeightWithSpacing();
  float gap = st.ItemSpacing.x * 2;
  for (const Binding* b : rows) {
    l.keyWidth = std::max(
        l.keyWidth, ImGui::CalcTextSize(sequenceName(b->sequence).c_str()).x);
    l.cell = std::max(l.cell, ImGui::CalcTextSize(b->help).x + l.keyWidth + gap);
  }
  float avail = std::max(120.0f, width - st.WindowPadding.x * 2);
  // Never wider than the space there is: a column that does not fit is
  // a column whose text runs off the screen.
  l.cell = std::min(l.cell, avail);
  l.columns = std::max(1, (int)(avail / l.cell));
  l.cell = avail / (float)l.columns;
  int fits = std::max(1, (int)((maxHeight - line - st.WindowPadding.y * 2) / line));
  l.rows = (int)((rows.size() + (size_t)l.columns - 1) / (size_t)l.columns);
  l.rows = std::clamp(l.rows, 1, fits);
  l.shown = std::min(rows.size(), (size_t)(l.rows * l.columns));
  l.hidden = rows.size() - l.shown;
  l.height = line * (float)(l.rows + 1) + st.WindowPadding.y * 2;
  return l;
}

// Text cut to fit, with an ellipsis rather than a hard edge.
std::string elide(const std::string& text, float room) {
  if (ImGui::CalcTextSize(text.c_str()).x <= room) return text;
  std::string out = text;
  while (!out.empty() &&
         ImGui::CalcTextSize((out + "...").c_str()).x > room)
    out.pop_back();
  return out + "...";
}

void drawWhichKey(AppState& app, const std::vector<const Binding*>& rows,
                  const WhichKeyLayout& l, float width, float top) {
  ImGui::SetNextWindowPos(ImVec2(0, top));
  ImGui::SetNextWindowSize(ImVec2(width, l.height));
  ImGui::Begin("###whichkey", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoNavFocus);
  std::string lead = app.keys.prefixLabel();
  if (lead.empty()) lead = modeName(app.keys.mode);
  ImGui::TextColored(kBlue, "%s", lead.c_str());
  ImGui::SameLine();
  if (l.hidden)
    ImGui::TextDisabled("(%zu more - ? for all of them)", l.hidden);
  else
    ImGui::TextDisabled("(Alt+w to put this away)");

  float x0 = ImGui::GetCursorPosX();
  float y0 = ImGui::GetCursorPosY();
  for (size_t i = 0; i < l.shown; i++) {
    int col = (int)i / l.rows, row = (int)i % l.rows;
    ImGui::SetCursorPos(ImVec2(x0 + (float)col * l.cell,
                               y0 + (float)row * ImGui::GetTextLineHeightWithSpacing()));
    // The chord that would come next, not the whole sequence: that is
    // what you are about to press.
    const Chord& next = rows[i]->sequence[app.keys.pending.size()];
    std::string key = chordName(next);
    // With a prefix already typed, the modifier is spoken for - showing
    // it on every row would be noise.
    if (app.keys.sticky && next.alt && !next.ctrl) {
      key = chordName(Chord{next.key, false, next.shift, false});
    }
    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.5f, 1.0f), "%s", key.c_str());
    ImGui::SameLine(x0 + (float)col * l.cell + l.keyWidth);
    ImGui::TextUnformatted(
        elide(rows[i]->help, l.cell - l.keyWidth - ImGui::GetStyle().ItemSpacing.x)
            .c_str());
  }
  ImGui::End();
}

// ---------------------------------------------------------------------
// Input: real keys in, table actions out.
// ---------------------------------------------------------------------

Chord readChord() {
  ImGuiIO& io = ImGui::GetIO();
  Chord c;
  c.alt = io.KeyAlt;
  c.shift = io.KeyShift;
  c.ctrl = io.KeyCtrl;
  // Auto-repeat is wanted: holding Alt+l should walk across the windows,
  // and holding `l` in resize mode should keep resizing.
  for (int i = 0; i < 26; i++)
    if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_A + i), true)) {
      c.key = (Key)((int)Key::A + i);
      return c;
    }
  for (int i = 0; i < 10; i++)
    if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + i), true)) {
      c.key = (Key)((int)Key::D0 + i);
      return c;
    }
  static const struct {
    ImGuiKey imk;
    Key key;
  } rest[] = {
      {ImGuiKey_LeftArrow, Key::Left},   {ImGuiKey_RightArrow, Key::Right},
      {ImGuiKey_UpArrow, Key::Up},       {ImGuiKey_DownArrow, Key::Down},
      {ImGuiKey_Enter, Key::Enter},      {ImGuiKey_KeypadEnter, Key::Enter},
      {ImGuiKey_Escape, Key::Escape},    {ImGuiKey_Tab, Key::Tab},
      {ImGuiKey_Space, Key::Space},      {ImGuiKey_Slash, Key::Slash},
      {ImGuiKey_Comma, Key::Comma},      {ImGuiKey_Period, Key::Period},
      {ImGuiKey_Minus, Key::Minus},      {ImGuiKey_Equal, Key::Equal},
  };
  for (auto& r : rest)
    if (ImGui::IsKeyPressed(r.imk, true)) {
      c.key = r.key;
      return c;
    }
  return Chord{};
}

// The rows of the focused window, in draw order: what Tab steps through
// and what the hints label.
void refreshHintOrder(AppState& app) {
  app.hintOrder.clear();
  auto w = focusedWindow(app.tab());
  if (!w) return;
  for (const WindowElement& e : app.elementsOf(*w))
    app.hintOrder.push_back(e.name);
}

unsigned contextBits(AppState& app) {
  unsigned ctx = 0;
  Tab& t = app.tab();
  if (windowsIn(t).size() > 1) ctx |= CtxTiled;
  if (focusIsContainer(t)) ctx |= CtxContainer;
  // Not at the root: there is a container above this to step out to.
  if (!t.empty() && !t.focused.empty()) ctx |= CtxNested;
  if (app.sel.active())
    for (const WindowElement& e : app.elementsOf(app.sel.window))
      if (e.name == app.sel.element) {
        ctx |= CtxRow;
        // A linked group is not one value to nudge, and a panel tickbox
        // is not a value at all - both can still be activated.
        if (e.kind == WindowElement::Kind::Target) ctx |= CtxWave;
        if (e.kind == WindowElement::Kind::Control) ctx |= CtxWidget;
      }
  return ctx;
}

// The control the selection is on, if it is on one at all (a group or a
// waveform is not one control, and the nudge keys leave it alone).
ControlMeta* selectedControl(AppState& app, UnitState** unit) {
  if (!app.sel.active()) return nullptr;
  UnitState* u = app.unitFor(app.sel.window.unit);
  if (!u) return nullptr;
  for (ControlMeta& c : u->loaded.meta.controls)
    if (c.name == app.sel.element && c.group.empty()) {
      *unit = u;
      return &c;
    }
  return nullptr;
}

bool discreteKind(const std::string& kind) {
  return kind == "int_slider" || kind == "toggle" || kind == "choice";
}

void adjustSelected(AppState& app, double frac) {
  UnitState* u = nullptr;
  ControlMeta* c = selectedControl(app, &u);
  if (!c || !u) return;
  ControlUi& ui = u->controlUi[c->name];
  double next;
  if (discreteKind(c->kind))
    next = (double)ui.value + (frac > 0 ? 1 : -1);
  else
    next = (double)ui.value + frac * (c->max - c->min);
  ui.value = (float)std::clamp(next, c->min, c->max);
  noteControlEdit(*u, ui, /*released=*/true);
}

// Enter on a tickbox flips it; on a list of options it takes the next
// one, wrapping - the keyboard's version of clicking the one you want.
void activateSelected(AppState& app) {
  // A row of the overview is a panel tickbox: activating it shows or
  // hides that panel, exactly as clicking it does.
  if (app.sel.window.kind == WindowRef::Kind::Overview) {
    if (auto w = app.panelRowTarget(app.sel.element)) app.togglePanel(*w);
    return;
  }
  UnitState* u = nullptr;
  ControlMeta* c = selectedControl(app, &u);
  if (!c || !u) return;
  ControlUi& ui = u->controlUi[c->name];
  if (c->kind == "toggle") {
    ui.value = ui.value >= 0.5f ? 0.0f : 1.0f;
  } else if (c->kind == "choice") {
    int n = (int)std::max<size_t>(1, c->options.size());
    ui.value = (float)(((int)std::lround((double)ui.value) + 1) % n);
  } else {
    return;
  }
  noteControlEdit(*u, ui, /*released=*/true);
}

void stepSelection(AppState& app, int by) {
  refreshHintOrder(app);
  if (app.hintOrder.empty()) return;
  auto w = focusedWindow(app.tab());
  if (!w) return;
  int at = -1;
  if (app.sel.active() && app.sel.window == *w)
    for (size_t i = 0; i < app.hintOrder.size(); i++)
      if (app.hintOrder[i] == app.sel.element) at = (int)i;
  int n = (int)app.hintOrder.size();
  int next = at < 0 ? (by > 0 ? 0 : n - 1) : (at + by % n + n) % n;
  app.sel.window = *w;
  app.sel.element = app.hintOrder[(size_t)next];
}

void applyAction(AppState& app, const KeyMachine::Step& s) {
  Tab& tab = app.tab();
  auto focused = focusedWindow(tab);
  switch (s.action) {
    case Action::FocusDir:
      if (focusDir(tab, (Dir)s.arg)) app.sel.clear();
      break;
    case Action::MoveDir: moveDir(tab, (Dir)s.arg); break;
    case Action::CloseWindow:
      // Whatever the focus covers: one window, or every window in the
      // focused container.
      closeFocused(tab);
      app.sel.clear();
      break;
    case Action::ShowOverview: app.openWindow(overviewWindow()); break;
    case Action::GotoTab: app.gotoTab(s.arg); break;
    case Action::SendToTab:
      // A container goes across intact, laid out as it was.
      if (s.arg != app.activeTab &&
          sendFocusedToTab(app.tabs, app.activeTab, s.arg, app.contentArea))
        app.sel.clear();
      break;
    case Action::NewTab: app.gotoTab(nextFreeTabIndex(app.tabs)); break;
    case Action::RenameTab:
      std::snprintf(app.textBuf, sizeof app.textBuf, "%s", tab.name.c_str());
      app.captureFocus = true;
      break;
    case Action::SplitH: tab.pendingSplit = Split::H; break;
    case Action::SplitV: tab.pendingSplit = Split::V; break;
    case Action::FocusParent:
      // The container's own contents are not a widget any more.
      if (focusParent(tab)) app.sel.clear();
      break;
    case Action::FocusChild:
      if (focusChild(tab)) app.sel.clear();
      break;
    case Action::EnterSelect:
      clearSelection(tab);
      app.sel.clear();
      break;
    case Action::ExtendSel: extendSelection(tab, (Dir)s.arg); break;
    case Action::Group: groupSelection(tab, (Split)s.arg); break;
    case Action::Flatten: flattenFocused(tab); break;
    case Action::ResizeDir: resizeFocused(tab, (Dir)s.arg, s.step); break;
    case Action::OpenSearch:
      app.searchQuery.clear();
      app.textBuf[0] = '\0';
      app.searchPick = 0;
      app.captureFocus = true;
      rebuildIndex(app);
      break;
    case Action::ToggleOutline: app.outline = !app.outline; break;
    case Action::ToggleWhichKey: app.whichKey = !app.whichKey; break;
    case Action::EnterHint:
      app.hintTyped.clear();
      refreshHintOrder(app);
      break;
    case Action::LeaveMode:
      app.sel.clear();
      app.hintTyped.clear();
      break;
    case Action::Scroll: app.deferred.scrollLines = (float)s.step; break;
    case Action::ScrollPage: app.deferred.scrollPages = (float)s.step; break;
    case Action::WidgetAdjust: adjustSelected(app, s.step); break;
    case Action::WidgetActivate: activateSelected(app); break;
    case Action::WidgetReset: {
      UnitState* u = nullptr;
      if (ControlMeta* c = selectedControl(app, &u)) resetControl(*u, *c);
      break;
    }
    case Action::WidgetStep: stepSelection(app, s.arg); break;
    case Action::ScaleWindow: app.scaleFocusedWindow(s.arg); break;
    case Action::WavePlay:
    case Action::WaveZoom:
    case Action::WaveFit:
    case Action::WaveLoop:
      app.deferred.wave = s.action;
      app.deferred.waveStep = s.step;
      break;
    case Action::SearchAccept: acceptSearch(app); break;
    case Action::SearchStep:
      app.searchPick = std::max(0, app.searchPick + s.arg);
      break;
    case Action::RenameAccept: tab.name = app.textBuf; break;
    default: break;
  }
}

// Hint mode: the letters are the labels, not shortcuts. Returns true
// when a key was consumed here - it must not go on to the map as well,
// or one press of `f` picks the label `f` and then reopens the labels.
bool typeHints(AppState& app) {
  std::vector<std::string> labels = hintLabels(app.hintOrder.size());
  bool consumed = false;
  for (int i = 0; i < 26; i++) {
    if (!ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_A + i), false)) continue;
    consumed = true;
    app.hintTyped += (char)('a' + i);
    size_t picked = 0;
    switch (matchHint(labels, app.hintTyped, picked)) {
      case HintMatch::Exact:
        if (auto w = focusedWindow(app.tab())) {
          app.sel.window = *w;
          app.sel.element = app.hintOrder[picked];
        }
        app.hintTyped.clear();
        app.keys.mode = Mode::Normal;
        return true;
      case HintMatch::Prefix: break;
      case HintMatch::None:
        // Nothing can start with what has been typed: give up rather
        // than leaving the labels up with no way out but Escape.
        app.hintTyped.clear();
        break;
    }
  }
  return consumed;
}

// Alt tapped on its own arms the prefix; Alt used as a modifier for
// some other key does not. Anything pressed while it is down disarms
// it, so holding Alt and typing works exactly as it always did.
void trackAltTap(AppState& app) {
  ImGuiIO& io = ImGui::GetIO();
  bool altWasHeld = app.altHeld;
  app.altHeld = io.KeyAlt;
  if (app.altHeld && !altWasHeld) {
    app.altSince = ImGui::GetTime();
    app.altArmed = true;
  }
  if (app.altHeld && app.altArmed && ImGui::IsAnyItemActive())
    app.altArmed = false;
  if (!app.altHeld && altWasHeld) {
    if (app.altArmed && app.keys.mode == Mode::Normal) {
      app.keys.sticky = true;
      app.pendingSince = 0;  // the pane comes up at once: it was asked for
    }
    app.altArmed = false;
  }
}

void handleInput(AppState& app, unsigned ctx) {
  // The only thing the app decides for itself: whether the press was a
  // hint label, which needs the labels it just drew. Everything else -
  // whether it is spent, what it means, what mode it leaves behind - is
  // the machine's, in one place, in one order.
  bool captured = app.keys.mode == Mode::Hint && typeHints(app);
  trackAltTap(app);

  Chord c = readChord();
  if (c.valid() && app.altHeld) app.altArmed = false;

  KeyMachine::Step s = app.keys.dispatch(c, captured, ctx);
  switch (s.kind) {
    case KeyMachine::Step::Kind::Pending:
      app.pendingSince = ImGui::GetTime();
      break;
    case KeyMachine::Step::Kind::Fired:
      if (s.action == Action::EnterResize || s.action == Action::EnterHint ||
          s.action == Action::EnterSelect)
        app.pendingSince = ImGui::GetTime();
      applyAction(app, s);
      break;
    case KeyMachine::Step::Kind::Consumed:
    case KeyMachine::Step::Kind::None:
      break;
  }
}

void drawEmptyTab(AppState& app, const Rect& area) {
  ImGui::SetNextWindowPos(ImVec2(area.x + area.w * 0.5f - 160 * gUiScale,
                                 area.y + area.h * 0.5f - 30 * gUiScale));
  ImGui::SetNextWindowSize(ImVec2(320 * gUiScale, 0));
  ImGui::Begin("###empty", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::TextDisabled("tab %s is empty", tabLabel(app.tab()).c_str());
  ImGui::TextDisabled("Alt+o  the overview, and its list of panels");
  ImGui::TextDisabled("Alt+d  find a panel by name");
  ImGui::End();
}

// Audio behind a window nothing is showing is released, its view
// remembered first so reopening comes back where you left it.
void releaseUnusedWaves(AppState& app, const std::set<std::string>& wanted) {
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

void drawFrame(AppState& app) {
  unsigned ctx = contextBits(app);
  handleInput(app, ctx);
  ctx = contextBits(app);  // the action may have changed what is selected
  if (app.activeTab < 1) app.activeTab = 1;
  // A tab you emptied and left is not a tab any more. Named ones stay -
  // naming it says you meant to keep it - and so does the one you are
  // standing on, which is how you get an empty tab to fill.
  std::erase_if(app.tabs, [&app](const Tab& t) {
    return t.empty() && t.name.empty() && t.index != app.activeTab;
  });
  ensureTab(app.tabs, app.activeTab);

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  float width = vp->WorkSize.x, height = vp->WorkSize.y;
  drawTabBar(app, width);

  std::vector<const Binding*> wk = whichKeyRows(app, ctx);
  bool showWk = whichKeyShowing(app, wk.size());
  WhichKeyLayout wkl;
  if (showWk) wkl = layOutWhichKey(wk, width, height * 0.4f);
  float wkH = showWk ? wkl.height : 0.0f;

  Rect area{vp->WorkPos.x, vp->WorkPos.y + tabBarHeight(), width,
            std::max(60.0f, height - tabBarHeight() - wkH)};
  app.contentArea = area;
  Placement placement = place(app.tab(), area);
  drawContainerFrames(placement);

  std::set<std::string> wanted;
  for (const PlacedWindow& pw : placement.windows) drawLeaf(app, pw, wanted);
  if (placement.windows.empty()) drawEmptyTab(app, area);
  dragDividers(app, placement, area);
  releaseUnusedWaves(app, wanted);

  drawFocusRing(app);
  if (app.outline) drawOutline(app, area);
  if (app.keys.mode == Mode::Hint) drawHints(app, placement);
  if (app.keys.mode == Mode::Search) drawSearch(app, area);
  if (app.keys.mode == Mode::Rename) drawRename(app, area);
  if (app.keys.mode == Mode::Help) drawHelp(app, area, ctx);
  if (showWk) drawWhichKey(app, wk, wkl, width, area.y + area.h);
  app.deferred.clear();
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
  bool selfTestFailed = false;  // a checked expectation the run did not meet
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
  if (selfTest) {
    // A fresh shell: the smoke test must report the same thing whatever
    // layout the project was last left in. That means dropping the saved
    // tabs *and* the saved open-panel set the migration would read -
    // both are the user's, and neither is this test's business.
    app.state.ui.tabs.clear();
    app.state.ui.panels.clear();
    app.tabs.clear();
    app.activeTab = 1;
    app.adoptTabs();
    app.migratePending = true;
  }

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
  if (gUiScale != 1.0f) ImGui::GetStyle().ScaleAllSizes(gUiScale);
  // The ladder is baked with --scale already in it, so the global font
  // scale stays at 1: every size on screen is a size something was
  // actually rasterised at.
  gFonts.bake(ImGui::GetIO(), gUiScale);
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
  const ImGuiStyle restingStyle = ImGui::GetStyle();
  Uint64 last = SDL_GetPerformanceCounter();
  while (!done) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL2_ProcessEvent(&e);
      if (e.type == SDL_QUIT) done = true;
      // Only sample the OS window's geometry when it actually changed:
      // on X11 these are round trips, too costly to poll every frame.
      // Alt+Tab away and the release never reaches us, so the prefix
      // would still be armed when you came back and eat your next key.
      if (e.type == SDL_WINDOWEVENT &&
          e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        app.keys.sticky = false;
        app.altArmed = false;
        app.altHeld = false;
      }
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

    // ...and every overlay: one frame per mode, with a row selected, so
    // the smoke test draws the search, help, hint and which-key surfaces
    // as well as the tiling. An unbalanced Begin/End in any of them is
    // an assert here rather than a crash in front of the user.
    if (selfTest && frames >= 1 && frames <= 4) {
      static const Mode modes[] = {Mode::Search, Mode::Help, Mode::Hint,
                                   Mode::Resize};
      app.keys.mode = modes[frames - 1];  // one frame each
      app.outline = true;
      app.altHeld = true;
      app.altSince = -10;
      if (auto w = focusedWindow(app.tab())) {
        std::vector<WindowElement> els = app.elementsOf(*w);
        if (!els.empty()) {
          app.sel.window = *w;
          app.sel.element = els[0].name;
        }
      }
      refreshHintOrder(app);
    }

    // The one seam the unit tests cannot reach: real key events becoming
    // chords. These frames type a short script through ImGui's own input
    // queue - gather a neighbour, group the two, switch tab - and the
    // report below prints the tree it produced. A broken ImGuiKey
    // translation, or a mode that swallows its keys, fails here rather
    // than under someone's hands.
    // The overlay frames above leave the machine in whatever mode they
    // were showing; the script below types from a resting state.
    if (selfTest && frames == 5) app.keys.reset();
    if (selfTest) {
      struct Press {
        int frame;
        ImGuiKey key;
        bool alt;
        bool ctrl = false;
      };
      // Every other frame: ImGui trickles a queue that changes one key
      // twice, so the modifier released with a chord is still down on
      // the very next frame - `l` typed there would arrive as Alt+l.
      static const Press script[] = {
          {5, ImGuiKey_S, true},        // select mode
          {7, ImGuiKey_L, false},       // gather the neighbour to the right
          {9, ImGuiKey_V, false},       // group the two of them stacked
          {11, ImGuiKey_Escape, false}, // back to normal
          {13, ImGuiKey_2, true},       // over to tab 2
          // ...and back again with Alt *tapped* rather than held, which
          // is a different path through the input code: the modifier is
          // spent on its own frame and the digit arrives bare.
          {15, ImGuiKey_None, true},
          {19, ImGuiKey_1, false},
          // f, then a label that is *also* a normal-mode shortcut: the
          // press that picks a row must not reach the map behind it and
          // reopen the labels. `f` is the fourth label, so a window with
          // four or more rows exercises it.
          {21, ImGuiKey_J, true},   // focus a window with rows in it
          {23, ImGuiKey_F, false},  // label them
          {25, ImGuiKey_F, false},  // take the row labelled `f`
          {27, ImGuiKey_A, false},  // (or the first, where there is no `f`)
          // ...and the overview's own rows, which are panel tickboxes:
          // label them, take the first, and press it.
          {29, ImGuiKey_K, true},   // back up to the overview
          {31, ImGuiKey_F, false},
          {33, ImGuiKey_A, false},
          {35, ImGuiKey_Enter, false},
          // Two notches smaller, so the report can say the style came
          // back: a scale left applied would compound every frame.
          {37, ImGuiKey_Minus, false, true},
          {39, ImGuiKey_Minus, false, true},
      };
      for (const Press& k : script) {
        if (k.frame != frames) continue;
        ImGuiIO& io = ImGui::GetIO();
        // Exactly what the SDL backend sends: the modifier travels as
        // its own event, not as the Alt key going down. Down and up in
        // one frame, which ImGui applies over two - so auto-repeat never
        // fires the shortcut twice.
        if (k.key == ImGuiKey_None) {
          // Alt on its own, held down; the release comes two frames on.
          io.AddKeyEvent(ImGuiMod_Alt, true);
          io.AddKeyEvent(ImGuiKey_LeftAlt, true);
          continue;
        }
        if (k.alt) io.AddKeyEvent(ImGuiMod_Alt, true);
        if (k.ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(k.key, true);
        io.AddKeyEvent(k.key, false);
        if (k.ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
        if (k.alt) io.AddKeyEvent(ImGuiMod_Alt, false);
      }
      if (frames == 17) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiKey_LeftAlt, false);
        io.AddKeyEvent(ImGuiMod_Alt, false);
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

    if (selfTest && ++frames >= 41) done = true;
    if (!selfTest && !vsync) {
      double frameMs = (double)(SDL_GetPerformanceCounter() - now) * 1000.0 /
                       (double)SDL_GetPerformanceFrequency();
      if (frameMs < targetFrameMs)
        SDL_Delay((Uint32)(targetFrameMs - frameMs));
    }
  }

  if (selfTest) {
    // Read before the reset below: these are what the typed keys left
    // behind, and they are the point of the exercise.
    std::string endedIn = modeName(app.keys.mode);
    std::string picked = app.sel.active() ? app.sel.element : "(none)";
    size_t labelled = app.hintOrder.size();
    app.keys.reset();  // the mode cycling in the loop above is over
    app.sel.clear();
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
    // What the app would actually show, so a project that declares no
    // panels reports the one holding everything rather than nothing.
    for (auto& u : app.units)
      for (auto& p : app.panels[unitKey(u.unit)])
        std::printf("self-test: panel '%s' (%zu control(s), %zu target(s))\n",
                    p.name.c_str(), p.controls.size(), p.targets.size());
    // ...and the shell they sit in: every tab, with its tree written out
    // the way the outline overlay writes it. The layout is built fresh
    // above, so this says the same thing however the app was last left.
    std::printf("self-test: typed keys left tab %d selected\n",
                app.activeTab);
    std::printf("self-test: hints labelled %zu row(s), left mode %s, "
                "selection '%s'\n",
                labelled, endedIn.c_str(), picked.c_str());
    for (auto& [id, scale] : app.windowScales) {
      int at = gFonts.indexNear(gFonts.base * scale);
      std::printf("self-test: window '%s' draws at %.0f%% - font baked at "
                  "%.0fpx\n",
                  id.c_str(), scale * 100.0f, gFonts.sizeAt(at));
    }
    // Every step a window can reach has to land back on the size it came
    // from. A step that resolved to a different one would be drawn by
    // stretching the atlas, which is the whole thing this avoids.
    bool exact = true;
    for (int i = 0; i < gFonts.count(); i++) {
      float scale = gFonts.scaleAt(i);
      if (gFonts.indexNear(gFonts.base * scale) != i) exact = false;
    }
    std::printf("self-test: %d zoom step(s), base %.0fpx, all exact: %s\n",
                gFonts.count(), gFonts.base, exact ? "yes" : "NO");
    if (!exact) selfTestFailed = true;
    // A window drawn at its own scale must leave the style exactly as
    // it found it, or every frame would shrink the whole app a little
    // further.
    const ImGuiStyle& now = ImGui::GetStyle();
    bool intact = now.FramePadding.x == restingStyle.FramePadding.x &&
                  now.ItemSpacing.y == restingStyle.ItemSpacing.y &&
                  now.ScrollbarSize == restingStyle.ScrollbarSize &&
                  now.WindowPadding.x == restingStyle.WindowPadding.x;
    std::printf("self-test: style after scaled windows: %s\n",
                intact ? "unchanged" : "LEAKED");
    if (!intact) selfTestFailed = true;
    // What `?` would list where the app came to rest - the overlay is
    // only as good as this, and an empty one is a bug worth catching.
    for (Mode m : {Mode::Normal, Mode::Select, Mode::Resize}) {
      size_t listed = 0;
      for (const Binding* b : bindingsFor(m, CtxTiled | CtxNested))
        if (b->listed) listed++;
      std::printf("self-test: help lists %zu shortcut(s) in %s mode\n", listed,
                  modeName(m));
    }
    for (const Tab& t : app.tabs) {
      std::printf("self-test: tab %s (%zu window(s))\n", tabLabel(t).c_str(),
                  windowsIn(t).size());
      std::vector<std::string> lines;
      outlineLines(t.root, t, {}, "", true, lines);
      for (const std::string& l : lines)
        std::printf("self-test:   %s\n", l.c_str());
    }
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
  return selfTestFailed ? 1 : 0;
}
