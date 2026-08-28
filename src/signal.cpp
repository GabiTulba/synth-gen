#include "signal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace synth {

namespace {
constexpr double kPi = 3.14159265358979323846;

// FNV-1a over structured values, for SigNode::contentHash.
constexpr uint64_t kHashOffset = 1469598103934665603ull;
constexpr uint64_t kHashPrime = 1099511628211ull;
inline uint64_t hashMix(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    h ^= (v >> (i * 8)) & 0xff;
    h *= kHashPrime;
  }
  return h;
}
inline uint64_t hashDouble(uint64_t h, double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, sizeof bits);
  return hashMix(h, bits);
}
// Per-node-type tag; parameters and child hashes mix in after it.
inline uint64_t hashTag(uint64_t tag) { return hashMix(kHashOffset, tag); }

int mergeChannels(int a, int b, const char* what) {
  if (a == -1) return b;
  if (b == -1) return a;
  if (a != b)
    throw EngineError(std::string(what) + ": channel count mismatch (" +
                      std::to_string(a) + " vs " + std::to_string(b) + ")");
  return a;
}

// A child's block, with broadcast-aware element access: a silent block
// reads as zeros, a single-channel block (mono or broadcast) supplies its
// value for every channel of the consumer.
struct Block {
  const double* p = nullptr;  // nullptr => known all-zero
  int cc = 1;                 // the child's concrete channel count
  bool silent() const { return p == nullptr; }
  double at(int f, int i) const {
    if (!p) return 0.0;
    return cc == 1 ? p[f] : p[(size_t)f * (size_t)cc + (size_t)i];
  }
};

Block pull(RenderCtx& ctx, const SigPtr& n, int64_t start, int frames) {
  Block b;
  b.cc = n->concreteChannels();
  bool s = false;
  b.p = n->renderBlock(ctx, start, frames, s);
  return b;
}

// PolyBLEP residual: the two-sample polynomial correction subtracted at
// (or added around) a waveform discontinuity to suppress aliasing.
// `t` is the phase in [0, 1), `dt` the per-sample phase increment.
double polyBlep(double t, double dt) {
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0;
  }
  if (t > 1.0 - dt) {
    t = (t - 1.0) / dt;
    return t * t + t + t + 1.0;
  }
  return 0.0;
}

}  // namespace

NodeState& RenderCtx::stateFor(const SigNode& node) {
  auto it = states.find(&node);
  if (it == states.end())
    it = states.emplace(&node, node.makeState()).first;
  return *it->second;
}

const double* SigNode::renderBlock(RenderCtx& ctx, int64_t start, int frames,
                                   bool& silent) const {
  if (ctx.preRendered) {
    auto it = ctx.preRendered->find(this);
    if (it != ctx.preRendered->end()) {
      const PreRenderedWindow& pr = it->second;
      int64_t rel = start - pr.startFrame;
      if (pr.data && rel >= 0 && rel + frames <= pr.data->frames &&
          pr.data->channels == concreteChannels()) {
        // Serve the block verbatim from the finished render; no state is
        // touched, so partial coverage simply falls through to a normal
        // (deterministic, hence identical) computation below.
        // Grid-aligned queries first consult the provider's recorded
        // silence flags, which spares the zero scan its worst case (a
        // silent block scans to the end before concluding); a false flag
        // only means "not proven silent", so it still falls to the scan.
        if (!pr.data->blockSilent.empty() && rel % kBlockFrames == 0 &&
            frames ==
                (int)std::min<int64_t>(kBlockFrames, pr.data->frames - rel) &&
            pr.data->blockSilent[(size_t)(rel / kBlockFrames)]) {
          silent = true;
          return nullptr;
        }
        const double* p =
            pr.data->interleaved.data() + (size_t)rel * (size_t)pr.data->channels;
        size_t count = (size_t)frames * (size_t)pr.data->channels;
        bool allZero = true;
        for (size_t k = 0; k < count; k++)
          if (p[k] != 0.0) { allZero = false; break; }
        silent = allZero;
        return allZero ? nullptr : p;
      }
    }
  }
  NodeState& st = ctx.stateFor(*this);
  if (st.cachedStart == start && st.cachedFrames == frames) {
    silent = st.cachedSilent;
    return silent ? nullptr : st.buf.data();
  }
  if (frames <= 0 || frames > kBlockFrames)
    throw EngineError("internal error: bad render block size");
  size_t need = (size_t)kBlockFrames * (size_t)concreteChannels();
  if (st.buf.size() < need) st.buf.resize(need);
  if (stateful()) {
    if (start < st.nextSeq)
      throw EngineError("internal error: stateful node queried backwards");
    // Catch up over any gap so state always evolves from the epoch. The
    // catch-up blocks reuse the state buffer as scratch; only the final
    // block is cached.
    while (st.nextSeq < start) {
      int step = (int)std::min<int64_t>(kBlockFrames, start - st.nextSeq);
      computeBlock(ctx, st, st.nextSeq, step, st.buf.data());
      st.nextSeq += step;
    }
  }
  bool s = computeBlock(ctx, st, start, frames, st.buf.data());
  if (stateful()) st.nextSeq = start + frames;
  st.cachedStart = start;
  st.cachedFrames = frames;
  st.cachedSilent = s;
  silent = s;
  return s ? nullptr : st.buf.data();
}

// --- Generators ------------------------------------------------------------

struct OscNode final : SigNode {
  OscKind kind;
  double freq;
  OscNode(OscKind k, double f) : kind(k), freq(f) {
    contentHash = hashDouble(hashMix(hashTag(1), (uint64_t)kind), freq);
  }
  int channels() const override { return 1; }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    // The bandlimited kinds correct their discontinuities with PolyBLEP
    // residuals; dt is the per-sample phase increment (clamped so the
    // correction windows stay disjoint near Nyquist).
    double dt = std::min(std::fabs(freq) / ctx.rate, 0.49);
    for (int f = 0; f < frames; f++) {
      double t = (double)(start + f) / ctx.rate;
      double phase = freq * t;
      double frac = phase - std::floor(phase);
      switch (kind) {
        case OscKind::Sine: out[f] = std::sin(2.0 * kPi * phase); break;
        case OscKind::Saw: out[f] = 2.0 * frac - 1.0; break;
        case OscKind::Square: out[f] = frac < 0.5 ? 1.0 : -1.0; break;
        case OscKind::SawBl:
          out[f] = 2.0 * frac - 1.0 - (dt > 0 ? polyBlep(frac, dt) : 0.0);
          break;
        case OscKind::SquareBl: {
          double v = frac < 0.5 ? 1.0 : -1.0;
          if (dt > 0) {
            double frac2 = frac + 0.5;
            frac2 -= std::floor(frac2);
            v += polyBlep(frac, dt) - polyBlep(frac2, dt);
          }
          out[f] = v;
          break;
        }
      }
    }
    return false;
  }
};

// --- Modulation ------------------------------------------------------------

struct FmState final : NodeState {
  double phase = 0;  // cycles
};

// Phase-integrating sine VCO: the catch-up stepping of stateful nodes makes
// the integral run from the epoch, so placement/window semantics match every
// other signal. With a constant-zero modulator this is exactly `sine`.
struct FmNode final : SigNode {
  double carrier;
  SigPtr modulator;
  FmNode(double c, SigPtr m) : carrier(c), modulator(std::move(m)) {
    int mc = modulator->channels();
    if (mc != 1 && mc != -1)
      throw EngineError("fm: the modulator must be a mono signal");
    contentHash =
        hashMix(hashDouble(hashTag(2), carrier), modulator->contentHash);
  }
  int channels() const override { return 1; }
  bool stateful() const override { return true; }
  std::unique_ptr<NodeState> makeState() const override {
    return std::make_unique<FmState>();
  }
  bool computeBlock(RenderCtx& ctx, NodeState& st0, int64_t start, int frames,
                    double* out) const override {
    auto& st = static_cast<FmState&>(st0);
    Block m = pull(ctx, modulator, start, frames);
    for (int f = 0; f < frames; f++) {
      out[f] = std::sin(2.0 * kPi * st.phase);
      double freq = carrier + m.at(f, 0);
      st.phase += freq / ctx.rate;
      st.phase -= std::floor(st.phase);  // keep precision over long renders
    }
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(modulator);
  }
};

struct PmNode final : SigNode {
  double carrier;
  SigPtr modulator;
  PmNode(double c, SigPtr m) : carrier(c), modulator(std::move(m)) {
    int mc = modulator->channels();
    if (mc != 1 && mc != -1)
      throw EngineError("pm: the modulator must be a mono signal");
    contentHash =
        hashMix(hashDouble(hashTag(3), carrier), modulator->contentHash);
  }
  int channels() const override { return 1; }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    Block m = pull(ctx, modulator, start, frames);
    for (int f = 0; f < frames; f++) {
      double t = (double)(start + f) / ctx.rate;
      out[f] = std::sin(2.0 * kPi * carrier * t + m.at(f, 0));
    }
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(modulator);
  }
};

struct AmNode final : SigNode {
  SigPtr carrier;
  SigPtr modulator;
  double depth;
  AmNode(SigPtr c, SigPtr m, double d)
      : carrier(std::move(c)), modulator(std::move(m)), depth(d) {
    int mc = modulator->channels();
    if (mc != 1 && mc != -1)
      throw EngineError("am: the modulator must be a mono signal");
    contentHash = hashDouble(
        hashMix(hashMix(hashTag(4), carrier->contentHash),
                modulator->contentHash),
        depth);
  }
  int channels() const override { return carrier->channels(); }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    Block c = pull(ctx, carrier, start, frames);
    Block m = pull(ctx, modulator, start, frames);
    if (c.silent()) return true;
    int cc = concreteChannels();
    for (int f = 0; f < frames; f++) {
      double gain = 1.0 + depth * m.at(f, 0);
      for (int i = 0; i < cc; i++) out[f * cc + i] = c.at(f, i) * gain;
    }
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(carrier);
    fn(modulator);
  }
};

// Feedforward delay: out(t) = in(t - by) for t >= by, silence before.
// Implemented with a ring buffer rather than by querying the input at
// shifted indices: the input is pulled at the same monotonically increasing
// blocks as every other consumer, so a subgraph shared between dry and
// delayed paths (the echo idiom) keeps its stateful nodes consistent.
struct DelayState final : NodeState {
  std::vector<double> ring;  // shiftFrames * channelCount doubles, zeroed
  // Highest frame index at which a possibly-nonzero input was written.
  // While the input stays silent past the delay length, every ring slot in
  // play holds an (explicitly written or never-touched) zero, so whole
  // blocks can be skipped without touching the ring.
  int64_t lastLoud = std::numeric_limits<int64_t>::min();
};

