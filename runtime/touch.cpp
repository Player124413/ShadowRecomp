// Touch controls (see touch.h): SDL3 fingers -> the game's pad, the overlay on screen, and the in-game layout editor.
//
// Input model
//   * every finger is looked up against the active layout when it lands (a circle around each control);
//   * a finger on a button keeps it pressed until it lifts, however far it slides (a thumb always drifts);
//   * a finger on a stick becomes that stick's owner: in "floating" mode the stick starts where the thumb landed (the
//     drawn control is only a hint where to put it), in "fixed" mode its centre is the drawn circle;
//   * a finger is claimed by the first control it lands on, and a quick tap is seen through the SDL event watcher
//     rather than the frame poll, so a press never falls between two frames.
#include "touch.h"
#include "touch_layout.h"
#include "plat.h"
#include "sys.h"
#include "input.h"
#include "settings_ui.h"
#include "gx_lle.h"
#include <imgui.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace touch {
namespace {

// --------------------------------------------------------------------------------------------------------------------
// state
File g_file;
bool g_loaded = false;
bool g_enabled = true;             // touch_layouts.ini "enabled"; --touch overrides it for that run
bool g_enabled_forced = false;     // --touch was given (the launcher's switch)
int g_active = 0;                  // index into g_file.layouts
State g_state;
bool g_menu_press = false;
bool g_editor = false;
int g_selected = 0;                // editor: the selected widget's id (0 = nothing)
bool g_dirty = false;
std::string g_path;
std::string g_toast;
uint64_t g_toast_until = 0;

struct Held {
  SDL_FingerID finger = 0;
  int widget = 0;                  // Widget::id
  bool stick = false;
  bool camera = false;
  float fx = 0, fy = 0;            // where the finger is (0..1 of the window)
  float ox = 0, oy = 0;            // the stick's origin (floating: where it landed)
};
std::vector<Held> g_held;

struct Ev {
  int type;                        // 0 down, 1 motion, 2 up
  SDL_FingerID finger;
  float x, y;
};
std::vector<Ev> g_events;
std::mutex g_events_mutex;
bool g_watch = false;

SDL_Haptic* g_haptic = nullptr;
bool g_haptic_tried = false;
bool g_gamepad_seen = false;       // hide_with_gamepad: a real pad was connected, so the glass is not needed

// --------------------------------------------------------------------------------------------------------------------
// the file

void reload();   // defined below: reads the file (the editor's Cancel uses it)

void ensure_loaded() {
  if (g_loaded) return;
  g_loaded = true;
  reload();
}

// Reads the file (or falls back to the presets). The editor's Cancel calls this to forget what was changed.
void reload() {
  g_path = plat::data_path("touch_layouts.ini");
  if (FILE* f = plat::open(g_path.c_str(), "rb")) {
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    g_file = parse(text);
    logf("[touch] %s: %d layout(s), active '%s'\n", g_path.c_str(), (int)g_file.layouts.size(), g_file.active.c_str());
  } else {
    g_file = defaults();           // first run: the presets, written out so they can be edited by hand too
    logf("[touch] no %s: the built-in presets\n", g_path.c_str());
    save();
  }
  g_active = std::max(0, index_of(g_file, g_file.active));
  if (g_file.layouts.empty()) {
    g_file = defaults();
    g_active = 0;
  }
  if (!g_enabled_forced) g_enabled = g_file.enabled;
}

Layout* active() {
  if (g_file.layouts.empty()) return nullptr;
  g_active = std::clamp(g_active, 0, (int)g_file.layouts.size() - 1);
  return &g_file.layouts[g_active];
}
const Layout* active_const() {
  if (g_file.layouts.empty()) return nullptr;
  const int i = std::clamp(g_active, 0, (int)g_file.layouts.size() - 1);
  return &g_file.layouts[i];
}

// The file's global picture (scale / opacity / left-handed) applied to one widget: what is drawn and hit tested.
Widget effective(const Widget& w) {
  Widget e = w;
  if (g_file.left_handed) e.x = 1.0f - e.x;
  e.r = w.r * (g_file.scale / 100.0f);
  e.alpha = w.alpha * (g_file.opacity / 100.0f);
  return e;
}

// aspect of the drawable picture: the fingers are normalised to it too
float aspect() {
  int w = 0, h = 0;
  if (gxlle::g_window) SDL_GetWindowSizeInPixels(gxlle::g_window, &w, &h);
  return (w > 0 && h > 0) ? (float)w / (float)h : 16.0f / 9.0f;
}

void toast(const char* text, double seconds = 2.5) {
  g_toast = text ? text : "";
  g_toast_until = sys_time_ns() + (uint64_t)(seconds * 1e9);
}

// --------------------------------------------------------------------------------------------------------------------
// haptics

void haptic_init() {
  if (g_haptic_tried) return;
  g_haptic_tried = true;
  int n = 0;
  SDL_HapticID* ids = SDL_GetHaptics(&n);
  for (int i = 0; i < n; ++i) {
    SDL_Haptic* h = SDL_OpenHaptic(ids[i]);
    if (!h) continue;
    if (SDL_HapticRumbleSupported(h) && SDL_InitHapticRumble(h)) {
      g_haptic = h;
      const char* name = SDL_GetHapticName(h);
      logf("[touch] haptics: %s\n", name ? name : "?");
      break;
    }
    SDL_CloseHaptic(h);
  }
  SDL_free(ids);
}

void buzz(float strength) {
  const Layout* l = active_const();
  if (!l || !l->haptics || g_editor) return;
  haptic_init();
  if (g_haptic) SDL_PlayHapticRumble(g_haptic, std::clamp(strength, 0.05f, 1.f), 22);
}

// --------------------------------------------------------------------------------------------------------------------
// fingers

bool SDLCALL watch(void*, SDL_Event* e) {
  if (!e) return true;
  Ev ev;
  switch (e->type) {
    case SDL_EVENT_FINGER_DOWN: ev = {0, e->tfinger.fingerID, e->tfinger.x, e->tfinger.y}; break;
    case SDL_EVENT_FINGER_MOTION: ev = {1, e->tfinger.fingerID, e->tfinger.x, e->tfinger.y}; break;
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: ev = {2, e->tfinger.fingerID, e->tfinger.x, e->tfinger.y}; break;
    default: return true;
  }
  std::lock_guard<std::mutex> lk(g_events_mutex);
  if (g_events.size() < 512) g_events.push_back(ev);   // a stuck pump must not grow without bound
  return true;
}

int index_of_widget(const Layout& l, int id) {
  for (size_t i = 0; i < l.widgets.size(); ++i)
    if (l.widgets[i].id == id) return (int)i;
  return -1;
}

uint16_t pad_bit(int action) {
  switch (action) {
    case A: return PAD_A;
    case B: return PAD_B;
    case X: return PAD_X;
    case Y: return PAD_Y;
    case Z: return PAD_Z;
    case L: return PAD_L;
    case R: return PAD_R;
    case START: return PAD_START;
    case UP: return PAD_UP;
    case DOWN: return PAD_DOWN;
    case LEFT: return PAD_LEFT;
    case RIGHT: return PAD_RIGHT;
    default: return 0;
  }
}

void update_sticks() {
  const Layout* l = active_const();
  if (!l) return;
  const float dead = std::clamp(l->deadzone, 0, 60) / 100.0f;
  int8_t sx = 0, sy = 0, cx = 0, cy = 0;
  for (const auto& h : g_held) {
    if (!h.stick) continue;
    const int idx = index_of_widget(*l, h.widget);
    if (idx < 0) continue;
    const Widget e = effective(l->widgets[idx]);
    const float r = std::max(0.02f, e.r);
    const float dx = (h.fx - h.ox) / r, dy = (h.fy - h.oy) / r;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) continue;
    const float v = std::min(1.0f, len) - dead;          // the deadzone in the same units as the radius
    if (v <= 0.0f) continue;
    const float k = v / ((1.0f - dead) * len);
    const int8_t ax = (int8_t)std::lround(std::clamp(dx * k, -1.f, 1.f) * 127.0f);
    const int8_t ay = (int8_t)std::lround(std::clamp(-dy * k, -1.f, 1.f) * 127.0f);   // screen y grows downwards
    if (h.camera) {
      cx = ax;
      cy = ay;
    } else {
      sx = ax;
      sy = ay;
    }
  }
  g_state.sx = sx;
  g_state.sy = sy;
  g_state.cx = cx;
  g_state.cy = cy;
}

