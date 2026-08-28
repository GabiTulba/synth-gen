// synth engine API (public). External implementations include this (via
// <synth/external.hpp>) to build signal graphs with the same constructors
// the engine itself uses; the symbols resolve against the host process at
// dlopen time. The host's signal.hpp includes it too - this file is the
// single definition of the constructor surface.
#pragma once
#include <memory>
#include <vector>

namespace synth {

// A signal: a lazy, conceptually continuous graph node. Opaque here -
// externals combine and return signals, only the engine discretizes them.
struct SigNode;
using SigPtr = std::shared_ptr<const SigNode>;

enum class OscKind { Sine, Saw, Square, SawBl, SquareBl };
enum class FilterKind { Lowpass, Highpass };
enum class ClipKind { Hard, Soft };
enum class SigBinOp { Add, Sub, Mul, Div, Pow };
enum class SigUnaryOp { Exp, Sqrt, Log, Sin, Cos, Tan, Atan, Abs };

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
// Attack is always a linear ramp; the decay and release segments take
// their shape from a curvature each, independently. A curvature of 0 is
// a straight ramp; positive falls fast and tails off (the larger, the
// sharper - the fall spans e^0..e^-k before it is rescaled onto the
// segment's endpoints); negative is the mirror, slow first then steep.
// Both are bounded by kMaxEnvCurvature.
constexpr double kMaxEnvCurvature = 64.0;
SigPtr makeAdsr(double attack, double decay, double sustain, double release,
                double hold, double decayCurve, double releaseCurve);
SigPtr makeConst(double value);  // broadcast scalar
SigPtr makeTime();               // t in seconds (broadcast ramp)
SigPtr makeFilter(FilterKind kind, double cutoff, SigPtr input);
// One-pole filter with a signal-rate cutoff (Hz, sampled per frame). The
// cutoff must be mono (or a broadcast constant); it applies to every
// channel of the input, exactly like am's modulator rule.
SigPtr makeModFilter(FilterKind kind, SigPtr cutoff, SigPtr input);
// Two-pole state-variable lowpass with resonance. `cutoff` is a mono
// signal in Hz; `q` is the resonance (0.5 ~ no resonance, higher rings).
// Cutoffs are clamped to a stable fraction of the render rate.
SigPtr makeResonant(SigPtr cutoff, double q, SigPtr input);
// Envelope follower: full-wave rectification into an attack/release
// one-pole smoother. The input must be mono; the output is a mono
// control signal in [0, +inf).
SigPtr makeFollow(double attack, double release, SigPtr input);
// Sample-wise select: gate(t) >= threshold picks `above`, else `below`.
// The gate must be mono; above/below merge channels like a mix.
SigPtr makeSelect(SigPtr gate, double threshold, SigPtr above, SigPtr below);
// Feedback delay: out(t) = in(t) + gain * out(t - by). The feedback loop
// lives inside the node's per-render state (the language-level graph
// stays acyclic, like reverb); |gain| < 1 and by > 0 are validated at
// construction, and `by` is bounded below by one output frame.
SigPtr makeFeedbackDelay(double by, double gain, SigPtr input);
// Channel extraction: channel `n` (0-based) of a multichannel signal as
// a mono signal - the inverse `channels` never had. The index is
// validated against the input's static channel count at graph build.
SigPtr makeChannel(int n, SigPtr input);
// Distortion: hard clamps flat at +/-threshold; soft saturates smoothly as
// threshold*tanh(x/threshold). threshold must be positive.
SigPtr makeClip(ClipKind kind, double threshold, SigPtr input);
SigPtr makeBinOp(SigBinOp op, SigPtr l, SigPtr r);
// Elementwise transcendental (IEEE domain semantics: log of a
// non-positive input or sqrt of a negative one yield -inf/NaN samples).
SigPtr makeUnaryOp(SigUnaryOp op, SigPtr x);
SigPtr makeMix(std::vector<SigPtr> items);
SigPtr makeChannels(std::vector<SigPtr> monoItems);
// A placed sample: silence outside [at, at + (to-from)), the source
// signal's window [from, to) inside it. Each placement evaluates its
// source in a private context, so repeated placements of one sample
// replay identical content even for stateful sources.
SigPtr makePlace(SigPtr source, double from, double to, double at);
// Time-warping resampler: out(t) = input(phi(t)) where phi' = rate. The
// rate signal must be mono; it is a multiplier on the source's playback
// speed (1 = identity, 0.5 = an octave down, 2 = an octave up). Negative
// rates clamp to zero - the source is only ever read forward, which is
// what keeps the read window bounded and satisfies the engine's rule that
// stateful nodes are never queried backwards. Like a placement, the
// source runs in a private context.
SigPtr makeResample(SigPtr input, SigPtr rate);
// Ceiling on the rate multiplier; beyond it a render is almost certainly
// a runaway rather than an intended effect (and the source pull per output
// block would grow without bound).
constexpr double kMaxResampleRate = 64.0;

// Static channel count of a signal's graph; -1 for broadcast-only
// graphs, which adapt to any width.
int signalChannels(const SigPtr& s);

}  // namespace synth
