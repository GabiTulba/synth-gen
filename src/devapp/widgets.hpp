#pragma once
#include <map>
#include <string>
#include <vector>

#include "layout.hpp"
#include "metadata.hpp"
#include "player.hpp"
#include "wav.hpp"
#include "projectstate.hpp"
#include "waveform.hpp"

namespace synth::devapp {

// The pieces the dev app draws inside a window: the live-control widgets
// (slider, knob, tickbox, option list, linked lanes) and the waveform
// canvas, together with the per-unit editing state they read and write.
// The shell around them - tabs, tiling, focus, search - is app.cpp's.

// UI scale factor (--scale). ImGui's style/font scaling covers most of
// the layout; this covers the few sizes the app picks in raw pixels
// (knob diameter, slider width, wave heights).
extern float gUiScale;

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

// The control values worth recording: everything that differs from its
// declaration's default. The same set goes to the project state file and
// to the build's controls.json, so the two can be compared directly.
std::map<std::string, double> unitOverrides(const UnitState& u);

// Writes the unit's override file from the UI values. Also called with
// everything back at defaults - the (empty) overrides object still
// reaches the daemon and rebuilds.
void writeUnitOverrides(UnitState& u);

// Marks a control edited and pushes it to the daemon. Mid-drag writes
// make the sound track the drag; the throttle keeps a fast drag from
// flooding the daemon with rebuilds, and the release write always lands.
void noteControlEdit(UnitState& u, ControlUi& ui, bool released);

// Puts a control back to the value its declaration gives, and tells the
// daemon. The keyboard and the `reset` button share it.
void resetControl(UnitState& u, const ControlMeta& c);

std::string formatSeconds(double s);

// One artifact's waveform: the decoded WAV, per-channel peak bins, and
// the view state (zoom, selection, loop) laid over them. Drawn inside
// whichever window names its target - there is one of these per
// artifact, not per window - and reloaded when a rebuild rewrites it.
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
  bool loop = false;                  // replay the played range

  void open(const std::string& path, const std::string& name);
  // Reopens from saved UI state, clamped to whatever length the file has
  // now; a range that no longer makes sense falls back to a fitted view.
  void restore(const std::string& path, const WavePanelState& st);
  void load();
  // A rebuild rewrote (or removed) the artifact. `force` reloads even
  // when the stamp looks unchanged - a same-sized rewrite inside the
  // filesystem's mtime granularity is invisible to it.
  void reloadIfChanged(bool force);
  bool hasSelection() const { return selStart >= 0 && selEnd > selStart; }
};

// The key a row answers to, drawn in a gutter at the head of the row
// and followed by whatever the row draws. Every row starts with one, so
// the keys line up in a column down the left of the window and a row
// you can address is never one you have to hunt for. Empty draws the
// gutter and nothing in it, keeping the rows aligned.
void keyGutter(const std::string& key);
// The width that gutter takes, for anything laying out around it.
float keyGutterWidth();

// One ungrouped control: a knob, slider, tickbox or option list, its
// name, the pending marker and its reset button. Returns true while it
// is waiting on a rebuild.
bool drawOneControl(UnitState& u, const ControlMeta& c,
                    const std::string& key = {});

// One multi_slider group: lanes that share a budget for their sum, each
// drawn against the band the others leave it. The heading answers to no
// key of its own - there is nothing to select in it - so only the lanes
// are keyed: `laneKeys` is the key each answers to, in lane order, and
// `laneRects` collects where they landed, a lane being a row like any
// other, selected and nudged on its own.
bool drawControlGroup(UnitState& u, const std::vector<ControlMeta>& controls,
                      size_t first, size_t count,
                      const std::vector<std::string>* laneKeys = nullptr,
                      std::map<std::string, Rect>* laneRects = nullptr);

// The lanes of one group, found by name. False when no such group
// exists, which is how a caller tells a group member from a control.
bool drawGroupByName(UnitState& u, const std::vector<ControlMeta>& controls,
                     const std::string& group, bool& anyDirty,
                     const std::vector<std::string>* laneKeys = nullptr,
                     std::map<std::string, Rect>* laneRects = nullptr);

// The waveform: min/max envelope lanes per channel, wheel zoom about the
// cursor, right-drag pan, left-drag selection, and playback of the
// selection or the visible range. `availY` is the vertical room to take
// and `availX` the horizontal, each -1 to fill what is left of the
// window. A panel wide enough to scroll sideways passes its own width
// rather than the visible one, so scrolling reveals more of the
// waveform instead of carrying it off the left edge.
void drawWaveContent(AudioPlayer& player, std::string& playError,
                     WavePanel& p, float availY = -1.0f,
                     float availX = -1.0f);

}  // namespace synth::devapp
