///
/// AutoPlay strategy registry.
///
/// All available strategies are statically registered. Index ordering matches
/// the AUTOPLAY_STRATEGY_* constants in core/config.h.
///

#pragma once

#include "strategy.h"

namespace AutoPlayRegistry {

int Count();

// Returns a reference to the requested strategy, clamped to a valid index.
IAutoPlayStrategy &Get(int id);

const char *Name(int id);

} // namespace AutoPlayRegistry