Held* held_for(SDL_FingerID id) {
  for (auto& h : g_held)
    if (h.finger == id) return &h;
  return nullptr;
}

bool release(Held& h) {
  const Layout* l = active_const();
  if (l && !h.stick) {
    const int idx = index_of_widget(*l, h.widget);
    if (idx >= 0 && l->widgets[idx].kind == kButton) {
      g_state.buttons &= (uint16_t)~pad_bit(l->widgets[idx].action);
      return true;
    }
  }
  return false;
}

void finger_down(SDL_FingerID id, float x, float y) {
  const Layout* real = active_const();
  if (!real || real->widgets.empty()) return;
  if (held_for(id)) return;
  // hit test against the drawn picture (scale / mirroring applied), then work on the widget with the same index
  Layout eff = *real;
  for (auto& w : eff.widgets) w = effective(w);
  const Widget* hitw = hit(eff, x, y, aspect());
  if (!hitw) return;
  const int index = (int)(hitw - eff.widgets.data());
  if (index < 0 || index >= (int)real->widgets.size()) return;
  const Widget& w = real->widgets[index];
  Held h;
  h.finger = id;
  h.widget = w.id;
  h.fx = x;
  h.fy = y;
  if (w.kind == kButton) {
    g_state.buttons |= pad_bit(w.action);
    buzz(0.35f);
  } else if (w.kind == kStick) {
    h.stick = true;
    h.camera = w.action == kCamera;
    if (real->stick_mode == 1) {           // floating: the stick starts where the thumb landed
      h.ox = x;
      h.oy = y;
    } else {                                // fixed: the drawn circle is the centre
      const Widget e = effective(w);
      h.ox = e.x;
      h.oy = e.y;
    }
  } else if (w.kind == kMenu) {
    g_menu_press = true;
    buzz(0.5f);
  } else if (w.kind == kEditor) {
    open_editor();               // declared in touch.h; also takes the finger off the glass (release_all)
    buzz(0.5f);
    return;
  }
  g_state.any = true;
  g_held.push_back(h);
  update_sticks();
}