struct DelayNode final : SigNode {
  double by;
  SigPtr input;
  DelayNode(double b, SigPtr in) : by(b), input(std::move(in)) {
    if (by < 0) throw EngineError("delay: negative delay time");
    contentHash = hashMix(hashDouble(hashTag(5), by), input->contentHash);
  }
  int channels() const override { return input->channels(); }
  bool stateful() const override { return true; }
  std::unique_ptr<NodeState> makeState() const override {
    return std::make_unique<DelayState>();
  }
  bool computeBlock(RenderCtx& ctx, NodeState& st0, int64_t start, int frames,
                    double* out) const override {
    auto& st = static_cast<DelayState&>(st0);
    Block in = pull(ctx, input, start, frames);
    int64_t shift = llround(by * ctx.rate);
    int cc = concreteChannels();
    if (shift == 0) {
      if (in.silent()) return true;
      std::memcpy(out, in.p, (size_t)frames * (size_t)cc * sizeof(double));
      return false;
    }
    bool outSilent = in.silent() && st.lastLoud < start - shift;
    if (!in.silent()) st.lastLoud = start + frames - 1;
    if (outSilent) return true;
    if (st.ring.empty()) st.ring.assign((size_t)(shift * cc), 0.0);
    for (int f = 0; f < frames; f++) {
      int64_t n = start + f;
      size_t slot = (size_t)((n % shift) * cc);
      // The slot still holds the frame written `shift` steps ago (zero for
      // the first `shift` frames); read it, then overwrite with the current
      // input for the future read.
      for (int i = 0; i < cc; i++) {
        out[f * cc + i] = st.ring[slot + (size_t)i];
        st.ring[slot + (size_t)i] = in.at(f, i);
      }
    }
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(input);
  }
};

// Schroeder reverb: four parallel feedback comb filters (mutually detuned
// delays, damped feedback) followed by two series allpass diffusers, per
// channel. Feedback gains follow the RT60 rule g = 10^(-3 d / decay) so
// `decay` is the time for the tail to fall by ~60 dB.
constexpr double kCombDelays[4] = {0.0297, 0.0371, 0.0411, 0.0437};
constexpr double kAllpassDelays[2] = {0.0050, 0.0017};
constexpr double kAllpassGain = 0.5;

struct ReverbState final : NodeState {
  struct ChannelBank {
    std::vector<double> comb[4];
    double combFilt[4] = {0, 0, 0, 0};  // damping lowpass state
    std::vector<double> allpass[2];
  };
  std::vector<ChannelBank> banks;  // one per channel
  int64_t combLen[4] = {0, 0, 0, 0};
  int64_t allpassLen[2] = {0, 0};
  bool ready = false;
  // Until the first non-silent input arrives the whole bank is zeros and
  // blocks can be skipped outright; afterwards the tail must be computed.
  bool everLoud = false;
};

struct ReverbNode final : SigNode {
  double decay, damping, mix;
  SigPtr input;
  ReverbNode(double dec, double damp, double m, SigPtr in)
      : decay(dec), damping(damp), mix(m), input(std::move(in)) {
    if (decay < 0) throw EngineError("reverb: negative decay time");
    if (damping < 0 || damping > 1)
      throw EngineError("reverb: damping must be in [0, 1]");
    if (mix < 0 || mix > 1)
      throw EngineError("reverb: mix must be in [0, 1]");
    contentHash = hashMix(
        hashDouble(hashDouble(hashDouble(hashTag(6), decay), damping), mix),
        input->contentHash);
  }
  int channels() const override { return input->channels(); }
  bool stateful() const override { return true; }
  std::unique_ptr<NodeState> makeState() const override {
    return std::make_unique<ReverbState>();
  }
  bool computeBlock(RenderCtx& ctx, NodeState& st0, int64_t start, int frames,
                    double* out) const override {
    auto& st = static_cast<ReverbState&>(st0);
    Block in = pull(ctx, input, start, frames);
    if (in.silent() && !st.everLoud) return true;
    if (!in.silent()) st.everLoud = true;
    int cc = concreteChannels();
    if (!st.ready) {
      for (int i = 0; i < 4; i++)
        st.combLen[i] = std::max<int64_t>(1, llround(kCombDelays[i] * ctx.rate));
      for (int i = 0; i < 2; i++)
        st.allpassLen[i] =
            std::max<int64_t>(1, llround(kAllpassDelays[i] * ctx.rate));
      st.banks.resize((size_t)cc);
      for (auto& b : st.banks) {
        for (int i = 0; i < 4; i++) b.comb[i].assign((size_t)st.combLen[i], 0.0);
        for (int i = 0; i < 2; i++)
          b.allpass[i].assign((size_t)st.allpassLen[i], 0.0);
      }
      st.ready = true;
    }
    double g[4];
    for (int i = 0; i < 4; i++)
      g[i] = decay > 0 ? std::pow(10.0, -3.0 * kCombDelays[i] / decay) : 0.0;
    for (int f = 0; f < frames; f++) {
      int64_t n = start + f;
      for (int c = 0; c < cc; c++) {
        auto& b = st.banks[(size_t)c];
        double x = in.at(f, c);
        double wet = 0;
        for (int i = 0; i < 4; i++) {
          size_t pos = (size_t)(n % st.combLen[i]);
          double read = b.comb[i][pos];
          // Damped feedback: a one-pole lowpass inside the loop.
          b.combFilt[i] = read * (1.0 - damping) + b.combFilt[i] * damping;
          b.comb[i][pos] = x + b.combFilt[i] * g[i];
          wet += read;
        }
        wet *= 0.25;
        for (int i = 0; i < 2; i++) {
          size_t pos = (size_t)(n % st.allpassLen[i]);
          double buffered = b.allpass[i][pos];
          b.allpass[i][pos] = wet + buffered * kAllpassGain;
          wet = buffered - wet;
        }
        out[f * cc + c] = x * (1.0 - mix) + wet * mix;
      }
    }
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(input);
  }
};

struct ExpDecayNode final : SigNode {
  double rate;
  explicit ExpDecayNode(double r) : rate(r) {
    contentHash = hashDouble(hashTag(7), rate);
  }
  int channels() const override { return 1; }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    for (int f = 0; f < frames; f++) {
      double t = (double)(start + f) / ctx.rate;
      out[f] = std::exp(-rate * t);
    }
    return false;
  }
};

// Curvatures this close to zero are the straight ramp: the exponential
// form degenerates into 0/0 there, and the two shapes are already
// indistinguishable well above it.
constexpr double kEnvCurvatureEps = 1e-6;

// One falling envelope segment: `from` at x = 0 down to `to` at x = 1,
// as a straight ramp (curvature 0) or along the exponential e^-kx,
// rescaled so both endpoints are hit exactly whatever k is - a decay
// lands on `sustain`, a release on silence, curve regardless. Positive k
// falls fast and tails off; negative k is the mirror.
inline double envFall(double k, double from, double to, double x) {
  if (std::fabs(k) < kEnvCurvatureEps) return from + (to - from) * x;
  const double end = std::exp(-k);
  return to + (from - to) * (std::exp(-k * x) - end) / (1.0 - end);
}

// Envelope shape: linear attack to 1 over [0, a); decay to `sustain` over
// [a, a+d); sustain until `hold`; release to 0 over [hold, hold+r); 0
// afterwards. The decay and release segments carry a curvature each
// (0 = straight ramp), independently of each other; the attack is always
// linear.
struct AdsrNode final : SigNode {
  double a, d, s, r, hold;
  double decayCurve, releaseCurve;
  AdsrNode(double a_, double d_, double s_, double r_, double h,
           double dc, double rc)
      : a(a_), d(d_), s(s_), r(r_), hold(h), decayCurve(dc),
        releaseCurve(rc) {
    contentHash = hashDouble(
        hashDouble(hashDouble(hashDouble(hashDouble(hashDouble(
                                  hashDouble(hashTag(8), a), d), s), r),
                              hold),
                   decayCurve),
        releaseCurve);
  }
  int channels() const override { return 1; }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    double releaseStart = std::max(hold, a + d);
    // The envelope is identically zero past the end of the release, which
    // is what gates most placed samples' tails into silence.
    if ((double)start / ctx.rate >= releaseStart + r) return true;
    for (int f = 0; f < frames; f++) {
      double t = (double)(start + f) / ctx.rate;
      double v;
      if (t < 0) v = 0;
      else if (t < a) v = a > 0 ? t / a : 1.0;
      else if (t < a + d)
        v = d > 0 ? envFall(decayCurve, 1.0, s, (t - a) / d) : s;
      else if (t < releaseStart) v = s;
      else if (t < releaseStart + r)
        v = r > 0 ? envFall(releaseCurve, s, 0.0, (t - releaseStart) / r)
                  : 0.0;
      else v = 0;
      out[f] = v;
    }
    return false;
  }
};

struct ConstNode final : SigNode {
  double value;
  explicit ConstNode(double v) : value(v) {
    contentHash = hashDouble(hashTag(9), value);
  }
  int channels() const override { return -1; }
  bool computeBlock(RenderCtx&, NodeState&, int64_t, int frames,
                    double* out) const override {
    if (value == 0.0) return true;
    std::fill(out, out + frames, value);
    return false;
  }
};

// The time ramp: sample f holds its own timestamp in seconds. Broadcast
// like ConstNode so it combines with signals of any channel count.
struct TimeNode final : SigNode {
  TimeNode() { contentHash = hashTag(17); }
  int channels() const override { return -1; }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    for (int f = 0; f < frames; f++)
      out[f] = (double)(start + f) / ctx.rate;
    return false;
  }
};

// --- Filters ---------------------------------------------------------------

struct FilterState final : NodeState {
  double lp[kMaxChannels] = {};  // low-pass accumulator
  bool primed = false;
  bool everLoud = false;
};

// One-pole filters, stepped sequentially from the epoch.
struct FilterNode final : SigNode {
  FilterKind kind;
  double cutoff;
  SigPtr input;
  FilterNode(FilterKind k, double c, SigPtr in)
      : kind(k), cutoff(c), input(std::move(in)) {
    contentHash = hashMix(hashDouble(hashMix(hashTag(10), (uint64_t)kind),
                                     cutoff),
                          input->contentHash);
  }
  int channels() const override { return input->channels(); }
  bool stateful() const override { return true; }
  std::unique_ptr<NodeState> makeState() const override {
    return std::make_unique<FilterState>();
  }
  bool computeBlock(RenderCtx& ctx, NodeState& st0, int64_t start, int frames,
                    double* out) const override {
    auto& st = static_cast<FilterState&>(st0);
    Block in = pull(ctx, input, start, frames);
    if (in.silent() && !st.everLoud) {
      // Filtering exact silence from the epoch leaves the accumulator at
      // zero, which is also what priming on a zero input would set.
      st.primed = true;
      return true;
    }
    if (!in.silent()) st.everLoud = true;
    int cc = concreteChannels();
    double alpha = 1.0 - std::exp(-2.0 * kPi * cutoff / ctx.rate);
    if (alpha > 1.0) alpha = 1.0;
    for (int f = 0; f < frames; f++) {
      for (int i = 0; i < cc; i++) {
        double x = in.at(f, i);
        if (!st.primed) st.lp[i] = kind == FilterKind::Lowpass ? x : 0.0;
        st.lp[i] += alpha * (x - st.lp[i]);
        out[f * cc + i] = kind == FilterKind::Lowpass ? st.lp[i] : x - st.lp[i];
      }
      st.primed = true;
    }
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(input);
  }
};

