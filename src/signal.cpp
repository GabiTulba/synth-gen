#include "signal.hpp"

#include <cmath>

namespace synth {

namespace {
constexpr double kPi = 3.14159265358979323846;

Frame scalarFrame(double v) {
  Frame f;
  f.ch = -1;
  f.v[0] = v;
  return f;
}

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

// Broadcast-aware element access: a frame with ch == -1 supplies its single
// value for every channel.
double chanAt(const Frame& f, int i) { return f.ch == -1 ? f.v[0] : f.v[i]; }

}  // namespace

NodeState& RenderCtx::stateFor(const SigNode& node) {
  auto it = states.find(&node);
  if (it == states.end())
    it = states.emplace(&node, node.makeState()).first;
  return *it->second;
}

Frame SigNode::get(RenderCtx& ctx, int64_t n) const {
  NodeState& st = ctx.stateFor(*this);
  if (n == st.lastN) return st.last;
  if (stateful()) {
    if (n < st.lastN)
      throw EngineError("internal error: stateful node queried backwards");
    for (int64_t m = st.lastN + 1; m <= n; m++) st.last = compute(ctx, st, m);
  } else {
    st.last = compute(ctx, st, n);
  }
  st.lastN = n;
  return st.last;
}

// --- Generators ------------------------------------------------------------

struct OscNode final : SigNode {
  OscKind kind;
  double freq;
  OscNode(OscKind k, double f) : kind(k), freq(f) {}
  int channels() const override { return 1; }
  Frame compute(RenderCtx& ctx, NodeState&, int64_t n) const override {
    double t = (double)n / ctx.rate;
    double phase = freq * t;
    double frac = phase - std::floor(phase);
    Frame f;
    f.ch = 1;
    switch (kind) {
      case OscKind::Sine: f.v[0] = std::sin(2.0 * kPi * phase); break;
      case OscKind::Saw: f.v[0] = 2.0 * frac - 1.0; break;
      case OscKind::Square: f.v[0] = frac < 0.5 ? 1.0 : -1.0; break;
    }
    return f;
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
  Frame compute(RenderCtx& ctx, NodeState& st0, int64_t n) const override {
    auto& st = static_cast<FmState&>(st0);
    Frame f;
    f.ch = 1;
    f.v[0] = std::sin(2.0 * kPi * st.phase);
    double freq = carrier + chanAt(modulator->get(ctx, n), 0);
    st.phase += freq / ctx.rate;
    st.phase -= std::floor(st.phase);  // keep precision over long renders
    return f;
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
  Frame compute(RenderCtx& ctx, NodeState&, int64_t n) const override {
    double t = (double)n / ctx.rate;
    Frame f;
    f.ch = 1;
    f.v[0] = std::sin(2.0 * kPi * carrier * t +
                      chanAt(modulator->get(ctx, n), 0));
    return f;
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
  Frame compute(RenderCtx& ctx, NodeState&, int64_t n) const override {
    Frame c = carrier->get(ctx, n);
    double gain = 1.0 + depth * chanAt(modulator->get(ctx, n), 0);
    Frame out;
    out.ch = c.ch;
    int count = c.ch == -1 ? 1 : c.ch;
    for (int i = 0; i < count; i++) out.v[i] = chanAt(c, i) * gain;
    return out;
  }
};

// Feedforward delay: out(t) = in(t - by) for t >= by, silence before.
// Implemented with a ring buffer rather than by querying the input at
// shifted indices: the input is pulled at the same monotonically increasing
// frame as every other consumer, so a subgraph shared between dry and
// delayed paths (the echo idiom) keeps its stateful nodes consistent.
struct DelayState final : NodeState {
  std::vector<double> ring;  // shiftFrames * channelCount doubles, zeroed
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
  Frame compute(RenderCtx& ctx, NodeState& st0, int64_t n) const override {
    auto& st = static_cast<DelayState&>(st0);
    Frame in = input->get(ctx, n);
    int64_t shift = llround(by * ctx.rate);
    if (shift == 0) return in;
    int count = in.ch == -1 ? 1 : in.ch;
    if (st.ring.empty()) st.ring.assign((size_t)(shift * count), 0.0);
    size_t slot = (size_t)((n % shift) * count);
    Frame out;
    out.ch = in.ch;
    // The slot still holds the frame written `shift` steps ago (zero for
    // the first `shift` frames); read it, then overwrite with the current
    // input for the future read.
    for (int i = 0; i < count; i++) {
      out.v[i] = st.ring[slot + (size_t)i];
      st.ring[slot + (size_t)i] = chanAt(in, i);
    }
    return out;
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
  Frame compute(RenderCtx& ctx, NodeState& st0, int64_t n) const override {
    auto& st = static_cast<ReverbState&>(st0);
    Frame in = input->get(ctx, n);
    int count = in.ch == -1 ? 1 : in.ch;
    if (!st.ready) {
      for (int i = 0; i < 4; i++)
        st.combLen[i] = std::max<int64_t>(1, llround(kCombDelays[i] * ctx.rate));
      for (int i = 0; i < 2; i++)
        st.allpassLen[i] =
            std::max<int64_t>(1, llround(kAllpassDelays[i] * ctx.rate));
      st.banks.resize(count);
      for (auto& b : st.banks) {
        for (int i = 0; i < 4; i++) b.comb[i].assign((size_t)st.combLen[i], 0.0);
        for (int i = 0; i < 2; i++)
          b.allpass[i].assign((size_t)st.allpassLen[i], 0.0);
      }
      st.ready = true;
    }
    Frame out;
    out.ch = in.ch;
    for (int c = 0; c < count; c++) {
      auto& b = st.banks[(size_t)c];
      double x = chanAt(in, c);
      double wet = 0;
      for (int i = 0; i < 4; i++) {
        size_t pos = (size_t)(n % st.combLen[i]);
        double read = b.comb[i][pos];
        // Damped feedback: a one-pole lowpass inside the loop.
        b.combFilt[i] = read * (1.0 - damping) + b.combFilt[i] * damping;
        double g = decay > 0
                       ? std::pow(10.0, -3.0 * kCombDelays[i] / decay)
                       : 0.0;
        b.comb[i][pos] = x + b.combFilt[i] * g;
        wet += read;
      }
      wet *= 0.25;
      for (int i = 0; i < 2; i++) {
        size_t pos = (size_t)(n % st.allpassLen[i]);
        double buffered = b.allpass[i][pos];
        b.allpass[i][pos] = wet + buffered * kAllpassGain;
        wet = buffered - wet;
      }
      out.v[c] = x * (1.0 - mix) + wet * mix;
    }
    return out;
  }
};

struct ExpDecayNode final : SigNode {
  double rate;
  explicit ExpDecayNode(double r) : rate(r) {}
  int channels() const override { return 1; }
  Frame compute(RenderCtx& ctx, NodeState&, int64_t n) const override {
    double t = (double)n / ctx.rate;
    Frame f;
    f.ch = 1;
    f.v[0] = std::exp(-rate * t);
    return f;
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
  Frame compute(RenderCtx& ctx, NodeState&, int64_t n) const override {
    double t = (double)n / ctx.rate;
    double v;
    double releaseStart = std::max(hold, a + d);
    if (t < 0) v = 0;
    else if (t < a) v = a > 0 ? t / a : 1.0;
    else if (t < a + d) v = d > 0 ? 1.0 - (1.0 - s) * ((t - a) / d) : s;
    else if (t < releaseStart) v = s;
    else if (t < releaseStart + r)
      v = r > 0 ? s * (1.0 - (t - releaseStart) / r) : 0.0;
    else v = 0;
    Frame f;
    f.ch = 1;
    f.v[0] = v;
    return f;
  }
};

struct ConstNode final : SigNode {
  double value;
  explicit ConstNode(double v) : value(v) {}
  int channels() const override { return -1; }
  Frame compute(RenderCtx&, NodeState&, int64_t) const override {
    return scalarFrame(value);
  }
};

// --- Filters ---------------------------------------------------------------

struct FilterState final : NodeState {
  std::array<double, kMaxChannels> lp{};   // low-pass accumulator
  std::array<double, kMaxChannels> lastIn{};
  bool primed = false;
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
  Frame compute(RenderCtx& ctx, NodeState& st0, int64_t n) const override {
    auto& st = static_cast<FilterState&>(st0);
    Frame in = input->get(ctx, n);
    int ch = in.ch == -1 ? 1 : in.ch;
    double alpha = 1.0 - std::exp(-2.0 * kPi * cutoff / ctx.rate);
    if (alpha > 1.0) alpha = 1.0;
    Frame out;
    out.ch = ch;
    for (int i = 0; i < ch; i++) {
      double x = chanAt(in, i);
      if (!st.primed) st.lp[i] = kind == FilterKind::Lowpass ? x : 0.0;
      st.lp[i] += alpha * (x - st.lp[i]);
      out.v[i] = kind == FilterKind::Lowpass ? st.lp[i] : x - st.lp[i];
    }
    st.primed = true;
    return out;
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
  Frame compute(RenderCtx& ctx, NodeState&, int64_t n) const override {
    Frame a = l->get(ctx, n);
    Frame b = r->get(ctx, n);
    Frame out;
    out.ch = ch;
    int count = ch == -1 ? 1 : ch;
    for (int i = 0; i < count; i++)
      out.v[i] = applyOp(op, chanAt(a, i), chanAt(b, i));
    return out;
  }
};

struct MixNode final : SigNode {
  std::vector<SigPtr> items;
  int ch;
  explicit MixNode(std::vector<SigPtr> xs) : items(std::move(xs)), ch(-1) {
    for (auto& x : items) ch = mergeChannels(ch, x->channels(), "mix_all");
  }
  int channels() const override { return ch; }
  Frame compute(RenderCtx& ctx, NodeState&, int64_t n) const override {
    Frame out;
    out.ch = ch;
    int count = ch == -1 ? 1 : ch;
    for (int i = 0; i < count; i++) out.v[i] = 0;
    for (auto& x : items) {
      Frame f = x->get(ctx, n);
      for (int i = 0; i < count; i++) out.v[i] += chanAt(f, i);
    }
    return out;
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
  Frame compute(RenderCtx& ctx, NodeState&, int64_t n) const override {
    Frame out;
    out.ch = (int)items.size();
    for (size_t i = 0; i < items.size(); i++)
      out.v[i] = chanAt(items[i]->get(ctx, n), 0);
    return out;
  }
};

// --- Placement -------------------------------------------------------------

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
  Frame compute(RenderCtx& ctx, NodeState&, int64_t n) const override {
    int64_t nAt = llround(at * ctx.rate);
    int64_t nFrom = llround(from * ctx.rate);
    int64_t len = llround((to - from) * ctx.rate);
    if (n < nAt || n >= nAt + len) {
      Frame silence;
      silence.ch = channels();
      int count = silence.ch == -1 ? 1 : silence.ch;
      for (int i = 0; i < count; i++) silence.v[i] = 0;
      return silence;
    }
    return source->get(ctx, n - nAt + nFrom);
  }
};

// --- File signals ----------------------------------------------------------

struct FileNode final : SigNode {
  std::vector<std::vector<double>> data;  // per channel
  double fileRate;
  FileNode(std::vector<std::vector<double>> d, double r)
      : data(std::move(d)), fileRate(r) {
    if (data.empty()) throw EngineError("audio file has no channels");
    if ((int)data.size() > kMaxChannels)
      throw EngineError("audio files with more than " +
                        std::to_string(kMaxChannels) +
                        " channels are not supported in v1");
  }
  int channels() const override { return (int)data.size(); }
  Frame compute(RenderCtx& ctx, NodeState&, int64_t n) const override {
    double t = (double)n / ctx.rate;
    double pos = t * fileRate;
    size_t i0 = (size_t)pos;
    double frac = pos - (double)i0;
    Frame out;
    out.ch = (int)data.size();
    for (size_t c = 0; c < data.size(); c++) {
      const auto& buf = data[c];
      double v = 0;
      if (i0 < buf.size()) {
        double s0 = buf[i0];
        double s1 = i0 + 1 < buf.size() ? buf[i0 + 1] : 0.0;
        v = s0 + (s1 - s0) * frac;
      }
      out.v[c] = v;
    }
    return out;
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

Rendered renderWindow(const SigPtr& node, double from, double to,
                      double rate) {
  if (rate <= 0) throw EngineError("render: sample rate must be positive");
  if (from < 0 || to < from)
    throw EngineError("render: invalid sample window");
  Rendered out;
  out.channels = node->channels() == -1 ? 1 : node->channels();
  int64_t nFrom = llround(from * rate);
  int64_t nTo = llround(to * rate);
  out.frames = nTo - nFrom;
  out.interleaved.resize((size_t)(out.frames * out.channels));
  RenderCtx ctx(rate);
  for (int64_t n = nFrom; n < nTo; n++) {
    Frame f = node->get(ctx, n);
    for (int c = 0; c < out.channels; c++)
      out.interleaved[(size_t)((n - nFrom) * out.channels + c)] = chanAt(f, c);
  }
  return out;
}

}  // namespace synth
