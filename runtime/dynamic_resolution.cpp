// Adaptive render resolution - see dynamic_resolution.h.
//
// The decision uses the *median* of the last window of frames, not an average and not a single frame: a disk read at a
// stage load or a GC pause must not cost the player 200 lines of sharpness. Two windows in a row have to be late before
// the height comes down (about 1.5 s of real load), after which it drops straight to where the deadline can be met; it
// climbs back one small step at a time while frames are comfortable.
#include "dynamic_resolution.h"
#include "plat.h"
#include "settings_ui.h"
#include "sys.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace drs {
namespace {

constexpr int kWindow = 40;          // frames per decision (~0.7 s at 60 fps)
constexpr double kMinScale = 0.50;   // never below half of the chosen height (perf_auto.cpp can lower it to 0.40)
constexpr int kMinHeight = 360;      // and never below 360 lines, whatever half of the choice is
constexpr uint64_t kChangeCooldownNs = 450000000ull;   // between two changes
constexpr uint64_t kReportNs = 5000000000ull;          // one "[drs]" line per 5 s: what a bug report needs

double g_samples[kWindow];
int g_count = 0;
int g_next = 0;
double g_scale = 1.0;
double g_min_scale = kMinScale;
int g_slow_windows = 0;   // consecutive windows that were late
uint64_t g_last_change = 0;
uint64_t g_last_report = 0;

bool env_enabled() {
  const char* v = getenv("SHADOW_DRS");
  return v == nullptr || (*v != '0' && *v != 'f' && *v != 'F');
}

double median_of_samples() {
  double copy[kWindow];
  std::copy(g_samples, g_samples + g_count, copy);
  std::nth_element(copy, copy + g_count / 2, copy + g_count);
  return copy[g_count / 2];
}

}  // namespace

bool enabled() {
  static const bool on = env_enabled() && (plat::is_android() || settings::g.dynamic_resolution);
  return on;
}

double scale() { return g_scale; }

void set_min_scale(double scale) {
  g_min_scale = std::clamp(scale, 0.30, 1.0);
  if (g_scale < g_min_scale) {   // a raised floor lifts the picture immediately, a lowered one only allows it
    g_scale = g_min_scale;
    g_count = 0;
    g_next = 0;
  }
}
double min_scale() { return g_min_scale; }

void reset() {
  g_count = 0;
  g_next = 0;
  g_scale = 1.0;
  g_slow_windows = 0;
  g_last_change = 0;
  g_last_report = 0;
}

int height_for(int wanted_height) {
  if (!enabled() || wanted_height <= 0 || g_scale >= 0.999) return wanted_height;
  int h = (int)std::lround(wanted_height * g_scale);
  h = (h / 8) * 8;   // whole blocks: the render grid and the EFB copies like them, and the log stays readable
  // The absolute floor follows the allowed floor: 360 lines at 50 %, 288 at the 40 % that perf_auto.cpp can unlock.
  // Without this the 40 % floor would do nothing for the 540-line preset (0.4 x 540 = 216, still clamped to 360).
  const int floor_h = (int)std::lround(kMinHeight * (g_min_scale / kMinScale));
  return std::clamp(h, std::min(floor_h, wanted_height), wanted_height);
}

void tick(double frame_seconds, double target_seconds) {
  if (!enabled() || frame_seconds <= 0.0 || target_seconds <= 0.0) return;
  g_samples[g_next] = frame_seconds;
  g_next = (g_next + 1) % kWindow;
  g_count = std::min(g_count + 1, kWindow);

  const uint64_t now = sys_time_ns();
  const bool window_full = g_count >= kWindow;
  if (!window_full) return;

  const double med = median_of_samples();
  const double fps = 1.0 / med;
  if (now - g_last_report > kReportNs) {
    g_last_report = now;
    logf("[drs] %.1f fps, render scale %.2f (target %.0f fps)\n", fps, g_scale, 1.0 / target_seconds);
  }
  if (now - g_last_change < kChangeCooldownNs) return;

  const bool late = med > target_seconds * 1.12;
  const bool comfy = med <= target_seconds * 1.05;

  if (late) {
    ++g_slow_windows;
  } else {
    g_slow_windows = 0;
  }

  // Two late windows in a row (a real load, not a hitch) before the picture gets softer.
  if (late && g_slow_windows >= 2 && g_scale > g_min_scale) {
    // Cost is roughly proportional to the pixel count, so the square root of the time ratio is the scale ratio.
    const double want = std::clamp(g_scale * std::sqrt(target_seconds / med), g_min_scale, g_scale * 0.92);
    const double stepped = std::max(g_min_scale, std::floor(want * 20.0) / 20.0);   // steps of 0.05
    if (stepped < g_scale - 0.011) {
      logf("[drs] %.1f fps (target %.1f): render scale %.2f -> %.2f\n", fps, 1.0 / target_seconds, g_scale, stepped);
      g_scale = stepped;
      g_last_change = now;
      g_slow_windows = 0;
      g_count = 0;   // measure again at the new size before deciding anything else
      g_next = 0;
    }
    return;
  }

  // Comfortable with margin to spare: one small step back up. This is what keeps a single bad stage from leaving the
  // whole session soft, and the 5 % margin is what stops it from oscillating around the deadline.
  if (comfy && g_scale < 1.0) {
    const double stepped = std::min(1.0, std::ceil((g_scale + 0.05) * 20.0) / 20.0);
    logf("[drs] %.1f fps (target %.1f): render scale %.2f -> %.2f\n", fps, 1.0 / target_seconds, g_scale, stepped);
    g_scale = stepped;
    g_last_change = now;
    g_count = 0;
    g_next = 0;
  }
}

}  // namespace drs
