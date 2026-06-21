///
/// Strategy registry - statically owns all available AutoPlay strategies.
///
/// To add a new strategy:
///   1. Implement it under strategies/ (inheriting IAutoPlayStrategy)
///   2. Add a header include here
///   3. Add an instance to kStrategies[] in the same order as the
///      AUTOPLAY_STRATEGY_* constants in core/config.h
///

#include "strategy_registry.h"
#include "strategies/greedy.h"
#include "strategies/potential.h"
#include "strategies/reachability.h"

#include <algorithm>
#include <array>

namespace {

// Order MUST match AUTOPLAY_STRATEGY_* in core/config.h.
GreedyStrategy g_greedy;
ReachabilityStrategy g_reachability;
PotentialFieldStrategy g_potential;

IAutoPlayStrategy *const kStrategies[] = {
    &g_greedy,
    &g_reachability,
    &g_potential,
};

constexpr int kCount =
    static_cast<int>(sizeof(kStrategies) / sizeof(kStrategies[0]));

int Clamp(int id) { return std::clamp(id, 0, kCount - 1); }

} // namespace

namespace AutoPlayRegistry {

int Count() { return kCount; }

IAutoPlayStrategy &Get(int id) { return *kStrategies[Clamp(id)]; }

const char *Name(int id) { return kStrategies[Clamp(id)]->Name(); }

} // namespace AutoPlayRegistry
