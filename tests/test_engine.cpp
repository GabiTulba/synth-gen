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

TEST(engine_noise_is_bounded_and_centered) {
  Rendered r = renderWindow(makeNoise(1000.0), 0.0, 1.0, 48000.0);
  double mean = 0, var = 0, peak = 0;
  for (double v : r.interleaved) mean += v;
  mean /= (double)r.interleaved.size();
  for (double v : r.interleaved) {
    var += (v - mean) * (v - mean);
    peak = std::max(peak, std::fabs(v));
  }
  var /= (double)r.interleaved.size();
  CHECK(peak <= 1.0 + 1e-9);          // it is still a sine at heart
  CHECK(std::fabs(mean) < 0.1);        // no DC offset
  CHECK(std::sqrt(var) > 0.3);         // substantial energy
}

TEST(engine_noise_is_not_periodic) {
  // A pure 1 kHz sine at 48 kHz has autocorrelation ~1.0 at its period
  // (48 samples); the noise must show none, and must decorrelate fast.
  Rendered r = renderWindow(makeNoise(1000.0), 0.0, 1.0, 48000.0);
  auto& x = r.interleaved;
  size_t n = x.size();
  double mean = 0, var = 0;
  for (double v : x) mean += v;
  mean /= (double)n;
  for (double v : x) var += (v - mean) * (v - mean);
  var /= (double)n;
  auto autocorr = [&](size_t lag) {
    double c = 0;
    for (size_t i = lag; i < n; i++) c += (x[i] - mean) * (x[i - lag] - mean);
    return c / ((double)(n - lag) * var);
  };
  CHECK(std::fabs(autocorr(48)) < 0.3);   // no 1 kHz periodicity
  CHECK(std::fabs(autocorr(480)) < 0.2);  // decorrelated at 10 ms
}

TEST(engine_noise_is_deterministic) {
  // Purity by construction: no RNG, so two renders are bit-identical.
  Rendered a = renderWindow(makeNoise(700.0), 0.0, 0.25, 48000.0);
  Rendered b = renderWindow(makeNoise(700.0), 0.0, 0.25, 48000.0);
  CHECK(a.interleaved.size() == b.interleaved.size());
  for (size_t i = 0; i < a.interleaved.size(); i++)
    CHECK(a.interleaved[i] == b.interleaved[i]);
}

TEST(engine_noise_rejects_nonpositive_frequency) {
  int threw = 0;
  try { makeNoise(0.0); } catch (const EngineError&) { threw++; }
  try { makeNoise(-100.0); } catch (const EngineError&) { threw++; }
  CHECK(threw == 2);
}

TEST(engine_hard_clip_clamps_flat) {
  // A unit sine hard-clipped at 0.5: peaks sit exactly at 0.5, values
  // inside the threshold pass through untouched.
  SigPtr clipped = makeClip(ClipKind::Hard, 0.5, makeOsc(OscKind::Sine, 100.0));
  Rendered r = renderWindow(clipped, 0.0, 0.1, 48000.0);
  Rendered dry = renderWindow(makeOsc(OscKind::Sine, 100.0), 0.0, 0.1, 48000.0);
  double peak = 0;
  for (size_t i = 0; i < r.interleaved.size(); i++) {
    peak = std::max(peak, std::fabs(r.interleaved[i]));
    if (std::fabs(dry.interleaved[i]) <= 0.5)
      CHECK_NEAR(r.interleaved[i], dry.interleaved[i], 1e-12);
    else
      CHECK_NEAR(std::fabs(r.interleaved[i]), 0.5, 1e-12);
  }
  CHECK_NEAR(peak, 0.5, 1e-12);
}

TEST(engine_hard_clip_below_threshold_is_identity) {
  SigPtr src = makeBinOp(SigBinOp::Mul, makeOsc(OscKind::Sine, 440.0),
                         makeConst(0.4));
  Rendered a = renderWindow(makeClip(ClipKind::Hard, 0.8, src), 0.0, 0.05,
                            48000.0);
  Rendered b = renderWindow(src, 0.0, 0.05, 48000.0);
  for (size_t i = 0; i < a.interleaved.size(); i++)
    CHECK_NEAR(a.interleaved[i], b.interleaved[i], 1e-12);
}

