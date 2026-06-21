///
/// PotentialFieldStrategy - see header for design overview.
///

#include "potential.h"
#include "bullet/bullet.h"
#include "bullet/laser_manager.h"
#include "core/gian.h"
#include "enemy/boss.h"
#include "enemy/boss_manager.h"
#include "enemy/enemy.h"
#include "enemy/enemy_manager.h"
#include "game/input.h"
#include "game/ut_math.h"
#include "player/item.h"
#include "player/item_manager.h"
#include "player/player.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace {

// One "pixel" in game coordinates.
constexpr int64_t PX = 64;

// Repulsion strengths. Tuned so that:
//   - a bullet ~1 cell away (~16 px) generates a force comparable to walking;
//   - a wall ~1 cell away generates a similar force;
//   - the attack attractor is weak enough that it never overrides dodging.
// All values are in game-unit^2 (1/r^2 falloff).
constexpr int64_t K_BULLET = 8'000'000;
constexpr int64_t K_ENEMY = 4'000'000;
constexpr int64_t K_LASER = 16'000'000;
constexpr int64_t K_WALL = 20'000'000;
constexpr int64_t K_ATTACK_NUM = 1'000;
constexpr int64_t K_ITEM = 600'000;

// Distance clamps to avoid divide-by-zero / runaway forces when very close.
constexpr int64_t MIN_DIST = 4 * PX;
constexpr int64_t MIN_DIST_SQ = MIN_DIST * MIN_DIST;

// Walls only repel when player is within this distance of an edge.
constexpr int64_t WALL_INFLUENCE = 80 * PX;

// Items only attract when within this radius.
constexpr int64_t ITEM_INFLUENCE = 120 * PX;

// Below this magnitude the player stays still.
constexpr int64_t MOVE_THRESHOLD = 500;

// Eight-direction angle table. Index 0..7 maps to dirs 4,8,2,7,3,5,1,6.
constexpr int kAngleToDir[8] = {4, 8, 2, 7, 3, 5, 1, 6};

inline int64_t sq(int64_t v) { return v * v; }

// 1/r^2 force from source toward player, scaled by K.
// Writes contribution into (fx, fy). Returns squared distance for callers
// that want to track proximity stats.
int64_t Repulse(int64_t &fx, int64_t &fy, int64_t px, int64_t py,
                int64_t sx, int64_t sy, int64_t k) {
  int64_t dx = px - sx;
  int64_t dy = py - sy;
  int64_t d2 = dx * dx + dy * dy;
  if (d2 < MIN_DIST_SQ) {
    d2 = MIN_DIST_SQ;
  }
  // Force magnitude = k / d2; direction = (dx, dy) / sqrt(d2).
  // Combined: contribution = k * (dx, dy) / d2^(3/2).
  // We avoid sqrt by using k * (dx, dy) / d2 (linear falloff in dir, 1/r^2
  // in magnitude). This is the standard "Coulomb" approximation in 2D.
  fx += (k * dx) / d2;
  fy += (k * dy) / d2;
  return d2;
}

} // namespace

void PotentialFieldStrategy::SetDifficulty(int level) {
  difficulty_ = std::clamp(level, DIFFICULTY_EASY, DIFFICULTY_HARD);
}

void PotentialFieldStrategy::Reset() {
  prev_dir_ = 0;
  frame_counter_ = 0;
}

int PotentialFieldStrategy::LeadFrames() const {
  switch (difficulty_) {
  case DIFFICULTY_EASY:
    return 3;
  case DIFFICULTY_HARD:
    return 1;
  default:
    return 2;
  }
}

int PotentialFieldStrategy::FocusRange() const {
  switch (difficulty_) {
  case DIFFICULTY_EASY:
    return 36 * PX;
  case DIFFICULTY_HARD:
    return 18 * PX;
  default:
    return 24 * PX;
  }
}

int PotentialFieldStrategy::BombRadius() const {
  switch (difficulty_) {
  case DIFFICULTY_EASY:
    return 80 * PX;
  case DIFFICULTY_HARD:
    return 35 * PX;
  default:
    return 55 * PX;
  }
}

int PotentialFieldStrategy::BombThreshold() const {
  switch (difficulty_) {
  case DIFFICULTY_EASY:
    return 3;
  case DIFFICULTY_HARD:
    return 8;
  default:
    return 5;
  }
}

