#pragma once
// The automatic quality governor's decision logic, with no dependencies at all: no settings, no SDL, no logging, no
// clock. perf_auto.cpp feeds it the frame times and applies what it asks for; tools/android/perf_auto_test.cpp drives it
// with a synthetic phone (a fast one, a slow one, one with hitches) and checks the ladder. Keeping it pure is the only
// way to test "does a weak phone end up playable, and does a strong one ever lose its glow for nothing?" without a
// device in hand.
//
// The rule it encodes, in one sentence: take something away only after the frame times have been late for a long while
// (two windows in a row, ~2 s), give it back only after they have been comfortable for much longer (~5 s), and never
// change anything twice in a row without a pause. A single hitch - a stage load, a texture upload, the GC - must cost
// the player nothing.
#include <algorithm>
#include <array>
#include <cstdint>

namespace perf_auto {

// What the governor asks for. Higher = less work for the GPU/CPU.
//   0 = nothing taken away (the settings are exactly what the player chose)
//   1 = the game's glow pass off (a full-screen additive pass plus the copy that feeds it)
//   2 = glow off and the adaptive resolution allowed to go deeper than half of the chosen height
enum class Level : int { Full = 0, NoGlow = 1, NoGlowLowRes = 2 };
inline constexpr int kMaxLevel = int(Level::NoGlowLowRes);

class Governor {
public:
  static constexpr int kWindow = 40;            // frames per decision (~0.7 s at 60 fps)
  static constexpr int kLateWindows = 2;        // consecutive late windows before a step down
  static constexpr int kComfortWindows = 5;     // consecutive comfortable windows before a step back up
  static constexpr int kQuietWindows = 4;       // windows of no change after any change, before the next one
  static constexpr int kBlockWindows = 90;      // a level that was too slow is not tried again for this long (~60 s),
                                                // doubled every further time the phone proves it too slow (max 8x)
  static constexpr double kLateFactor = 1.10;   // "late": the median frame is 10 % over the target
  static constexpr double kComfortFactor = 1.02;  // "comfortable": at the target (frame pacing cannot do better)

  void reset() {
    count_ = 0;
    next_ = 0;
    level_ = 0;
    late_windows_ = 0;
    comfort_windows_ = 0;
    quiet_windows_ = 0;
    min_level_ = 0;   // a new stage may be cheaper: the ladder starts from the top again
    block_left_ = 0;
    fails_ = 0;
  }

  int level() const { return level_; }

  // One finished frame. Returns true when the level changed (the caller then applies it and logs it).
  bool add_frame(double frame_seconds, double target_seconds) {
    if (frame_seconds <= 0.0 || target_seconds <= 0.0) return false;
    samples_[next_] = frame_seconds;
    next_ = (next_ + 1) % kWindow;
    count_ = std::min(count_ + 1, kWindow);
    if (count_ < kWindow) return false;   // a window is judged as a whole, never on one frame

    const double med = median();
    // This window is spent: the next frame starts a new one. (Forgetting this turned every frame after the first full
    // window into a "window" of its own, which is exactly the flapping the block below exists to prevent.)
    count_ = 0;
    next_ = 0;
    if (quiet_windows_ < kQuietWindows) ++quiet_windows_;

    const bool late = med > target_seconds * kLateFactor;
    const bool comfy = med <= target_seconds * kComfortFactor;
    late_windows_ = late ? late_windows_ + 1 : 0;
    comfort_windows_ = comfy ? comfort_windows_ + 1 : 0;

    if (quiet_windows_ < kQuietWindows) return false;

    if (block_left_ > 0) --block_left_;

    if (late && late_windows_ >= kLateWindows && level_ < kMaxLevel) {
      ++level_;
      // The level we just left was not enough. Remembering that is what stops the "give it back, take it away again"
      // loop a phone whose exact threshold sits between two levels would otherwise ride every few seconds.
      min_level_ = level_;
      block_left_ = kBlockWindows << std::min(fails_, 3);   // 1, 2, 4, 8 minutes: a phone that keeps failing is believed
      if (fails_ < 3) ++fails_;
      changed();
      return true;
    }
    if (level_ == min_level_ && block_left_ == 0) min_level_ = 0;   // the block expired: the whole ladder is open again
    if (comfy && comfort_windows_ >= kComfortWindows && level_ > 0 && level_ - 1 >= min_level_) {
      --level_;
      changed();
      return true;
    }
    return false;
  }

private:
  void changed() {
    quiet_windows_ = 0;
    late_windows_ = 0;
    comfort_windows_ = 0;
    count_ = 0;
    next_ = 0;
  }

  double median() {
    std::array<double, kWindow> sorted{};
    std::copy(samples_.begin(), samples_.begin() + count_, sorted.begin());
    std::nth_element(sorted.begin(), sorted.begin() + count_ / 2, sorted.begin() + count_);
    return sorted[count_ / 2];
  }

  std::array<double, kWindow> samples_{};
  int count_ = 0;
  int next_ = 0;
  int level_ = 0;
  int late_windows_ = 0;
  int comfort_windows_ = 0;
  int quiet_windows_ = 0;
  int min_level_ = 0;    // the lowest level the ladder may return to (the last one that was too slow)
  int block_left_ = 0;   // windows left before that level is worth trying again
  int fails_ = 0;        // how often a step back up was proved wrong (until reset())
};

// The resolution floor a level allows, as a fraction of the player's chosen height. Level 2 exists because dropping
// pixels is the only lever that helps a GPU-bound phone once the glow is gone, and half of 540 lines (270) is below the
// port's usual 360-line floor: a weak phone at 360 lines and 40 fps plays worse than the same phone at 300 lines and 60.
inline double min_scale_for(int level) { return level >= int(Level::NoGlowLowRes) ? 0.40 : 0.50; }

}  // namespace perf_auto
