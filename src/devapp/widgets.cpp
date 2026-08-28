#include "widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "imgui.h"

namespace synth::devapp {

float gUiScale = 1.0f;

// The control values worth recording: everything that differs from its
// declaration's default. The same set goes to the project state file and
// to the build's controls.json, so the two can be compared directly.
std::string rowLabel(const std::string& key, const std::string& name) {
  return key.empty() ? name : "[" + key + "] " + name;
}

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
void WavePanel::open(const std::string& path, const std::string& name) {
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
void WavePanel::restore(const std::string& path, const WavePanelState& st) {
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

void WavePanel::load() {
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
void WavePanel::reloadIfChanged(bool force) {
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

std::string formatSeconds(double s) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3fs", s);
  return buf;
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
void resetControl(UnitState& u, const ControlMeta& c) {
  ControlUi& ui = u.controlUi[c.name];
  ui.value = (float)c.def;
  ui.editing = false;
  ui.dirty = true;
  writeUnitOverrides(u);
}

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
  if (ImGui::SmallButton("reset")) resetControl(u, c);
  return dirty;
}

// One multi_slider group: lanes that share a budget for their sum. Each
// lane is drawn against the band the other lanes leave it
// (controlLaneBand), and dragging clamps to that band - so the group
// invariant holds after every drag without any lane moving on its own.
// Returns true if any lane is waiting on a rebuild.
bool drawControlGroup(UnitState& u, const std::vector<ControlMeta>& controls,
                      size_t first, size_t count, const std::string& key,
                      const std::vector<std::string>* laneKeys,
                      std::map<std::string, Rect>* laneRects) {
  const ControlMeta& head = controls[first];
  double sum = 0;
  for (size_t k = 0; k < count; k++)
    sum += u.controlUi[controls[first + k].name].value;

  ImGui::PushID(head.group.c_str());
  float width = std::max(200.0f * gUiScale,
                         ImGui::GetContentRegionAvail().x * 0.45f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "%s",
                     rowLabel(key, head.group).c_str());
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
    ImVec2 laneTop = ImGui::GetCursorScreenPos();
    float laneW = std::max(16.0f, ImGui::GetContentRegionAvail().x);
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
    // The lane's own name; the group already named itself above. A lane
    // is a row of its own, so it wears its own key.
    const char* lane = c.name.c_str() + head.group.size() + 1;
    std::string laneKey =
        laneKeys && k < laneKeys->size() ? (*laneKeys)[k] : std::string();
    ImGui::Text("%s  %.4g", rowLabel(laneKey, lane).c_str(),
                (double)ui.value);
    if (hi < c.max - 1e-6) {
      ImGui::SameLine();
      ImGui::TextDisabled("(<= %.4g)", (double)hi);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("the other lanes have spoken for the rest of the "
                          "budget;\nlower one of them to take more here");
    }
    anyDirty |= drawControlTail(u, ui, c);
    if (laneRects) {
      ImVec2 after = ImGui::GetCursorScreenPos();
      (*laneRects)[c.name] =
          Rect{laneTop.x, laneTop.y, laneW, std::max(2.0f, after.y - laneTop.y)};
    }
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
bool drawOneControl(UnitState& u, const ControlMeta& c,
                    const std::string& key) {
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
      ImGui::Text("%s  %.4g", rowLabel(key, c.name).c_str(), (double)ui.value);
    } else if (c.kind == "toggle") {
      // A tickbox has no drag: the click that flips it is also the end
      // of the edit, so the override write lands immediately.
      bool on = ui.value >= 0.5f;
      if (ImGui::Checkbox("##toggle", &on)) {
        ui.value = on ? 1.0f : 0.0f;
        edited = released = true;
      }
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(rowLabel(key, c.name).c_str());
    } else if (c.kind == "choice") {
      // One tickbox per option, the value their index. Labels come from
      // the build; "##k" keeps two same-named options apart for ImGui
      // without showing anything extra.
      int pick = (int)std::lround((double)ui.value);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(rowLabel(key, c.name).c_str());
      for (size_t k = 0; k < c.options.size(); k++) {
        ImGui::SameLine();
        std::string label = c.options[k] + "##" + std::to_string(k);
        if (ImGui::RadioButton(label.c_str(), &pick, (int)k)) {
          ui.value = (float)pick;
          edited = released = true;
        }
      }
    } else if (c.kind == "int_slider") {
      ImGui::SetNextItemWidth(
          std::max(160.0f * gUiScale,
                   ImGui::GetContentRegionAvail().x * 0.45f));
      int v = (int)std::lround((double)ui.value);
      if (ImGui::SliderInt("##int", &v, (int)c.min, (int)c.max)) {
        ui.value = (float)v;
        edited = true;
      }
      released = ImGui::IsItemDeactivatedAfterEdit();
      ImGui::SameLine();
      ImGui::TextUnformatted(rowLabel(key, c.name).c_str());
    } else {
      ImGui::SetNextItemWidth(
          std::max(160.0f * gUiScale,
                   ImGui::GetContentRegionAvail().x * 0.45f));
      edited = ImGui::SliderFloat("##slider", &ui.value, (float)c.min,
                                  (float)c.max, "%.4g");
      released = ImGui::IsItemDeactivatedAfterEdit();
      ImGui::SameLine();
      ImGui::TextUnformatted(rowLabel(key, c.name).c_str());
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
                     const std::string& group, bool& anyDirty,
                     const std::string& key,
                     const std::vector<std::string>* laneKeys,
                     std::map<std::string, Rect>* laneRects) {
  for (size_t i = 0; i < controls.size(); i++) {
    if (controls[i].group != group) continue;
    size_t n = 1;
    while (i + n < controls.size() && controls[i + n].group == group) n++;
    anyDirty |= drawControlGroup(u, controls, i, n);
    return true;
  }
  return false;
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
void drawWaveContent(AudioPlayer& player, std::string& playError,
                     WavePanel& p, float availY) {
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
      player.playing() && player.currentPath() == p.artifactPath;
  if (isThisPlaying) {
    if (ImGui::SmallButton("stop")) player.stop();
  } else {
    if (ImGui::SmallButton(p.hasSelection() ? "play selection"
                                            : "play view")) {
      double from = p.hasSelection() ? p.selStart : v.start;
      double to = p.hasSelection() ? p.selEnd : v.end;
      playError.clear();
      if (!player.playRange(p.artifactPath, (int64_t)std::floor(from),
                                (int64_t)std::ceil(to), playError,
                                p.loop) &&
          !playError.empty())
        playError = p.targetName + ": " + playError;
    }
  }
  ImGui::SameLine();
  // Replays the played range (the selection, usually) until stopped;
  // toggling mid-play applies to the running playback too.
  if (ImGui::Checkbox("loop", &p.loop) && isThisPlaying)
    player.setLooping(p.loop);
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
  // The canvas owns the wheel while the pointer is on it: a window that
  // scrolls sits around every waveform now, and zooming must not scroll
  // it out from under the cursor at the same time.
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
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
    double f = player.positionSeconds() * rate;
    if (f >= v.start && f <= v.end) {
      float x = frameToX(f);
      dl->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + canvasSize.y),
                  IM_COL32(255, 230, 120, 220));
    }
  }
}
}  // namespace synth::devapp
