// Touch control layouts: parsing, writing and the built-in presets (see touch_layout.h for the format).
#include "touch_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace touch {
namespace {
const char* const kActionKeys[kNumActions] = {"a", "b", "x", "y", "z", "l", "r", "start", "up", "down", "left", "right"};
const char* const kActionLabels[kNumActions] = {"A", "B", "X", "Y", "Z", "L", "R", "START", "D-pad up", "D-pad down",
                                                "D-pad left", "D-pad right"};

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t at = 0;
  for (;;) {
    const size_t next = s.find(sep, at);
    out.push_back(trim(s.substr(at, next == std::string::npos ? std::string::npos : next - at)));
    if (next == std::string::npos) break;
    at = next + 1;
  }
  return out;
}

Widget button(int action, float x, float y, float r, float alpha = 0.5f, int style = 0) {
  Widget w;
  w.kind = kButton;
  w.action = action;
  w.x = x;
  w.y = y;
  w.r = r;
  w.alpha = alpha;
  w.style = style;
  return w;
}

Widget stick(int axis, float x, float y, float r) {
  Widget w;
  w.kind = kStick;
  w.action = axis;
  w.x = x;
  w.y = y;
  w.r = r;
  w.alpha = 0.45f;
  return w;
}

Widget menu_button(float x, float y, float r) {
  Widget w;
  w.kind = kMenu;
  w.action = 0;
  w.x = x;
  w.y = y;
  w.r = r;
  w.alpha = 0.5f;
  w.style = 2;
  w.label = "MENU";
  return w;
}

Widget edit_button(float x, float y, float r) {
  Widget w = menu_button(x, y, r);
  w.kind = kEditor;
  w.alpha = 0.4f;
  w.r = r * 0.8f;
  w.label = "EDIT";
  return w;
}

void renumber(Layout& l) {
  int id = 1;
  for (auto& w : l.widgets) w.id = id++;
}
}  // namespace

const char* action_label(int action) {
  return action >= 0 && action < kNumActions ? kActionLabels[action] : "?";
}
const char* action_key(int action) {
  return action >= 0 && action < kNumActions ? kActionKeys[action] : "";
}
int action_from_key(const std::string& key) {
  for (int i = 0; i < kNumActions; ++i)
    if (key == kActionKeys[i]) return i;
  return -1;
}

// All distances are in units of the screen's width: x is already, y needs the height (1/aspect) and the radius is of
// the short side (min(1, 1/aspect) of the width). The widgets are circles on the screen, whatever the screen's shape.
const Widget* hit(const Layout& layout, float x, float y, float aspect) {
  if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
  const float h = 1.0f / aspect;              // the screen's height in width units
  const float s = std::min(1.0f, h);          // its short side, the unit the radius is in
  for (auto& w : layout.widgets) {
    if (!w.on) continue;
    const float dx = x - w.x, dy = (y - w.y) * h;
    const float r = w.r * s;
    if (dx * dx + dy * dy <= r * r) return &w;
  }
  return nullptr;
}
Widget* hit(Layout& layout, float x, float y, float aspect) {
  return const_cast<Widget*>(hit(static_cast<const Layout&>(layout), x, y, aspect));
}

File defaults() {
  File f;
  f.version = 1;
  f.active = "classic";

  // Classic: what a GameCube pad maps to on glass, in landscape. The left thumb moves, the right thumb turns the
  // camera and presses the four face buttons; everything else is at the top.
  Layout c;
  c.name = "classic";
  c.builtin = true;
  c.stick_mode = 1;
  c.widgets = {
      stick(kMove, 0.15f, 0.72f, 0.125f),
      stick(kCamera, 0.84f, 0.44f, 0.085f),
      button(A, 0.885f, 0.735f, 0.056f, 0.55f),
      button(B, 0.775f, 0.83f, 0.05f),
      button(X, 0.77f, 0.63f, 0.05f),
      button(Y, 0.665f, 0.735f, 0.05f),
      button(Z, 0.64f, 0.9f, 0.042f),
      button(START, 0.055f, 0.075f, 0.04f, 0.45f),
      button(L, 0.2f, 0.06f, 0.042f, 0.42f, 2),
      button(R, 0.42f, 0.06f, 0.042f, 0.42f, 2),
      menu_button(0.945f, 0.075f, 0.045f),
      edit_button(0.945f, 0.19f, 0.045f),
  };

  // Left-handed: the same, mirrored.
  Layout l = c;
  l.name = "left_handed";
  for (auto& w : l.widgets) w.x = 1.0f - w.x;   // only the positions move: the letters stay readable

  // Minimal: just what a stage needs.
  Layout m;
  m.name = "minimal";
  m.stick_mode = 1;
  m.widgets = {
      stick(kMove, 0.14f, 0.74f, 0.115f),
      stick(kCamera, 0.85f, 0.5f, 0.08f),
      button(A, 0.88f, 0.76f, 0.055f, 0.5f),
      button(B, 0.76f, 0.86f, 0.048f, 0.45f),
      button(X, 0.76f, 0.66f, 0.048f, 0.45f),
      button(Z, 0.78f, 0.05f, 0.04f, 0.4f, 2),
      menu_button(0.94f, 0.06f, 0.042f),
      edit_button(0.94f, 0.165f, 0.042f),
  };

  f.layouts = {c, l, m};
  for (auto& l2 : f.layouts) renumber(l2);
  return f;
}

