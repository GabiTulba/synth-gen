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
  want.samples = 1024;  // ~21ms at 48k: keeps the device's own buffering
                        // from adding much to the live-reload latency
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
  cursor_ = 0;  // the queued stream ends at end-of-range = offset 0
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
  cursor_ = 0;
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
  // Splice the new audio in exactly where the queued old audio ends
  // (`cursor_`, sample-exact - nothing is guessed and nothing queued is
  // dropped), through a short crossfade so the parameter step never
  // lands as an amplitude step (a click). The old read-ahead before the
  // splice is at most ~0.2s (see update()), which is the change's
  // latency.
  size_t frameBytes = (size_t)channels_ * sizeof(float);
  size_t oldFrames = totalBytes_ / frameBytes;
  size_t newFrames = data.size() / (size_t)channels_;
  size_t c = cursor_ / frameBytes;
  size_t newPos = c < newFrames ? c : 0;  // same phase, or wrap if shorter
  size_t fade = (size_t)(rate_ * 0.015);
  fade = std::min({fade, oldFrames - c, newFrames - newPos});
  if (fade > 0) {
    std::vector<float> mix(fade * (size_t)channels_);
    for (size_t f = 0; f < fade; f++) {
      float t = (float)(f + 1) / (float)(fade + 1);
      for (int ch = 0; ch < channels_; ch++)
        mix[f * (size_t)channels_ + ch] =
            data_[(c + f) * (size_t)channels_ + ch] * (1.0f - t) +
            data[(newPos + f) * (size_t)channels_ + ch] * t;
    }
    SDL_QueueAudio(dev_, mix.data(), (Uint32)(mix.size() * sizeof(float)));
  }
  data_ = std::move(data);
  totalBytes_ = data_.size() * sizeof(float);
  cursor_ = ((newPos + fade) % newFrames) * frameBytes;
  rangeEndSec_ = rate_ > 0 ? (double)to / rate_ : 0;
}

void AudioPlayer::update() {
  if (dev_ == 0) return;
  size_t remaining = SDL_GetQueuedAudioSize(dev_);
  if (loop_ && totalBytes_ > 0) {
    // Keep a short read-ahead queued as wrapping chunks from `cursor_`
    // rather than whole copies of the range: on a reload, at most this
    // much stale audio stands between the listener and the new sound.
    // Long enough to survive a dropped UI frame or two (update() runs
    // once per frame), short enough to feel live.
    size_t minAhead = (size_t)(rate_ * channels_ * sizeof(float) * 0.2);
    while (remaining < minAhead) {
      size_t chunk = std::min(totalBytes_ - cursor_, minAhead);
      if (SDL_QueueAudio(dev_, (const char*)data_.data() + cursor_,
                         (Uint32)chunk) != 0)
        break;
      cursor_ = (cursor_ + chunk) % totalBytes_;
      remaining += chunk;
    }
  } else if (remaining == 0) {
    stop();
  }
}

double AudioPlayer::progress() const {
  if (dev_ == 0 || totalBytes_ == 0) return 0;
  // The queued stream ends at offset `cursor_` in the range, so the
  // playing position is cursor_ minus what is still queued, wrapped into
  // the range.
  double pos = std::fmod(
      (double)cursor_ - (double)SDL_GetQueuedAudioSize(dev_),
      (double)totalBytes_);
  if (pos < 0) pos += (double)totalBytes_;
  return pos / (double)totalBytes_;
}

}  // namespace synth::devapp