void PotentialFieldStrategy::AccumulateBullets(Vec2 &force, int px, int py,
                                               int &near_count,
                                               int &min_dist_sq) {
  const int lead = LeadFrames();
  const int64_t focus_sq = sq(FocusRange());
  const int64_t bomb_radius_sq = sq(BombRadius());

  auto scan = [&](uint16_t count,
                  const std::array<uint16_t, TAMA_MAX> &indices) {
    for (uint16_t i = 0; i < count; i++) {
      const Bullet &b = Bullets.bullets[indices[i]];
      if ((b.flag & TF_DELETE) != 0) {
        continue;
      }
      const int64_t bx = static_cast<int64_t>(b.x) + b.vx * lead;
      const int64_t by = static_cast<int64_t>(b.y) + b.vy * lead;
      const int64_t d2 = Repulse(force.x, force.y, px, py, bx, by, K_BULLET);
      if (d2 < bomb_radius_sq) {
        near_count++;
      }
      if (d2 < focus_sq && static_cast<int64_t>(min_dist_sq) > d2) {
        min_dist_sq = static_cast<int>(d2);
      }
    }
  };
  scan(Bullets.count_small, Bullets.indices_small);
  scan(Bullets.count_large, Bullets.indices_large);
}

void PotentialFieldStrategy::AccumulateLasers(Vec2 &force, int px, int py,
                                              int &near_count,
                                              int &min_dist_sq) {
  const int64_t focus_sq = sq(FocusRange());

  // Short / reflect lasers - treat as oriented segment, push perpendicular
  // away from the closest point on the segment.
  for (uint16_t i = 0; i < Lasers.count; i++) {
    const LASER_DATA &lp = Lasers.lasers[Lasers.laser_indices[i]];
    if ((lp.flag & LF_DELETE) != 0) {
      continue;
    }
    const int64_t lx = lp.x;
    const int64_t ly = lp.y;
    const int64_t len = std::min<int64_t>(lp.l, lp.lmax);
    if (len <= 0) {
      continue;
    }
    // Project player onto the laser axis.
    const int64_t tx = px - lx;
    const int64_t ty = py - ly;
    int64_t along = cosl(lp.d, tx) + sinl(lp.d, ty);
    along = std::clamp<int64_t>(along, 0, len);
    const int64_t cx = lx + cosl(lp.d, along);
    const int64_t cy = ly + sinl(lp.d, along);
    const int64_t d2 = Repulse(force.x, force.y, px, py, cx, cy, K_LASER);
    if (d2 < focus_sq && static_cast<int64_t>(min_dist_sq) > d2) {
      min_dist_sq = static_cast<int>(d2);
      near_count++;
    }
  }

  // Long lasers - infinite oriented beam, repulse from perpendicular foot.
  // Warning-line (LLF_LINE) state is included with its eventual width so the
  // field already pushes us off the line before it actually fires.
  for (int i = 0; i < LLASER_MAX; i++) {
    const LongLaserData &lp = Lasers.long_lasers[i];
    if (lp.flag != LLF_OPEN && lp.flag != LLF_NORM &&
        lp.flag != LLF_LINE) {
      continue;
    }
    const int64_t tx = px - lp.x;
    const int64_t ty = py - lp.y;
    const int64_t along = cosl(lp.d, tx) + sinl(lp.d, ty);
    if (along <= 0) {
      continue;
    }
    const int64_t cx = lp.x + cosl(lp.d, along);
    const int64_t cy = lp.y + sinl(lp.d, along);
    // LLF_LINE has w=0 in game terms but we treat it with full repulsion
    // strength so the player is pushed clear before it actually fires.
    const int64_t d2 = Repulse(force.x, force.y, px, py, cx, cy, K_LASER);
    if (d2 < focus_sq && static_cast<int64_t>(min_dist_sq) > d2) {
      min_dist_sq = static_cast<int>(d2);
      near_count++;
    }
  }

  // Homing laser trails - point-by-point repulsion.
  constexpr int HOMINGL_TRAIL = 7 * 4;
  const HomingLaserData *hl = Lasers.active.Next;
  while (hl != nullptr) {
    if (hl->State != HLS_DEAD) {
      int ci = hl->Current;
      for (int j = 0; j < HOMINGL_TRAIL; j++) {
        const int64_t hx = hl->p[ci].x;
        const int64_t hy = hl->p[ci].y;
        const int64_t d2 =
            Repulse(force.x, force.y, px, py, hx, hy, K_BULLET);
        if (d2 < focus_sq && static_cast<int64_t>(min_dist_sq) > d2) {
          min_dist_sq = static_cast<int>(d2);
          near_count++;
        }
        ci = (ci + 1) % HOMINGL_TRAIL;
      }
    }
    hl = hl->Next;
  }
}

