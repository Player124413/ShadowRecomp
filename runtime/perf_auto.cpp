// See perf_auto.h. The decisions are in perf_auto_core.h; this file is the glue: is it on, measure, apply, log.
#include "perf_auto.h"

#include "perf_auto_core.h"

#include "dynamic_resolution.h"
#include "gx_lle.h"
#include "plat.h"
#include "settings_ui.h"
#include "sys.h"

#include <cstdlib>
#include <cstring>

namespace perf_auto {
namespace {

perf_auto::Governor g_governor;

bool env_off() {
  const char* v = getenv("SHADOW_AUTO");
  return v != nullptr && (*v == '0' || *v == 'f' || *v == 'F');
}

// The governor only ever removes what the player *has*: if they turned the glow off themselves, level 1 costs them
// nothing and there is nothing to restore either. That is what these two helpers keep straight.
bool glow_available() {
  return settings::g.game_bloom && !settings::g.light;   // the real-time lighting engine draws its own glow
}

void apply_glow(bool wanted) {
  // gx_lle's copy of the setting is what the renderer reads (game_menu.cpp after_change does the same thing).
  gxlle::g_game_bloom = wanted && glow_available();
}

}  // namespace

bool enabled() {
  // Not cached in a static: a phone picks it up from the settings file, and the player can switch it off in the menu
  // while playing (then everything it took away comes back on the next call).
  return !env_off() && (plat::is_android() || settings::g.auto_perf);
}

int level() { return g_governor.level(); }

void reset() {
  g_governor.reset();
  drs::set_min_scale(perf_auto::min_scale_for(0));
  apply_glow(settings::g.game_bloom);
}

void tick(double frame_seconds, double target_seconds) {
  if (!enabled() || frame_seconds <= 0.0 || target_seconds <= 0.0) return;
  if (!g_governor.add_frame(frame_seconds, target_seconds)) return;

  const int level = g_governor.level();
  if (level >= int(Level::NoGlow)) apply_glow(false);
  else apply_glow(settings::g.game_bloom);
  drs::set_min_scale(perf_auto::min_scale_for(level));

  const double fps = 1.0 / frame_seconds;
  logf("[auto] %.0f fps (target %.0f): %s\n", fps, 1.0 / target_seconds,
       level == int(Level::Full)          ? "everything back: glow on, resolution floor 50 %"
       : level == int(Level::NoGlow)      ? "glow off to hold the frame rate"
       : level == int(Level::NoGlowLowRes) ? "glow off and the resolution floor down to 40 %"
                                           : "changed");
}

}  // namespace perf_auto
