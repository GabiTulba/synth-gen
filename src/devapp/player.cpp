#include "player.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>

namespace synth::devapp {

std::vector<float> interleaveToFloat(const WavData& w) {
  return interleaveToFloat(w, 0, w.frames());
}

std::vector<float> interleaveToFloat(const WavData& w, int64_t fromFrame,
                                     int64_t toFrame) {
  size_t channelCount = w.channels.size();
  fromFrame = std::clamp<int64_t>(fromFrame, 0, w.frames());
  toFrame = std::clamp<int64_t>(toFrame, fromFrame, w.frames());
  size_t frames = (size_t)(toFrame - fromFrame);
  std::vector<float> out(frames * channelCount);
  for (size_t f = 0; f < frames; f++)
    for (size_t c = 0; c < channelCount; c++)
      out[f * channelCount + c] = (float)w.channels[c][fromFrame + f];
  return out;
}

AudioPlayer::~AudioPlayer() { stop(); }

bool AudioPlayer::play(const std::string& wavPath, std::string& error) {
  return playRange(wavPath, 0, -1, error);
}

bool AudioPlayer::playRange(const std::string& wavPath, int64_t fromFrame,
                            int64_t toFrame, std::string& error, bool loop) {
  stop();
  WavData w;
  try {
    w = readWav(wavPath);
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
  if (toFrame < 0 || toFrame > w.frames()) toFrame = w.frames();
  fromFrame = std::clamp<int64_t>(fromFrame, 0, toFrame);
  if (w.channels.empty() || toFrame <= fromFrame) {
    error = "artifact range is empty";
    return false;
  }
  if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    error = SDL_GetError();
    return false;
  }
  SDL_AudioSpec want{};
  want.freq = (int)std::lround(w.rate);
  want.format = AUDIO_F32SYS;
  want.channels = (Uint8)w.channels.size();
  want.samples = 4096;
  SDL_AudioSpec have{};
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (dev == 0) {
    error = SDL_GetError();
    return false;
  }
  std::vector<float> data = interleaveToFloat(w, fromFrame, toFrame);
  if (SDL_QueueAudio(dev, data.data(),
                     (Uint32)(data.size() * sizeof(float))) != 0) {
    error = SDL_GetError();
    SDL_CloseAudioDevice(dev);
    return false;
  }
  SDL_PauseAudioDevice(dev, 0);
  dev_ = dev;
  totalBytes_ = data.size() * sizeof(float);
  queuedBytes_ = totalBytes_;
  loop_ = loop;
  // Kept even when not (yet) looping: setLooping(true) mid-play needs
  // the range to re-queue from.
  data_ = std::move(data);
  path_ = wavPath;
  fromFrame_ = fromFrame;
  toFrame_ = toFrame;
  rate_ = w.rate;
  channels_ = (int)w.channels.size();
  rangeStartSec_ = w.rate > 0 ? (double)fromFrame / w.rate : 0;
  rangeEndSec_ = w.rate > 0 ? (double)toFrame / w.rate : 0;
  return true;
}

void AudioPlayer::stop() {
  if (dev_ != 0) {
    SDL_CloseAudioDevice(dev_);
    dev_ = 0;
  }
  totalBytes_ = 0;
  queuedBytes_ = 0;
  loop_ = false;
  data_.clear();
  data_.shrink_to_fit();
  path_.clear();
  fromFrame_ = toFrame_ = 0;
  rate_ = 0;
  channels_ = 0;
  rangeStartSec_ = rangeEndSec_ = 0;
}

void AudioPlayer::reloadIfLooping() {
  if (dev_ == 0 || !loop_) return;
  WavData w;
  try {
    w = readWav(path_);
  } catch (const std::exception&) {
    return;  // keep looping the old audio
  }
  // The device is fixed to the original rate/channel spec; a format
  // change needs a fresh device, so restart the playback (the range
  // resets to its beginning - rare enough not to matter).
  if (w.rate != rate_ || (int)w.channels.size() != channels_) {
    std::string path = path_;
    int64_t from = fromFrame_, to = toFrame_;
    std::string ignored;
    playRange(path, from, to, ignored, true);
    return;
  }
  int64_t to = std::min<int64_t>(toFrame_, w.frames());
  std::vector<float> data = interleaveToFloat(w, fromFrame_, to);
  if (data.empty()) {  // the file shrank past the range; nothing to loop
    stop();
    return;
  }
  // Swap the re-queue buffer; the already-queued old copies drain first,
  // so the new audio starts at a loop boundary. If the range's length
  // changed, the playhead is approximate until those old copies drain
  // (progress() divides by the new length).
  data_ = std::move(data);
  totalBytes_ = data_.size() * sizeof(float);
  rangeEndSec_ = rate_ > 0 ? (double)to / rate_ : 0;
}

void AudioPlayer::update() {
  if (dev_ == 0) return;
  size_t remaining = SDL_GetQueuedAudioSize(dev_);
  if (loop_ && totalBytes_ > 0) {
    // Keep at least a quarter second (and at least one full copy of the
    // range) queued ahead, so short selections survive frame hiccups.
    double rate = rangeEndSec_ > rangeStartSec_
                      ? totalBytes_ / (rangeEndSec_ - rangeStartSec_)
                      : 0;
    size_t minAhead = std::max(totalBytes_, (size_t)(rate * 0.25));
    while (remaining < minAhead) {
      if (SDL_QueueAudio(dev_, data_.data(), (Uint32)totalBytes_) != 0) break;
      remaining += totalBytes_;
      queuedBytes_ += totalBytes_;
    }
  } else if (remaining == 0) {
    stop();
  }
}

double AudioPlayer::progress() const {
  if (dev_ == 0 || totalBytes_ == 0) return 0;
  double remaining = (double)SDL_GetQueuedAudioSize(dev_);
  double consumed = (double)queuedBytes_ - remaining;
  if (queuedBytes_ <= totalBytes_)  // never looped: plain 0..1
    return std::clamp(consumed / (double)totalBytes_, 0.0, 1.0);
  return std::fmod(std::max(consumed, 0.0), (double)totalBytes_) /
         (double)totalBytes_;
}

}  // namespace synth::devapp
