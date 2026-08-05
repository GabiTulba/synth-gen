#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace synth {

// The signal engine. Signals are conceptually continuous and infinite;
// user programs build a DAG of SigNodes and discretization happens only when
// a render target is evaluated at a concrete rate (design doc §5, §8.2).
//
// Channel counts are static: they are known once the graph is built (audio
// files are read at build time), so channel mismatches are detected before
// any frame is computed. v1 caps channels at kMaxChannels.

constexpr int kMaxChannels = 16;

// Channel count -1 means "broadcast": a scalar constant that adapts to the
// channel count of whatever it is combined with.
struct Frame {
  int ch = 1;
  std::array<double, kMaxChannels> v{};
};

struct EngineError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct SigNode;
using SigPtr = std::shared_ptr<const SigNode>;

struct NodeState {
  int64_t lastN = -1;
  Frame last;
  virtual ~NodeState() = default;
};

// Per-render evaluation context. Each render instantiates fresh state, so a
// shared subgraph used by several targets never leaks filter state between
// renders. Nodes are evaluated at monotonically non-decreasing frame
// indices; stateful nodes (filters) catch up frame-by-frame when queried
// with a gap (this is what gives placed samples "from the epoch" semantics).
struct RenderCtx {
  double rate;
  std::unordered_map<const SigNode*, std::unique_ptr<NodeState>> states;

  explicit RenderCtx(double r) : rate(r) {}
  NodeState& stateFor(const SigNode& node);
};

struct SigNode : std::enable_shared_from_this<SigNode> {
  virtual ~SigNode() = default;

  // Static channel count; -1 = broadcast.
  virtual int channels() const = 0;
  virtual bool stateful() const { return false; }
  virtual std::unique_ptr<NodeState> makeState() const {
    return std::make_unique<NodeState>();
  }
  virtual Frame compute(RenderCtx& ctx, NodeState& st, int64_t n) const = 0;

  // Memoized entry point (safe for DAG sharing within one frame).
  Frame get(RenderCtx& ctx, int64_t n) const;
};

enum class OscKind { Sine, Saw, Square };
enum class FilterKind { Lowpass, Highpass };
enum class SigBinOp { Add, Sub, Mul, Div };

SigPtr makeOsc(OscKind kind, double freq);
SigPtr makeExpDecay(double rate);
SigPtr makeAdsr(double attack, double decay, double sustain, double release,
                double hold);
SigPtr makeConst(double value);  // broadcast scalar
SigPtr makeFilter(FilterKind kind, double cutoff, SigPtr input);
SigPtr makeBinOp(SigBinOp op, SigPtr l, SigPtr r);
SigPtr makeMix(std::vector<SigPtr> items);
SigPtr makeChannels(std::vector<SigPtr> monoItems);
// A placed sample: silence outside [at, at + (to-from)), the source signal's
// window [from, to) inside it.
SigPtr makePlace(SigPtr source, double from, double to, double at);
// An audio file as a signal: [0, duration) then silence; linear-interpolation
// resampling from the file's native rate.
SigPtr makeFileSignal(std::vector<std::vector<double>> channelData,
                      double fileRate);

// Renders `node`'s window [from, to) seconds at `rate` into an interleaved
// buffer. Returns the concrete channel count (broadcast-only graphs render
// as mono). Throws EngineError on channel mismatch or invalid windows.
struct Rendered {
  int channels = 1;
  int64_t frames = 0;
  std::vector<double> interleaved;
};
Rendered renderWindow(const SigPtr& node, double from, double to, double rate);

}  // namespace synth
