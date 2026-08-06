#include "signal.hpp"

#include <algorithm>
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

int mergeChannels(int a, int b, const char* what) {
  if (a == -1) return b;
  if (b == -1) return a;
  if (a != b)
    throw EngineError(std::string(what) + ": channel count mismatch (" +
                      std::to_string(a) + " vs " + std::to_string(b) + ")");
  return a;
}

double applyOp(SigBinOp op, double a, double b) {
  switch (op) {
    case SigBinOp::Add: return a + b;
    case SigBinOp::Sub: return a - b;
    case SigBinOp::Mul: return a * b;
    case SigBinOp::Div: return a / b;
  }
  return 0;
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
  OscNode(OscKind k, double f) : kind(k), freq(f) {}
  int channels() const override { return 1; }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    for (int f = 0; f < frames; f++) {
      double t = (double)(start + f) / ctx.rate;
      double phase = freq * t;
      double frac = phase - std::floor(phase);
      switch (kind) {
        case OscKind::Sine: out[f] = std::sin(2.0 * kPi * phase); break;
        case OscKind::Saw: out[f] = 2.0 * frac - 1.0; break;
        case OscKind::Square: out[f] = frac < 0.5 ? 1.0 : -1.0; break;
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
  explicit ExpDecayNode(double r) : rate(r) {}
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

// Envelope shape: linear attack to 1 over [0, a); linear decay to `sustain`
// over [a, a+d); sustain until `hold`; linear release to 0 over
// [hold, hold+r); 0 afterwards.
struct AdsrNode final : SigNode {
  double a, d, s, r, hold;
  AdsrNode(double a_, double d_, double s_, double r_, double h)
      : a(a_), d(d_), s(s_), r(r_), hold(h) {}
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
      else if (t < a + d) v = d > 0 ? 1.0 - (1.0 - s) * ((t - a) / d) : s;
      else if (t < releaseStart) v = s;
      else if (t < releaseStart + r)
        v = r > 0 ? s * (1.0 - (t - releaseStart) / r) : 0.0;
      else v = 0;
      out[f] = v;
    }
    return false;
  }
};

struct ConstNode final : SigNode {
  double value;
  explicit ConstNode(double v) : value(v) {}
  int channels() const override { return -1; }
  bool computeBlock(RenderCtx&, NodeState&, int64_t, int frames,
                    double* out) const override {
    if (value == 0.0) return true;
    std::fill(out, out + frames, value);
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
      : kind(k), cutoff(c), input(std::move(in)) {}
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

struct BinOpNode final : SigNode {
  SigBinOp op;
  SigPtr l, r;
  int ch;
  BinOpNode(SigBinOp o, SigPtr l_, SigPtr r_)
      : op(o), l(std::move(l_)), r(std::move(r_)),
        ch(mergeChannels(l->channels(), r->channels(), "signal arithmetic")) {}
  int channels() const override { return ch; }
  bool computeBlock(RenderCtx& ctx, NodeState&, int64_t start, int frames,
                    double* out) const override {
    // Both sides are always pulled, even when silence lets the arithmetic
    // be skipped: stateful subtrees must keep advancing in lockstep or a
    // shared node would later be queried backwards.
    Block a = pull(ctx, l, start, frames);
    Block b = pull(ctx, r, start, frames);
    if ((op == SigBinOp::Add || op == SigBinOp::Sub) && a.silent() &&
        b.silent())
      return true;
    if (op == SigBinOp::Mul && (a.silent() || b.silent())) return true;
    // Div is never short-circuited: 0/0 must still produce NaN.
    int cc = concreteChannels();
    for (int f = 0; f < frames; f++)
      for (int i = 0; i < cc; i++)
        out[f * cc + i] = applyOp(op, a.at(f, i), b.at(f, i));
    return false;
  }
  void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const override {
    fn(l);
    fn(r);
  }
};

struct MixNode final : SigNode {
  std::vector<SigPtr> items;
  int ch;
  explicit MixNode(std::vector<SigPtr> xs) : items(std::move(xs)), ch(-1) {
    for (auto& x : items) ch = mergeChannels(ch, x->channels(), "mix_all");
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

// Each placement owns a private evaluation context for its source subtree:
// a Sample is a fixed finite slice, so every placement of it must replay
// identical content. Isolating state per placement makes that hold even
// when the source contains stateful nodes (filters, fm, delay) and the
// same sample value is placed at several timestamps - the canonical
// `mix_all [place k 0s; place k 500ms]` idiom (§5.1).
struct PlaceState final : NodeState {
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
    if (!st.sub) st.sub = std::make_unique<RenderCtx>(ctx.rate);
    int64_t s = std::max(start, nAt);
    int64_t e = std::min(start + (int64_t)frames, end);
    int q = (int)(e - s);
    // The window maps onto a contiguous run of source frames, so the
    // private context only ever sees monotonically advancing queries.
    bool srcSilent = false;
    const double* p =
        source->renderBlock(*st.sub, s - nAt + nFrom, q, srcSilent);
    if (srcSilent) return true;
    int cc = concreteChannels();
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
SigPtr makeAdsr(double a, double d, double s, double r, double hold) {
  return std::make_shared<AdsrNode>(a, d, s, r, hold);
}
SigPtr makeConst(double v) { return std::make_shared<ConstNode>(v); }
SigPtr makeFilter(FilterKind kind, double cutoff, SigPtr input) {
  return std::make_shared<FilterNode>(kind, cutoff, std::move(input));
}
SigPtr makeClip(ClipKind kind, double threshold, SigPtr input) {
  return std::make_shared<ClipNode>(kind, threshold, std::move(input));
}
SigPtr makeBinOp(SigBinOp op, SigPtr l, SigPtr r) {
  return std::make_shared<BinOpNode>(op, std::move(l), std::move(r));
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
SigPtr makeFileSignal(std::vector<std::vector<double>> channelData,
                      double fileRate) {
  return std::make_shared<FileNode>(std::move(channelData), fileRate);
}

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
    if (auto* bin = dynamic_cast<const BinOpNode*>(n.get())) {
      decomposeInto(bin->l, depth - 1, leaves);
      decomposeInto(bin->r, depth - 1, leaves);
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
// sequential render).
void collectSharedState(const SigNode* n,
                        std::unordered_set<const SigNode*>& out) {
  if (!out.insert(n).second) return;
  if (dynamic_cast<const PlaceNode*>(n)) return;
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
                      int maxThreads, const PreRenderedMap* preRendered) {
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
  };

  if (maxThreads <= 0) {
    unsigned hw = std::thread::hardware_concurrency();
    maxThreads = hw > 0 ? (int)hw : 1;
  }

  std::vector<SigPtr> leaves;
  decomposeInto(node, kDecomposeDepth, leaves);
  std::vector<std::vector<int>> groups;
  if (maxThreads > 1 && leaves.size() > 1) groups = groupLeaves(leaves);
  int workers = std::min<int>(maxThreads, (int)groups.size());

  RenderCtx mainCtx(rate);
  mainCtx.preRendered = preRendered;

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
