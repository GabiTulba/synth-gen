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

}  // namespace synth
