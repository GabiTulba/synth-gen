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
  // toFrame means end of file); with `loop` the range replays
  // indefinitely until stop() or setLooping(false).
  bool play(const std::string& wavPath, std::string& error);
  bool playRange(const std::string& wavPath, int64_t fromFrame,
                 int64_t toFrame, std::string& error, bool loop = false);
  void stop();

  // Toggle looping of the currently playing range; turning it off lets
  // the already-queued audio drain and then stops as usual.
  void setLooping(bool loop) { loop_ = loop; }
  bool looping() const { return loop_; }

  // Call once per UI frame: keeps a looping range's queue topped up, and
  // releases the device when (non-looping) playback finishes.
  void update();

  // A rebuild rewrote the playing artifact: re-read it and cut the loop
  // over to the new audio immediately, at the same loop phase (dropping
  // the queued old audio; a small splice discontinuity beats waiting a
  // whole loop to hear the change). The played range stays the same,
  // clamped to the new length. Non-looping playback is left alone (its
  // audio is already queued in full), and a file that can't be re-read
  // keeps the old audio.
  void reloadIfLooping();

  bool playing() const { return dev_ != 0; }
  double progress() const;  // 0..1 through the range (wraps when looping)
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
  size_t totalBytes_ = 0;   // one copy of the playing range
  size_t queuedBytes_ = 0;  // total ever queued (grows while looping)
  bool loop_ = false;
  std::vector<float> data_;  // the range, kept for loop re-queueing
  std::string path_;
  int64_t fromFrame_ = 0, toFrame_ = 0;  // the playing range, post-clamp
  double rate_ = 0;    // the spec the device was opened with, for
  int channels_ = 0;   // detecting a format change on reload
  double rangeStartSec_ = 0;
  double rangeEndSec_ = 0;
};

}  // namespace synth::devapp
