// Graphics presets for phones (see perf.h).
//
// The tiers are built from what actually costs time on a mobile GPU/CPU in this game, in that order:
//   1. the number of shaded pixels     -> render height (a 1080p phone at 1080p is 2.25x the work of 720p)
//   2. overdraw from the effects layer -> bloom, AO, the modern lighting engine and its shadows
//   3. per-object CPU work             -> object draw distance and the SET capacity
// Free wins (ETC2 textures, frame pacing, audio buffering) are not settings: they are always on.
//
// Nothing here is a guess about a specific phone: the tiers are the ones cheap 64-bit hardware can hold at 60 fps, and
// "auto" only decides which of them to start from. The player can always change everything in the launcher again, and
// every value is in the in-game menu too.
#include "perf.h"
#include "settings_ui.h"
#include "gx_lle.h"
#include "plat.h"
#include "sys.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace perf {
namespace {
std::string g_tier = "balanced";   // what "auto" resolved to

int kv_int(const std::string& info, const char* key, int fallback) {
  const std::string needle = std::string(key) + "=";
  const size_t at = info.find(needle);
  if (at == std::string::npos) return fallback;
  return atoi(info.c_str() + at + needle.size());
}

// A phone's own report: many cores and a lot of RAM do not make it a fast phone, but few cores and little RAM do make
// it a slow one, and that is the only thing this has to catch.
const char* pick_auto() {
  const std::string& info = plat::android_info();
  const int cores = kv_int(info, "cores", 8);
  const int ram = kv_int(info, "ram_mb", 4096);
  const std::string soc = [&] {
    const std::string needle = "soc=";
    const size_t at = info.find(needle);
    if (at == std::string::npos) return std::string();
    const size_t end = info.find(';', at);
    return info.substr(at + needle.size(), end == std::string::npos ? std::string::npos : end - at - needle.size());
  }();
  // the two 2019-2020 budget families that still sell in volume: 6-core big.LITTLE with 3-4 GB
  const bool budget = soc.find("ums512") != std::string::npos || soc.find("ums9230") != std::string::npos ||
                      soc.find("mt6765") != std::string::npos || soc.find("mt6768") != std::string::npos ||
                      soc.find("sm6125") != std::string::npos || soc.find("sm6225") != std::string::npos;
  if (ram <= 3072 || cores <= 4 || budget) return "performance";
  if (ram <= 4096 || cores <= 6) return "balanced";
  return "quality";
}

void tier_quality() {
  settings::g.height = 0;              // native: the phone's own panel, which is what "quality" means
  settings::g.fps_target = 60;
  settings::g.cutscene_fps_cap = 60;
  settings::g.game_bloom = true;
  settings::g.ao = false;              // contact AO is the most expensive effect per pixel; off unless asked for
  settings::g.light = false;           // the new lighting engine is still experimental: opt in, never on by default
  settings::g.light_shadows = false;
  settings::g.draw_distance = 100;
  settings::g.draw_objects = 100;
  settings::g.set_objects = 0;
}

void tier_balanced() {
  settings::g.height = 720;            // the sweet spot: 720p upscaled to a 1080p panel is hard to tell from native
  settings::g.fps_target = 60;
  settings::g.cutscene_fps_cap = 60;
  settings::g.game_bloom = true;
  settings::g.game_bloom_quality = 1;  // smooth bloom is the cheaper of the two on tiled GPUs (one pass, no upscale)
  settings::g.ao = false;
  settings::g.light = false;
  settings::g.light_shadows = false;
  settings::g.draw_distance = 90;
  settings::g.draw_objects = 90;
  settings::g.set_objects = 0;
}

void tier_performance() {
  settings::g.height = 540;            // 540p: a 4x fewer pixels than 1080p, still sharp on a 6" screen
  settings::g.fps_target = 60;
  settings::g.cutscene_fps_cap = 60;
  settings::g.game_bloom = true;       // the game's glow is part of its look: keep it, but at the cheap quality
  settings::g.game_bloom_quality = 0;
  settings::g.game_bloom_strength = 80;
  settings::g.ao = false;
  settings::g.light = false;
  settings::g.light_shadows = false;
  settings::g.draw_distance = 80;      // fewer objects on screen; the stages are still fully playable
  settings::g.draw_objects = 80;
  settings::g.set_objects = 0;         // the original SET capacity: less memory traffic, same behaviour
}
}  // namespace

void apply(const char* preset) {
  std::string name = preset ? preset : "auto";
  if (name == "auto" || name.empty()) name = pick_auto();
  if (name == "quality") {
    tier_quality();
  } else if (name == "performance" || name == "low") {
    name = "performance";
    tier_performance();
  } else {
    name = "balanced";
    tier_balanced();
  }
  g_tier = name;
  gxlle::g_render_height = settings::g.height;
  logf("[perf] preset %s (device: %s): %d lines, draw %d%% / objects %d%%, bloom %d, AO %d, lighting %d\n",
       g_tier.c_str(), plat::android_info().c_str(), settings::g.height, settings::g.draw_distance,
       settings::g.draw_objects, (int)settings::g.game_bloom, (int)settings::g.ao, (int)settings::g.light);
}

void apply_render_scale(int scale) {
  // 0 = Auto: the preset's own choice stands. The launcher's slider wins over the preset otherwise.
  if (scale <= 0) return;
  if (scale == 1) settings::g.height = 480;
  else if (scale == 2) settings::g.height = 720;
  else settings::g.height = 0;
  gxlle::g_render_height = settings::g.height;
  logf("[perf] render height %d (launcher: %s)\n", settings::g.height,
       settings::g.height == 0 ? "native" : scale == 1 ? "50 %" : "75 %");
}

const char* tier() { return g_tier.c_str(); }

}  // namespace perf
