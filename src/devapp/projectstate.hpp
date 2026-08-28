#pragma once
#include <map>
#include <string>
#include <vector>

#include "layout.hpp"

namespace synth::devapp {

// The project's own settings file - `project.json` beside `build.json`
// (MetadataLayout::projectStatePath). Everything in it is the user's,
// not the build's: where the windows are, which waveforms are open, and
// where the knobs are set. It is the durable record; the build's
// `_build/<unit>/controls.json` is a projection of the control values
// from it, kept in step because that is the file a `synthc watch`
// daemon actually reads.
//
// Nothing here is a build input, so a missing or corrupt file only costs
// you the layout - never a build.

// One waveform window to reopen on the next run. `artifact` is stored
// relative to the layout root - the same form the build's metadata uses -
// so a panel survives the tree being moved or checked out elsewhere.
struct WavePanelState {
  std::string artifact;
  std::string target;
  // The zoom window, in frames. An empty/degenerate range means "fit",
  // which is also what a restore falls back to when the artifact's
  // length changed under us.
  double viewStart = 0, viewEnd = 0;
  double selStart = -1, selEnd = -1;  // -1 = no selection
  bool loop = false;
  bool operator==(const WavePanelState&) const = default;
};

// The OS window's own placement. `valid` is false until we have seen a
// real window, so a first run keeps the built-in default size.
struct WindowGeometry {
  bool valid = false;
  int x = 0, y = 0, w = 0, h = 0;
  bool operator==(const WindowGeometry&) const = default;
};

struct UiState {
  WindowGeometry window;
  // Dear ImGui's own settings dump (window positions, sizes, collapsed
  // state, table columns) - saved verbatim as ImGui writes it, and handed
  // straight back to LoadIniSettingsFromMemory on the next run. We carry
  // it here instead of letting ImGui manage an imgui.ini so it stays with
  // the project rather than with the working directory.
  std::string imguiIni;
  // Waveform view state (zoom, selection, loop) by artifact, for every
  // target a panel has shown. Not a list of open windows: waveforms live
  // inside panels, so this is only what you had set up inside them.
  std::vector<WavePanelState> waves;
  // Collapsing-header open/closed state, by "<unit>/<section>". ImGui
  // deliberately does not persist tree state in its ini, so the app
  // tracks the few headers it owns itself.
  std::map<std::string, bool> sections;
  // The tiling shell: the tabs, their trees, and which one was on
  // screen. This is where every window is and how big it is - the app
  // places them itself, so ImGui's ini has no say over any of it.
  std::vector<Tab> tabs;
  int activeTab = 1;
  bool outline = false;   // the tree outline overlay was open
  bool whichKey = true;   // the which-key pane was on
  // How big each window draws its own contents, by window id, for the
  // ones that are not at 1. A panel of tiny numbers you lean into is
  // worth remembering.
  std::map<std::string, double> windowScales;
  // Which panel windows are open, by "<unit>/<panel>". Derived from
  // `tabs` now and written for what reads it: a settings file from
  // before tabs existed, which is migrated into tab 1, and an older
  // build of the app, which still finds what it expects. A panel absent
  // from the map has never been touched and opens by default.
  std::map<std::string, bool> panels;
  bool operator==(const UiState&) const = default;
};

struct ProjectState {
  // Control values, per unit ("." for a standalone project, else the
  // root's rule path), holding only what differs from the declaration's
  // default - exactly the set controls.json carries. Defaults live in
  // the .synth source; this file records departures from them.
  std::map<std::string, std::map<std::string, double>> controls;
  UiState ui;
  bool operator==(const ProjectState&) const = default;
};

struct ProjectStateLoad {
  // False when the file is absent or unreadable as JSON. The caller
  // needs to tell those apart from an empty-but-valid file: a project
  // that has no state yet adopts whatever controls.json already holds
  // instead of overwriting it.
  bool found = false;
  ProjectState state;
};

ProjectStateLoad loadProjectState(const std::string& path);

// Atomically writes the state, indented, with numbers at their shortest
// round-tripping form - it sits in the source tree next to build.json,
// so it has to survive being read and diffed by a person.
bool saveProjectState(const std::string& path, const ProjectState& state,
                      std::string& error);

}  // namespace synth::devapp