void finger_move(SDL_FingerID id, float x, float y) {
  if (Held* h = held_for(id)) {
    h->fx = x;
    h->fy = y;
    update_sticks();
  }
}

void finger_up(SDL_FingerID id) {
  for (size_t i = 0; i < g_held.size(); ++i) {
    if (g_held[i].finger != id) continue;
    release(g_held[i]);
    g_held.erase(g_held.begin() + i);
    break;
  }
  update_sticks();
}

void release_all() {
  g_held.clear();
  g_state = State{};
}

// --------------------------------------------------------------------------------------------------------------------
// drawing: one control. Everything is in screen pixels so it looks the same on any phone.
struct Look {
  ImFont* font = nullptr;
  float unit = 1.0f;               // 1 = the reference size (a 1080p-ish phone); labels scale with it
};

ImU32 col_of(float alpha, int pressed, float r, float g, float b) {
  const float a = std::clamp(alpha * (pressed ? 1.0f : 0.85f), 0.0f, 1.0f);
  return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), (int)(a * 255));
}

void draw_widget(ImDrawList* dl, const Widget& w, const Look& look, ImVec2 centre, float radius_px, bool pressed,
                 bool selected, bool editing) {
  const int segs = radius_px > 40 ? 48 : 24;
  const float t = std::max(1.5f, look.unit * 2.0f);
  // the body: a translucent disc so the picture stays visible under the thumb
  if (w.style == 2) {   // pill (long horizontal): the shoulders and jump buttons
    const ImVec2 a(centre.x - radius_px * 1.6f, centre.y - radius_px * 0.62f);
    const ImVec2 b(centre.x + radius_px * 1.6f, centre.y + radius_px * 0.62f);
    dl->AddRectFilled(a, b, col_of(w.alpha * (pressed ? 0.55f : 0.32f), pressed, 0.10f, 0.12f, 0.16f), radius_px * 0.62f);
    dl->AddRect(a, b, col_of(w.alpha, pressed, 0.95f, 0.95f, 1.0f), radius_px * 0.62f, 0, t);
  } else {
    dl->AddCircleFilled(centre, radius_px, col_of(w.alpha * (pressed ? 0.55f : 0.30f), pressed, 0.10f, 0.12f, 0.16f), segs);
    dl->AddCircle(centre, radius_px, col_of(w.alpha, pressed, 0.95f, 0.95f, 1.0f), segs, t);
    if (w.style == 1)   // diamond: a triangle pair over the disc, so the shapes are told apart by touch
      dl->AddQuad(ImVec2(centre.x, centre.y - radius_px), ImVec2(centre.x + radius_px, centre.y),
                  ImVec2(centre.x, centre.y + radius_px), ImVec2(centre.x - radius_px, centre.y),
                  col_of(w.alpha * 0.5f, pressed, 0.95f, 0.95f, 1.0f), t);
  }
  if (selected && editing) {
    dl->AddCircle(centre, radius_px + look.unit * 4, IM_COL32(90, 200, 255, 230), segs, look.unit * 2.5f);
  }
  // the label
  if (look.font) {
    const char* text = w.label ? w.label
                               : w.kind == kButton ? action_label(w.action)
                               : w.kind == kStick ? (w.action == kCamera ? "CAM" : "MOVE")
                               : w.kind == kEditor ? "EDIT"
                                                   : "MENU";
    float size = radius_px * (w.style == 2 ? 0.55f : 0.75f);
    size = std::clamp(size, look.unit * 10, look.unit * 34);
    if (std::strlen(text) > 2) size = std::min(size, radius_px * 1.4f);
    const ImVec2 measured = look.font->CalcTextSizeA(size, FLT_MAX, 0, text);
    dl->AddText(look.font, size, ImVec2(centre.x - measured.x * 0.5f, centre.y - measured.y * 0.5f),
                col_of(std::min(1.0f, w.alpha + 0.35f), pressed, 1.0f, 1.0f, 1.0f), text);
  }
}

