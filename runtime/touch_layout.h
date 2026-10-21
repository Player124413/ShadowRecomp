#pragma once
// Touch control layouts: the data, the file format and the built-in presets - no SDL, no ImGui, no game headers, so
// the format can be tested on its own (tools/touch_test) and the editor and the game agree on one description.
//
// The file is plain text (touch_layouts.ini, next to the settings):
//
//   version=2
//   active=classic            which layout the game uses (a name from this file)
//   scale=100                 every widget's size, %
//   opacity=100               every widget's opacity, %
//   enabled=1                 show the controls at all (the launcher writes this)
//   left_handed=0             mirror the whole picture left <-> right
//   hide_with_gamepad=1       hide the overlay while a physical controller is connected
//   [classic]                 one layout; the section name is its name (a saved one may be renamed)
//   stick_mode=0              0 fixed stick (the ring stays where it is drawn), 1 floating (under the thumb)
//   deadzone=15               % of the stick's travel that does nothing
//   haptics=1                 a short vibration on button presses
//   w=button,256,0.880,0.780,0.075,0.55,0,1
//                             kind is button, stick, menu or edit
//                             kind,action,x,y,radius,opacity,style,visible   (x,y 0..1 of the screen, radius of the
//                             screen's short side)
#include <string>
#include <vector>

namespace touch {

enum Action {                       // the game buttons a widget can press (input.h PAD_* mirrors this)
  A = 0, B, X, Y, Z, L, R, START, UP, DOWN, LEFT, RIGHT, kNumActions
};
enum Kind { kButton = 0, kStick, kMenu, kEditor };   // kMenu: the in-game settings menu; kEditor: the layout editor
enum StickAxis { kMove = 0, kCamera = 1 };

struct Widget {
  int kind = kButton;
  int action = A;        // kButton: Action; kStick: StickAxis; kMenu: unused
  float x = 0.5f, y = 0.5f;
  float r = 0.07f;
  float alpha = 0.55f;
  int style = 0;         // 0 round, 1 diamond, 2 pill (long horizontal)
  bool on = true;
  int id = 0;            // stable within a session: the editor's selection, not saved
  const char* label = nullptr;   // nullptr = the action's own letter
};

struct Layout {
  std::string name;
  bool builtin = false;
  int stick_mode = 0;      // 0 fixed, 1 floating; a version 1 file is read as fixed (load() in touch_layout.cpp)
  int deadzone = 15;       // %
  bool haptics = true;
  std::vector<Widget> widgets;
};

struct File {
  int version = 2;   // written by dump(); a version 1 file is migrated in load()
  std::string active;
  int scale = 100;         // %
  int opacity = 100;       // %
  bool enabled = true;     // the overlay is on
  bool left_handed = false;
  bool hide_with_gamepad = true;
  std::vector<Layout> layouts;
};

// The built-in presets (Classic, Left-handed, Minimal) as a whole file with "classic" active.
File defaults();

// Parse / write the text above. Unknown lines and broken widgets are skipped (never a crash on a hand-edited file);
// a file without a usable layout parses to defaults().
File parse(const std::string& text);
std::string dump(const File& file);

// Helpers
Layout* find(File& file, const std::string& name);
const Layout* find(const File& file, const std::string& name);
int index_of(const File& file, const std::string& name);
Widget* hit(Layout& layout, float x, float y, float aspect);          // finger (0..1 of the screen); nullptr = glass
const Widget* hit(const Layout& layout, float x, float y, float aspect);
// A widget's radius in units of the screen's width (r is of the short side): what the drawing code needs.
inline float radius_x(const Widget& w, float aspect) {
  if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
  return w.r * (aspect >= 1.0f ? 1.0f / aspect : 1.0f);
}
const char* action_label(int action);
const char* action_key(int action);                                   // "a", "b", ... (the saved form)
int action_from_key(const std::string& key);

}  // namespace touch
