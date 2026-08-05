#include "vis.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace synth {

namespace {

std::string xmlEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string fmt(double v, int precision = 1) {
  std::ostringstream ss;
  ss.precision(precision);
  ss << std::fixed << v;
  return ss.str();
}

}  // namespace

std::string renderWaveformSvg(const std::string& name, const Rendered& r,
                              double rate) {
  const int width = 1200;
  const int laneHeight = 200;
  const int laneGap = 12;
  const int headerHeight = 34;
  const int margin = 12;
  const int plotWidth = width - 2 * margin;
  const int channels = std::max(1, r.channels);
  const int height =
      headerHeight + channels * laneHeight + (channels - 1) * laneGap + margin;
  const double durationSeconds = rate > 0 ? (double)r.frames / rate : 0;

  std::ostringstream svg;
  svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width
      << "\" height=\"" << height << "\" viewBox=\"0 0 " << width << " "
      << height << "\">\n";
  svg << "<rect width=\"100%\" height=\"100%\" fill=\"#14141a\"/>\n";
  svg << "<text x=\"" << margin << "\" y=\"22\" fill=\"#d8d8e0\" "
      << "font-family=\"monospace\" font-size=\"14\">" << xmlEscape(name)
      << "  -  " << fmt(durationSeconds, 3) << "s @ " << fmt(rate, 0)
      << " Hz, " << channels << " channel" << (channels == 1 ? "" : "s")
      << "</text>\n";

  for (int c = 0; c < channels; c++) {
    int top = headerHeight + c * (laneHeight + laneGap);
    double mid = top + laneHeight / 2.0;
    double half = laneHeight / 2.0 - 4.0;

    svg << "<rect x=\"" << margin << "\" y=\"" << top << "\" width=\""
        << plotWidth << "\" height=\"" << laneHeight
        << "\" fill=\"#1b1b24\"/>\n";
    // Zero line.
    svg << "<line x1=\"" << margin << "\" y1=\"" << fmt(mid) << "\" x2=\""
        << margin + plotWidth << "\" y2=\"" << fmt(mid)
        << "\" stroke=\"#34343f\" stroke-width=\"1\"/>\n";
    // Second marks along the zero line.
    for (int s = 1; s < (int)durationSeconds + 1; s++) {
      double x = margin + plotWidth * ((double)s / durationSeconds);
      if (x >= margin + plotWidth) break;
      svg << "<line x1=\"" << fmt(x) << "\" y1=\"" << top + laneHeight - 8
          << "\" x2=\"" << fmt(x) << "\" y2=\"" << top + laneHeight
          << "\" stroke=\"#4a4a58\" stroke-width=\"1\"/>\n";
    }

    // Min/max per pixel column - the classic waveform drawing that stays
    // faithful at any zoom level.
    svg << "<path stroke=\"#6cc5f0\" stroke-width=\"1\" fill=\"none\" d=\"";
    for (int x = 0; x < plotWidth; x++) {
      int64_t lo = r.frames * x / plotWidth;
      int64_t hi = std::max(lo + 1, r.frames * (x + 1) / plotWidth);
      double vmin = 1e9, vmax = -1e9;
      for (int64_t f = lo; f < hi && f < r.frames; f++) {
        double v = r.interleaved[(size_t)(f * r.channels + c)];
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
      }
      if (vmin > vmax) continue;
      double clampedMin = std::clamp(vmin, -1.0, 1.0);
      double clampedMax = std::clamp(vmax, -1.0, 1.0);
      svg << "M" << margin + x << " " << fmt(mid - clampedMax * half)
          << "L" << margin + x << " " << fmt(mid - clampedMin * half + 0.5)
          << "";
    }
    svg << "\"/>\n";
  }
  svg << "</svg>\n";
  return svg.str();
}

}  // namespace synth