TEST(engine_soft_clip_saturates_smoothly) {
  // Bounded strictly below the threshold, near-identity for small
  // signals, and approaching the cap for large drive.
  SigPtr quiet = makeBinOp(SigBinOp::Mul, makeOsc(OscKind::Sine, 440.0),
                           makeConst(0.05));
  Rendered smallIn = renderWindow(quiet, 0.0, 0.05, 48000.0);
  Rendered smallOut =
      renderWindow(makeClip(ClipKind::Soft, 1.0, quiet), 0.0, 0.05, 48000.0);
  for (size_t i = 0; i < smallIn.interleaved.size(); i++)
    CHECK_NEAR(smallOut.interleaved[i], smallIn.interleaved[i], 1e-4);

  SigPtr driven = makeBinOp(SigBinOp::Mul, makeOsc(OscKind::Sine, 440.0),
                            makeConst(10.0));
  Rendered hot = renderWindow(makeClip(ClipKind::Soft, 0.5, driven), 0.0,
                              0.05, 48000.0);
  double peak = 0;
  for (double v : hot.interleaved) {
    CHECK(std::fabs(v) < 0.5 + 1e-12);  // never exceeds the cap
    peak = std::max(peak, std::fabs(v));
  }
  CHECK(peak > 0.49);  // but drives right up against it
}

TEST(engine_clip_preserves_channels) {
  SigPtr stereo = makeChannels({makeConst(0.9), makeConst(-0.9)});
  SigPtr c = makeClip(ClipKind::Hard, 0.5, stereo);
  CHECK(c->channels() == 2);
  Rendered r = renderWindow(c, 0.0, 0.01, 8000.0);
  CHECK_NEAR(r.interleaved[0], 0.5, 1e-12);
  CHECK_NEAR(r.interleaved[1], -0.5, 1e-12);
}

TEST(engine_clip_rejects_nonpositive_threshold) {
  int threw = 0;
  SigPtr src = makeOsc(OscKind::Sine, 440.0);
  try { makeClip(ClipKind::Hard, 0.0, src); } catch (const EngineError&) { threw++; }
  try { makeClip(ClipKind::Soft, -0.5, src); } catch (const EngineError&) { threw++; }
  CHECK(threw == 2);
}

TEST(engine_shared_stateful_sample_placed_twice) {
  // The canonical `mix_all [place k 0s; place k 500ms]` idiom with a
  // *stateful* shared source: per-placement state isolation must make
  // both placements replay identical content.
  double rate = 8000.0;
  SigPtr voice = makeFilter(FilterKind::Lowpass, 800.0,
                            makeOsc(OscKind::Saw, 220.0));
  SigPtr p1 = makePlace(voice, 0.0, 0.25, 0.0);
  SigPtr p2 = makePlace(voice, 0.0, 0.25, 0.5);
  Rendered r = renderWindow(makeMix({p1, p2}), 0.0, 1.0, rate);
  int64_t shift = 4000;  // 0.5s
  int64_t len = 2000;    // 0.25s
  for (int64_t i = 0; i < len; i++)
    CHECK_NEAR(r.interleaved[(size_t)i], r.interleaved[(size_t)(i + shift)],
               1e-12);
  // Silence in the gap between the placements.
  for (int64_t i = len; i < shift; i++)
    CHECK_NEAR(r.interleaved[(size_t)i], 0.0, 1e-12);
}

// --- Block engine & parallel rendering -------------------------------------

TEST(engine_parallel_matches_sequential) {
  // A master-shaped graph: a soft-clipped mix of state-disjoint "buses"
  // (each with its own stateful filters/reverb and placed samples). The
  // planner must split it across workers and still produce exactly the
  // sequential result.
  double rate = 8000.0;
  auto voice = [&](double freq) {
    SigPtr env = makeAdsr(0.01, 0.1, 0.4, 0.2, 0.3);
    SigPtr osc = makeBinOp(SigBinOp::Mul, makeOsc(OscKind::Saw, freq), env);
    return makeFilter(FilterKind::Lowpass, 1200.0, osc);
  };
  auto bus = [&](double base) {
    std::vector<SigPtr> notes;
    for (int i = 0; i < 4; i++)
      notes.push_back(makePlace(voice(base * (1.0 + 0.02 * i)), 0.0, 0.5,
                                0.4 * i));
    return makeReverb(0.8, 0.3, 0.25, makeMix(std::move(notes)));
  };
  SigPtr master = makeClip(
      ClipKind::Soft, 0.9,
      makeBinOp(SigBinOp::Mul,
                makeMix({bus(110.0), bus(220.0), bus(330.0), bus(440.0)}),
                makeConst(0.5)));
  Rendered seq = renderWindow(master, 0.0, 2.5, rate, 1);
  Rendered par = renderWindow(master, 0.0, 2.5, rate, 4);
  CHECK(seq.frames == par.frames);
  CHECK(seq.channels == par.channels);
  bool identical = true;
  for (size_t i = 0; i < seq.interleaved.size(); i++)
    if (seq.interleaved[i] != par.interleaved[i]) identical = false;
  CHECK(identical);  // bit-identical, not merely close
}

