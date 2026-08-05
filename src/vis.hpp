#pragma once
#include <string>

#include "signal.hpp"

namespace synth {

// Renders a discretized window as a waveform image (SVG - dependency-free
// and viewable anywhere, including straight from a git host). One lane per
// channel, classic min/max-per-column drawing, with a header showing the
// target's basic facts.
std::string renderWaveformSvg(const std::string& name, const Rendered& r,
                              double rate);

// One SVG with a labeled lane per entry (e.g. a song's master over its
// composing buses). Lanes share a common time axis (the longest entry);
// multi-channel entries draw as min/max across their channels. Lane
// colors cycle through a small palette.
std::string renderStackedWaveformSvg(
    const std::string& title,
    const std::vector<std::pair<std::string, Rendered>>& lanes, double rate);

}  // namespace synth
