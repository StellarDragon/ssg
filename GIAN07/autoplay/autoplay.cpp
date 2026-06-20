///
/// AutoPlay facade implementation.
///

#include "autoplay.h"
#include "core/config.h"
#include "strategy_registry.h"

AutoPlayController AutoPlay;

void AutoPlayController::SetDifficulty(int level) {
  AutoPlayRegistry::Get(ConfigDat.AutoPlayStrategy.v).SetDifficulty(level);
}

INPUT_BITS AutoPlayController::Update() {
  auto &strategy = AutoPlayRegistry::Get(ConfigDat.AutoPlayStrategy.v);
  strategy.SetDifficulty(ConfigDat.AutoPlayDifficulty.v);
  return strategy.Update();
}
