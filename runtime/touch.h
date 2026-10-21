#pragma once
// Touch controls: on-screen buttons and sticks for phones and tablets, with a layout editor in the game's own menu.
//
// The layouts and their file format live in touch_layout.h/.cpp (no SDL there, so the format is testable on its own);
// this part reads the fingers (SDL3 multi-touch), turns them into the game's pad, draws the overlay and runs the
// editor. Everything is editable: each control can be dragged anywhere, resized, made more or less solid, and bound
// to any game button; whole layouts can be added, renamed and deleted, and the game ships three presets.
//
// Touch is an input device like any other: the pad state below is merged with the controller / keyboard in input.cpp,
// so nothing in the game or the settings menu needed a special case.
#include <cstdint>

namespace touch {

struct State {
  uint16_t buttons = 0;              // input.h PAD_* bits pressed on the glass
  int8_t sx = 0, sy = 0;             // left (move) stick
  int8_t cx = 0, cy = 0;             // right (camera) stick
  bool menu = false;                 // a MENU press is waiting for input_poll() (opens / closes the settings menu)
  bool any = false;                  // a finger is on a control
};

// Once per frame, before input_poll() reads the pad: drains the finger events the SDL watcher collected.
void tick();
State state();
bool consume_menu();               // true once per MENU press

// The overlay, drawn inside an Aurora frame (settings::draw calls it). Also draws the editor when it is open.
void draw();
bool visible();                    // the controls are on screen right now
bool wanted();                     // the setting is on and no physical controller is being used
bool editor_open();
void open_editor();
void close_editor(bool save);

// Layouts (used by the settings menu's Touch controls page).
void load();                       // reads touch_layouts.ini once (called on the first tick; safe to call again)
void save();
bool enabled();
void set_enabled(bool on);
const char* active_layout();       // "classic", a saved one, or "" when there is nothing
int layout_count();
const char* layout_name(int index);
void set_layout(int index);
void cycle_layout(int dir);        // the menu row's left / right
void reset_active_layout();        // back to the shipped preset of that name (or the classic one)
bool add_layout(const char* name); // a copy of the active one under a new name
bool delete_layout(const char* name);

// Editor actions (also reachable from the menu page: "Add button", "Remove", ...)
void editor_add(int kind);         // touch::kButton / kStick / kMenu (touch_layout.h)
void editor_remove_selected();

}  // namespace touch