void PotentialFieldStrategy::AccumulateEnemies(Vec2 &force, int px, int py) {
  for (uint16_t i = 0; i < Enemies.count; i++) {
    const EnemyData &e = Enemies.entities[Enemies.indices[i]];
    if ((e.flag & EF_DELETE) != 0 || (e.flag & EF_HITSB) == 0) {
      continue;
    }
    Repulse(force.x, force.y, px, py, e.x, e.y, K_ENEMY);
  }
  for (uint16_t i = 0; i < Bosses.count; i++) {
    const BossData &b = Bosses.bosses[i];
    if (!b.IsUsed || (b.Edat.flag & EF_DELETE) != 0 ||
        (b.Edat.flag & EF_HITSB) == 0) {
      continue;
    }
    Repulse(force.x, force.y, px, py, b.Edat.x, b.Edat.y, K_ENEMY);
  }
}

void PotentialFieldStrategy::AccumulateWalls(Vec2 &force, int px, int py) {
  auto wall_push = [&](int64_t d, int64_t dirx, int64_t diry) {
    if (d <= 0) {
      d = 1;
    }
    if (d > WALL_INFLUENCE) {
      return;
    }
    const int64_t d2 = std::max<int64_t>(d * d, MIN_DIST_SQ);
    // Push toward (dirx, diry) of unit length 1*PX.
    force.x += (K_WALL * dirx * PX) / d2;
    force.y += (K_WALL * diry * PX) / d2;
  };
  wall_push(px - SX_MIN, +1, 0);  // left wall pushes right
  wall_push(SX_MAX - px, -1, 0);  // right wall pushes left
  wall_push(py - SY_MIN, 0, +1);  // top wall pushes down
  wall_push(SY_MAX - py, 0, -1);  // bottom wall pushes up
}

void PotentialFieldStrategy::AccumulateAttack(Vec2 &force, int px, int py) {
  // Find nearest damageable enemy/boss; aim for a point just below it.
  int64_t best_d2 = (int64_t{1} << 60);
  int64_t tx = (SX_MIN + SX_MAX) / 2;
  int64_t ty = SY_MAX - 30 * PX;
  bool found = false;
  for (uint16_t i = 0; i < Enemies.count; i++) {
    const EnemyData &e = Enemies.entities[Enemies.indices[i]];
    if ((e.flag & EF_DELETE) != 0 || (e.flag & EF_DAMAGE) == 0) {
      continue;
    }
    int64_t dx = e.x - px;
    int64_t dy = e.y - py;
    int64_t d2 = dx * dx + dy * dy;
    if (d2 < best_d2) {
      best_d2 = d2;
      tx = e.x;
      ty = e.y + 80 * PX;
      found = true;
    }
  }
  for (uint16_t i = 0; i < Bosses.count; i++) {
    const BossData &b = Bosses.bosses[i];
    if (!b.IsUsed || (b.Edat.flag & EF_DELETE) != 0) {
      continue;
    }
    int64_t dx = b.Edat.x - px;
    int64_t dy = b.Edat.y - py;
    int64_t d2 = dx * dx + dy * dy;
    if (d2 < best_d2) {
      best_d2 = d2;
      tx = b.Edat.x;
      ty = b.Edat.y + 80 * PX;
      found = true;
    }
  }
  (void)found;
  const int64_t dx = tx - px;
  const int64_t dy = ty - py;
  const int64_t d = std::max<int64_t>(
      1, static_cast<int64_t>(std::abs(dx) + std::abs(dy)));
  // Constant magnitude (K_ATTACK_NUM), normalised by Manhattan distance.
  force.x += (K_ATTACK_NUM * dx) / d;
  force.y += (K_ATTACK_NUM * dy) / d;
}