// The game's touch pad inside the frame (settings::draw).
void draw_sticks(ImDrawList* dl, const Layout& l, const Look& look, ImVec2 screen) {
  for (const auto& w : l.widgets) {
    if (w.kind != kStick || !w.on) continue;
    const float r = w.r * std::min(screen.x, screen.y);
    ImVec2 centre(w.x * screen.x, w.y * screen.y);
    // a held floating stick is drawn where the thumb is
    for (const auto& h : g_held) {
      if (h.widget != w.id || !h.stick) continue;
      centre = ImVec2(h.ox * screen.x, h.oy * screen.y);
    }
    dl->AddCircle(centre, r, col_of(w.alpha, 0, 0.85f, 0.88f, 1.0f), 40, std::max(1.5f, look.unit * 2.0f));
    dl->AddCircleFilled(centre, r * 0.34f, col_of(w.alpha * 0.9f, 0, 0.75f, 0.80f, 0.95f), 24);
  }
}

void draw_layer(bool editing) {
  ensure_loaded();
  const Layout* l = active_const();
  const ImGuiViewport* v = ImGui::GetMainViewport();
  if (!l || !v) return;
  const ImVec2 screen = v->Size;
  if (screen.x < 1 || screen.y < 1) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  Look look;
  look.font = ImGui::GetFont();
  look.unit = std::max(0.75f, screen.y / 800.0f);
  const float short_px = std::min(screen.x, screen.y);

  // the picture as it is drawn (the global scale / mirroring applied)
  for (const auto& w0 : l->widgets) {
    const Widget w = effective(w0);
    if (!w.on) continue;
    const ImVec2 centre(w.x * screen.x, w.y * screen.y);
    const float r = w.r * short_px;
    bool pressed = false;
    for (const auto& h : g_held)
      if (h.widget == w0.id && !h.stick) pressed = true;
    if (w.kind == kStick) continue;   // sticks are drawn under the buttons, with their knob
    draw_widget(dl, w, look, centre, r, pressed, w0.id == g_selected, editing);
  }
  Layout eff = *l;
  for (auto& w : eff.widgets) w = effective(w);
  draw_sticks(dl, eff, look, screen);

  if (!g_toast.empty() && sys_time_ns() < g_toast_until) {
    const ImVec2 m = ImGui::CalcTextSize(g_toast.c_str());
    dl->AddRectFilled(ImVec2((screen.x - m.x) * 0.5f - 12, screen.y * 0.06f - 8),
                      ImVec2((screen.x + m.x) * 0.5f + 12, screen.y * 0.06f + m.y + 8), IM_COL32(12, 14, 20, 200), 8);
    dl->AddText(ImVec2((screen.x - m.x) * 0.5f, screen.y * 0.06f), IM_COL32(235, 238, 245, 255), g_toast.c_str());
  }
}

