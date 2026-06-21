///
/// ReachabilityStrategy implementation. See header for algorithm overview.
///

#include "reachability.h"
#include "bullet/bullet.h"
#include "bullet/laser_manager.h"
#include "core/gian.h"
#include "enemy/boss.h"
#include "enemy/boss_manager.h"
#include "enemy/enemy.h"
#include "enemy/enemy_manager.h"
#include "game/input.h"
#include "game/ut_math.h"
#include "player/player.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <queue>

namespace {

// Bullet hitbox half-extents (game units). Mirrors the values used by the
// game's own collision logic.
constexpr int TAMA_HX = 2 * 64;
constexpr int TAMA_HY = 4 * 64;

// 4-connected BFS neighbour deltas. Index 0 = stay.
constexpr int kDX[5] = {0, 1, -1, 0, 0};
constexpr int kDY[5] = {0, 0, 0, 1, -1};

// 4-connected direction → 8-dir keymap code (1..4).
constexpr int kStepDir[5] = {0, 4, 3, 2, 1};

inline int DivFloor(int a, int b) { return (a < 0) ? -((-a + b - 1) / b) : a / b; }

} // namespace

void ReachabilityStrategy::SetDifficulty(int level) {
  difficulty_ = std::clamp(level, DIFFICULTY_EASY, DIFFICULTY_HARD);
}

void ReachabilityStrategy::Reset() {
  frame_counter_ = 0;
  grid_inited_ = false;
  for (auto &b : danger_) {
    std::fill(b.begin(), b.end(), 0ULL);
  }
}

int ReachabilityStrategy::Horizon() const {
  switch (difficulty_) {
  case DIFFICULTY_EASY:
    return 12;
  case DIFFICULTY_HARD:
    return 8;
  default:
    return 10;
  }
}

int ReachabilityStrategy::SafetyExpand() const {
  // Extra hitbox dilation beyond the cell quantization. Easy stays further
  // from danger, Hard hugs walls.
  switch (difficulty_) {
  case DIFFICULTY_EASY:
    return 2 * 64;
  case DIFFICULTY_HARD:
    return 0;
  default:
    return 1 * 64;
  }
}

void ReachabilityStrategy::EnsureGridSized() {
  if (grid_inited_) {
    return;
  }
  const int play_w = SX_MAX - SX_MIN;
  const int play_h = SY_MAX - SY_MIN;
  grid_w_ = (play_w + CELL_SIZE - 1) / CELL_SIZE;
  grid_h_ = (play_h + CELL_SIZE - 1) / CELL_SIZE;
  cells_total_ = grid_w_ * grid_h_;
  words_per_frame_ = (cells_total_ + 63) / 64;
  for (auto &b : danger_) {
    b.assign(words_per_frame_, 0ULL);
  }
  arrival_.assign(cells_total_, -1);
  parent_.assign(cells_total_, -1);
  grid_inited_ = true;
}

void ReachabilityStrategy::ClearDanger() {
  const int frames = Horizon() + 1;
  for (int t = 0; t < frames; t++) {
    std::fill(danger_[t].begin(), danger_[t].end(), 0ULL);
  }
}

void ReachabilityStrategy::GameToCell(int gx, int gy, int &cx, int &cy) const {
  cx = std::clamp(DivFloor(gx - SX_MIN, CELL_SIZE), 0, grid_w_ - 1);
  cy = std::clamp(DivFloor(gy - SY_MIN, CELL_SIZE), 0, grid_h_ - 1);
}

void ReachabilityStrategy::CellCenterGame(int cx, int cy, int &gx,
                                          int &gy) const {
  gx = SX_MIN + cx * CELL_SIZE + CELL_HALF;
  gy = SY_MIN + cy * CELL_SIZE + CELL_HALF;
}

inline void ReachabilityStrategy::SetDanger(int frame, int cx, int cy) {
  const int idx = CellIndex(cx, cy);
  danger_[frame][idx >> 6] |= (1ULL << (idx & 63));
}

inline bool ReachabilityStrategy::IsDanger(int frame, int cx, int cy) const {
  const int idx = CellIndex(cx, cy);
  return (danger_[frame][idx >> 6] >> (idx & 63)) & 1ULL;
}

void ReachabilityStrategy::MarkBox(int frame, int gx, int gy, int hx, int hy) {
  // Expand the hitbox by half a cell so cell-center sampling never under-marks.
  const int ex = hx + CELL_HALF + SafetyExpand();
  const int ey = hy + CELL_HALF + SafetyExpand();
  int cx0, cy0, cx1, cy1;
  GameToCell(gx - ex, gy - ey, cx0, cy0);
  GameToCell(gx + ex, gy + ey, cx1, cy1);
  for (int cy = cy0; cy <= cy1; cy++) {
    for (int cx = cx0; cx <= cx1; cx++) {
      SetDanger(frame, cx, cy);
    }
  }
}

