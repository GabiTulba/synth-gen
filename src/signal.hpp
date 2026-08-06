#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace synth {

// The signal engine. Signals are conceptually continuous and infinite;
// user programs build a DAG of SigNodes and discretization happens only
// when a render target is evaluated at a concrete rate (design doc §5,
// §8.2).
//
// Rendering is block-based: nodes produce kBlockFrames frames per call,
// so per-node bookkeeping (state lookup, virtual dispatch, memoization)
// is paid once per block instead of once per frame, and inner loops are
// tight loops over arrays. Nodes can also report a block as known-silent,
// which lets mixes skip placed samples outside their windows entirely.
//
// Channel counts are static: they are known once the graph is built
// (audio files are read at build time), so channel mismatches are
// detected before any frame is computed. v1 caps channels at
// kMaxChannels. A channel count of -1 means "broadcast": a scalar
// constant that adapts to whatever it is combined with (its blocks carry
// one value per frame).

constexpr int kMaxChannels = 16;
constexpr int kBlockFrames = 1024;

struct EngineError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct SigNode;
using SigPtr = std::shared_ptr<const SigNode>;

struct NodeState {
  // Last computed block (memo for DAG sharing within a block).
  int64_t cachedStart = -1;
  int cachedFrames = 0;
  bool cachedSilent = false;
  std::vector<double> buf;  // concreteChannels() * frames, interleaved
  // Stateful nodes: the next frame index to be computed sequentially.
  int64_t nextSeq = 0;
  virtual ~NodeState() = default;
};

// A discretized window: `frames` frames of `channels` interleaved values.
// blockSilent records, per kBlockFrames render block, whether the engine
// proved the block all-zero while rendering (structural silence). It is
// advisory: a false entry only means "not proven", and consumers serving
// this window re-check by scanning.
struct Rendered {
  int channels = 1;
  int64_t frames = 0;
  std::vector<double> interleaved;
  std::vector<uint8_t> blockSilent;
};

// A finished render of some node's window, indexed by the frame (at the
// consuming render's rate) its first sample corresponds to. renderBlock
// serves blocks that fall inside the window verbatim from the buffer, so
// a build that has already rendered a bus for its own target can hand
// that buffer to every other target whose graph contains the same node
// (a master summing buses, an overview stacking lanes) instead of
// re-discretizing the subtree. The buffer must stay alive for the whole
// consuming render, and must have been produced at the same rate.
struct PreRenderedWindow {
  int64_t startFrame = 0;
  const Rendered* data = nullptr;
};
using PreRenderedMap = std::unordered_map<const SigNode*, PreRenderedWindow>;

// Per-render evaluation context. Each render instantiates fresh state, so
// a shared subgraph used by several targets never leaks filter state
// between renders. Stateful nodes are evaluated at monotonically
// increasing frame indices and catch up block-by-block when queried with
// a gap (this is what gives placed samples "from the epoch" semantics).
struct RenderCtx {
  double rate;
  // Optional cross-render reuse; not inherited by placements' private
  // contexts (a placement remaps time, so a shared window would be
  // served at the wrong offset there).
  const PreRenderedMap* preRendered = nullptr;
  std::unordered_map<const SigNode*, std::unique_ptr<NodeState>> states;

  explicit RenderCtx(double r) : rate(r) {}
  NodeState& stateFor(const SigNode& node);
};

struct SigNode : std::enable_shared_from_this<SigNode> {
  virtual ~SigNode() = default;

  // Static channel count; -1 = broadcast.
  virtual int channels() const = 0;
  int concreteChannels() const {
    int c = channels();
    return c < 0 ? 1 : c;
  }
  virtual bool stateful() const { return false; }
  virtual std::unique_ptr<NodeState> makeState() const {
    return std::make_unique<NodeState>();
  }

  // Fills `out` with frames * concreteChannels() interleaved values for
  // [start, start+frames). Returns true if the block is known all-zero;
  // the contents of `out` are then unspecified.
  virtual bool computeBlock(RenderCtx& ctx, NodeState& st, int64_t start,
                            int frames, double* out) const = 0;

  // Enumerate direct SigNode children (for graph analysis such as the
  // parallel-render planner).
  virtual void forEachChild(
      const std::function<void(const SigPtr&)>& fn) const {
    (void)fn;
  }