// --------------------------------------------------------------------------------------------------------------------
// the editor

void select(int id) { g_selected = id; }

int selected_index() {
  const Layout* l = active_const();
  if (!l || g_selected == 0) return -1;
  return index_of_widget(*l, g_selected);
}

void add_widget(int kind) {
  ensure_loaded();
  Layout* l = active();
  if (!l) return;
  Widget w;
  w.kind = kind;
  w.id = 0;
  for (auto& x : l->widgets) w.id = std::max(w.id, x.id);
  ++w.id;
  w.x = 0.5f;
  w.y = 0.5f;
  if (kind == kStick) {
    w.action = kMove;
    w.r = 0.11f;
    w.alpha = 0.45f;
  } else if (kind == kMenu) {
    w.r = 0.045f;
    w.alpha = 0.5f;
    w.style = 2;
    w.label = "MENU";
  } else if (kind == kEditor) {
    w.r = 0.036f;
    w.alpha = 0.4f;
    w.style = 2;
    w.label = "EDIT";
  } else {
    w.action = A;
    w.r = 0.055f;
    w.alpha = 0.5f;
  }
  l->widgets.push_back(w);
  g_selected = w.id;
  g_dirty = true;
}

void remove_widget(int id) {
  Layout* l = active();
  if (!l) return;
  l->widgets.erase(std::remove_if(l->widgets.begin(), l->widgets.end(), [&](const Widget& w) { return w.id == id; }),
                   l->widgets.end());
  if (g_selected == id) g_selected = 0;
  g_dirty = true;
}

