// Automatic quality governor, without a phone: the decisions live in runtime/perf_auto_core.h, so a synthetic device can
// be played back through them. What has to be true before this can be shipped as "playable on every phone":
//
//   * a phone that holds the target never loses anything (level stays 0) - the picture the player chose is the picture,
//   * one slow frame (a stage load, an upload, the GC) never costs a thing,
//   * a phone that is persistently late steps down, and keeps stepping while it is still late,
//   * once it is comfortable the level comes back up, so a bad stage does not make the whole session soft,
//   * it never oscillates: no change happens twice in a row without a quiet window.
//
//   g++ -std=c++20 -O2 -Wall -Wextra -Iruntime tools/android/perf_auto_test.cpp -o /tmp/perf_auto_test && /tmp/perf_auto_test
#include "perf_auto_core.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

// A synthetic phone: frame_seconds for the current level, and a governor watching it. `steady` is the frame time the
// device delivers with nothing taken away; each level buys a factor of that time.
struct Phone {
  perf_auto::Governor gov;
  double target = 1.0 / 60.0;
  double steady;
  int changes = 0;
  std::vector<int> trace;

  explicit Phone(double steady_seconds) : steady(steady_seconds) {}

  // What each level buys on this synthetic device. Level 1 removes the glow: one full-screen additive pass plus the EFB
  // copy that feeds it, so ~12 % of the frame at 60 fps on a phone (measured on this port's bloom pass). Level 2 also
  // allows the resolution floor to drop from 50 % to 40 % of the chosen height: (0.40 / 0.50)^2 = 64 % of the pixels,
  // which is what the cost is proportional to.
  double cost() const {
    switch (gov.level()) {
    case 1: return steady * 0.88;
    case 2: return steady * 0.88 * 0.64;
    default: return steady;
    }
  }

  void run(int frames) {
    for (int i = 0; i < frames; ++i) {
      if (gov.add_frame(cost(), target)) {
        ++changes;
        trace.push_back(gov.level());
      }
    }
  }

  // A long run with a settle after every step: the governor measures the *new* cost at the new level.
  void settle(int frames) { run(frames); }
};

void fast_phone_keeps_everything() {
  Phone p(1.0 / 60.0);   // exactly on the deadline
  p.run(perf_auto::Governor::kWindow * 30);   // ~20 s
  check(p.gov.level() == 0, "a phone that holds 60 fps never loses the glow");
  check(p.changes == 0, "a phone that holds 60 fps sees no change at all");
}

void single_hitch_costs_nothing() {
  Phone p(1.0 / 60.0);
  p.run(perf_auto::Governor::kWindow * 3);
  // one 300 ms stall (a stage load) in the middle of an otherwise perfect run
  for (int i = 0; i < perf_auto::Governor::kWindow * 3; ++i) {
    const bool stall = i == perf_auto::Governor::kWindow;
    p.gov.add_frame(stall ? 0.3 : 1.0 / 60.0, p.target);
  }
  check(p.gov.level() == 0, "one stalled frame does not take the glow away");
}

void slow_phone_steps_down_and_stays_playable() {
  Phone p(1.0 / 40.0);   // 40 fps with everything on: the report this governor exists for
  p.run(perf_auto::Governor::kWindow * 8);   // it waits a few windows first (a hitch must cost nothing), then acts
  check(p.gov.level() >= 1, "a phone at 40 fps gives up the glow");
  // keep running: it must reach the deepest level, not sit in the middle
  for (int round = 0; round < 12; ++round) p.run(perf_auto::Governor::kWindow);
  check(p.gov.level() == perf_auto::kMaxLevel, "still late: the resolution floor goes down too");
  check(p.changes == perf_auto::kMaxLevel, "the ladder is climbed one step at a time, never in one jump");
  check(p.cost() <= p.target * 1.10, "at the deepest level the phone is inside the frame budget");
}

void comfortable_phone_gets_everything_back() {
  Phone p(1.0 / 52.0);   // 52 fps: late enough to step down
  for (int round = 0; round < 12; ++round) p.run(perf_auto::Governor::kWindow);
  check(p.gov.level() > 0, "52 fps is enough to step down");
  const int taken = p.gov.level();
  // the phone cools down / the stage gets cheaper: back to a frame time with headroom to spare. A level that was too
  // slow earlier is held back for a while (so the ladder does not flap), which is about a minute - hence the long run.
  p.steady = 1.0 / 60.0 * 0.90;
  for (int round = 0; round < 130; ++round) p.run(perf_auto::Governor::kWindow);
  check(p.gov.level() < taken, "a comfortable frame time gives something back");
}

void no_oscillation() {
  // Right on the edge (1.12x the target - the case where an unstable governor flaps between levels every second).
  Phone p(1.0 / 60.0 * 1.12);
  p.run(perf_auto::Governor::kWindow * 180);   // three minutes
  check(p.changes <= 3, "an edge-of-budget phone does not flap between levels");
}

void deepest_level_only_once() {
  Phone p(1.0 / 20.0);   // hopeless: it must stop at the bottom, not keep "stepping"
  for (int round = 0; round < 30; ++round) p.run(perf_auto::Governor::kWindow);
  check(p.gov.level() == perf_auto::kMaxLevel, "the ladder stops at the deepest level");
  int steps = 0;
  for (int lv : p.trace) {
    (void)lv;
    ++steps;
  }
  check(steps <= 2 + 2, "hopeless device: two steps down and no more (plus at most two recoveries)");
}

void hopeless_phone_settles() {
  // 1.35x the target with nothing taken away: the ladder reaches the bottom and then must stop fiddling with the
  // picture (every change is a visible jump in sharpness or in the glow).
  Phone p(1.0 / 60.0 * 1.35);
  p.run(perf_auto::Governor::kWindow * 180);   // three minutes
  check(p.gov.level() == perf_auto::kMaxLevel, "a hopeless phone ends at the deepest level");
  check(p.changes <= 4, "the picture settles: at most a few changes in three minutes");
}

void floor_values() {
  check(perf_auto::min_scale_for(0) == 0.50, "level 0 keeps the port's usual half-resolution floor");
  check(perf_auto::min_scale_for(int(perf_auto::Level::NoGlowLowRes)) == 0.40, "the deepest level allows 40 %");
}

}  // namespace

int main() {
  fast_phone_keeps_everything();
  single_hitch_costs_nothing();
  slow_phone_steps_down_and_stays_playable();
  comfortable_phone_gets_everything_back();
  no_oscillation();
  hopeless_phone_settles();
  deepest_level_only_once();
  floor_values();
  if (failures == 0) {
    std::printf("automatic quality: all checks passed\n");
    return 0;
  }
  std::printf("automatic quality: %d check(s) failed\n", failures);
  return 1;
}
