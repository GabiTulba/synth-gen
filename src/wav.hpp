#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace synth {

struct WavData {
  double rate = 0;
  std::vector<std::vector<double>> channels;  // [-1, 1] nominal range
  int64_t frames() const {
    return channels.empty() ? 0 : (int64_t)channels[0].size();
  }
};

// Reads a RIFF/WAVE file (PCM 16/24/32-bit and IEEE float 32/64-bit).
// Throws std::runtime_error with a descriptive message on failure.
WavData readWav(const std::string& path);

// Writes 16-bit PCM, clamping to [-1, 1]. `interleaved` holds
// frames*channels doubles. Throws std::runtime_error on I/O failure.
void writeWav(const std::string& path, double rate, int channelCount,
              const std::vector<double>& interleaved);

}  // namespace synth