void draw_editor() {
  ensure_loaded();
  Layout* l = active();
  if (!l) return;
  ImGuiIO& io = ImGui::GetIO();
  const ImGuiViewport* v = ImGui::GetMainViewport();
  const ImVec2 screen = v->Size;
  const float u = std::max(0.75f, screen.y / 800.0f);

  // ---- drag on the glass (only outside the panel, and only when the panel is not taking the mouse) -----------------
  static int drag_id = 0;
  static bool was_down = false;
  const ImVec2 m = io.MousePos;
  const bool over_panel = io.WantCaptureMouse;
  const float mx = m.x / std::max(1.0f, screen.x), my = m.y / std::max(1.0f, screen.y);
  if (io.MouseDown[0] && !over_panel) {
    if (!was_down) {
      Layout eff = *l;
      for (auto& w : eff.widgets) w = effective(w);
      const Widget* hitw = hit(eff, mx, my, aspect());
      drag_id = hitw ? hitw->id : 0;
      if (drag_id) select(drag_id);
    }
    if (drag_id) {
      const int idx = index_of_widget(*l, drag_id);
      if (idx >= 0) {
        // dragging the drawn control: undo the mirroring and let the widget follow the finger, inside the screen
        const float x = g_file.left_handed ? 1.0f - mx : mx;
        l->widgets[idx].x = std::clamp(x, 0.02f, 0.98f);
        l->widgets[idx].y = std::clamp(my, 0.02f, 0.98f);
        g_dirty = true;
      }
    }
  } else {
    drag_id = 0;
  }
  was_down = io.MouseDown[0];

  // ---- the panel ---------------------------------------------------------------------------------------------------
  ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y - 12 * u), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  ImGui::SetNextWindowSize(ImVec2(std::min(screen.x - 24 * u, 760 * u), 0), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.94f);
  ImGui::Begin("Touch controls", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                             ImGuiWindowFlags_AlwaysAutoResize);

  // layout picker
  if (ImGui::BeginCombo("Layout", l->name.c_str())) {
    for (int i = 0; i < (int)g_file.layouts.size(); ++i) {
      const bool sel = i == g_active;
      if (ImGui::Selectable(g_file.layouts[i].name.c_str(), sel) && !sel) {
        g_active = i;
        g_file.active = g_file.layouts[i].name;
        g_selected = 0;
        g_dirty = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button("New copy")) {
    char name[64];
    for (int n = 2; n < 100; ++n) {
      snprintf(name, sizeof name, "%s %d", l->name.c_str(), n);
      if (!find(g_file, name)) break;
    }
    if (add_layout(name)) g_active = (int)g_file.layouts.size() - 1;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(l->builtin && g_file.layouts.size() <= 3);
  if (ImGui::Button("Delete")) {
    const std::string name = l->name;
    if (delete_layout(name.c_str())) toast("Layout deleted");
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Reset")) {
    reset_active_layout();
    toast("Back to the preset");
  }

  // whole-picture sliders
  int scale = g_file.scale, opacity = g_file.opacity;
  if (ImGui::SliderInt("Size, %", &scale, 50, 200)) { g_file.scale = scale; g_dirty = true; }
  if (ImGui::SliderInt("Opacity, %", &opacity, 20, 100)) { g_file.opacity = opacity; g_dirty = true; }
  if (ImGui::Checkbox("Left-handed", &g_file.left_handed)) g_dirty = true;
  ImGui::SameLine();
  if (ImGui::Checkbox("Hide with a controller", &g_file.hide_with_gamepad)) g_dirty = true;

  // this layout's feel
  int mode = l->stick_mode, dz = l->deadzone;
  const char* modes[] = {"Fixed", "Floating"};
  if (ImGui::Combo("Stick", &mode, modes, 2)) { l->stick_mode = mode; g_dirty = true; }
  ImGui::SameLine();
  if (ImGui::SliderInt("Deadzone, %", &dz, 0, 60)) { l->deadzone = dz; g_dirty = true; }
  ImGui::SameLine();
  if (ImGui::Checkbox("Vibration", &l->haptics)) g_dirty = true;

  // add / remove
  if (ImGui::Button("+ Button")) add_widget(kButton);
  ImGui::SameLine();
  if (ImGui::Button("+ Stick")) add_widget(kStick);
  ImGui::SameLine();
  if (ImGui::Button("+ Menu")) add_widget(kMenu);
  ImGui::SameLine();
  if (ImGui::Button("+ Edit")) add_widget(kEditor);
  ImGui::SameLine();
  ImGui::BeginDisabled(selected_index() < 0);
  if (ImGui::Button("Remove")) remove_widget(g_selected);
  ImGui::EndDisabled();

  // the selected control
  if (const int idx = selected_index(); idx >= 0) {
    Widget& w = l->widgets[idx];
    ImGui::Separator();
    ImGui::Text("%s %d of %d - drag it on the screen",
                w.kind == kStick ? "Stick" : w.kind == kMenu ? "Menu" : w.kind == kEditor ? "Edit" : "Button", idx + 1,
                (int)l->widgets.size());
    if (w.kind == kButton) {
      int a = w.action;
      if (ImGui::BeginCombo("Action", action_label(a))) {
        for (int i = 0; i < kNumActions; ++i)
          if (ImGui::Selectable(action_label(i), i == a)) { w.action = i; g_dirty = true; }
        ImGui::EndCombo();
      }
      const char* styles[] = {"Round", "Diamond", "Pill"};
      int st = w.style;
      if (ImGui::Combo("Shape", &st, styles, 3)) { w.style = st; g_dirty = true; }
    } else if (w.kind == kStick) {
      const char* axes[] = {"Move (left)", "Camera (right)"};
      int ax = w.action;
      if (ImGui::Combo("Stick for", &ax, axes, 2)) { w.action = ax; g_dirty = true; }
    }
    int r = (int)std::lround(w.r * 1000.0f), a = (int)std::lround(w.alpha * 100.0f);
    if (ImGui::SliderInt("Radius, 1/1000 of the screen", &r, 20, 200)) { w.r = r / 1000.0f; g_dirty = true; }
    if (ImGui::SliderInt("Opacity, %", &a, 5, 100)) { w.alpha = a / 100.0f; g_dirty = true; }
    bool on = w.on;
    if (ImGui::Checkbox("Show", &on)) { w.on = on; g_dirty = true; }
    ImGui::SameLine();
    ImGui::TextDisabled("(x %.3f  y %.3f)", (double)w.x, (double)w.y);
  } else {
    ImGui::Separator();
    ImGui::TextDisabled("Tap a control to edit it, or drag it where you want it.");
  }

  ImGui::Separator();
  if (ImGui::Button("Done", ImVec2(120 * u, 0))) {
    close_editor(true);
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(120 * u, 0))) close_editor(false);
  ImGui::SameLine();
  ImGui::TextDisabled("Layouts are saved in touch_layouts.ini");
  ImGui::End();
}

}  // namespace

