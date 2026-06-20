///
/// ReachabilityStrategy - grid-based safe-region planner.
///
/// Per-frame algorithm:
///   1. Build a danger field on a coarse grid covering the play area, for
///      each future frame up to HORIZON. A cell is marked dangerous at
///      frame t if any bullet / enemy / laser hitbox (expanded by half a
///      cell) covers that cell after linear extrapolation by t frames.
///   2. Time-expanded BFS from the player cell with a one-cell-per-frame
///      move budget (4-connected + stay-in-place), rejecting cells that
///      are dangerous at the arrival frame. Stores arrival time per cell
///      and a parent pointer for path reconstruction.
///   3. Score every reachable cell on:
///        - survival horizon from arrival (how many more safe frames the
///          cell affords if we stop there),
///        - bias toward an attack position (just under the nearest enemy),
///        - mild bias toward play-field center,
///      pick the best-scoring cell as a target.
///   4. Reconstruct the BFS path and emit the input bits for its first
///      step (8-direction encoding falls out of the next-cell delta).
///   5. Bomb only when the player cell is dangerous within the next 2
///      frames AND no reachable target has positive score (deathbomb).
///   6. Focus when at least one neighbor cell is dangerous in the next
///      two frames (precision dodging).
///

#pragma once

#include "strategy.h"

#include <array>
#include <cstdint>
#include <vector>

class ReachabilityStrategy : public IAutoPlayStrategy {
public:
  static constexpr int DIFFICULTY_EASY = 0;
  static constexpr int DIFFICULTY_NORMAL = 1;
  static constexpr int DIFFICULTY_HARD = 2;

  // Game units per grid cell (1 px = 64 game units). 256 = 4 px.
  static constexpr int CELL_SIZE = 256;
  static constexpr int CELL_HALF = CELL_SIZE / 2;
  // Max time horizon (frames). Difficulty caps actual horizon below this.
  static constexpr int MAX_HORIZON = 12;

  const char *Name() const override { return "Reach"; }
  void SetDifficulty(int level) override;
  void Reset() override;
  INPUT_BITS Update() override;

private:
  // Per-frame danger bitset (row-major; 1 bit per cell).
  using Bitset = std::vector<uint64_t>;

  int difficulty_ = DIFFICULTY_NORMAL;
  uint8_t frame_counter_ = 0;
  // Grid sized lazily on first Update().
  bool grid_inited_ = false;
  int grid_w_ = 0;
  int grid_h_ = 0;
  int cells_total_ = 0;
  int words_per_frame_ = 0;
  std::array<Bitset, MAX_HORIZON + 1> danger_;
  // BFS state.
  std::vector<int16_t> arrival_; // -1 if unreachable
  std::vector<int16_t> parent_;  // direction index from which we came

  void EnsureGridSized();
  void ClearDanger();
  int Horizon() const;
  int SafetyExpand() const;

  void GameToCell(int gx, int gy, int &cx, int &cy) const;
  void CellCenterGame(int cx, int cy, int &gx, int &gy) const;
  inline int CellIndex(int cx, int cy) const { return cy * grid_w_ + cx; }
  inline bool InGrid(int cx, int cy) const {
    return cx >= 0 && cy >= 0 && cx < grid_w_ && cy < grid_h_;
  }
  inline void SetDanger(int frame, int cx, int cy);
  inline bool IsDanger(int frame, int cx, int cy) const;

  // Mark cells covered by an axis-aligned hitbox (in game coords) at frame t.
  void MarkBox(int frame, int gx, int gy, int hx, int hy);

  void RasterizeBullets();
  void RasterizeEnemies();
  void RasterizeLasers();

  void BFS(int start_cx, int start_cy);

  // Returns true when a target cell was found and writes (tcx, tcy).
  bool SelectTarget(int pcx, int pcy, int &tcx, int &tcy);

  // Walks the BFS parent chain back from target to the first step, returns
  // direction index (1..8, 0 = stay) for that step.
  int FirstStepDirection(int pcx, int pcy, int tcx, int tcy) const;

  bool NeedBomb(int pcx, int pcy) const;
  bool NeedFocus(int pcx, int pcy) const;

  static INPUT_BITS DirectionToKeys(int dir);
};
