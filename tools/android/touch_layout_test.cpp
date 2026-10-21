// Test for the touch layout format (runtime/touch_layout.cpp): built-ins, the file round trip, hit testing and the
// damage tolerance of the parser. Needs no SDL and no game files.
//   g++ -std=c++20 -Iruntime tools/android/touch_layout_test.cpp runtime/touch_layout.cpp -o /tmp/touch_layout_test
//   /tmp/touch_layout_test
#include "touch_layout.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace touch;

static int g_fail = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      ++g_fail;                                                         \
    }                                                                   \
  } while (0)

int main() {
  // ---- the built-in presets -------------------------------------------------------------------------------------
  File d = defaults();
  CHECK(d.layouts.size() == 3);
  CHECK(d.active == "classic");
  const Layout* classic = find(d, "classic");
  CHECK(classic != nullptr);
  CHECK(find(d, "minimal") != nullptr);
  CHECK(find(d, "left_handed") != nullptr);
  if (!classic) return 1;
  CHECK(classic->widgets.size() >= 8);
  int sticks = 0, buttons = 0, menus = 0, edits = 0;
  for (auto& w : classic->widgets) {
    if (w.kind == kStick) ++sticks;
    if (w.kind == kButton) ++buttons;
    if (w.kind == kMenu) ++menus;
    if (w.kind == kEditor) ++edits;
  }
  CHECK(sticks == 2);
  CHECK(menus == 1);
  CHECK(edits == 1);   // every preset can reach the editor with a finger
  for (auto& l : d.layouts) {
    int e = 0;
    for (auto& w : l.widgets) e += w.kind == kEditor;
    CHECK(e == 1);
  }
  CHECK(buttons >= 6);
  // left-handed is the mirror image
  const Layout* lh = find(d, "left_handed");
  CHECK(lh && lh->widgets.size() == classic->widgets.size());
  if (lh) {
    for (size_t i = 0; i < lh->widgets.size(); ++i)
      CHECK(std::fabs((1.0f - lh->widgets[i].x) - classic->widgets[i].x) < 1e-5f);
  }

  // ---- hit testing ----------------------------------------------------------------------------------------------
  const float aspect = 16.0f / 9.0f;
  for (const auto& w : classic->widgets) {
    CHECK(hit(*classic, w.x, w.y, aspect) != nullptr);
    // just inside the edge (radius is of the short side; in x units it is smaller in landscape)
    const float rx = w.r * (1.0f / aspect) * 0.9f;
    CHECK(hit(*classic, w.x + rx, w.y, aspect) == &w);
  }
  CHECK(hit(*classic, 0.5f, 0.45f, aspect) == nullptr);   // the middle of the picture: no widget
  // outside every circle, even at the screen's edge
  CHECK(hit(*classic, 0.5f, 0.02f, aspect) == nullptr);

  // ---- the file round trip --------------------------------------------------------------------------------------
  std::string text = dump(d);
  File r = parse(text);
  CHECK(r.layouts.size() == d.layouts.size());
  CHECK(r.active == d.active);
  CHECK(r.scale == d.scale);
  CHECK(r.left_handed == d.left_handed);
  const Layout* rc = find(r, "classic");
  CHECK(rc != nullptr);
  if (rc) {
    CHECK(rc->widgets.size() == classic->widgets.size());
    CHECK(rc->stick_mode == classic->stick_mode);
    CHECK(rc->deadzone == classic->deadzone);
    for (size_t i = 0; i < rc->widgets.size(); ++i) {
      CHECK(rc->widgets[i].kind == classic->widgets[i].kind);
      CHECK(rc->widgets[i].action == classic->widgets[i].action);
      CHECK(std::fabs(rc->widgets[i].x - classic->widgets[i].x) < 1e-3f);
      CHECK(std::fabs(rc->widgets[i].y - classic->widgets[i].y) < 1e-3f);
      CHECK(std::fabs(rc->widgets[i].r - classic->widgets[i].r) < 1e-3f);
      CHECK(rc->widgets[i].style == classic->widgets[i].style);
      CHECK(rc->widgets[i].on == classic->widgets[i].on);
    }
  }

  // ---- a hand-written file --------------------------------------------------------------------------------------
  const char* hand =
      "# my layout\n"
      "version=1\n"
      "active=my\n"
      "scale=120\n"
      "opacity=80\n"
      "left_handed=1\n"
      "[my]\n"
      "stick_mode=0\n"
      "deadzone=25\n"
      "haptics=0\n"
      "w=stick,0,0.2,0.8,0.1,0.4,0,1\n"
      "w=button,0,0.9,0.8,0.06,0.6,0,1\n"
      "w=garbage,1,2,3\n"
      "w=edit,0,0.5,0.5,0.05,0.4,2,1\n"
      "[empty]\n";
  File h = parse(hand);
  CHECK(h.layouts.size() == 1);   // the empty section is dropped
  CHECK(h.active == "my");
  CHECK(h.scale == 120);
  CHECK(h.opacity == 80);
  CHECK(h.left_handed);
  const Layout* my = find(h, "my");
  CHECK(my != nullptr);
  if (my) {
    CHECK(my->widgets.size() == 3);   // the broken line is skipped
    CHECK(my->stick_mode == 0);
    CHECK(my->deadzone == 25);
    CHECK(!my->haptics);
    CHECK(my->widgets[0].kind == kStick);
    CHECK(my->widgets[1].action == A);
  }

  // ---- damage tolerance ------------------------------------------------------------------------------------------
  CHECK(parse("").layouts.size() == 3);                    // nothing: the built-ins
  CHECK(parse("active=nope\n").layouts.size() == 3);
  CHECK(parse("[a]\n").layouts.size() == 3);               // a section without widgets: built-ins
  File broken = parse("active=x\n[a]\nw=button,zzz,0.5,0.5,0.1,0.5,0,1\n");
  CHECK(broken.layouts.size() == 1);
  CHECK(broken.active == "a");                             // the asked-for layout is gone: the first one wins
  File clamped = parse("active=k\n[k]\nw=button,0,-5,9,9,9,9,1\n");
  const Layout* k = find(clamped, "k");
  CHECK(k != nullptr);
  if (k && !k->widgets.empty()) {
    CHECK(k->widgets[0].x >= 0.0f && k->widgets[0].x <= 1.0f);
    CHECK(k->widgets[0].y >= 0.0f && k->widgets[0].y <= 1.0f);
    CHECK(k->widgets[0].r <= 0.3f);
    CHECK(k->widgets[0].alpha <= 1.0f);
    CHECK(k->widgets[0].style <= 2);
  }

  // ---- action names ---------------------------------------------------------------------------------------------
  for (int i = 0; i < kNumActions; ++i) CHECK(action_from_key(action_key(i)) == i);
  CHECK(action_from_key("nope") == -1);

  if (g_fail == 0) printf("touch layout: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