void PotentialFieldStrategy::AccumulateItems(Vec2 &force, int px, int py) {
  int64_t best_d = (int64_t{1} << 60);
  int64_t bx = 0, by = 0;
  bool found = false;
  for (uint16_t i = 0; i < Items.count; i++) {
    const ItemData &it = Items.entities[Items.indices[i]];
    if (it.type == ITEM_DELETE) {
      continue;
    }
    const int64_t dx = it.x - px;
    const int64_t dy = it.y - py;
    const int64_t d = std::abs(dx) + std::abs(dy);
    int64_t bonus = 0;
    if (it.type == ITEM_BOMB) {
      bonus = -40 * PX;
    } else if (it.type == ITEM_EXTEND) {
      bonus = -30 * PX;
    }
    const int64_t adjusted = d + bonus;
    if (adjusted < best_d) {
      best_d = adjusted;
      bx = it.x;
      by = it.y;
      found = true;
    }
  }
  if (!found) {
    return;
  }
  const int64_t dx = bx - px;
  const int64_t dy = by - py;
  const int64_t d2 = std::max<int64_t>(MIN_DIST_SQ, dx * dx + dy * dy);
  if (d2 > sq(ITEM_INFLUENCE)) {
    return;
  }
  // Attractor: -K_ITEM along (player - item) = +K_ITEM along (item - player).
  force.x += (K_ITEM * dx) / d2;
  force.y += (K_ITEM * dy) / d2;
}

int PotentialFieldStrategy::VectorToDir(int64_t fx, int64_t fy) {
  // 8-direction snap. Use the relative magnitudes of |fx| and |fy| to pick.
  const int64_t ax = std::abs(fx);
  const int64_t ay = std::abs(fy);
  // Threshold for "diagonal" vs "cardinal": minor axis must be at least
  // ~tan(22.5deg) ≈ 0.414 of major axis to count as diagonal. We use 1/2
  // for cheap integer math.
  const int64_t major = std::max(ax, ay);
  const int64_t minor = std::min(ax, ay);
  const bool diagonal = (minor * 2 >= major);

  if (diagonal) {
    if (fx >= 0 && fy >= 0) {
      return 8; // down-right
    }
    if (fx >= 0 && fy < 0) {
      return 6; // up-right
    }
    if (fx < 0 && fy >= 0) {
      return 7; // down-left
    }
    return 5; // up-left
  }
  if (ax >= ay) {
    return (fx >= 0) ? 4 : 3;
  }
  return (fy >= 0) ? 2 : 1;
}

INPUT_BITS PotentialFieldStrategy::DirToKeys(int dir) {
  switch (dir) {
  case 1: return KEY_UP;
  case 2: return KEY_DOWN;
  case 3: return KEY_LEFT;
  case 4: return KEY_RIGHT;
  case 5: return KEY_ULEFT;
  case 6: return KEY_URIGHT;
  case 7: return KEY_DLEFT;
  case 8: return KEY_DRIGHT;
  default: return 0;
  }
}

INPUT_BITS PotentialFieldStrategy::Update() {
  frame_counter_ = (frame_counter_ + 1) % 5;
  if (Players.IsGameOver()) {
    return 0;
  }

  const int px = Players.X();
  const int py = Players.Y();

  Vec2 force{0, 0};
  int near_count = 0;
  int min_dist_sq = (1 << 30);

  AccumulateBullets(force, px, py, near_count, min_dist_sq);
  AccumulateLasers(force, px, py, near_count, min_dist_sq);
  AccumulateEnemies(force, px, py);
  AccumulateWalls(force, px, py);
  AccumulateAttack(force, px, py);
  AccumulateItems(force, px, py);

  int dir;
  const int64_t mag2 = force.x * force.x + force.y * force.y;
  if (mag2 < MOVE_THRESHOLD * MOVE_THRESHOLD) {
    dir = 0;
  } else {
    dir = VectorToDir(force.x, force.y);
  }
  prev_dir_ = dir;

  INPUT_BITS keys = DirToKeys(dir);

  const int64_t focus_sq = sq(FocusRange());
  const bool focus_now = (min_dist_sq < focus_sq);
  if (focus_now) {
    keys |= KEY_SHIFT;
  }

  // Bomb: many bullets crowding us AND we have a bomb to spend AND the
  // closest threat is dangerously close.
  if (Players.Bombs() && !Players.IsInvincible() && !Players.IsBombActive()) {
    const bool very_close = (min_dist_sq < sq(12 * PX));
    if (near_count >= BombThreshold() ||
        (very_close && near_count >= BombThreshold() / 2)) {
      keys |= KEY_BOMB;
    }
  }

  if (frame_counter_ < 4) {
    keys |= KEY_TAMA;
  }

  return keys;
}