// One-pole filter with a signal-rate cutoff: the coefficient is
// recomputed per frame from the cutoff signal, everything else matches
// FilterNode (stateful from the epoch, silence-skips before first sound).
struct ModFilterNode final : SigNode {
  FilterKind kind;
  SigPtr cutoff;
  SigPtr input;
  ModFilterNode(FilterKind k, SigPtr c, SigPtr in)
      : kind(k), cutoff(std::move(c)), input(std::move(in)) {
    int mc = cutoff->channels();
    if (mc != 1 && mc != -1)
      throw EngineError(
          (kind == FilterKind::Lowpass ? std::string("lowpass_mod")
                                       : std::string("highpass_mod")) +
          ": the cutoff must be a mono signal");
    contentHash = hashMix(hashMix(hashMix(hashTag(19), (uint64_t)kind),
                                  cutoff->contentHash),
                          input->contentHash);
  }
  int channels() const override { return input->channels(); }
  bool stateful() const override { return true; }
  std::unique_ptr<NodeState> makeState() const override {
    return std::make_unique<FilterState>();
  }
  bool computeBlock(RenderCtx& ctx, NodeState& st0, int64_t start, int frames,
                    double* out) const override {
    auto& st = static_cast<FilterState&>(st0);
    // The cutoff is pulled unconditionally so a stateful cutoff subtree
    // keeps advancing in lockstep even through silent input blocks.
    Block c = pull(ctx, cutoff, start, frames);
    Block in = pull(ctx, input, start, frames);
    if (in.silent() && !st.everLoud) {
      st.primed = true;
      return true;
    }
    if (!in.silent()) st.everLoud = true;
    int cc = concreteChannels();
    for (int f = 0; f < frames; f++) {
      double fc = c.at(f, 0);
      if (fc < 0) fc = 0;
      double alpha = 1.0 - std::exp(-2.0 * kPi * fc / ctx.rate);
      if (alpha > 1.0) alpha = 1.0;
      for (int i = 0; i < cc; i++) {
        double x = in.at(f, i);
        if (!st.primed) st.lp[i] = kind == FilterKind::Lowpass ? x : 0.0;
        st.lp[i] += alpha * (x - st.lp[i]);
        out[f * cc + i] = kind == FilterKind::Lowpass ? st.lp[i] : x - st.lp[i];
      }
      st.primed = true;
    }
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(cutoff);
    fn(input);
  }
};

// Two-pole Chamberlin state-variable lowpass with resonance. The cutoff
// signal is clamped to a stable fraction of the render rate; damping is
// 1/q. Once excited the filter rings, so blocks are only skipped before
// the first non-silent input.
struct ResonantState final : NodeState {
  double low[kMaxChannels] = {};
  double band[kMaxChannels] = {};
  bool everLoud = false;
};

struct ResonantNode final : SigNode {
  SigPtr cutoff;
  double q;
  SigPtr input;
  ResonantNode(SigPtr c, double q_, SigPtr in)
      : cutoff(std::move(c)), q(q_), input(std::move(in)) {
    int mc = cutoff->channels();
    if (mc != 1 && mc != -1)
      throw EngineError("resonant: the cutoff must be a mono signal");
    if (!(q > 0))
      throw EngineError("resonant: q must be positive");
    contentHash = hashMix(hashDouble(hashMix(hashTag(20),
                                             cutoff->contentHash),
                                     q),
                          input->contentHash);
  }
  int channels() const override { return input->channels(); }
  bool stateful() const override { return true; }
  std::unique_ptr<NodeState> makeState() const override {
    return std::make_unique<ResonantState>();
  }
  bool computeBlock(RenderCtx& ctx, NodeState& st0, int64_t start, int frames,
                    double* out) const override {
    auto& st = static_cast<ResonantState&>(st0);
    Block c = pull(ctx, cutoff, start, frames);
    Block in = pull(ctx, input, start, frames);
    if (in.silent() && !st.everLoud) return true;
    if (!in.silent()) st.everLoud = true;
    int cc = concreteChannels();
    double damp = 1.0 / q;
    if (damp > 2.0) damp = 2.0;
    for (int f = 0; f < frames; f++) {
      double fc = c.at(f, 0);
      if (fc < 0) fc = 0;
      // The Chamberlin form is stable for fc below about rate/6 (g <= 1
      // with any damping); the clamp keeps a sweep that overshoots the
      // render rate a filter rather than a runaway.
      double fmax = ctx.rate / 6.0;
      if (fc > fmax) fc = fmax;
      double g = 2.0 * std::sin(kPi * fc / ctx.rate);
      for (int i = 0; i < cc; i++) {
        double x = in.at(f, i);
        st.low[i] += g * st.band[i];
        double high = x - st.low[i] - damp * st.band[i];
        st.band[i] += g * high;
        out[f * cc + i] = st.low[i];
      }
    }
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(cutoff);
    fn(input);
  }
};

// Envelope follower: |x| into a one-pole smoother whose coefficient
// depends on whether the envelope is rising (attack) or falling
// (release). Zero times snap instantly.
struct FollowState final : NodeState {
  double env = 0;
  bool everLoud = false;
};

struct FollowNode final : SigNode {
  double attack, release;
  SigPtr input;
  FollowNode(double a, double r, SigPtr in)
      : attack(a), release(r), input(std::move(in)) {
    if (attack < 0 || release < 0)
      throw EngineError("follow: attack and release must be non-negative");
    int ic = input->channels();
    if (ic != 1 && ic != -1)
      throw EngineError("follow: the input must be a mono signal "
                        "(follow a bus after mixing it down)");
    contentHash = hashMix(hashDouble(hashDouble(hashTag(21), attack),
                                     release),
                          input->contentHash);
  }
  int channels() const override { return 1; }
  bool stateful() const override { return true; }
  std::unique_ptr<NodeState> makeState() const override {
    return std::make_unique<FollowState>();
  }
  bool computeBlock(RenderCtx& ctx, NodeState& st0, int64_t start, int frames,
                    double* out) const override {
    auto& st = static_cast<FollowState&>(st0);
    Block in = pull(ctx, input, start, frames);
    if (in.silent() && !st.everLoud) return true;
    if (!in.silent()) st.everLoud = true;
    double aA = attack > 0 ? std::exp(-1.0 / (attack * ctx.rate)) : 0.0;
    double aR = release > 0 ? std::exp(-1.0 / (release * ctx.rate)) : 0.0;
    for (int f = 0; f < frames; f++) {
      double x = std::fabs(in.at(f, 0));
      double a = x > st.env ? aA : aR;
      st.env = a * st.env + (1.0 - a) * x;
      out[f] = st.env;
    }
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(input);
  }
};

// Sample-wise select: the signal-level choice `if` deliberately does not
// make. All three children are pulled every block so stateful subtrees
// stay in lockstep whichever side is chosen.
struct SelectNode final : SigNode {
  SigPtr gate;
  double threshold;
  SigPtr above, below;
  int ch;
  SelectNode(SigPtr g, double t, SigPtr a, SigPtr b)
      : gate(std::move(g)), threshold(t), above(std::move(a)),
        below(std::move(b)),
        ch(mergeChannels(above->channels(), below->channels(), "select")) {
    int gc = gate->channels();
    if (gc != 1 && gc != -1)
      throw EngineError("select: the gate must be a mono signal");
    contentHash = hashMix(
        hashMix(hashDouble(hashMix(hashTag(22), gate->contentHash),
                           threshold),
                above->contentHash),
        below->contentHash);
  }
  int channels() const override { return ch; }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    Block g = pull(ctx, gate, start, frames);
    Block a = pull(ctx, above, start, frames);
    Block b = pull(ctx, below, start, frames);
    if (a.silent() && b.silent()) return true;
    int cc = concreteChannels();
    for (int f = 0; f < frames; f++) {
      const Block& src = g.at(f, 0) >= threshold ? a : b;
      for (int i = 0; i < cc; i++) out[f * cc + i] = src.at(f, i);
    }
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(gate);
    fn(above);
    fn(below);
  }
};

// Feedback delay: out(t) = in(t) + gain * out(t - by). The ring buffer
// holds the *output*, so the tail regenerates geometrically; |gain| < 1
// keeps it bounded. Like reverb, the feedback lives in per-render state
// and the language-level graph stays acyclic.
struct FeedbackDelayState final : NodeState {
  std::vector<double> ring;  // shiftFrames * channelCount doubles, zeroed
  bool everLoud = false;
};

struct FeedbackDelayNode final : SigNode {
  double by, gain;
  SigPtr input;
  FeedbackDelayNode(double b, double g, SigPtr in)
      : by(b), gain(g), input(std::move(in)) {
    if (by <= 0) throw EngineError("feedback: delay time must be positive");
    if (!(std::fabs(gain) < 1.0))
      throw EngineError("feedback: |gain| must be < 1 (the tail would "
                        "never decay)");
    contentHash = hashMix(hashDouble(hashDouble(hashTag(23), by), gain),
                          input->contentHash);
  }
  int channels() const override { return input->channels(); }
  bool stateful() const override { return true; }
  std::unique_ptr<NodeState> makeState() const override {
    return std::make_unique<FeedbackDelayState>();
  }
  bool computeBlock(RenderCtx& ctx, NodeState& st0, int64_t start, int frames,
                    double* out) const override {
    auto& st = static_cast<FeedbackDelayState&>(st0);
    Block in = pull(ctx, input, start, frames);
    if (in.silent() && !st.everLoud) return true;
    if (!in.silent()) st.everLoud = true;
    int64_t shift = std::max<int64_t>(1, llround(by * ctx.rate));
    int cc = concreteChannels();
    if (st.ring.empty()) st.ring.assign((size_t)(shift * cc), 0.0);
    for (int f = 0; f < frames; f++) {
      int64_t n = start + f;
      size_t slot = (size_t)((n % shift) * cc);
      for (int i = 0; i < cc; i++) {
        double v = in.at(f, i) + gain * st.ring[slot + (size_t)i];
        out[f * cc + i] = v;
        st.ring[slot + (size_t)i] = v;
      }
    }
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(input);
  }
};

// Channel extraction: one channel of a multichannel signal as mono - the
// inverse `channels` never had.
struct ChannelNode final : SigNode {
  int index;
  SigPtr input;
  ChannelNode(int n, SigPtr in) : index(n), input(std::move(in)) {
    if (index < 0) throw EngineError("channel: negative channel index");
    int ic = input->channels();
    if (ic >= 0 && index >= ic)
      throw EngineError("channel: index " + std::to_string(index) +
                        " out of range for a " + std::to_string(ic) +
                        "-channel signal");
    contentHash =
        hashMix(hashMix(hashTag(24), (uint64_t)index), input->contentHash);
  }
  int channels() const override { return 1; }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    Block in = pull(ctx, input, start, frames);
    if (in.silent()) return true;
    for (int f = 0; f < frames; f++) out[f] = in.at(f, index);
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(input);
  }
};

