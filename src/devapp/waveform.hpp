#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace synth::devapp {

// The interactive waveform view's model (§9): zoom/pan and envelope math,
// pure and unit-tested without a UI. app.cpp only does the ImGui drawing
// on top of these.

// The visible window over a waveform, in fractional frames. Zooming keeps
// the frame under the cursor fixed; panning and zooming clamp to the file
// and to a minimum span so the view can't invert or vanish.
struct WaveView {
  int64_t frames = 0;  // total frames in the file
  double start = 0;    // visible range [start, end), fractional frames
  double end = 0;

  static constexpr double kMinSpan = 32;

  void reset(int64_t totalFrames) {
    frames = totalFrames;
    start = 0;
    end = (double)totalFrames;
  }
  double span() const { return end - start; }

  // `frac` in [0, 1] is the anchor's horizontal position within the view
  // (the frame under it stays put); `factor` > 1 zooms out, < 1 zooms in.
  void zoomAt(double frac, double factor);
  void pan(double deltaFrames);
  void clamp();
};

// The selection over a waveform, and the keyboard machine that places
// it. Enter opens the machine on a single head; the arrows walk that
// head, dragging the view along when it reaches an edge; Enter fixes it
// and opens the second; Enter again settles the pair. Escape puts back
// whatever was selected before it opened. The mouse writes `start` and
// `end` directly and leaves the machine alone.
struct WaveSelection {
  double start = -1, end = -1;  // the settled range, in frames; -1 = none

  enum class Phase {
    Off,     // nothing being placed
    First,   // the first head is under the cursor
    Second,  // the first is fixed at `anchor`, the second is the cursor
  };
  Phase phase = Phase::Off;
  double anchor = 0;  // the head the first Enter fixed
  double cursor = 0;  // the head the arrows move
  double heldStart = -1, heldEnd = -1;  // what Escape puts back

  bool has() const { return start >= 0 && end > start; }
  void clear() { start = end = -1; }
  bool placing() const { return phase != Phase::Off; }

  // Opens on the selection's left edge when there is one and on the
  // middle of the view when there is not.
  void begin(const WaveView& v);
  // Enter again: First fixes the head and opens the second, Second
  // settles the pair. True once the pair is settled and this is over.
  bool advance();
  void cancel();
  // One nudge of the moving head. It stays inside the file and pulls
  // the view after it rather than walking off the edge of it.
  void moveCursor(double deltaFrames, WaveView& v);
};

// Precomputed per-channel min/max bins so a fully zoomed-out draw over a
// long file doesn't rescan every raw sample each frame.
struct PeakBins {
  int binSize = 1024;
  std::vector<float> mins, maxs;  // one entry per binSize frames
};
PeakBins buildPeakBins(const std::vector<double>& channel, int binSize = 1024);

// The min/max envelope of one channel across `columns` equal slices of
// [viewStart, viewEnd) — one pair per drawn pixel column. Uses `bins`
// when a column spans many frames and scans raw samples otherwise, so it
// stays cheap at every zoom level.
std::vector<std::pair<float, float>> minMaxColumns(
    const std::vector<double>& channel, const PeakBins& bins,
    double viewStart, double viewEnd, int columns);

}  // namespace synth::devapp
