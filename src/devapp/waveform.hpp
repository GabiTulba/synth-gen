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