void ReachabilityStrategy::RasterizeBullets() {
  const int horizon = Horizon();
  auto scan = [&](uint16_t count,
                  const std::array<uint16_t, TAMA_MAX> &indices) {
    for (uint16_t i = 0; i < count; i++) {
      const Bullet &b = Bullets.bullets[indices[i]];
      if ((b.flag & TF_DELETE) != 0) {
        continue;
      }
      for (int t = 0; t <= horizon; t++) {
        const int px = b.x + b.vx * t;
        const int py = b.y + b.vy * t;
        MarkBox(t, px, py, TAMA_HX, TAMA_HY);
      }
    }
  };
  scan(Bullets.count_small, Bullets.indices_small);
  scan(Bullets.count_large, Bullets.indices_large);
}

void ReachabilityStrategy::RasterizeEnemies() {
  const int horizon = Horizon();
  auto mark = [&](int x, int y, int vx, int vy, int hx, int hy) {
    for (int t = 0; t <= horizon; t++) {
      MarkBox(t, x + vx * t, y + vy * t, hx, hy);
    }
  };
  for (uint16_t i = 0; i < Enemies.count; i++) {
    const EnemyData &e = Enemies.entities[Enemies.indices[i]];
    if ((e.flag & EF_DELETE) != 0 || (e.flag & EF_HITSB) == 0) {
      continue;
    }
    mark(e.x, e.y, e.vx, e.vy, e.g_width, e.g_height);
  }
  for (uint16_t i = 0; i < Bosses.count; i++) {
    const BossData &b = Bosses.bosses[i];
    if (!b.IsUsed || (b.Edat.flag & EF_DELETE) != 0 ||
        (b.Edat.flag & EF_HITSB) == 0) {
      continue;
    }
    mark(b.Edat.x, b.Edat.y, b.Edat.vx, b.Edat.vy, b.Edat.g_width,
         b.Edat.g_height);
  }
}

void ReachabilityStrategy::RasterizeLasers() {
  const int horizon = Horizon();

  // Short / reflect lasers (oriented segment, sampled along its length).
  for (uint16_t i = 0; i < Lasers.count; i++) {
    const LASER_DATA &lp = Lasers.lasers[Lasers.laser_indices[i]];
    if ((lp.flag & LF_DELETE) != 0) {
      continue;
    }
    const int hw = std::max(lp.w, lp.wmax);
    for (int t = 0; t <= horizon; t++) {
      const int len = std::min(lp.l + lp.v * t, lp.lmax);
      if (len <= 0) {
        continue;
      }
      const int lx = lp.x + lp.vx * t;
      const int ly = lp.y + lp.vy * t;
      const int steps = std::max(1, len / CELL_HALF);
      for (int s = 0; s <= steps; s++) {
        const int dist = (len * s) / steps;
        const int sx = lx + cosl(lp.d, dist);
        const int sy = ly + sinl(lp.d, dist);
        MarkBox(t, sx, sy, hw, hw);
      }
    }
  }

  // Long lasers (infinite-length oriented beam from a fixed origin).
  for (int i = 0; i < LLASER_MAX; i++) {
    const LongLaserData &lp = Lasers.long_lasers[i];
    if (lp.flag != LLF_OPEN && lp.flag != LLF_NORM) {
      continue;
    }
    // Cover the beam to the edge of the play area.
    const int max_len = (SX_MAX - SX_MIN) + (SY_MAX - SY_MIN);
    const int steps = std::max(1, max_len / CELL_HALF);
    for (int t = 0; t <= horizon; t++) {
      for (int s = 0; s <= steps; s++) {
        const int dist = (max_len * s) / steps;
        const int sx = lp.x + cosl(lp.d, dist);
        const int sy = lp.y + sinl(lp.d, dist);
        if (sx < SX_MIN - CELL_SIZE || sx > SX_MAX + CELL_SIZE ||
            sy < SY_MIN - CELL_SIZE || sy > SY_MAX + CELL_SIZE) {
          break;
        }
        MarkBox(t, sx, sy, lp.w, lp.w);
      }
    }
  }
}