  // Memoized, sequence-enforcing entry point. Returns a pointer to the
  // block data, or nullptr if the block is silent.
  const double* renderBlock(RenderCtx& ctx, int64_t start, int frames,
                            bool& silent) const;
};

enum class OscKind { Sine, Saw, Square };
enum class FilterKind { Lowpass, Highpass };
enum class ClipKind { Hard, Soft };
enum class SigBinOp { Add, Sub, Mul, Div };

SigPtr makeOsc(OscKind kind, double freq);
// Deterministic pseudo-noise: two cascaded FM stages with golden-ratio
// frequency relationships and large modulation indices. Chaotic,
// broadband, bounded to [-1, 1], and bit-identical across renders.
SigPtr makeNoise(double freq);
// FM: sine with instantaneous frequency carrier + modulator(t) Hz,
// integrated sample-by-sample from the epoch (stateful).
SigPtr makeFm(double carrier, SigPtr modulator);
// PM: sin(2*pi*carrier*t + modulator(t)); modulator in radians.
SigPtr makePm(double carrier, SigPtr modulator);
// AM: carrier * (1 + depth * modulator). The modulator must be mono (or a
// broadcast constant); it applies to every carrier channel.
SigPtr makeAm(SigPtr carrier, SigPtr modulator, double depth);
// Feedforward delay: input shifted `by` seconds later; silence before.
SigPtr makeDelay(double by, SigPtr input);
// Schroeder reverb. decay: RT60-style tail length in seconds; damping and
// mix in [0,1]. Per-channel filter banks; the feedback lives inside the
// node's state, not in the (acyclic) signal graph.
SigPtr makeReverb(double decay, double damping, double mix, SigPtr input);
SigPtr makeExpDecay(double rate);
SigPtr makeAdsr(double attack, double decay, double sustain, double release,
                double hold);
SigPtr makeConst(double value);  // broadcast scalar
SigPtr makeFilter(FilterKind kind, double cutoff, SigPtr input);
// Distortion: hard clamps flat at +/-threshold; soft saturates smoothly as
// threshold*tanh(x/threshold). threshold must be positive.
SigPtr makeClip(ClipKind kind, double threshold, SigPtr input);
SigPtr makeBinOp(SigBinOp op, SigPtr l, SigPtr r);
SigPtr makeMix(std::vector<SigPtr> items);
SigPtr makeChannels(std::vector<SigPtr> monoItems);
// A placed sample: silence outside [at, at + (to-from)), the source
// signal's window [from, to) inside it. Each placement evaluates its
// source in a private context, so repeated placements of one sample
// replay identical content even for stateful sources.
SigPtr makePlace(SigPtr source, double from, double to, double at);
// An audio file as a signal: [0, duration) then silence;
// linear-interpolation resampling from the file's native rate.
SigPtr makeFileSignal(std::vector<std::vector<double>> channelData,
                      double fileRate);

// Renders `node`'s window [from, to) seconds at `rate` into an
// interleaved buffer. Returns the concrete channel count (broadcast-only
// graphs render as mono). Throws EngineError on channel mismatch or
// invalid windows.
//
// When the top of the graph decomposes into an additive core whose
// summands have disjoint state (e.g. a master that sums independent
// buses), the summands render on up to `maxThreads` worker threads with
// a per-block barrier; summation order is preserved, so the result is
// identical to a sequential render. maxThreads <= 0 means "hardware
// concurrency"; pass 1 to force sequential rendering.
//
// `preRendered` (optional) supplies already-discretized windows for nodes
// occurring in this graph; matching blocks are served from those buffers
// instead of being recomputed. Renders are deterministic, so the result
// is identical either way.
Rendered renderWindow(const SigPtr& node, double from, double to, double rate,
                      int maxThreads = 0,
                      const PreRenderedMap* preRendered = nullptr);

// True if `needle` occurs in `root`'s graph outside of any placement's
// private subtree - i.e. at a position where a pre-rendered window for
// `needle` would actually be served (placements remap time, so their
// sources are excluded). Used by the build system to schedule targets
// that contain other targets' signals after them.
bool graphContainsShared(const SigPtr& root, const SigNode* needle);

}  // namespace synth
