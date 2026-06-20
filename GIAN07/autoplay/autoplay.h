///
/// AutoPlay - facade that dispatches per-frame input generation to the
/// currently configured strategy (see autoplay/strategies/*).
///

#pragma once

#include "game/input.h"

class AutoPlayController {
public:
  // Forwards SetDifficulty to the active strategy. Difficulty values match
  // the AUTOPLAY_DIFFICULTY_* constants in core/config.h (0=Easy, 1=Normal,
  // 2=Hard).
  void SetDifficulty(int level);

  // Produces one frame of synthesized player input.
  INPUT_BITS Update();
};

extern AutoPlayController AutoPlay;
