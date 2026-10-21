#pragma once
// Graphics presets for phones (perf.cpp).
//
// The port's target is a locked 60 fps on every 64-bit phone, so the game has to start from a setting that fits the
// device instead of from a desktop setting. The launcher asks for a preset (or "auto"); "auto" reads the device report
// the launcher passes in (core count, RAM, chip) and picks the tier whose cost the cheap phones can pay.
//
// A preset only touches settings that already exist and are live (resolution, draw distances, lighting, AO, bloom):
// the game's logic is never changed, so every device simulates exactly the same game.
namespace perf {

// Applies a preset: "auto", "quality", "balanced", "performance" (anything else = auto). Values are set for this run;
// the player's saved settings are overwritten only by settings::save(), which the launcher-driven flow does not call.
void apply(const char* preset);

// What "auto" resolved to ("quality" / "balanced" / "performance"), for the log and for the in-game menu.
const char* tier();

// 0 = Auto, 1 = 50 %, 2 = 75 %, 3 = native: the launcher's render resolution slider. Auto leaves the preset's choice.
void apply_render_scale(int scale);

}  // namespace perf
