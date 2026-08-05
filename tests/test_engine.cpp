#include <cstdio>
#include <filesystem>

#include "signal.hpp"
#include "test_framework.hpp"
#include "wav.hpp"

using namespace synth;

TEST(engine_sine_values) {
  SigPtr s = makeOsc(OscKind::Sine, 1.0);  // 1 Hz
  Rendered r = renderWindow(s, 0.0, 1.0, 8.0);
  CHECK(r.channels == 1);
  CHECK(r.frames == 8);
  CHECK_NEAR(r.interleaved[0], 0.0, 1e-9);           // t = 0
  CHECK_NEAR(r.interleaved[2], 1.0, 1e-9);           // t = 0.25
  CHECK_NEAR(r.interleaved[4], 0.0, 1e-9);           // t = 0.5
  CHECK_NEAR(r.interleaved[6], -1.0, 1e-9);          // t = 0.75
}

TEST(engine_scalar_broadcast_mul) {
  SigPtr s = makeBinOp(SigBinOp::Mul, makeOsc(OscKind::Sine, 1.0),
                       makeConst(0.5));
  Rendered r = renderWindow(s, 0.0, 1.0, 8.0);
  CHECK_NEAR(r.interleaved[2], 0.5, 1e-9);
}

TEST(engine_exp_decay) {
  SigPtr s = makeExpDecay(1.0);
  Rendered r = renderWindow(s, 0.0, 2.0, 2.0);
  CHECK_NEAR(r.interleaved[0], 1.0, 1e-9);
  CHECK_NEAR(r.interleaved[1], std::exp(-0.5), 1e-9);
  CHECK_NEAR(r.interleaved[2], std::exp(-1.0), 1e-9);
}

TEST(engine_adsr_shape) {
  // attack 1s to 1.0, decay 1s to 0.5, hold until 4s, release 1s.
  SigPtr s = makeAdsr(1.0, 1.0, 0.5, 1.0, 4.0);
  Rendered r = renderWindow(s, 0.0, 6.0, 2.0);
  CHECK_NEAR(r.interleaved[1], 0.5, 1e-9);   // t=0.5: mid-attack
  CHECK_NEAR(r.interleaved[2], 1.0, 1e-9);   // t=1: attack peak
  CHECK_NEAR(r.interleaved[4], 0.5, 1e-9);   // t=2: decayed to sustain
  CHECK_NEAR(r.interleaved[6], 0.5, 1e-9);   // t=3: sustaining
  CHECK_NEAR(r.interleaved[9], 0.25, 1e-9);  // t=4.5: mid-release
  CHECK_NEAR(r.interleaved[11], 0.0, 1e-9);  // t=5.5: done
}

TEST(engine_place_windowing) {
  // A constant-ish source: sample [0s,1s) of exp_decay(0) == 1.0, placed
  // at 2s. Silence outside [2s, 3s).
  SigPtr one = makeExpDecay(0.0);
  SigPtr placed = makePlace(one, 0.0, 1.0, 2.0);
  Rendered r = renderWindow(placed, 0.0, 4.0, 4.0);
  CHECK(r.frames == 16);
  CHECK_NEAR(r.interleaved[0], 0.0, 1e-9);   // t=0: before
  CHECK_NEAR(r.interleaved[7], 0.0, 1e-9);   // t=1.75: before
  CHECK_NEAR(r.interleaved[8], 1.0, 1e-9);   // t=2: inside
  CHECK_NEAR(r.interleaved[11], 1.0, 1e-9);  // t=2.75: inside
  CHECK_NEAR(r.interleaved[12], 0.0, 1e-9);  // t=3: after
}

TEST(engine_sample_window_offset) {
  // Placing the window [1s,2s) of exp_decay(1.0) at 0s must reproduce the
  // source's values from its own timeline (e^-1 at the placed start).
  SigPtr src = makeExpDecay(1.0);
  SigPtr placed = makePlace(src, 1.0, 2.0, 0.0);
  Rendered r = renderWindow(placed, 0.0, 1.0, 2.0);
  CHECK_NEAR(r.interleaved[0], std::exp(-1.0), 1e-9);
  CHECK_NEAR(r.interleaved[1], std::exp(-1.5), 1e-9);
}

