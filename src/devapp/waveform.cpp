#include "waveform.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace synth::devapp {

void WaveView::zoomAt(double frac, double factor) {
  double anchor = start + frac * span();
  double newSpan = span() * factor;
  if (newSpan < kMinSpan) newSpan = kMinSpan;
  if (newSpan > (double)frames) newSpan = (double)frames;
  start = anchor - frac * newSpan;
  end = start + newSpan;
  clamp();
}

void WaveView::pan(double deltaFrames) {
  start += deltaFrames;
  end += deltaFrames;
  clamp();
}

void WaveView::clamp() {
  double s = span();
  if (s > (double)frames || s < 0) {
    start = 0;
    end = (double)frames;
    return;
  }
  if (start < 0) {
    start = 0;
    end = s;
  }
  if (end > (double)frames) {
    end = (double)frames;
    start = end - s;
  }
}

PeakBins buildPeakBins(const std::vector<double>& channel, int binSize) {
  PeakBins b;
  b.binSize = std::max(1, binSize);
  size_t binCount = (channel.size() + b.binSize - 1) / b.binSize;
  b.mins.assign(binCount, 0.0f);
  b.maxs.assign(binCount, 0.0f);
  for (size_t i = 0; i < binCount; i++) {
    size_t from = i * (size_t)b.binSize;
    size_t to = std::min(channel.size(), from + (size_t)b.binSize);
    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    for (size_t f = from; f < to; f++) {
      float v = (float)channel[f];
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    b.mins[i] = lo;
    b.maxs[i] = hi;
  }
  return b;
}

std::vector<std::pair<float, float>> minMaxColumns(
    const std::vector<double>& channel, const PeakBins& bins,
    double viewStart, double viewEnd, int columns) {
  std::vector<std::pair<float, float>> out(
      (size_t)std::max(columns, 0), {0.0f, 0.0f});
  if (columns <= 0 || channel.empty() || viewEnd <= viewStart) return out;
  double perColumn = (viewEnd - viewStart) / columns;
  // Bins trade edge precision for speed; only worth it when a column
  // covers at least a couple of them.
  bool useBins = !bins.mins.empty() && perColumn >= 2.0 * bins.binSize;
  int64_t last = (int64_t)channel.size();
  for (int c = 0; c < columns; c++) {
    double f0 = viewStart + c * perColumn;
    int64_t a = (int64_t)std::floor(f0);
    int64_t b = (int64_t)std::ceil(f0 + perColumn);
    a = std::clamp<int64_t>(a, 0, last - 1);
    b = std::clamp<int64_t>(b, a + 1, last);
    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    if (useBins) {
      int64_t b0 = a / bins.binSize;
      int64_t b1 = std::min((b - 1) / bins.binSize,
                            (int64_t)bins.mins.size() - 1);
      for (int64_t i = b0; i <= b1; i++) {
        lo = std::min(lo, bins.mins[i]);
        hi = std::max(hi, bins.maxs[i]);
      }
    } else {
      for (int64_t i = a; i < b; i++) {
        float v = (float)channel[i];
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
    }
    out[c] = {lo, hi};
  }
  return out;
}

}  // namespace synth::devapp