Layout* find(File& file, const std::string& name) {
  for (auto& l : file.layouts)
    if (l.name == name) return &l;
  return nullptr;
}
const Layout* find(const File& file, const std::string& name) {
  for (auto& l : file.layouts)
    if (l.name == name) return &l;
  return nullptr;
}
int index_of(const File& file, const std::string& name) {
  for (size_t i = 0; i < file.layouts.size(); ++i)
    if (file.layouts[i].name == name) return (int)i;
  return -1;
}

namespace {
bool parse_widget(const std::string& text, Widget& w) {
  const auto f = split(text, ',');
  if (f.size() < 8) return false;
  if (f[0] == "button") w.kind = kButton;
  else if (f[0] == "stick") w.kind = kStick;
  else if (f[0] == "menu") w.kind = kMenu;
  else if (f[0] == "edit" || f[0] == "editor") w.kind = kEditor;
  else return false;
  w.action = clampi(atoi(f[1].c_str()), 0, kNumActions - 1);
  w.x = clampf((float)atof(f[2].c_str()), 0.0f, 1.0f);
  w.y = clampf((float)atof(f[3].c_str()), 0.0f, 1.0f);
  w.r = clampf((float)atof(f[4].c_str()), 0.02f, 0.3f);
  w.alpha = clampf((float)atof(f[5].c_str()), 0.05f, 1.0f);
  w.style = clampi(atoi(f[6].c_str()), 0, 2);
  w.on = atoi(f[7].c_str()) != 0;
  if (w.kind == kMenu) w.label = "MENU";
  if (w.kind == kEditor) w.label = "EDIT";
  if (w.kind == kStick) w.action = clampi(w.action, 0, 1);
  return true;
}

std::string widget_line(const Widget& w) {
  char buf[160];
  snprintf(buf, sizeof buf, "w=%s,%d,%.4f,%.4f,%.4f,%.3f,%d,%d",
           w.kind == kButton ? "button" : w.kind == kStick ? "stick" : w.kind == kMenu ? "menu" : "edit",
           w.action, (double)w.x, (double)w.y, (double)w.r, (double)w.alpha, w.style, w.on ? 1 : 0);
  return buf;
}
}  // namespace

File parse(const std::string& text) {
  File f;
  f.layouts.clear();
  Layout* cur = nullptr;
  bool saw_section = false;
  size_t at = 0;
  while (at <= text.size()) {
    size_t nl = text.find('\n', at);
    std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    at = nl == std::string::npos ? text.size() + 1 : nl + 1;
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      const std::string name = trim(line.substr(1, line.size() - 2));
      if (name.empty()) continue;
      Layout l;
      l.name = name;
      l.builtin = false;
      f.layouts.push_back(l);
      cur = &f.layouts.back();
      saw_section = true;
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    const std::string val = trim(line.substr(eq + 1));
    if (!cur) {
      if (key == "version") f.version = atoi(val.c_str());
      else if (key == "active") f.active = val;
      else if (key == "scale") f.scale = clampi(atoi(val.c_str()), 50, 200);
      else if (key == "opacity") f.opacity = clampi(atoi(val.c_str()), 20, 100);
      else if (key == "enabled") f.enabled = atoi(val.c_str()) != 0;
      else if (key == "left_handed") f.left_handed = atoi(val.c_str()) != 0;
      else if (key == "hide_with_gamepad") f.hide_with_gamepad = atoi(val.c_str()) != 0;
      continue;
    }
    if (key == "stick_mode") cur->stick_mode = clampi(atoi(val.c_str()), 0, 1);
    else if (key == "deadzone") cur->deadzone = clampi(atoi(val.c_str()), 0, 60);
    else if (key == "haptics") cur->haptics = atoi(val.c_str()) != 0;
    else if (key == "w") {
      Widget w;
      if (parse_widget(val, w)) cur->widgets.push_back(w);
    }
  }
  // a file with no usable layout (missing, all broken, or only sections without widgets) is the built-in set
  bool usable = false;
  for (auto& l : f.layouts)
    if (!l.widgets.empty()) usable = true;
  if (!saw_section || !usable) return defaults();
  // drop empty sections (a hand-edited file may hold a bare "[name]")
  f.layouts.erase(std::remove_if(f.layouts.begin(), f.layouts.end(), [](const Layout& l) { return l.widgets.empty(); }),
                  f.layouts.end());
  for (auto& l : f.layouts) renumber(l);
  if (!find(f, f.active)) f.active = f.layouts.front().name;
  return f;
}

std::string dump(const File& file) {
  std::string out;
  out += "# ShadowRecomp touch controls. Edit this file in game (Settings > Touch controls > Edit layout) or by\n";
  out += "# hand: x and y are 0..1 of the screen, r is of the screen's short side.\n";
  char head[256];
  snprintf(head, sizeof head,
           "version=%d\nactive=%s\nscale=%d\nopacity=%d\nenabled=%d\nleft_handed=%d\nhide_with_gamepad=%d\n\n",
           file.version, file.active.c_str(), file.scale, file.opacity, file.enabled ? 1 : 0,
           file.left_handed ? 1 : 0, file.hide_with_gamepad ? 1 : 0);
  out += head;
  for (const auto& l : file.layouts) {
    out += "[" + l.name + "]\n";
    char opts[128];
    snprintf(opts, sizeof opts, "stick_mode=%d\ndeadzone=%d\nhaptics=%d\n", l.stick_mode, l.deadzone, l.haptics ? 1 : 0);
    out += opts;
    for (const auto& w : l.widgets) out += widget_line(w) + "\n";
    out += "\n";
  }
  return out;
}

}  // namespace touch