void ReachabilityStrategy::BFS(int start_cx, int start_cy) {
  std::fill(arrival_.begin(), arrival_.end(), int16_t(-1));
  std::fill(parent_.begin(), parent_.end(), int16_t(-1));

  const int horizon = Horizon();
  if (!InGrid(start_cx, start_cy)) {
    return;
  }
  if (IsDanger(0, start_cx, start_cy)) {
    // We're already in a dangerous cell. Still try to plan an escape, but
    // mark the start as reachable at frame 0 anyway.
  }
  const int sidx = CellIndex(start_cx, start_cy);
  arrival_[sidx] = 0;

  // Queue stores cell indices. Time is read from arrival_[idx].
  std::queue<int> q;
  q.push(sidx);

  while (!q.empty()) {
    const int idx = q.front();
    q.pop();
    const int t = arrival_[idx];
    if (t >= horizon) {
      continue;
    }
    const int cx = idx % grid_w_;
    const int cy = idx / grid_w_;
    const int nt = t + 1;
    for (int d = 0; d < 5; d++) {
      const int nx = cx + kDX[d];
      const int ny = cy + kDY[d];
      if (!InGrid(nx, ny)) {
        continue;
      }
      const int nidx = CellIndex(nx, ny);
      if (arrival_[nidx] != -1) {
        continue;
      }
      if (IsDanger(nt, nx, ny)) {
        continue;
      }
      arrival_[nidx] = static_cast<int16_t>(nt);
      parent_[nidx] = static_cast<int16_t>(d);
      q.push(nidx);
    }
  }
}

bool ReachabilityStrategy::SelectTarget(int pcx, int pcy, int &tcx, int &tcy) {
  const int horizon = Horizon();

  // Bias toward the attack position (just under the nearest enemy/boss).
  int attack_x = -1;
  int attack_y = -1;
  int best_enemy_dist_sq = (1 << 30);
  const int player_gx = Players.X();
  const int player_gy = Players.Y();
  for (uint16_t i = 0; i < Enemies.count; i++) {
    const EnemyData &e = Enemies.entities[Enemies.indices[i]];
    if ((e.flag & EF_DELETE) != 0 || (e.flag & EF_DAMAGE) == 0) {
      continue;
    }
    const int dx = e.x - player_gx;
    const int dy = e.y - player_gy;
    const int d2 = dx * dx + dy * dy;
    if (d2 < best_enemy_dist_sq) {
      best_enemy_dist_sq = d2;
      attack_x = e.x;
      attack_y = e.y + 80 * 64;
    }
  }
  for (uint16_t i = 0; i < Bosses.count; i++) {
    const BossData &b = Bosses.bosses[i];
    if (!b.IsUsed || (b.Edat.flag & EF_DELETE) != 0) {
      continue;
    }
    const int dx = b.Edat.x - player_gx;
    const int dy = b.Edat.y - player_gy;
    const int d2 = dx * dx + dy * dy;
    if (d2 < best_enemy_dist_sq) {
      best_enemy_dist_sq = d2;
      attack_x = b.Edat.x;
      attack_y = b.Edat.y + 80 * 64;
    }
  }

  int attack_cx = -1, attack_cy = -1;
  if (attack_x >= 0) {
    GameToCell(attack_x, attack_y, attack_cx, attack_cy);
  } else {
    // Fall back to play-field center horizontally, lower third vertically.
    GameToCell((SX_MIN + SX_MAX) / 2, SY_MAX - 30 * 64, attack_cx, attack_cy);
  }

  int best_score = -(1 << 30);
  int best_idx = -1;
  tcx = pcx;
  tcy = pcy;

  // Two-tier scoring:
  //   A. Cells whose survival reaches the full horizon ("fully safe"): pick
  //      the one nearest the attack position; ties broken by lower arrival.
  //   B. If no cell is fully safe, fall back to survival-weighted score with
  //      a stronger attack-position pull than before.
  // The huge constant offset keeps tier A strictly dominant over tier B.
  constexpr int FULLY_SAFE_OFFSET = 1 << 20;

  for (int cy = 0; cy < grid_h_; cy++) {
    for (int cx = 0; cx < grid_w_; cx++) {
      const int idx = CellIndex(cx, cy);
      const int arr = arrival_[idx];
      if (arr < 0) {
        continue;
      }
      // Survival horizon from this cell after arrival.
      int survive = 0;
      for (int t = arr; t <= horizon; t++) {
        if (IsDanger(t, cx, cy)) {
          break;
        }
        survive++;
      }
      if (survive == 0) {
        continue;
      }

      const int adx = std::abs(cx - attack_cx);
      const int ady = std::abs(cy - attack_cy);
      const int attack_cost = adx + ady;

      const bool fully_safe = (arr + survive > horizon);
      int score;
      if (fully_safe) {
        // Tier A: dominant constant + centering pull. Small arrival penalty
        // breaks ties toward cells we can reach sooner.
        score = FULLY_SAFE_OFFSET - attack_cost * 10 - arr;
      } else {
        // Tier B: weight survival, but give attack distance real influence.
        score = survive * 50 - arr * 5 - attack_cost * 4;
      }

      if (score > best_score) {
        best_score = score;
        best_idx = idx;
        tcx = cx;
        tcy = cy;
      }
    }
  }
  return best_idx >= 0;
}

