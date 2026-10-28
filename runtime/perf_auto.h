#pragma once
// Automatic quality (the governor behind settings::g.auto_perf, on by default on Android).
//
// A phone's speed is not known in advance, and the same APK has to stay playable on a 2019 budget chip and on a 2024
// flagship. The adaptive resolution already spends resolution to hold the frame rate; this spends the *effects* when
// resolution is not enough. The two levers, in the order it takes them:
//
//   1. the game's glow pass (a full-screen additive pass plus the EFB copy that feeds it) - the single most expensive
//      optional thing on screen, and the least missed at speed;
//   2. the resolution floor, from 50 % of the chosen height down to 40 % - for a phone that is GPU-bound, pixels are
//      the only currency left.
//
// It gives both back when the phone has been comfortable for a while, so a slow stage does not leave the whole session
// soft and a fast phone never loses anything at all (level 0 = exactly what the player chose). Every change is logged
// with the frame rate that caused it: "[auto] 47 fps: glow off" - a report of "it is still slow" then says what the
// governor already tried.
namespace perf_auto {

// Whether it is on at all (the setting, or the phone's own default; SHADOW_AUTO=0 turns it off).
bool enabled();

// Feed it every presented frame, next to drs::tick() (both measure the same thing, independently).
void tick(double frame_seconds, double target_seconds);

// The level in effect (0 = nothing taken away), for the log and the HUD.
int level();

// Forget the history and give everything back (a new stage, a settings change by the player).
void reset();

}  // namespace perf_auto
