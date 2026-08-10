#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "wav.hpp"

namespace synth::devapp {

// Converts decoded WAV channels (or the frame range [fromFrame, toFrame)
// of them) to the interleaved float32 stream SDL plays. Pure; unit-tested
// without any audio device.
std::vector<float> interleaveToFloat(const WavData& w);
std::vector<float> interleaveToFloat(const WavData& w, int64_t fromFrame,
                                     int64_t toFrame);

// Plays one artifact at a time through SDL audio. The device is opened
// per-play to match the artifact's rate/channel count and the whole file
// is queued up front — artifacts are finite Samples, so streaming
// machinery isn't needed in v1.
class AudioPlayer {
 public:
  ~AudioPlayer();

  // Starts playing `wavPath`; stops any current playback first. Returns
  // false (with `error` set) if the file can't be read or no audio device
  // is available. playRange plays only [fromFrame, toFrame) (a negative
  // toFrame means end of file).
  bool play(const std::string& wavPath, std::string& error);
  bool playRange(const std::string& wavPath, int64_t fromFrame,
                 int64_t toFrame, std::string& error);
  void stop();

  // Call once per UI frame: releases the device when playback finishes.
  void update();

  bool playing() const { return dev_ != 0; }
  double progress() const;  // 0..1 of the queued audio consumed
  const std::string& currentPath() const { return path_; }

  // The playing range in file time, for drawing a playhead: position
  // moves from rangeStartSeconds() to rangeEndSeconds() as progress goes
  // 0 -> 1. All three return 0 when nothing is playing.
  double rangeStartSeconds() const { return rangeStartSec_; }
  double rangeEndSeconds() const { return rangeEndSec_; }
  double positionSeconds() const {
    return rangeStartSec_ + progress() * (rangeEndSec_ - rangeStartSec_);
  }

 private:
  uint32_t dev_ = 0;  // SDL_AudioDeviceID
  size_t totalBytes_ = 0;
  std::string path_;
  double rangeStartSec_ = 0;
  double rangeEndSec_ = 0;
};

}  // namespace synth::devapp