// --------------------------------------------------------------------------------------------------------------------
// public

void load() {
  ensure_loaded();
}

void save() {
  if (g_path.empty()) g_path = plat::data_path("touch_layouts.ini");
  const std::string text = dump(g_file);
  FILE* f = plat::open(g_path.c_str(), "wb");
  if (!f) {
    logf("[touch] cannot write %s\n", g_path.c_str());
    return;
  }
  fwrite(text.data(), 1, text.size(), f);
  fclose(f);
  g_dirty = false;
}

bool enabled() { return g_enabled; }
void set_enabled(bool on) {
  g_enabled = on;
  g_enabled_forced = true;
  // remember it for the next start too (the in-game menu can change it as well)
  if (g_loaded && g_file.enabled != on) {
    g_file.enabled = on;
    save();
  }
}

int layout_count() { return (int)g_file.layouts.size(); }
const char* layout_name(int index) {
  ensure_loaded();
  return index >= 0 && index < (int)g_file.layouts.size() ? g_file.layouts[index].name.c_str() : "";
}
const char* active_layout() {
  ensure_loaded();
  const Layout* l = active_const();
  return l ? l->name.c_str() : "";
}
void set_layout(int index) {
  ensure_loaded();
  if (index < 0 || index >= (int)g_file.layouts.size()) return;
  g_active = index;
  g_file.active = g_file.layouts[index].name;
  g_selected = 0;
  save();
  const std::string msg = "Layout: " + g_file.active;
  toast(msg.c_str());
}
void cycle_layout(int dir) {
  ensure_loaded();
  const int n = (int)g_file.layouts.size();
  if (n == 0) return;
  set_layout(((g_active + dir) % n + n) % n);
}
void reset_active_layout() {
  ensure_loaded();
  Layout* l = active();
  if (!l) return;
  const File d = defaults();
  const Layout* preset = find(d, l->name);
  if (!preset) preset = &d.layouts.front();
  *l = *preset;
  l->builtin = true;
  g_selected = 0;
  save();
}
bool add_layout(const char* name) {
  ensure_loaded();
  if (!name || !*name || find(g_file, name)) return false;
  Layout* cur = active();
  if (!cur) return false;
  Layout copy = *cur;
  copy.name = name;
  copy.builtin = false;
  // unique ids
  int id = 0;
  for (auto& w : copy.widgets) w.id = ++id;
  g_file.layouts.push_back(copy);
  g_file.active = copy.name;
  g_active = (int)g_file.layouts.size() - 1;
  g_selected = 0;
  save();
  return true;
}
bool delete_layout(const char* name) {
  ensure_loaded();
  if (!name) return false;
  const int idx = index_of(g_file, name);
  if (idx < 0 || g_file.layouts.size() <= 1) return false;
  const bool was_active = idx == g_active;
  g_file.layouts.erase(g_file.layouts.begin() + idx);
  if (was_active) {
    g_active = std::clamp(idx, 0, (int)g_file.layouts.size() - 1);
    g_file.active = g_file.layouts[g_active].name;
  } else if (g_active > idx) {
    --g_active;
  }
  g_selected = 0;
  save();
  return true;
}

