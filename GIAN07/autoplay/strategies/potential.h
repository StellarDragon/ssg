///
/// PotentialFieldStrategy - Heuristic vector-field dodging.
///
/// Each frame, sum a few O(N) vector forces and snap the resultant to an
/// 8-direction key:
///   - Bullets / lasers / hostile enemies: 1/r^2 repulsion (with lead-time
///     prediction on bullets).
///   - Play-field walls: 1/r^2 repulsion from each of the 4 edges.
///   - Attack position (just under the nearest enemy/boss): constant pull.
///   - Items in pickup range: weak attractor.
///
/// No graph search, no per-frame BFS - this is an O(bullets + lasers)
/// reactive controller. It's smooth, fast, and naturally avoids the
/// "wall-hugging" pathology of survival-greedy planners, at the cost of
/// occasionally getting stuck in symmetric pincer patterns.
///

#pragma once

#include "strategy.h"
#include <cstdint>

class PotentialFieldStrategy : public IAutoPlayStrategy {
public:
  static constexpr int DIFFICULTY_EASY = 0;
  static constexpr int DIFFICULTY_NORMAL = 1;
  static constexpr int DIFFICULTY_HARD = 2;

  const char *Name() const override { return "PotenF"; }
  void SetDifficulty(int level) override;
  void Reset() override;
  INPUT_BITS Update() override;

private:
  struct Vec2 {
    int64_t x;
    int64_t y;
  };

  int LeadFrames() const;
  int FocusRange() const;
  int BombRadius() const;
  int BombThreshold() const;

  void AccumulateBullets(Vec2 &force, int px, int py, int &near_count,
                         int &min_dist_sq);
  void AccumulateLasers(Vec2 &force, int px, int py, int &near_count,
                        int &min_dist_sq);
  void AccumulateEnemies(Vec2 &force, int px, int py);
  void AccumulateWalls(Vec2 &force, int px, int py);
  void AccumulateAttack(Vec2 &force, int px, int py);
  void AccumulateItems(Vec2 &force, int px, int py);

  static int VectorToDir(int64_t fx, int64_t fy);
  static INPUT_BITS DirToKeys(int dir);

  int difficulty_ = DIFFICULTY_NORMAL;
  int prev_dir_ = 0;
  uint8_t frame_counter_ = 0;
};
