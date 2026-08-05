#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "wav.hpp"

namespace synth::devapp {

// Converts decoded WAV channels to the interleaved float32 stream SDL
// plays. Pure; unit-tested without any audio device.
std::vector<float> interleaveToFloat(const WavData& w);

// Plays one artifact at a time through SDL audio. The device is opened
// per-play to match the artifact's rate/channel count and the whole file
// is queued up front — artifacts are finite Samples, so streaming
// machinery isn't needed in v1.
class AudioPlayer {
 public:
  ~AudioPlayer();

  // Starts playing `wavPath`; stops any current playback first. Returns
  // false (with `error` set) if the file can't be read or no audio device
  // is available.
  bool play(const std::string& wavPath, std::string& error);
  void stop();

  // Call once per UI frame: releases the device when playback finishes.
  void update();

  bool playing() const { return dev_ != 0; }
  double progress() const;  // 0..1 of the queued audio consumed
  const std::string& currentPath() const { return path_; }

 private:
  uint32_t dev_ = 0;  // SDL_AudioDeviceID
  size_t totalBytes_ = 0;
  std::string path_;
};

}  // namespace synth::devapp
