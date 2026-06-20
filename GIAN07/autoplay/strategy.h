///
/// IAutoPlayStrategy - Abstract base for AutoPlay AI algorithms.
///
/// Each concrete strategy lives in autoplay/strategies/ and is registered in
/// strategy_registry.cpp. The active strategy is selected at runtime via
/// ConfigDat.AutoPlayStrategy and dispatched by AutoPlayController::Update().
///

#pragma once

#include "game/input.h"

class IAutoPlayStrategy {
public:
  virtual ~IAutoPlayStrategy() = default;

  // Short label shown in the menu (max ~6 chars to fit the existing layout).
  virtual const char *Name() const = 0;

  // 0=Easy, 1=Normal, 2=Hard. Called once per frame; cheap to ignore.
  virtual void SetDifficulty(int level) = 0;

  // Called at game start (currently invoked once on first Update of a new
  // game; strategies that hold cross-frame state should clear it here).
  virtual void Reset() = 0;

  // Produce a 1-frame INPUT_BITS for the player. Caller masks in ESC/BOMB/
  // RETURN from real input afterwards.
  virtual INPUT_BITS Update() = 0;
};