// --- Distortion ------------------------------------------------------------

struct ClipNode final : SigNode {
  ClipKind kind;
  double threshold;
  SigPtr input;
  ClipNode(ClipKind k, double t, SigPtr in)
      : kind(k), threshold(t), input(std::move(in)) {
    if (threshold <= 0)
      throw EngineError(
          (kind == ClipKind::Hard ? std::string("hard_clip")
                                  : std::string("soft_clip")) +
          ": threshold must be positive");
    contentHash = hashMix(hashDouble(hashMix(hashTag(11), (uint64_t)kind),
                                     threshold),
                          input->contentHash);
  }
  int channels() const override { return input->channels(); }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    Block in = pull(ctx, input, start, frames);
    if (in.silent()) return true;  // both shapes map 0 to 0
    int cc = concreteChannels();
    for (int f = 0; f < frames; f++) {
      for (int i = 0; i < cc; i++) {
        double x = in.at(f, i);
        out[f * cc + i] = kind == ClipKind::Hard
                              ? std::clamp(x, -threshold, threshold)
                              : threshold * std::tanh(x / threshold);
      }
    }
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(input);
  }
};

// --- Combination -----------------------------------------------------------

// Fused elementwise arithmetic. makeBinOp does not build a node per
// operator: it merges its operands' programs into one FusedNode holding
// the non-arithmetic subtrees as `inputs` and a small postfix program
// over them, with scalar constants folded in as immediates. A chain like
// `kick + snare * 0.85 + hat * 0.4` therefore renders in a single pass -
// three input reads and one output write per element, the intermediates
// living on a register stack - instead of one full buffer pass per
// operator. The program replays the exact operator-tree evaluation
// order, so results are bit-identical to unfused evaluation.
//
// Per block, a silence lattice folded over the program reproduces each
// operator's short-circuit rule exactly (add/sub: zero iff both sides
// are; mul: zero if either side is; div: never assumed zero, so 0/0
// still yields NaN); silent inputs read as literal zeros inside a
// non-zero expression, matching the unfused dense-with-zeros behavior.
//
// Inlining is capped at kMaxFusedOps: an oversized operand stays a
// (still internally fused) input node, so shared arithmetic DAGs cannot
// blow up the program exponentially and chains fuse in bounded chunks.
constexpr size_t kMaxFusedOps = 64;

struct FusedState final : NodeState {
  std::vector<double> scratch;  // depthNeed block-sized value-stack slabs
};

struct FusedNode final : SigNode {
  struct Op {
    enum class K : uint8_t {
      Input, Const,             // operands
      Add, Sub, Mul, Div, Pow,  // binary
      Exp, Sqrt, Log,           // unary
      Sin, Cos, Tan, Atan, Abs  // unary (trig rows; appended so existing
                                // program hashes stay stable)
    };
    K k = K::Const;
    int input = 0;     // K::Input: index into `inputs`
    double value = 0;  // K::Const
  };
  static bool isUnary(Op::K k) { return k >= Op::K::Exp; }
  std::vector<SigPtr> inputs;
  std::vector<Op> program;  // postfix, ends with an operator
  int ch = -1;
  int depthNeed = 0;  // maximum value-stack depth of `program`

  int channels() const override { return ch; }
  std::unique_ptr<NodeState> makeState() const override {
    return std::make_unique<FusedState>();
  }
  bool computeBlock(RenderCtx& ctx, NodeState& st0, int64_t start, int frames,
                    double* out) const override {
    // All inputs are always pulled, even when silence lets the
    // arithmetic be skipped: stateful subtrees must keep advancing in
    // lockstep or a shared node would later be queried backwards.
    int nIn = (int)inputs.size();
    std::array<Block, kMaxFusedOps> in;
    for (int k = 0; k < nIn; k++)
      in[(size_t)k] = pull(ctx, inputs[(size_t)k], start, frames);
    // Fold the silence lattice over the program.
    std::array<char, kMaxFusedOps> zs;
    size_t zp = 0;
    for (const Op& op : program) {
      switch (op.k) {
        case Op::K::Input: zs[zp++] = in[(size_t)op.input].silent(); break;
        case Op::K::Const: zs[zp++] = op.value == 0.0; break;
        case Op::K::Add:
        case Op::K::Sub:
          zp--;
          zs[zp - 1] = zs[zp - 1] && zs[zp];
          break;
        case Op::K::Mul:
          zp--;
          zs[zp - 1] = zs[zp - 1] || zs[zp];
          break;
        case Op::K::Div:
        case Op::K::Pow:
          zp--;
          zs[zp - 1] = 0;
          break;
        case Op::K::Exp:
        case Op::K::Log:
        case Op::K::Cos:
          // exp(0) == 1, log(0) == -inf, cos(0) == 1: never silent.
          zs[zp - 1] = 0;
          break;
        case Op::K::Sqrt:
        case Op::K::Sin:
        case Op::K::Tan:
        case Op::K::Atan:
        case Op::K::Abs:
          break;  // f(0) == 0: silence is preserved
      }
    }
    if (zs[0]) return true;
    // Dense pass, executed block-at-a-time: the value stack holds
    // block-sized slabs and every operator runs one tight loop over the
    // whole block, so the per-op dispatch is amortized over the block
    // and the loops auto-vectorize; the slabs live in the node's scratch
    // and stay cache-resident. Scalar constants fold eagerly (a
    // scalar-scalar op produces a scalar, not a slab). Input slabs are
    // borrowed pointers - results always land in scratch.
    int cc = concreteChannels();
    size_t slab = (size_t)frames * (size_t)cc;
    auto& st = static_cast<FusedState&>(st0);
    if (st.scratch.size() < slab * (size_t)depthNeed)
      st.scratch.resize(slab * (size_t)depthNeed);
    struct Slot {
      const double* p = nullptr;  // dense slab unless scalar
      double scalar = 0;
      bool isScalar = false;
      bool perFrame = false;  // 1 value/frame under a multi-channel node
    };
    static const std::vector<double> kZeroSlab(
        (size_t)kBlockFrames * (size_t)kMaxChannels, 0.0);
    std::array<Slot, kMaxFusedOps> stack;
    size_t sp = 0;
    for (size_t o = 0; o < program.size(); o++) {
      const Op& op = program[o];
      bool last = o + 1 == program.size();
      switch (op.k) {
        case Op::K::Input: {
          const Block& b = in[(size_t)op.input];
          Slot s;
          if (b.silent()) s.p = kZeroSlab.data();
          else { s.p = b.p; s.perFrame = b.cc == 1 && cc > 1; }
          stack[sp++] = s;
          break;
        }
        case Op::K::Const: {
          Slot s;
          s.isScalar = true;
          s.scalar = op.value;
          stack[sp++] = s;
          break;
        }
        case Op::K::Exp:
        case Op::K::Sqrt:
        case Op::K::Log:
        case Op::K::Sin:
        case Op::K::Cos:
        case Op::K::Tan:
        case Op::K::Atan:
        case Op::K::Abs: {
          Slot a = stack[sp - 1];
          Slot res;
          auto f1 = [&](double v) {
            switch (op.k) {
              case Op::K::Exp: return std::exp(v);
              case Op::K::Sqrt: return std::sqrt(v);
              case Op::K::Sin: return std::sin(v);
              case Op::K::Cos: return std::cos(v);
              case Op::K::Tan: return std::tan(v);
              case Op::K::Atan: return std::atan(v);
              case Op::K::Abs: return std::fabs(v);
              default: return std::log(v);
            }
          };
          if (a.isScalar) {
            res.isScalar = true;
            res.scalar = f1(a.scalar);
          } else {
            double* dst = last ? out : st.scratch.data() + (sp - 1) * slab;
            if (a.perFrame && !last) {
              // Stay compact: one value per frame under a wider node.
              for (int f = 0; f < frames; f++) dst[f] = f1(a.p[f]);
              res.perFrame = true;
            } else if (a.perFrame) {
              // Final op: widen into the caller's interleaved block.
              for (int f = 0; f < frames; f++) {
                double v = f1(a.p[f]);
                for (int i = 0; i < cc; i++) dst[f * cc + i] = v;
              }
            } else {
              switch (op.k) {
                case Op::K::Exp:
                  for (size_t n = 0; n < slab; n++) dst[n] = std::exp(a.p[n]);
                  break;
                case Op::K::Sqrt:
                  for (size_t n = 0; n < slab; n++) dst[n] = std::sqrt(a.p[n]);
                  break;
                case Op::K::Sin:
                  for (size_t n = 0; n < slab; n++) dst[n] = std::sin(a.p[n]);
                  break;
                case Op::K::Cos:
                  for (size_t n = 0; n < slab; n++) dst[n] = std::cos(a.p[n]);
                  break;
                case Op::K::Tan:
                  for (size_t n = 0; n < slab; n++) dst[n] = std::tan(a.p[n]);
                  break;
                case Op::K::Atan:
                  for (size_t n = 0; n < slab; n++) dst[n] = std::atan(a.p[n]);
                  break;
                case Op::K::Abs:
                  for (size_t n = 0; n < slab; n++) dst[n] = std::fabs(a.p[n]);
                  break;
                default:
                  for (size_t n = 0; n < slab; n++) dst[n] = std::log(a.p[n]);
                  break;
              }
            }
            res.p = dst;
          }
          stack[sp - 1] = res;
          break;
        }
        default: {
          Slot b = stack[--sp];
          Slot a = stack[sp - 1];
          Slot res;
          if (a.isScalar && b.isScalar) {
            res.isScalar = true;
            switch (op.k) {
              case Op::K::Add: res.scalar = a.scalar + b.scalar; break;
              case Op::K::Sub: res.scalar = a.scalar - b.scalar; break;
              case Op::K::Mul: res.scalar = a.scalar * b.scalar; break;
              case Op::K::Pow:
                res.scalar = std::pow(a.scalar, b.scalar);
                break;
              default: res.scalar = a.scalar / b.scalar; break;
            }
          } else {
            // The final operator writes straight into the caller's block.
            double* dst =
                last ? out : st.scratch.data() + (sp - 1) * slab;
            if (a.perFrame || b.perFrame) {
              // Rare shape (a non-constant broadcast input): generic loop.
              auto at = [&](const Slot& s, int f, int i) {
                return s.isScalar ? s.scalar
                                  : s.perFrame ? s.p[f] : s.p[f * cc + i];
              };
              for (int f = 0; f < frames; f++)
                for (int i = 0; i < cc; i++) {
                  double x = at(a, f, i), y = at(b, f, i);
                  switch (op.k) {
                    case Op::K::Add: dst[f * cc + i] = x + y; break;
                    case Op::K::Sub: dst[f * cc + i] = x - y; break;
                    case Op::K::Mul: dst[f * cc + i] = x * y; break;
                    case Op::K::Pow:
                      dst[f * cc + i] = std::pow(x, y);
                      break;
                    default: dst[f * cc + i] = x / y; break;
                  }
                }
            } else if (a.isScalar) {
              double x = a.scalar;
              const double* y = b.p;
              switch (op.k) {
                case Op::K::Add:
                  for (size_t n = 0; n < slab; n++) dst[n] = x + y[n];
                  break;
                case Op::K::Sub:
                  for (size_t n = 0; n < slab; n++) dst[n] = x - y[n];
                  break;
                case Op::K::Mul:
                  for (size_t n = 0; n < slab; n++) dst[n] = x * y[n];
                  break;
                case Op::K::Pow:
                  for (size_t n = 0; n < slab; n++) dst[n] = std::pow(x, y[n]);
                  break;
                default:
                  for (size_t n = 0; n < slab; n++) dst[n] = x / y[n];
                  break;
              }
            } else if (b.isScalar) {
              const double* x = a.p;
              double y = b.scalar;
              switch (op.k) {
                case Op::K::Add:
                  for (size_t n = 0; n < slab; n++) dst[n] = x[n] + y;
                  break;
                case Op::K::Sub:
                  for (size_t n = 0; n < slab; n++) dst[n] = x[n] - y;
                  break;
                case Op::K::Mul:
                  for (size_t n = 0; n < slab; n++) dst[n] = x[n] * y;
                  break;
                case Op::K::Pow:
                  for (size_t n = 0; n < slab; n++) dst[n] = std::pow(x[n], y);
                  break;
                default:
                  for (size_t n = 0; n < slab; n++) dst[n] = x[n] / y;
                  break;
              }
            } else {
              const double* x = a.p;
              const double* y = b.p;
              switch (op.k) {
                case Op::K::Add:
                  for (size_t n = 0; n < slab; n++) dst[n] = x[n] + y[n];
                  break;
                case Op::K::Sub:
                  for (size_t n = 0; n < slab; n++) dst[n] = x[n] - y[n];
                  break;
                case Op::K::Mul:
                  for (size_t n = 0; n < slab; n++) dst[n] = x[n] * y[n];
                  break;
                case Op::K::Pow:
                  for (size_t n = 0; n < slab; n++)
                    dst[n] = std::pow(x[n], y[n]);
                  break;
                default:
                  for (size_t n = 0; n < slab; n++) dst[n] = x[n] / y[n];
                  break;
              }
            }
            res.p = dst;
          }
          stack[sp - 1] = res;
          break;
        }
      }
    }
    const Slot& top = stack[0];
    if (top.isScalar) std::fill(out, out + slab, top.scalar);
    // else the last operator already wrote into `out`.
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    for (auto& x : inputs) fn(x);
  }
};

