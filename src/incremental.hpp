#pragma once
#include <cstdint>

#include "checker.hpp"

namespace synth {

// Dependency tracking for incremental rebuilds (Epic 8). Because the
// language is pure, the value a top-level definition evaluates to is a
// deterministic function of its own source text and of the definitions it
// references. defClosureHash computes a Merkle-style content hash over
// that closure: it changes iff the definition's source, or the source of
// anything it (transitively) references - across modules - changes.
// Primitive semantics are covered by an engine-version salt folded in by
// the caller.
uint64_t defClosureHash(const Program& prog, const CheckedModule& mod,
                        const TopDef& def);

// FNV-1a helpers shared with the build cache.
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
uint64_t fnv1a(const void* data, size_t len, uint64_t seed = kFnvOffset);
inline uint64_t fnvCombine(uint64_t seed, uint64_t value) {
  return fnv1a(&value, sizeof value, seed);
}

}  // namespace synth