int ReachabilityStrategy::FirstStepDirection(int pcx, int pcy, int tcx,
                                             int tcy) const {
  if (tcx == pcx && tcy == pcy) {
    return 0;
  }
  // Walk parent chain back from target until we reach a cell whose parent
  // is the start cell; the recorded direction at that cell IS the first
  // step (because parent[ni] stores the direction taken from cx/cy to ni).
  int cx = tcx;
  int cy = tcy;
  int last_dir = 0;
  while (!(cx == pcx && cy == pcy)) {
    const int idx = CellIndex(cx, cy);
    const int d = parent_[idx];
    if (d < 0) {
      return 0;
    }
    last_dir = d;
    cx -= kDX[d];
    cy -= kDY[d];
  }
  return kStepDir[last_dir];
}

bool ReachabilityStrategy::NeedBomb(int pcx, int pcy) const {
  if (!Players.Bombs() || Players.IsInvincible() || Players.IsBombActive()) {
    return false;
  }
  // Trigger only if the current cell is hit within the next two frames AND
  // no nearby cell (within a 1-step radius) is safe at t=1.
  bool imminent = false;
  for (int t = 0; t <= 2; t++) {
    if (IsDanger(t, pcx, pcy)) {
      imminent = true;
      break;
    }
  }
  if (!imminent) {
    return false;
  }
  // If at least one neighbour cell has arrival 1 and survives ≥ 3 frames,
  // we can dodge - don't bomb.
  const int horizon = Horizon();
  for (int d = 0; d < 5; d++) {
    const int nx = pcx + kDX[d];
    const int ny = pcy + kDY[d];
    if (!InGrid(nx, ny)) {
      continue;
    }
    const int nidx = CellIndex(nx, ny);
    if (arrival_[nidx] < 0 || arrival_[nidx] > 1) {
      continue;
    }
    int survive = 0;
    for (int t = arrival_[nidx]; t <= horizon; t++) {
      if (IsDanger(t, nx, ny)) {
        break;
      }
      survive++;
      if (survive >= 3) {
        return false;
      }
    }
  }
  return true;
}

bool ReachabilityStrategy::NeedFocus(int pcx, int pcy) const {
  // Focus when any of the 4-neighbours (or self) is dangerous within 2
  // frames - we're hugging a stream and want precision movement.
  for (int d = 0; d < 5; d++) {
    const int nx = pcx + kDX[d];
    const int ny = pcy + kDY[d];
    if (!InGrid(nx, ny)) {
      continue;
    }
    for (int t = 0; t <= 2; t++) {
      if (IsDanger(t, nx, ny)) {
        return true;
      }
    }
  }
  return false;
}

INPUT_BITS ReachabilityStrategy::DirectionToKeys(int dir) {
  switch (dir) {
  case 1:
    return KEY_UP;
  case 2:
    return KEY_DOWN;
  case 3:
    return KEY_LEFT;
  case 4:
    return KEY_RIGHT;
  default:
    return 0;
  }
}

INPUT_BITS ReachabilityStrategy::Update() {
  frame_counter_ = (frame_counter_ + 1) % 5;

  if (Players.IsGameOver()) {
    return 0;
  }

  EnsureGridSized();
  ClearDanger();
  RasterizeBullets();
  RasterizeEnemies();
  RasterizeLasers();

  int pcx, pcy;
  GameToCell(Players.X(), Players.Y(), pcx, pcy);

  BFS(pcx, pcy);

  int tcx = pcx, tcy = pcy;
  SelectTarget(pcx, pcy, tcx, tcy);
  const int dir = FirstStepDirection(pcx, pcy, tcx, tcy);

  INPUT_BITS keys = DirectionToKeys(dir);

  if (NeedFocus(pcx, pcy)) {
    keys |= KEY_SHIFT;
  }
  if (NeedBomb(pcx, pcy)) {
    keys |= KEY_BOMB;
  }
  if (frame_counter_ < 4) {
    keys |= KEY_TAMA;
  }

  return keys;
}