void tick() {
  ensure_loaded();
  if (!g_watch) {
    SDL_AddEventWatch(watch, nullptr);
    g_watch = true;
  }
  // hide_with_gamepad: any real pad in use means the glass overlay is in the way
  {
    int n = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&n);
    g_gamepad_seen = pads && n > 0;
    SDL_free(pads);
  }
  std::vector<Ev> events;
  {
    std::lock_guard<std::mutex> lk(g_events_mutex);
    events.swap(g_events);
  }
  // the editor owns the fingers (it works on ImGui's mouse): the game must not see presses meant for the panel
  const bool editor_now = g_editor;
  for (const auto& e : events) {
    if (editor_now) {
      if (e.type == 2) finger_up(e.finger);   // never leave a button stuck when the editor opens mid-press
      continue;
    }
    if (e.type == 0) finger_down(e.finger, e.x, e.y);
    else if (e.type == 1) finger_move(e.finger, e.x, e.y);
    else finger_up(e.finger);
  }
  if (editor_now) release_all();
  if (!wanted()) release_all();
}

State state() { return g_state; }

bool consume_menu() {
  const bool m = g_menu_press;
  g_menu_press = false;
  return m;
}

bool wanted() {
  if (!g_enabled || g_editor) return false;
  if (g_file.hide_with_gamepad && g_gamepad_seen) return false;
  return true;
}

bool visible() { return wanted(); }

bool editor_open() { return g_editor; }

void open_editor() {
  ensure_loaded();
  release_all();
  g_editor = true;
  g_selected = 0;
  g_dirty = false;
  toast("Drag the controls where your thumbs want them");
}

void close_editor(bool save_changes) {
  if (!g_editor) return;
  g_editor = false;
  release_all();
  if (save_changes && g_dirty) {
    save();
    toast("Touch layout saved");
  } else if (!save_changes && g_dirty) {
    reload();   // throws the edits away: the file on disk is the truth
    toast("Changes discarded");
  }
  g_dirty = false;
}

void draw() {
  ensure_loaded();
  // The editor draws even when the overlay is switched off: a layout that ended up unusable is fixed there, and the
  // launcher's "Edit the touch controls" would otherwise show nothing.
  if (!g_editor && (!g_enabled || !wanted())) return;
  draw_layer(g_editor);
  if (g_editor) draw_editor();
}

void editor_add(int kind) {
  ensure_loaded();
  add_widget(kind);
  save();
}

void editor_remove_selected() {
  ensure_loaded();
  remove_widget(g_selected);
  save();
}

}  // namespace touch
