#pragma once
// The last lines of the log, kept in memory so they can be shown on the screen (and photographed) with no cable, no
// adb and no filesystem: on a phone, "send me the log" is normally impossible, and this is the one thing that turns a
// report like "the world is black" into an answer - the renderer's own errors (a WebGPU validation failure names the
// exact format/pipeline that was refused) are in it.
//
// Everything the port logs goes through logf() (runtime/rt_sys.cpp), which feeds this ring; Aurora's own messages
// arrive there too through the log callback in gx_lle.cpp, so Dawn's errors are included.
//
// Settings > System > Bug reports > "Show log on the screen" (or SHADOW_LOG=1) draws it over the game.
#include <cstddef>
#include <string>
#include <vector>

namespace log_ring {

// Called by logf(): one complete line (without its newline is fine too).
void add(const char* text, size_t len);

// The last `count` lines, oldest first.
std::vector<std::string> lines(size_t count);

// The last few lines that look like an error (the renderer refused a pipeline, a shader failed to compile, ...).
// Keep them apart from the tail: they are the whole point of the overlay and must not scroll away.
std::vector<std::string> errors(size_t count);

// Total lines ever added (a cheap "has anything new arrived" check).
unsigned long long total();

// True when the overlay should be drawn (the setting or SHADOW_LOG=1).
bool overlay_wanted();

// Draw the overlay (Dear ImGui). Called every frame from settings::draw().
void draw_overlay();

}  // namespace log_ring
