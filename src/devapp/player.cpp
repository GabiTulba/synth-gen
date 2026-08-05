#include "player.hpp"

#include <SDL.h>

#include <cmath>

namespace synth::devapp {

std::vector<float> interleaveToFloat(const WavData& w) {
  size_t channelCount = w.channels.size();
  size_t frames = (size_t)w.frames();
  std::vector<float> out(frames * channelCount);
  for (size_t f = 0; f < frames; f++)
    for (size_t c = 0; c < channelCount; c++)
      out[f * channelCount + c] = (float)w.channels[c][f];
  return out;
}

AudioPlayer::~AudioPlayer() { stop(); }

bool AudioPlayer::play(const std::string& wavPath, std::string& error) {
  stop();
  WavData w;
  try {
    w = readWav(wavPath);
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
  if (w.channels.empty() || w.frames() == 0) {
    error = "artifact is empty";
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
  std::vector<float> data = interleaveToFloat(w);
  if (SDL_QueueAudio(dev, data.data(),
                     (Uint32)(data.size() * sizeof(float))) != 0) {
    error = SDL_GetError();
    SDL_CloseAudioDevice(dev);
    return false;
  }
  SDL_PauseAudioDevice(dev, 0);
  dev_ = dev;
  totalBytes_ = data.size() * sizeof(float);
  path_ = wavPath;
  return true;
}

void AudioPlayer::stop() {
  if (dev_ != 0) {
    SDL_CloseAudioDevice(dev_);
    dev_ = 0;
  }
  totalBytes_ = 0;
  path_.clear();
}

void AudioPlayer::update() {
  if (dev_ != 0 && SDL_GetQueuedAudioSize(dev_) == 0) stop();
}

double AudioPlayer::progress() const {
  if (dev_ == 0 || totalBytes_ == 0) return 0;
  double remaining = (double)SDL_GetQueuedAudioSize(dev_);
  return 1.0 - remaining / (double)totalBytes_;
}

}  // namespace synth::devapp
