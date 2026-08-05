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

TEST(engine_fm_zero_modulator_matches_sine) {
  SigPtr fm = makeFm(440.0, makeConst(0.0));
  SigPtr sine = makeOsc(OscKind::Sine, 440.0);
  Rendered a = renderWindow(fm, 0.0, 0.1, 48000.0);
  Rendered b = renderWindow(sine, 0.0, 0.1, 48000.0);
  for (size_t i = 0; i < a.interleaved.size(); i++)
    CHECK_NEAR(a.interleaved[i], b.interleaved[i], 1e-6);
}

TEST(engine_fm_constant_offset_shifts_frequency) {
  // carrier 100 Hz + constant 50 Hz modulator == a 150 Hz sine.
  SigPtr fm = makeFm(100.0, makeConst(50.0));
  SigPtr sine = makeOsc(OscKind::Sine, 150.0);
  Rendered a = renderWindow(fm, 0.0, 0.2, 48000.0);
  Rendered b = renderWindow(sine, 0.0, 0.2, 48000.0);
  for (size_t i = 0; i < a.interleaved.size(); i++)
    CHECK_NEAR(a.interleaved[i], b.interleaved[i], 1e-4);
}

TEST(engine_fm_vibrato_stays_bounded_and_periodic) {
  // 440 Hz carrier with +/-20 Hz vibrato at 5 Hz: output stays in [-1, 1]
  // and still oscillates near the carrier rate (zero crossings).
  SigPtr mod = makeBinOp(SigBinOp::Mul, makeOsc(OscKind::Sine, 5.0),
                         makeConst(20.0));
  SigPtr fm = makeFm(440.0, mod);
  Rendered r = renderWindow(fm, 0.0, 1.0, 48000.0);
  int crossings = 0;
  for (size_t i = 1; i < r.interleaved.size(); i++) {
    CHECK(std::fabs(r.interleaved[i]) <= 1.0 + 1e-9);
    if ((r.interleaved[i - 1] < 0) != (r.interleaved[i] < 0)) crossings++;
  }
  // ~2 crossings per cycle at ~440 Hz over 1 s.
  CHECK(crossings > 850 && crossings < 910);
}

TEST(engine_pm_zero_modulator_matches_sine) {
  SigPtr pm = makePm(440.0, makeConst(0.0));
  SigPtr sine = makeOsc(OscKind::Sine, 440.0);
  Rendered a = renderWindow(pm, 0.0, 0.1, 48000.0);
  Rendered b = renderWindow(sine, 0.0, 0.1, 48000.0);
  for (size_t i = 0; i < a.interleaved.size(); i++)
    CHECK_NEAR(a.interleaved[i], b.interleaved[i], 1e-9);
}

TEST(engine_pm_constant_phase_offset) {
  // Zero-frequency carrier with a constant pi/2 phase: sin(pi/2) == 1.
  SigPtr pm = makePm(0.0, makeConst(3.14159265358979323846 / 2.0));
  Rendered r = renderWindow(pm, 0.0, 0.01, 8000.0);
  for (double v : r.interleaved) CHECK_NEAR(v, 1.0, 1e-9);
}

TEST(engine_am_depth_and_formula) {
  // Constant carrier 1.0, constant modulator 0.5, depth 0.8:
  // out = 1 * (1 + 0.8 * 0.5) = 1.4.
  SigPtr am = makeAm(makeExpDecay(0.0), makeConst(0.5), 0.8);
  Rendered r = renderWindow(am, 0.0, 0.01, 8000.0);
  for (double v : r.interleaved) CHECK_NEAR(v, 1.4, 1e-9);
}

TEST(engine_am_mono_modulator_broadcasts_over_stereo) {
  SigPtr stereo = makeChannels({makeConst(0.5), makeConst(-0.5)});
  SigPtr am = makeAm(stereo, makeConst(1.0), 1.0);  // gain 2.0
  CHECK(am->channels() == 2);
  Rendered r = renderWindow(am, 0.0, 0.01, 8000.0);
  CHECK(r.channels == 2);
  CHECK_NEAR(r.interleaved[0], 1.0, 1e-9);
  CHECK_NEAR(r.interleaved[1], -1.0, 1e-9);
}

TEST(engine_modulators_must_be_mono) {
  SigPtr stereo = makeChannels({makeConst(0.1), makeConst(0.2)});
  bool threwFm = false, threwAm = false;
  try {
    makeFm(440.0, stereo);
  } catch (const EngineError&) {
    threwFm = true;
  }
  try {
    makeAm(makeConst(1.0), stereo, 1.0);
  } catch (const EngineError&) {
    threwAm = true;
  }
  CHECK(threwFm);
  CHECK(threwAm);
}

TEST(engine_delay_shifts_and_pads_with_silence) {
  // exp_decay(1.0) delayed by 0.5s at rate 2: frame 0 silent, then the
  // source's own values from its epoch.
  SigPtr d = makeDelay(0.5, makeExpDecay(1.0));
  Rendered r = renderWindow(d, 0.0, 2.0, 2.0);
  CHECK_NEAR(r.interleaved[0], 0.0, 1e-9);            // t=0: before
  CHECK_NEAR(r.interleaved[1], 1.0, 1e-9);            // in(0)
  CHECK_NEAR(r.interleaved[2], std::exp(-0.5), 1e-9); // in(0.5)
  CHECK_NEAR(r.interleaved[3], std::exp(-1.0), 1e-9); // in(1)
}

TEST(engine_delay_zero_is_identity) {
  SigPtr src = makeOsc(OscKind::Sine, 440.0);
  Rendered a = renderWindow(makeDelay(0.0, src), 0.0, 0.05, 8000.0);
  Rendered b = renderWindow(src, 0.0, 0.05, 8000.0);
  for (size_t i = 0; i < a.interleaved.size(); i++)
    CHECK_NEAR(a.interleaved[i], b.interleaved[i], 1e-12);
}