namespace {

void fusedAppendInput(const SigPtr& x, std::vector<SigPtr>& inputs,
                      std::vector<FusedNode::Op>& prog) {
  for (size_t k = 0; k < inputs.size(); k++)
    if (inputs[k] == x) {  // dedup: `x * x` pulls x once
      prog.push_back({FusedNode::Op::K::Input, (int)k, 0});
      return;
    }
  inputs.push_back(x);
  prog.push_back({FusedNode::Op::K::Input, (int)inputs.size() - 1, 0});
}

void fusedAppendOperand(const SigPtr& x, std::vector<SigPtr>& inputs,
                        std::vector<FusedNode::Op>& prog) {
  if (auto* c = dynamic_cast<const ConstNode*>(x.get())) {
    prog.push_back({FusedNode::Op::K::Const, 0, c->value});
    return;
  }
  if (auto* fx = dynamic_cast<const FusedNode*>(x.get())) {
    for (const auto& op : fx->program) {
      if (op.k == FusedNode::Op::K::Input)
        fusedAppendInput(fx->inputs[(size_t)op.input], inputs, prog);
      else
        prog.push_back(op);
    }
    return;
  }
  fusedAppendInput(x, inputs, prog);
}

}  // namespace

struct MixNode final : SigNode {
  std::vector<SigPtr> items;
  int ch;
  explicit MixNode(std::vector<SigPtr> xs) : items(std::move(xs)), ch(-1) {
    for (auto& x : items) ch = mergeChannels(ch, x->channels(), "mix_all");
    contentHash = hashTag(13);
    for (auto& x : items) contentHash = hashMix(contentHash, x->contentHash);
  }
  int channels() const override { return ch; }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    int cc = concreteChannels();
    size_t total = (size_t)frames * (size_t)cc;
    bool any = false;
    for (auto& x : items) {
      Block b = pull(ctx, x, start, frames);
      if (b.silent()) continue;  // adding exact zeros is a no-op
      if (!any) {
        std::fill(out, out + total, 0.0);
        any = true;
      }
      if (b.cc == 1 && cc > 1) {
        for (int f = 0; f < frames; f++) {
          double v = b.p[f];
          for (int i = 0; i < cc; i++) out[f * cc + i] += v;
        }
      } else {
        for (size_t k = 0; k < total; k++) out[k] += b.p[k];
      }
    }
    return !any;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    for (auto& x : items) fn(x);
  }
};

struct ChannelsNode final : SigNode {
  std::vector<SigPtr> items;
  explicit ChannelsNode(std::vector<SigPtr> xs) : items(std::move(xs)) {
    if (items.empty()) throw EngineError("channels: empty channel list");
    if ((int)items.size() > kMaxChannels)
      throw EngineError("channels: more than " +
                        std::to_string(kMaxChannels) +
                        " channels are not supported in v1");
    for (auto& x : items) {
      int c = x->channels();
      if (c != 1 && c != -1)
        throw EngineError("channels: every input must be mono");
    }
    contentHash = hashTag(14);
    for (auto& x : items) contentHash = hashMix(contentHash, x->contentHash);
  }
  int channels() const override { return (int)items.size(); }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    int nCh = (int)items.size();
    Block blocks[kMaxChannels];
    bool anyLoud = false;
    for (int i = 0; i < nCh; i++) {
      blocks[i] = pull(ctx, items[(size_t)i], start, frames);
      anyLoud = anyLoud || !blocks[i].silent();
    }
    if (!anyLoud) return true;
    for (int f = 0; f < frames; f++)
      for (int i = 0; i < nCh; i++) out[f * nCh + i] = blocks[i].at(f, 0);
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    for (auto& x : items) fn(x);
  }
};

// --- Placement -------------------------------------------------------------

// A discretized sample window, rendered once per render and shared by
// every placement of the same (source, window) pair. Values are exactly
// what a placement's private replay would compute (renders are
// deterministic and block partitioning does not affect values), so
// serving slices is bit-exact. [nzBegin, nzEnd) brackets the frames with
// any nonzero value; slices outside it are silent - which is what keeps
// envelope-gated tails and pre-attack padding free for every placement.
struct CachedSample {
  int cc = 1;
  int64_t frames = 0;
  std::vector<double> interleaved;
  int64_t nzBegin = 0, nzEnd = 0;  // nzBegin >= nzEnd means all-zero
};

struct SampleCache {
  // Keyed by the source's structural content hash (not its pointer), so
  // an entry outlives the graph that created it: after a rebuild, a
  // structurally unchanged sample hashes the same and its window is
  // served without re-rendering.
  struct Key {
    uint64_t src;
    double from, to, rate;
    bool operator==(const Key& o) const {
      return src == o.src && from == o.from && to == o.to && rate == o.rate;
    }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      size_t h = std::hash<uint64_t>()(k.src);
      h = h * 1000003u ^ std::hash<double>()(k.from);
      h = h * 1000003u ^ std::hash<double>()(k.to);
      h = h * 1000003u ^ std::hash<double>()(k.rate);
      return h;
    }
  };
  struct Entry {
    // nullptr = another thread is building this entry right now.
    std::shared_ptr<const CachedSample> data;
    uint64_t gen = 0;  // last generation that used this entry
  };
  std::mutex m;
  std::unordered_map<Key, Entry, KeyHash> entries;
  uint64_t generation = 0;
  size_t rendered = 0, reusedPrior = 0;  // per-generation stats

  // Returns the cached window, building it on first use. Returns nullptr
  // if the entry is mid-build on another thread; the caller then falls
  // back to a private replay (identical values, just not shared).
  std::shared_ptr<const CachedSample> acquire(const SigPtr& src, double from,
                                              double to, double rate) {
    Key key{src->contentHash, from, to, rate};
    {
      std::lock_guard<std::mutex> lk(m);
      auto it = entries.find(key);
      if (it != entries.end()) {
        if (it->second.data && it->second.gen != generation)
          reusedPrior++;  // survived from an earlier build
        it->second.gen = generation;
        return it->second.data;  // ready, or building (nullptr)
      }
      entries.emplace(key, Entry{nullptr, generation});  // claim
    }
    std::shared_ptr<const CachedSample> built;
    try {
      built = build(src, from, to, rate);
    } catch (...) {
      std::lock_guard<std::mutex> lk(m);
      entries.erase(key);
      throw;
    }
    std::lock_guard<std::mutex> lk(m);
    Entry& e = entries[key];
    e.data = built;
    e.gen = generation;
    rendered++;
    return built;
  }

 private:
  std::shared_ptr<const CachedSample> build(const SigPtr& src, double from,
                                            double to, double rate) {
    // Mirror PlaceNode's window arithmetic exactly, then render the
    // source the same way a placement's private context would: block by
    // block from n0, stateful nodes catching up from the epoch. Nested
    // placements inside the sample share this cache (keys differ, and
    // the graph is acyclic, so the recursion terminates).
    int64_t n0 = llround(from * rate);
    int64_t len = llround((to - from) * rate);
    auto cs = std::make_shared<CachedSample>();
    cs->cc = src->concreteChannels();
    cs->frames = len;
    cs->interleaved.assign((size_t)(len * cs->cc), 0.0);
    RenderCtx ctx(rate);
    ctx.sampleCache = this;
    for (int64_t n = n0; n < n0 + len; n += kBlockFrames) {
      int f = (int)std::min<int64_t>(kBlockFrames, n0 + len - n);
      bool silent = false;
      const double* p = src->renderBlock(ctx, n, f, silent);
      if (!silent)
        std::memcpy(cs->interleaved.data() + (size_t)((n - n0) * cs->cc), p,
                    (size_t)f * (size_t)cs->cc * sizeof(double));
    }
    // Bracket the nonzero span (per frame, any channel).
    const double* d = cs->interleaved.data();
    int64_t b = 0, e = len;
    while (b < e) {
      bool z = true;
      for (int c = 0; c < cs->cc && z; c++) z = d[b * cs->cc + c] == 0.0;
      if (!z) break;
      b++;
    }
    while (e > b) {
      bool z = true;
      for (int c = 0; c < cs->cc && z; c++)
        z = d[(e - 1) * cs->cc + c] == 0.0;
      if (!z) break;
      e--;
    }
    cs->nzBegin = b;
    cs->nzEnd = e;
    return cs;
  }
};