TEST(engine_mix_and_channels) {
  std::vector<SigPtr> chans{makeConst(0.25), makeConst(-0.25)};
  SigPtr stereo = makeChannels(chans);
  CHECK(stereo->channels() == 2);
  Rendered r = renderWindow(stereo, 0.0, 0.5, 4.0);
  CHECK(r.channels == 2);
  CHECK_NEAR(r.interleaved[0], 0.25, 1e-9);
  CHECK_NEAR(r.interleaved[1], -0.25, 1e-9);

  SigPtr mixed = makeMix({makeConst(0.25), makeConst(0.5)});
  Rendered m = renderWindow(mixed, 0.0, 0.5, 4.0);
  CHECK_NEAR(m.interleaved[0], 0.75, 1e-9);
}

TEST(engine_channel_mismatch_is_error) {
  SigPtr stereo = makeChannels({makeConst(0.1), makeConst(0.2)});
  SigPtr tri = makeChannels({makeConst(0.1), makeConst(0.2), makeConst(0.3)});
  bool threw = false;
  try {
    makeBinOp(SigBinOp::Add, stereo, tri);
  } catch (const EngineError&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(engine_lowpass_attenuates_high_freq) {
  // A 1 kHz tone through a 50 Hz lowpass loses most of its energy; through
  // a 20 kHz lowpass it survives.
  double rate = 48000.0;
  SigPtr tone = makeOsc(OscKind::Sine, 1000.0);
  auto rms = [&](SigPtr s) {
    Rendered r = renderWindow(s, 0.0, 0.5, rate);
    double acc = 0;
    for (double v : r.interleaved) acc += v * v;
    return std::sqrt(acc / (double)r.interleaved.size());
  };
  double open = rms(makeFilter(FilterKind::Lowpass, 20000.0, tone));
  double closed = rms(makeFilter(FilterKind::Lowpass, 50.0, tone));
  CHECK(closed < open * 0.2);
}

TEST(engine_highpass_blocks_dc) {
  SigPtr dc = makeExpDecay(0.0);  // constant 1.0
  SigPtr hp = makeFilter(FilterKind::Highpass, 100.0, dc);
  Rendered r = renderWindow(hp, 0.0, 0.5, 8000.0);
  // After settling, DC must be almost fully removed.
  double tail = 0;
  for (size_t i = r.interleaved.size() - 100; i < r.interleaved.size(); i++)
    tail = std::max(tail, std::fabs(r.interleaved[i]));
  CHECK(tail < 0.01);
}

TEST(engine_wav_roundtrip) {
  namespace fs = std::filesystem;
  fs::path p = fs::temp_directory_path() / "synthgraph-test-roundtrip.wav";
  std::vector<double> data;
  for (int i = 0; i < 100; i++) data.push_back(std::sin(i * 0.1) * 0.9);
  writeWav(p.string(), 44100.0, 1, data);
  WavData w = readWav(p.string());
  CHECK(w.channels.size() == 1);
  CHECK(w.frames() == 100);
  CHECK_NEAR(w.rate, 44100.0, 1e-9);
  for (int i = 0; i < 100; i++)
    CHECK_NEAR(w.channels[0][i], data[i], 1e-3);  // 16-bit quantization
  fs::remove(p);
}

TEST(engine_file_signal_silence_after_end) {
  std::vector<std::vector<double>> chans{{0.5, 0.5, 0.5, 0.5}};
  SigPtr f = makeFileSignal(std::move(chans), 4.0);  // 1 second long
  Rendered r = renderWindow(f, 0.0, 2.0, 4.0);
  CHECK_NEAR(r.interleaved[0], 0.5, 1e-9);
  CHECK_NEAR(r.interleaved[3], 0.5, 1e-9);
  CHECK_NEAR(r.interleaved[5], 0.0, 1e-9);  // past the file's end
}