TEST(engine_delay_preserves_channels) {
  SigPtr stereo = makeChannels({makeConst(0.3), makeConst(-0.3)});
  SigPtr d = makeDelay(0.25, stereo);
  CHECK(d->channels() == 2);
  Rendered r = renderWindow(d, 0.0, 1.0, 4.0);
  CHECK(r.channels == 2);
  CHECK_NEAR(r.interleaved[0], 0.0, 1e-9);   // t=0: silent, both channels
  CHECK_NEAR(r.interleaved[1], 0.0, 1e-9);
  CHECK_NEAR(r.interleaved[2], 0.3, 1e-9);   // t=0.25 onward: passthrough
  CHECK_NEAR(r.interleaved[3], -0.3, 1e-9);
}

TEST(engine_delay_echo_shares_stateful_subgraph) {
  // The echo idiom with a *stateful* shared source (fm): dry + delayed
  // copies of the same subgraph must not corrupt its state, and the result
  // must equal the manual sum of the shifted dry renders.
  double rate = 8000.0;
  int64_t shift = 400;  // 50ms
  auto mkVoice = [] {
    return makeFm(220.0, makeBinOp(SigBinOp::Mul, makeOsc(OscKind::Sine, 3.0),
                                   makeConst(10.0)));
  };
  SigPtr shared = mkVoice();
  SigPtr echo = makeMix({shared, makeDelay(0.05, shared)});
  Rendered e = renderWindow(echo, 0.0, 0.2, rate);
  Rendered dry = renderWindow(mkVoice(), 0.0, 0.2, rate);
  for (size_t i = 0; i < e.interleaved.size(); i++) {
    double expect = dry.interleaved[i] +
                    (i >= (size_t)shift ? dry.interleaved[i - shift] : 0.0);
    CHECK_NEAR(e.interleaved[i], expect, 1e-9);
  }
}

TEST(engine_delay_negative_is_error) {
  bool threw = false;
  try {
    makeDelay(-0.1, makeOsc(OscKind::Sine, 440.0));
  } catch (const EngineError&) {
    threw = true;
  }
  CHECK(threw);
}

namespace {
// A one-frame unit impulse as a signal (via the file-signal node).
SigPtr impulseSignal() {
  std::vector<std::vector<double>> chans{{1.0}};
  return makeFileSignal(std::move(chans), 8000.0);
}
double peakIn(const Rendered& r, double rate, double t0, double t1) {
  double peak = 0;
  for (int64_t i = (int64_t)(t0 * rate);
       i < (int64_t)(t1 * rate) && i < (int64_t)r.interleaved.size(); i++)
    peak = std::max(peak, std::fabs(r.interleaved[(size_t)i]));
  return peak;
}
}  // namespace

TEST(engine_reverb_produces_decaying_tail) {
  // An impulse through a fully-wet reverb: energy appears after the input
  // is gone and decays roughly per the RT60 rule.
  double rate = 8000.0;
  SigPtr rev = makeReverb(0.4, 0.2, 1.0, impulseSignal());
  Rendered r = renderWindow(rev, 0.0, 2.0, rate);
  double early = peakIn(r, rate, 0.05, 0.15);
  double mid = peakIn(r, rate, 0.4, 0.5);
  double late = peakIn(r, rate, 1.5, 2.0);
  CHECK(early > 0.05);          // a tail exists after the 1-frame impulse
  CHECK(mid < early * 0.5);     // and it decays
  CHECK(late < early * 0.01);   // ~gone well past the decay time
}

TEST(engine_reverb_dry_mix_is_identity) {
  SigPtr src = makeOsc(OscKind::Sine, 440.0);
  Rendered a = renderWindow(makeReverb(0.5, 0.5, 0.0, src), 0.0, 0.05, 8000.0);
  Rendered b = renderWindow(src, 0.0, 0.05, 8000.0);
  for (size_t i = 0; i < a.interleaved.size(); i++)
    CHECK_NEAR(a.interleaved[i], b.interleaved[i], 1e-12);
}

TEST(engine_reverb_longer_decay_longer_tail) {
  double rate = 8000.0;
  Rendered shortTail =
      renderWindow(makeReverb(0.1, 0.2, 1.0, impulseSignal()), 0.0, 1.0, rate);
  Rendered longTail =
      renderWindow(makeReverb(1.0, 0.2, 1.0, impulseSignal()), 0.0, 1.0, rate);
  double s = peakIn(shortTail, rate, 0.5, 0.8);
  double l = peakIn(longTail, rate, 0.5, 0.8);
  CHECK(l > s * 10.0);
}

TEST(engine_reverb_preserves_channels) {
  SigPtr stereo = makeChannels({makeOsc(OscKind::Sine, 440.0),
                                makeOsc(OscKind::Sine, 220.0)});
  SigPtr rev = makeReverb(0.3, 0.5, 0.4, stereo);
  CHECK(rev->channels() == 2);
  Rendered r = renderWindow(rev, 0.0, 0.1, 8000.0);
  CHECK(r.channels == 2);
  double peak = 0;
  for (double v : r.interleaved) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.1);
}

TEST(engine_reverb_validates_parameters) {
  SigPtr src = makeOsc(OscKind::Sine, 440.0);
  int threw = 0;
  try { makeReverb(-0.1, 0.5, 0.5, src); } catch (const EngineError&) { threw++; }
  try { makeReverb(0.5, 1.5, 0.5, src); } catch (const EngineError&) { threw++; }
  try { makeReverb(0.5, 0.5, -0.1, src); } catch (const EngineError&) { threw++; }
  CHECK(threw == 3);
}