std::shared_ptr<SampleCache> makeSampleCache() {
  return std::make_shared<SampleCache>();
}

void sampleCacheBeginBuild(SampleCache& cache) {
  std::lock_guard<std::mutex> lk(cache.m);
  cache.generation++;
  cache.rendered = 0;
  cache.reusedPrior = 0;
  // Keep only what the previous generation actually used: samples whose
  // definitions changed leave stale hashes behind, and this is where
  // they die.
  for (auto it = cache.entries.begin(); it != cache.entries.end();)
    it = it->second.gen + 1 < cache.generation ? cache.entries.erase(it)
                                               : std::next(it);
}

SampleCacheStats sampleCacheStats(const SampleCache& cache) {
  auto& c = const_cast<SampleCache&>(cache);
  std::lock_guard<std::mutex> lk(c.m);
  SampleCacheStats s;
  for (auto& [key, e] : c.entries) {
    if (!e.data) continue;
    s.entries++;
    s.bytes += e.data->interleaved.size() * sizeof(double);
  }
  s.rendered = c.rendered;
  s.reusedPrior = c.reusedPrior;
  return s;
}

// Each placement replays a fixed finite slice of its source: a Sample is
// a value, so every placement of it must produce identical content. The
// shared SampleCache exploits exactly that guarantee - the first
// placement renders the window once and all placements (of all
// timestamps, on all planner threads) serve slices of it. When the cache
// is unavailable (entry mid-build on another thread), the placement
// falls back to its own private evaluation context, which isolates
// stateful nodes (filters, fm, delay) per placement - the canonical
// `mix_all [place k 0s; place k 500ms]` idiom (§5.1) holds either way.
struct PlaceState final : NodeState {
  std::shared_ptr<const CachedSample> cached;
  std::unique_ptr<RenderCtx> sub;
};

struct PlaceNode final : SigNode {
  SigPtr source;
  double from, to, at;
  PlaceNode(SigPtr src, double f, double t, double a)
      : source(std::move(src)), from(f), to(t), at(a) {
    if (f < 0 || t < f)
      throw EngineError("sample: invalid window [" + std::to_string(f) +
                        "s, " + std::to_string(t) + "s)");
    if (a < 0)
      throw EngineError("place: negative timestamp");
    contentHash = hashDouble(
        hashDouble(hashDouble(hashMix(hashTag(15), source->contentHash),
                              from),
                   to),
        at);
  }
  int channels() const override { return source->channels(); }
  std::unique_ptr<NodeState> makeState() const override {
    return std::make_unique<PlaceState>();
  }
  bool computeBlock(RenderCtx& ctx, NodeState& st0, int64_t start, int frames,
                    double* out) const override {
    int64_t nAt = llround(at * ctx.rate);
    int64_t nFrom = llround(from * ctx.rate);
    int64_t len = llround((to - from) * ctx.rate);
    int64_t end = nAt + len;
    // Fully outside the placement window: silence, and the source is never
    // touched. This is what makes long mixes of short samples cheap.
    if (start >= end || start + (int64_t)frames <= nAt) return true;
    auto& st = static_cast<PlaceState&>(st0);
    int64_t s = std::max(start, nAt);
    int64_t e = std::min(start + (int64_t)frames, end);
    int cc = concreteChannels();
    // Adopt the shared window once, before any private replay starts.
    if (!st.cached && !st.sub && ctx.sampleCache)
      st.cached = ctx.sampleCache->acquire(source, from, to, ctx.rate);
    if (st.cached) {
      const CachedSample& cs = *st.cached;
      int64_t q0 = s - nAt, q1 = e - nAt;  // frame span within the window
      if (q1 <= cs.nzBegin || q0 >= cs.nzEnd) return true;
      std::fill(out, out + (size_t)frames * (size_t)cc, 0.0);
      std::memcpy(out + (size_t)(s - start) * (size_t)cc,
                  cs.interleaved.data() + (size_t)(q0 * cs.cc),
                  (size_t)(q1 - q0) * (size_t)cc * sizeof(double));
      return false;
    }
    if (!st.sub) {
      st.sub = std::make_unique<RenderCtx>(ctx.rate);
      st.sub->sampleCache = ctx.sampleCache;  // nested samples still share
    }
    int q = (int)(e - s);
    // The window maps onto a contiguous run of source frames, so the
    // private context only ever sees monotonically advancing queries.
    bool srcSilent = false;
    const double* p =
        source->renderBlock(*st.sub, s - nAt + nFrom, q, srcSilent);
    if (srcSilent) return true;
    std::fill(out, out + (size_t)frames * (size_t)cc, 0.0);
    std::memcpy(out + (size_t)(s - start) * (size_t)cc, p,
                (size_t)q * (size_t)cc * sizeof(double));
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(source);
  }
};

// --- Resampling ------------------------------------------------------------

// Time warping: out(t) = input(phi(t)), phi' = rate. The rate signal is
// read on the *output* time base (an ordinary streaming pull); the source
// is read on the warped base, so it needs a private context and a sliding
// window - hence the state below.
struct ResampleState final : NodeState {
  double pos = 0;                  // read head, in fractional source frames
  std::unique_ptr<RenderCtx> sub;  // the source's private context
  std::vector<double> buf;         // sliding source window, interleaved
  int64_t bufStart = 0;            // source frame index of buf's first frame
};

struct ResampleNode final : SigNode {
  SigPtr source, rate;
  ResampleNode(SigPtr s, SigPtr r) : source(std::move(s)), rate(std::move(r)) {
    int rc = rate->channels();
    if (rc != 1 && rc != -1)
      throw EngineError("resample: the rate must be a mono signal");
    contentHash =
        hashMix(hashMix(hashTag(18), source->contentHash), rate->contentHash);
  }
  int channels() const override { return source->channels(); }
  bool stateful() const override { return true; }
  std::unique_ptr<NodeState> makeState() const override {
    return std::make_unique<ResampleState>();
  }
  bool computeBlock(RenderCtx& ctx, NodeState& st0, int64_t start, int frames,
                    double* out) const override {
    auto& st = static_cast<ResampleState&>(st0);
    int cc = concreteChannels();
    Block r = pull(ctx, rate, start, frames);
    if (!st.sub) {
      st.sub = std::make_unique<RenderCtx>(ctx.rate);
      st.sub->sampleCache = ctx.sampleCache;  // nested samples still share
      // preRendered is deliberately not inherited: a warped source would be
      // served at the wrong offset (same reason as PlaceNode).
    }
    bool anySound = false;
    for (int f = 0; f < frames; f++) {
      int64_t i0 = (int64_t)std::floor(st.pos);
      // Extend the window to cover [i0, i0+1], compacting the consumed
      // prefix first so both the copy and the buffer stay O(kBlockFrames).
      while (st.bufStart + (int64_t)(st.buf.size() / (size_t)cc) < i0 + 2) {
        if (i0 > st.bufStart) {
          size_t drop = (size_t)(i0 - st.bufStart) * (size_t)cc;
          if (drop >= st.buf.size()) st.buf.clear();
          else st.buf.erase(st.buf.begin(), st.buf.begin() + (ptrdiff_t)drop);
          st.bufStart = i0;
        }
        int64_t next = st.bufStart + (int64_t)(st.buf.size() / (size_t)cc);
        bool srcSilent = false;
        const double* p =
            source->renderBlock(*st.sub, next, kBlockFrames, srcSilent);
        size_t old = st.buf.size();
        size_t n = (size_t)kBlockFrames * (size_t)cc;
        st.buf.resize(old + n);
        if (srcSilent) std::fill(st.buf.begin() + (ptrdiff_t)old, st.buf.end(),
                                 0.0);
        else std::memcpy(st.buf.data() + old, p, n * sizeof(double));
      }
      const double* w = st.buf.data() + (size_t)(i0 - st.bufStart) * (size_t)cc;
      double frac = st.pos - (double)i0;
      for (int c = 0; c < cc; c++) {
        double s0 = w[c], s1 = w[(size_t)cc + (size_t)c];
        double v = s0 + (s1 - s0) * frac;
        out[(size_t)f * (size_t)cc + (size_t)c] = v;
        if (v != 0.0) anySound = true;
      }
      double rr = r.at(f, 0);
      // Written so NaN trips too. Reverse playback is out of scope: a
      // negative rate holds the read head instead of rewinding.
      if (!(rr <= kMaxResampleRate))
        throw EngineError("resample: rate " + std::to_string(rr) +
                          " exceeds the maximum of " +
                          std::to_string(kMaxResampleRate) +
                          " (the source would have to advance " +
                          std::to_string(rr) + "x faster than the output)");
      // One output second advances rr source seconds, so one output frame
      // advances rr * (1/ctx.rate) * ctx.rate = rr source frames.
      st.pos += rr < 0 ? 0.0 : rr;
    }
    return !anySound;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(source);
    fn(rate);
  }
};

// --- File signals ----------------------------------------------------------

struct FileNode final : SigNode {
  std::vector<std::vector<double>> data;  // per channel
  double fileRate;
  size_t maxLen = 0;
  FileNode(std::vector<std::vector<double>> d, double r)
      : data(std::move(d)), fileRate(r) {
    if (data.empty()) throw EngineError("audio file has no channels");
    if ((int)data.size() > kMaxChannels)
      throw EngineError("audio files with more than " +
                        std::to_string(kMaxChannels) +
                        " channels are not supported in v1");
    for (auto& c : data) maxLen = std::max(maxLen, c.size());
    contentHash = hashDouble(hashTag(16), fileRate);
    for (auto& c : data) {
      contentHash = hashMix(contentHash, (uint64_t)c.size());
      for (double v : c) contentHash = hashDouble(contentHash, v);
    }
  }
  int channels() const override { return (int)data.size(); }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    int nCh = (int)data.size();
    {
      double pos0 = ((double)start / ctx.rate) * fileRate;
      if ((size_t)pos0 >= maxLen) return true;  // past the end of the file
    }
    for (int f = 0; f < frames; f++) {
      double t = (double)(start + f) / ctx.rate;
      double pos = t * fileRate;
      size_t i0 = (size_t)pos;
      double frac = pos - (double)i0;
      for (int c = 0; c < nCh; c++) {
        const auto& buf = data[(size_t)c];
        double v = 0;
        if (i0 < buf.size()) {
          double s0 = buf[i0];
          double s1 = i0 + 1 < buf.size() ? buf[i0 + 1] : 0.0;
          v = s0 + (s1 - s0) * frac;
        }
        out[f * nCh + c] = v;
      }
    }
    return false;
  }
};

// --- Factories -------------------------------------------------------------

SigPtr makeOsc(OscKind kind, double freq) {
  return std::make_shared<OscNode>(kind, freq);
}
SigPtr makeFm(double carrier, SigPtr modulator) {
  return std::make_shared<FmNode>(carrier, std::move(modulator));
}

