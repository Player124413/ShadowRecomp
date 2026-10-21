#pragma once
// Adaptive render resolution (dynamic resolution scaling).
//
// The port draws into an offscreen framebuffer of a chosen height and scales it to the window, so the cost of a frame
// is (almost) proportional to that height squared. Picking one height for every phone is a guess: the presets are a
// good starting point, but a stage with a lot of overdraw or a phone that thermally throttles a minute in will miss the
// 60 Hz deadline at a height another phone holds easily. This watches the frame times that were actually delivered and
// moves the height to where the deadline is met - down when frames are late, back up (slowly, and only towards the
// player's own choice) when there is headroom to spare.
//
// Input never depends on it and nothing changes mid-frame: the height is applied between frames, and only when the
// change is large enough to be worth the allocation.
namespace drs {

// Whether the phone (or the setting) asked for it.
bool enabled();

// One presented frame, in seconds, and the frame time the game is aiming for (1 / fps target).
void tick(double frame_seconds, double target_seconds);

// The height to render at, given the height the player/preset asked for (0 = the window's own height in pixels).
int height_for(int wanted_height);

// The multiplier currently in effect (1.0 = exactly what was asked for); for the log and the HUD.
double scale();

// Forget the history (a new stage, a display change): the next frames start from the player's height again.
void reset();

}  // namespace drs