TEST(engine_parallel_shared_subtree_grouped) {
  // Two mix items share a stateful node instance directly (no placement
  // isolation). The planner must put them in one group; the render must
  // still equal the sequential one exactly.
  double rate = 8000.0;
  SigPtr shared = makeFilter(FilterKind::Lowpass, 500.0,
                             makeOsc(OscKind::Saw, 220.0));
  SigPtr other = makeFilter(FilterKind::Lowpass, 900.0,
                            makeOsc(OscKind::Saw, 331.0));
  SigPtr top = makeMix({shared, makeDelay(0.1, shared), other});
  Rendered seq = renderWindow(top, 0.0, 1.0, rate, 1);
  Rendered par = renderWindow(top, 0.0, 1.0, rate, 4);
  bool identical = true;
  for (size_t i = 0; i < seq.interleaved.size(); i++)
    if (seq.interleaved[i] != par.interleaved[i]) identical = false;
  CHECK(identical);
}

TEST(engine_silence_shortcircuit_is_exact) {
  // Outside a placement's window the mix must be exact zeros (not merely
  // small), and an envelope past its release must gate a voice to exact
  // silence even inside the window.
  double rate = 8000.0;
  SigPtr env = makeAdsr(0.01, 0.05, 0.5, 0.1, 0.1);  // ends at 0.2s
  SigPtr voice = makeBinOp(SigBinOp::Mul, makeOsc(OscKind::Saw, 440.0), env);
  SigPtr placed = makePlace(voice, 0.0, 1.0, 1.0);  // window [1s, 2s)
  Rendered r = renderWindow(makeMix({placed}), 0.0, 3.0, rate);
  auto exactlyZero = [&](double t0, double t1) {
    int64_t a = (int64_t)(t0 * rate), b = (int64_t)(t1 * rate);
    for (int64_t i = a; i < b; i++)
      if (r.interleaved[(size_t)i] != 0.0) return false;
    return true;
  };
  CHECK(exactlyZero(0.0, 1.0));    // before the placement
  CHECK(!exactlyZero(1.0, 1.2));   // the voice itself
  CHECK(exactlyZero(1.3, 2.0));    // envelope finished: gated tail
  CHECK(exactlyZero(2.0, 3.0));    // after the placement
}

TEST(engine_block_boundaries_are_seamless) {
  // A stateful chain rendered in one call must match two renders that are
  // whole-window but at offsets that straddle block boundaries: the
  // windowed render re-derives state from the epoch.
  double rate = 8000.0;
  SigPtr chain = makeFilter(
      FilterKind::Lowpass, 600.0,
      makeFm(220.0, makeBinOp(SigBinOp::Mul, makeOsc(OscKind::Sine, 3.0),
                              makeConst(20.0))));
  Rendered whole = renderWindow(chain, 0.0, 2.0, rate);
  // 1.0s at 8000 Hz = 8000 frames, not a multiple of the block size.
  Rendered tail = renderWindow(chain, 1.0, 2.0, rate);
  bool identical = true;
  for (int64_t i = 0; i < tail.frames; i++)
    if (whole.interleaved[(size_t)(i + 8000)] != tail.interleaved[(size_t)i])
      identical = false;
  CHECK(identical);
}

TEST(engine_delay_tail_survives_silence_skip) {
  // A short loud burst into a long delay: the echo must come out intact
  // even though the input is silent (and skippable) in between.
  double rate = 8000.0;
  SigPtr burst = makePlace(makeOsc(OscKind::Sine, 440.0), 0.0, 0.05, 0.0);
  SigPtr echoed = makeDelay(1.5, burst);
  Rendered r = renderWindow(makeMix({echoed}), 0.0, 2.0, rate);
  double peak = 0;
  for (int64_t i = (int64_t)(1.5 * rate); i < (int64_t)(1.56 * rate); i++)
    peak = std::max(peak, std::abs(r.interleaved[(size_t)i]));
  CHECK(peak > 0.9);  // the echo arrived
  for (int64_t i = (int64_t)(0.06 * rate); i < (int64_t)(1.5 * rate); i++)
    CHECK(r.interleaved[(size_t)i] == 0.0);  // exact silence before it
}