// Two-step FM noise: stage 1 is an FM operator driven hard by a sine whose
// frequency relates to the center by the golden ratio (the "most
// irrational" number, so the stages never phase-lock into a periodic
// tone); stage 2 is FM again, with stage 1 as its modulator at a deviation
// several times the center frequency. Both indices are far above 1, which
// smears the sidebands into a dense, chaotic, noise-like spectrum.
SigPtr makeNoise(double freq) {
  if (freq <= 0) throw EngineError("noise: frequency must be positive");
  constexpr double kPhi = 1.6180339887498949;
  SigPtr innerMod = makeBinOp(SigBinOp::Mul, makeOsc(OscKind::Sine, freq / kPhi),
                              makeConst(freq * kPhi * 3.0));
  SigPtr stage1 = makeFm(freq * kPhi, innerMod);
  return makeFm(freq,
                makeBinOp(SigBinOp::Mul, stage1, makeConst(freq * 8.0)));
}
SigPtr makePm(double carrier, SigPtr modulator) {
  return std::make_shared<PmNode>(carrier, std::move(modulator));
}
SigPtr makeAm(SigPtr carrier, SigPtr modulator, double depth) {
  return std::make_shared<AmNode>(std::move(carrier), std::move(modulator),
                                  depth);
}
SigPtr makeDelay(double by, SigPtr input) {
  return std::make_shared<DelayNode>(by, std::move(input));
}
SigPtr makeReverb(double decay, double damping, double mix, SigPtr input) {
  return std::make_shared<ReverbNode>(decay, damping, mix, std::move(input));
}
SigPtr makeExpDecay(double rate) { return std::make_shared<ExpDecayNode>(rate); }
SigPtr makeAdsr(double a, double d, double s, double r, double hold,
                double decayCurve, double releaseCurve) {
  // Past this the exponential is a step in all but name, and far past it
  // e^-k overflows into NaN samples - better to say so at construction.
  for (double k : {decayCurve, releaseCurve})
    if (!(std::fabs(k) <= kMaxEnvCurvature))
      throw EngineError("adsr: segment curvature " + std::to_string(k) +
                        " is outside [-" + std::to_string(kMaxEnvCurvature) +
                        ", " + std::to_string(kMaxEnvCurvature) + "]");
  return std::make_shared<AdsrNode>(a, d, s, r, hold, decayCurve,
                                    releaseCurve);
}
SigPtr makeConst(double v) { return std::make_shared<ConstNode>(v); }
SigPtr makeTime() { return std::make_shared<TimeNode>(); }
SigPtr makeFilter(FilterKind kind, double cutoff, SigPtr input) {
  return std::make_shared<FilterNode>(kind, cutoff, std::move(input));
}
SigPtr makeModFilter(FilterKind kind, SigPtr cutoff, SigPtr input) {
  return std::make_shared<ModFilterNode>(kind, std::move(cutoff),
                                         std::move(input));
}
SigPtr makeResonant(SigPtr cutoff, double q, SigPtr input) {
  return std::make_shared<ResonantNode>(std::move(cutoff), q,
                                        std::move(input));
}
SigPtr makeFollow(double attack, double release, SigPtr input) {
  return std::make_shared<FollowNode>(attack, release, std::move(input));
}
SigPtr makeSelect(SigPtr gate, double threshold, SigPtr above, SigPtr below) {
  return std::make_shared<SelectNode>(std::move(gate), threshold,
                                      std::move(above), std::move(below));
}
SigPtr makeFeedbackDelay(double by, double gain, SigPtr input) {
  return std::make_shared<FeedbackDelayNode>(by, gain, std::move(input));
}
SigPtr makeChannel(int n, SigPtr input) {
  return std::make_shared<ChannelNode>(n, std::move(input));
}
SigPtr makeClip(ClipKind kind, double threshold, SigPtr input) {
  return std::make_shared<ClipNode>(kind, threshold, std::move(input));
}
namespace {
// Shared tail of the fused-node factories: stack depth (operands push,
// binary operators pop one net, unary operators are depth-neutral) and
// the program content hash.
void finalizeFused(FusedNode& node) {
  int depth = 0;
  for (const auto& o : node.program) {
    if (o.k == FusedNode::Op::K::Input || o.k == FusedNode::Op::K::Const)
      depth++;
    else if (!FusedNode::isUnary(o.k))
      depth--;
    node.depthNeed = std::max(node.depthNeed, depth);
  }
  node.contentHash = hashTag(12);
  for (const auto& o : node.program) {
    node.contentHash = hashMix(node.contentHash, (uint64_t)o.k);
    if (o.k == FusedNode::Op::K::Input)
      node.contentHash =
          hashMix(node.contentHash, node.inputs[(size_t)o.input]->contentHash);
    else if (o.k == FusedNode::Op::K::Const)
      node.contentHash = hashDouble(node.contentHash, o.value);
  }
}
}  // namespace

namespace {
// Channel merge for elementwise arithmetic: unlike a mix, arithmetic
// broadcasts a mono operand across a multichannel one (the operator
// table's `Scalar Signal (*) Vector Signal` row; FusedNode's perFrame
// slots carry the mono side compactly). Broadcast-only (-1) operands
// adapt as everywhere else; two concrete multichannel widths must match.
int arithMerge(int a, int b) {
  if (a == -1 || a == b) return b == -1 ? a : b;
  if (b == -1) return a;
  if (a == 1) return b;
  if (b == 1) return a;
  throw EngineError("signal arithmetic: channel count mismatch (" +
                    std::to_string(a) + " vs " + std::to_string(b) + ")");
}
}  // namespace

SigPtr makeBinOp(SigBinOp op, SigPtr l, SigPtr r) {
  FusedNode::Op::K k = FusedNode::Op::K::Add;
  switch (op) {
    case SigBinOp::Add: k = FusedNode::Op::K::Add; break;
    case SigBinOp::Sub: k = FusedNode::Op::K::Sub; break;
    case SigBinOp::Mul: k = FusedNode::Op::K::Mul; break;
    case SigBinOp::Div: k = FusedNode::Op::K::Div; break;
    case SigBinOp::Pow: k = FusedNode::Op::K::Pow; break;
  }
  auto node = std::make_shared<FusedNode>();
  node->ch = arithMerge(l->channels(), r->channels());
  fusedAppendOperand(l, node->inputs, node->program);
  fusedAppendOperand(r, node->inputs, node->program);
  node->program.push_back({k, 0, 0});
  if (node->program.size() > kMaxFusedOps) {
    // Too big to inline: keep both operands as inputs. Their own fused
    // programs stay intact, so long chains fuse in bounded chunks.
    node->inputs.clear();
    node->program.clear();
    fusedAppendInput(l, node->inputs, node->program);
    fusedAppendInput(r, node->inputs, node->program);
    node->program.push_back({k, 0, 0});
  }
  finalizeFused(*node);
  return node;
}
SigPtr makeUnaryOp(SigUnaryOp op, SigPtr x) {
  FusedNode::Op::K k = FusedNode::Op::K::Log;
  switch (op) {
    case SigUnaryOp::Exp: k = FusedNode::Op::K::Exp; break;
    case SigUnaryOp::Sqrt: k = FusedNode::Op::K::Sqrt; break;
    case SigUnaryOp::Log: k = FusedNode::Op::K::Log; break;
    case SigUnaryOp::Sin: k = FusedNode::Op::K::Sin; break;
    case SigUnaryOp::Cos: k = FusedNode::Op::K::Cos; break;
    case SigUnaryOp::Tan: k = FusedNode::Op::K::Tan; break;
    case SigUnaryOp::Atan: k = FusedNode::Op::K::Atan; break;
    case SigUnaryOp::Abs: k = FusedNode::Op::K::Abs; break;
  }
  auto node = std::make_shared<FusedNode>();
  node->ch = x->channels();
  fusedAppendOperand(x, node->inputs, node->program);
  node->program.push_back({k, 0, 0});
  if (node->program.size() > kMaxFusedOps) {
    node->inputs.clear();
    node->program.clear();
    fusedAppendInput(x, node->inputs, node->program);
    node->program.push_back({k, 0, 0});
  }
  finalizeFused(*node);
  return node;
}
SigPtr makeMix(std::vector<SigPtr> items) {
  return std::make_shared<MixNode>(std::move(items));
}
SigPtr makeChannels(std::vector<SigPtr> monoItems) {
  return std::make_shared<ChannelsNode>(std::move(monoItems));
}
SigPtr makePlace(SigPtr source, double from, double to, double at) {
  return std::make_shared<PlaceNode>(std::move(source), from, to, at);
}
SigPtr makeResample(SigPtr input, SigPtr rate) {
  return std::make_shared<ResampleNode>(std::move(input), std::move(rate));
}
SigPtr makeFileSignal(std::vector<std::vector<double>> channelData,
                      double fileRate) {
  return std::make_shared<FileNode>(std::move(channelData), fileRate);
}

int signalChannels(const SigPtr& s) { return s->channels(); }

// --- Rendering -------------------------------------------------------------

namespace {

// The parallel-render planner. A render's top is usually a cheap
// combination "spine" (mixes, arithmetic, channel assembly, elementwise or
// stateful wrappers) over a handful of expensive, independent subtrees: a
// master summing buses, a stems overview stacking lanes, a stereo pair
// built from two big per-side mixes. decompose() walks the spine and
// collects the subtrees hanging off it as leaves; the leaves render on
// worker threads and the spine replays on the main thread with the leaves'
// blocks injected into its context, so the combination arithmetic is the
// exact same code (and order) as a sequential render.
constexpr int kDecomposeDepth = 8;

void decomposeInto(const SigPtr& n, int depth, std::vector<SigPtr>& leaves) {
  if (depth > 0) {
    if (auto* mix = dynamic_cast<const MixNode*>(n.get())) {
      for (auto& x : mix->items) decomposeInto(x, depth - 1, leaves);
      return;
    }
    if (auto* chans = dynamic_cast<const ChannelsNode*>(n.get())) {
      for (auto& x : chans->items) decomposeInto(x, depth - 1, leaves);
      return;
    }
    if (auto* fused = dynamic_cast<const FusedNode*>(n.get())) {
      // The fused program is the spine's arithmetic; its inputs are the
      // independent subtrees hanging off it.
      for (auto& x : fused->inputs) decomposeInto(x, depth - 1, leaves);
      return;
    }
    // Walk through single-input wrappers (filter, clip, reverb, delay...)
    // so a `soft_clip (mix_all ...)` master still decomposes. Placements
    // are opaque: their source evaluates in a private context that the
    // main thread's injection could not reach.
    if (!dynamic_cast<const PlaceNode*>(n.get())) {
      std::vector<SigPtr> kids;
      n->forEachChild([&](const SigPtr& c) { kids.push_back(c); });
      if (kids.size() == 1) {
        decomposeInto(kids[0], depth - 1, leaves);
        return;
      }
    }
  }
  leaves.push_back(n);
}

// Nodes whose per-render state would live in a shared context. The walk
// stops below placements: a placement replays its source in a private
// context, so two leaves sharing a source through distinct placements do
// not actually share any state (and already duplicate that work in a
// sequential render). A resampler is the same for its source, but its
// rate signal is pulled from the shared context, so that child is walked.
void collectSharedState(const SigNode* n,
                        std::unordered_set<const SigNode*>& out) {
  if (!out.insert(n).second) return;
  if (dynamic_cast<const PlaceNode*>(n)) return;
  if (auto* rs = dynamic_cast<const ResampleNode*>(n)) {
    collectSharedState(rs->rate.get(), out);
    return;
  }
  n->forEachChild(
      [&](const SigPtr& c) { collectSharedState(c.get(), out); });
}

struct UnionFind {
  std::vector<int> p;
  explicit UnionFind(int n) : p((size_t)n) {
    for (int i = 0; i < n; i++) p[(size_t)i] = i;
  }
  int find(int x) {
    while (p[(size_t)x] != x) x = p[(size_t)x] = p[(size_t)p[(size_t)x]];
    return x;
  }
  void unite(int a, int b) { p[(size_t)find(a)] = find(b); }
};

// Groups leaves so that any two leaves sharing a stateful-context node land
// in the same group; groups are then provably state-disjoint and can render
// on different threads, each in its own context, while shared work inside a
// group stays shared (and memoized) exactly as in a sequential render.
std::vector<std::vector<int>> groupLeaves(const std::vector<SigPtr>& leaves) {
  int n = (int)leaves.size();
  UnionFind uf(n);
  std::unordered_map<const SigNode*, int> firstOwner;
  for (int i = 0; i < n; i++) {
    std::unordered_set<const SigNode*> reach;
    collectSharedState(leaves[(size_t)i].get(), reach);
    for (const SigNode* node : reach) {
      auto it = firstOwner.find(node);
      if (it == firstOwner.end()) firstOwner.emplace(node, i);
      else uf.unite(i, it->second);
    }
  }
  std::unordered_map<int, int> rootGroup;
  std::vector<std::vector<int>> groups;
  for (int i = 0; i < n; i++) {
    int r = uf.find(i);
    auto it = rootGroup.find(r);
    if (it == rootGroup.end()) {
      rootGroup.emplace(r, (int)groups.size());
      groups.push_back({i});
    } else {
      groups[(size_t)it->second].push_back(i);
    }
  }
  return groups;
}

bool containsSharedImpl(const SigNode* n, const SigNode* needle,
                        std::unordered_set<const SigNode*>& seen) {
  if (n == needle) return true;
  if (!seen.insert(n).second) return false;
  if (dynamic_cast<const PlaceNode*>(n)) return false;
  // A resampler's source is served from a private context (no preRendered),
  // its rate from the shared one.
  if (auto* rs = dynamic_cast<const ResampleNode*>(n))
    return containsSharedImpl(rs->rate.get(), needle, seen);
  bool found = false;
  n->forEachChild([&](const SigPtr& c) {
    if (!found) found = containsSharedImpl(c.get(), needle, seen);
  });
  return found;
}

}  // namespace

bool graphContainsShared(const SigPtr& root, const SigNode* needle) {
  std::unordered_set<const SigNode*> seen;
  return containsSharedImpl(root.get(), needle, seen);
}

Rendered renderWindow(const SigPtr& node, double from, double to, double rate,
                      int maxThreads, const PreRenderedMap* preRendered,
                      SampleCache* sampleCache) {
  if (rate <= 0) throw EngineError("render: sample rate must be positive");
  if (from < 0 || to < from)
    throw EngineError("render: invalid sample window");
  Rendered out;
  out.channels = node->channels() == -1 ? 1 : node->channels();
  int64_t nFrom = llround(from * rate);
  int64_t nTo = llround(to * rate);
  out.frames = nTo - nFrom;
  out.interleaved.resize((size_t)(out.frames * out.channels));
  if (out.frames == 0) return out;

  auto emit = [&](int64_t start, int frames, const double* p, bool silent) {
    size_t off = (size_t)(start - nFrom) * (size_t)out.channels;
    size_t count = (size_t)frames * (size_t)out.channels;
    if (silent)
      std::fill(out.interleaved.begin() + (ptrdiff_t)off,
                out.interleaved.begin() + (ptrdiff_t)(off + count), 0.0);
    else
      std::memcpy(out.interleaved.data() + off, p, count * sizeof(double));
    out.blockSilent.push_back(silent ? 1 : 0);  // blocks emit in order
  };

  if (maxThreads <= 0) {
    unsigned hw = std::thread::hardware_concurrency();
    maxThreads = hw > 0 ? (int)hw : 1;
  }

  std::vector<SigPtr> leaves;
  decomposeInto(node, kDecomposeDepth, leaves);
  // When every leaf is already served by a pre-rendered window, the
  // render is a single cheap spine pass; worker threads would only add
  // per-block barrier overhead.
  bool allServed = preRendered && !leaves.empty();
  if (allServed)
    for (auto& l : leaves)
      if (!preRendered->count(l.get())) { allServed = false; break; }
  std::vector<std::vector<int>> groups;
  if (!allServed && maxThreads > 1 && leaves.size() > 1)
    groups = groupLeaves(leaves);
  int workers = std::min<int>(maxThreads, (int)groups.size());

  // Sample-window cache: every placement of the same (source, window)
  // pair - across the whole graph and all planner threads - discretizes
  // it once and serves slices. Callers may pass a longer-lived cache
  // (shared across a build's targets, or across daemon rebuilds via
  // content-hash keys); otherwise the cache lives for this render.
  SampleCache localSampleCache;
  SampleCache* sc = sampleCache ? sampleCache : &localSampleCache;
  RenderCtx mainCtx(rate);
  mainCtx.preRendered = preRendered;
  mainCtx.sampleCache = sc;

  if (workers < 2) {
    // Sequential block loop.
    for (int64_t n = nFrom; n < nTo; n += kBlockFrames) {
      int f = (int)std::min<int64_t>(kBlockFrames, nTo - n);
      bool silent = false;
      const double* p = node->renderBlock(mainCtx, n, f, silent);
      emit(n, f, p, silent);
    }
    return out;
  }

  std::vector<int> leafGroup(leaves.size(), 0);
  for (int g = 0; g < (int)groups.size(); g++)
    for (int li : groups[(size_t)g]) leafGroup[(size_t)li] = g;
  std::vector<std::unique_ptr<RenderCtx>> groupCtx;
  groupCtx.reserve(groups.size());
  for (size_t g = 0; g < groups.size(); g++) {
    groupCtx.push_back(std::make_unique<RenderCtx>(rate));
    groupCtx.back()->preRendered = preRendered;
    groupCtx.back()->sampleCache = sc;
  }

  // Per-block barrier scheduling: for every block, the workers claim groups
  // off an atomic counter and render each group's leaves in that group's
  // private context; the main thread waits for all of them, injects the
  // leaf blocks into its own context and replays the spine, whose mix/
  // arithmetic nodes then find every leaf pre-cached. Dynamic claiming
  // balances load when groups differ wildly in cost (a bus versus a single
  // placed sample).
  struct Barrier {
    std::mutex m;
    std::condition_variable cvWork, cvDone;
    int64_t gen = 0;
    int64_t blockStart = 0;
    int blockFrames = 0;
    int workersDone = 0;
    bool stop = false;
    std::atomic<int> nextGroup{0};
    std::exception_ptr error;
  } bar;

  auto workerMain = [&]() {
    int64_t seenGen = 0;
    for (;;) {
      int64_t bs;
      int bf;
      {
        std::unique_lock<std::mutex> lk(bar.m);
        bar.cvWork.wait(lk, [&] { return bar.stop || bar.gen != seenGen; });
        if (bar.stop) return;
        seenGen = bar.gen;
        bs = bar.blockStart;
        bf = bar.blockFrames;
      }
      for (;;) {
        int g = bar.nextGroup.fetch_add(1);
        if (g >= (int)groups.size()) break;
        try {
          for (int li : groups[(size_t)g]) {
            bool silent = false;
            leaves[(size_t)li]->renderBlock(*groupCtx[(size_t)g], bs, bf,
                                            silent);
          }
        } catch (...) {
          std::lock_guard<std::mutex> lk(bar.m);
          if (!bar.error) bar.error = std::current_exception();
        }
      }
      {
        std::lock_guard<std::mutex> lk(bar.m);
        if (++bar.workersDone == workers) bar.cvDone.notify_one();
      }
    }
  };

  std::vector<std::thread> pool;
  pool.reserve((size_t)workers);
  for (int w = 0; w < workers; w++) pool.emplace_back(workerMain);
  auto shutdown = [&] {
    {
      std::lock_guard<std::mutex> lk(bar.m);
      bar.stop = true;
    }
    bar.cvWork.notify_all();
    for (auto& t : pool)
      if (t.joinable()) t.join();
  };

  try {
    for (int64_t n = nFrom; n < nTo; n += kBlockFrames) {
      int f = (int)std::min<int64_t>(kBlockFrames, nTo - n);
      {
        std::lock_guard<std::mutex> lk(bar.m);
        bar.blockStart = n;
        bar.blockFrames = f;
        bar.workersDone = 0;
        bar.nextGroup.store(0);
        bar.gen++;
      }
      bar.cvWork.notify_all();
      {
        std::unique_lock<std::mutex> lk(bar.m);
        bar.cvDone.wait(lk, [&] { return bar.workersDone == workers; });
        if (bar.error) {
          std::exception_ptr err = bar.error;
          lk.unlock();
          shutdown();
          std::rethrow_exception(err);
        }
      }
      // Inject the workers' leaf blocks into the main context as
      // pre-cached results, then replay the spine over them.
      for (size_t li = 0; li < leaves.size(); li++) {
        const SigNode& leaf = *leaves[li];
        NodeState& gs =
            groupCtx[(size_t)leafGroup[li]]->stateFor(leaf);
        // A leaf served from a pre-rendered window never touches its
        // group state; the main context resolves it the same way, so
        // there is nothing to inject.
        if (gs.cachedStart != n || gs.cachedFrames != f) continue;
        NodeState& ms = mainCtx.stateFor(leaf);
        ms.cachedStart = n;
        ms.cachedFrames = f;
        ms.cachedSilent = gs.cachedSilent;
        if (!gs.cachedSilent) {
          size_t count = (size_t)f * (size_t)leaf.concreteChannels();
          if (ms.buf.size() < count) ms.buf.resize(count);
          std::memcpy(ms.buf.data(), gs.buf.data(), count * sizeof(double));
        }
      }
      bool silent = false;
      const double* p = node->renderBlock(mainCtx, n, f, silent);
      emit(n, f, p, silent);
    }
  } catch (...) {
    shutdown();
    throw;
  }
  shutdown();
  return out;
}

}  // namespace synth
