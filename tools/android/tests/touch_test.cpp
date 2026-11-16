// Test of ShadowRecomp/ui/touch_controls.cpp on the build machine: the real Dear ImGui
// without a rendering backend, and SDL reduced to the four functions the overlay calls.
#include <SDL3/SDL.h>
#include <imgui.h>
#include <ui/touch_controls.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// --- SDL stubs -------------------------------------------------------------------------
static uint64_t g_nowNs = 1'000'000'000ull;
static int g_gamepadCount = 0;
static bool g_gamepadVirtual = false;

extern "C" Uint64 SDL_GetTicksNS(void) { return g_nowNs; }
extern "C" SDL_JoystickID* SDL_GetGamepads(int* count)
{
    *count = g_gamepadCount;
    auto* ids = static_cast<SDL_JoystickID*>(malloc(sizeof(SDL_JoystickID) * (g_gamepadCount + 1)));
    for (int i = 0; i < g_gamepadCount; i++) ids[i] = SDL_JoystickID(i + 1);
    ids[g_gamepadCount] = 0;
    return ids;
}
extern "C" bool SDL_IsJoystickVirtual(SDL_JoystickID) { return g_gamepadVirtual; }
extern "C" void SDL_free(void* mem) { free(mem); }

// --- Helpers ---------------------------------------------------------------------------
static float g_displayW = 1920.0f, g_displayH = 1080.0f;   // ImGui display (game image)
static float g_surfaceW = 1920.0f, g_surfaceH = 1080.0f;   // whole window

static void Frame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = { g_displayW, g_displayH };
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    TouchControls::Draw(g_surfaceW, g_surfaceH);
    ImGui::Render();
}

// Position given in DISPLAY pixels; converted to the window-normalized form SDL reports.
static void Finger(Uint32 type, SDL_FingerID id, float displayX, float displayY)
{
    SDL_Event event{};
    event.type = type;
    event.tfinger.type = SDL_EventType(type);
    event.tfinger.timestamp = g_nowNs;
    event.tfinger.fingerID = id;
    event.tfinger.x = (displayX + (g_surfaceW - g_displayW) * 0.5f) / g_surfaceW;
    event.tfinger.y = (displayY + (g_surfaceH - g_displayH) * 0.5f) / g_surfaceH;
    TouchControls::OnEvent(event);
}

static void Simple(Uint32 type)
{
    SDL_Event event{};
    event.type = type;
    TouchControls::OnEvent(event);
}

static TouchControls::PadState Pad(bool* active = nullptr)
{
    TouchControls::PadState state;
    const bool result = TouchControls::GetPadState(state);
    if (active) *active = result;
    return state;
}

// Default layout, mirrored from touch_controls.cpp (16:9 and wider: unit == height).
struct Pos { float x, y; };
static Pos Left(float offset, float y) { return { offset * std::min(1.0f, (g_displayW / g_displayH) / 1.6f) * g_displayH, y * g_displayH }; }
static Pos Right(float offset, float y) { return { g_displayW - offset * std::min(1.0f, (g_displayW / g_displayH) / 1.6f) * g_displayH, y * g_displayH }; }

static bool NeutralExceptNothing(const TouchControls::PadState& s)
{
    if (s.buttons || s.thumbLX || s.thumbLY || s.thumbRX || s.thumbRY) return false;
    for (uint8_t b : s.analogButtons) if (b) return false;
    return true;
}

int main()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "shadow_touch_test";
    fs::remove_all(dir);
    const fs::path layout = dir / "nested" / "touch_layout.cfg";

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char* pixels; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    // 0. Not configured: everything is a no-op.
    {
        bool active = true;
        auto s = Pad(&active);
        CHECK(!active && NeutralExceptNothing(s));
        CHECK(!TouchControls::IsEditing());
        Frame();
    }

    // 1. Default layout, policy On, swipe camera.
    TouchControls::Configure(ETouchControls::On, ETouchCamera::Swipe, layout, false);
    Frame();
    {
        bool active = false;
        auto s = Pad(&active);
        CHECK(active && NeutralExceptNothing(s));
    }

    // A button.
    {
        Pos a = Right(0.26f, 0.82f);
        Finger(SDL_EVENT_FINGER_DOWN, 1, a.x, a.y);
        auto s = Pad();
        CHECK(s.analogButtons[0] == 0xFF);
        CHECK(s.analogButtons[1] == 0 && s.analogButtons[2] == 0 && s.analogButtons[3] == 0);
        CHECK(s.buttons == 0);
        Frame();
        // Sliding off does not release (finger keeps its control), release does.
        Finger(SDL_EVENT_FINGER_MOTION, 1, a.x + 400, a.y - 300);
        CHECK(Pad().analogButtons[0] == 0xFF);
        Finger(SDL_EVENT_FINGER_UP, 1, a.x + 400, a.y - 300);
        CHECK(NeutralExceptNothing(Pad()));
    }

    // Every button of the default layout maps to its own target.
    {
        struct { Pos p; int analog; uint16_t digital; } cases[] = {
            { Right(0.26f, 0.82f), 0, 0 }, { Right(0.12f, 0.66f), 1, 0 }, { Right(0.40f, 0.66f), 2, 0 },
            { Right(0.26f, 0.50f), 3, 0 }, { Right(0.31f, 0.13f), 4, 0 }, { Left(0.31f, 0.13f), 5, 0 },
            { Left(0.12f, 0.14f), 6, 0 }, { Right(0.12f, 0.14f), 7, 0 },
            { Right(0.52f, 0.10f), -1, 0x0010 }, { Left(0.52f, 0.10f), -1, 0x0020 },
            { Left(0.66f, 0.62f), -1, 0x0001 }, { Left(0.66f, 0.84f), -1, 0x0002 },
            { Left(0.55f, 0.73f), -1, 0x0004 }, { Left(0.77f, 0.73f), -1, 0x0008 },
        };
        for (auto& c : cases)
        {
            Finger(SDL_EVENT_FINGER_DOWN, 7, c.p.x, c.p.y);
            auto s = Pad();
            for (int i = 0; i < 8; i++) CHECK(s.analogButtons[i] == (i == c.analog ? 0xFF : 0));
            CHECK(s.buttons == c.digital);
            CHECK(s.thumbLX == 0 && s.thumbLY == 0 && s.thumbRX == 0 && s.thumbRY == 0);
            Finger(SDL_EVENT_FINGER_UP, 7, c.p.x, c.p.y);
        }
        CHECK(NeutralExceptNothing(Pad()));
    }

    // Left stick: full right, then up-left diagonal, clamped to the unit circle.
    {
        Pos c = Left(0.26f, 0.70f);
        const float radius = 0.135f * g_displayH;
        Finger(SDL_EVENT_FINGER_DOWN, 2, c.x, c.y);
        auto s = Pad();
        CHECK(s.thumbLX == 0 && s.thumbLY == 0);
        Finger(SDL_EVENT_FINGER_MOTION, 2, c.x + radius * 3, c.y);
        s = Pad();
        CHECK(s.thumbLX == 32767 && s.thumbLY == 0);
        Finger(SDL_EVENT_FINGER_MOTION, 2, c.x - radius * 2, c.y - radius * 2);
        s = Pad();
        CHECK(std::abs(s.thumbLX - (-23170)) <= 2 && std::abs(s.thumbLY - 23170) <= 2); // up is positive
        Finger(SDL_EVENT_FINGER_MOTION, 2, c.x + radius * 0.5f, c.y + radius * 0.25f);
        s = Pad();
        CHECK(std::abs(s.thumbLX - 16384) <= 2 && std::abs(s.thumbLY - (-8192)) <= 2);

        // Second finger on A at the same time (multi-touch).
        Pos a = Right(0.26f, 0.82f);
        Finger(SDL_EVENT_FINGER_DOWN, 3, a.x, a.y);
        s = Pad();
        CHECK(s.analogButtons[0] == 0xFF && std::abs(s.thumbLX - 16384) <= 2);
        Frame();
        Finger(SDL_EVENT_FINGER_UP, 2, c.x, c.y);
        s = Pad();
        CHECK(s.analogButtons[0] == 0xFF && s.thumbLX == 0 && s.thumbLY == 0);
        Finger(SDL_EVENT_FINGER_CANCELED, 3, a.x, a.y);
        CHECK(NeutralExceptNothing(Pad()));
    }

    // Swipe camera on the free area; the right stick is disabled in this mode.
    {
        const float cx = g_displayW * 0.5f, cy = g_displayH * 0.62f;
        Finger(SDL_EVENT_FINGER_DOWN, 4, cx, cy);
        CHECK(NeutralExceptNothing(Pad()));
        g_nowNs += 16'000'000;
        Finger(SDL_EVENT_FINGER_MOTION, 4, cx + 60, cy);
        auto s = Pad();
        CHECK(s.thumbRX > 8000 && s.thumbRY == 0);
        g_nowNs += 16'000'000;
        Finger(SDL_EVENT_FINGER_MOTION, 4, cx + 60, cy - 60);
        s = Pad();
        CHECK(s.thumbRY > 8000); // finger up = camera up
        // A second finger on the free area does not become a second camera.
        Finger(SDL_EVENT_FINGER_DOWN, 5, cx - 200, cy);
        g_nowNs += 16'000'000;
        Finger(SDL_EVENT_FINGER_MOTION, 5, cx - 400, cy);
        s = Pad();
        CHECK(s.thumbRX >= 0);
        Finger(SDL_EVENT_FINGER_UP, 5, cx - 400, cy);
        // Resting finger: the deflection decays to zero after the hold time.
        g_nowNs += 16'000'000;
        Finger(SDL_EVENT_FINGER_MOTION, 4, cx + 120, cy - 60);
        CHECK(Pad().thumbRX != 0);
        g_nowNs += 200'000'000;
        s = Pad();
        CHECK(s.thumbRX == 0 && s.thumbRY == 0);
        // Lifting the finger zeroes it immediately.
        g_nowNs += 16'000'000;
        Finger(SDL_EVENT_FINGER_MOTION, 4, cx + 200, cy - 60);
        CHECK(Pad().thumbRX != 0);
        Finger(SDL_EVENT_FINGER_UP, 4, cx + 200, cy - 60);
        s = Pad();
        CHECK(s.thumbRX == 0 && s.thumbRY == 0);

        // Where the right stick would be: free area in swipe mode.
        Pos rs = Right(0.62f, 0.80f);
        Finger(SDL_EVENT_FINGER_DOWN, 6, rs.x, rs.y);
        g_nowNs += 16'000'000;
        Finger(SDL_EVENT_FINGER_MOTION, 6, rs.x + 50, rs.y);
        CHECK(Pad().thumbRX > 0);
        Finger(SDL_EVENT_FINGER_UP, 6, rs.x, rs.y);
    }

    // Background/focus events drop every held finger.
    {
        Pos a = Right(0.26f, 0.82f);
        Finger(SDL_EVENT_FINGER_DOWN, 1, a.x, a.y);
        CHECK(Pad().analogButtons[0] == 0xFF);
        Simple(SDL_EVENT_WILL_ENTER_BACKGROUND);
        CHECK(NeutralExceptNothing(Pad()));
        // The stale "up" that may or may not follow is harmless.
        Finger(SDL_EVENT_FINGER_UP, 1, a.x, a.y);
        Finger(SDL_EVENT_FINGER_MOTION, 1, a.x, a.y);
        CHECK(NeutralExceptNothing(Pad()));
    }

    // More than 10 fingers: extra ones are ignored, nothing overflows.
    {
        for (int i = 0; i < 16; i++) Finger(SDL_EVENT_FINGER_DOWN, 100 + i, 50.0f + i, g_displayH * 0.4f);
        Frame();
        for (int i = 0; i < 16; i++) Finger(SDL_EVENT_FINGER_UP, 100 + i, 50.0f + i, g_displayH * 0.4f);
        CHECK(NeutralExceptNothing(Pad()));
    }

    // 2. Right-stick camera mode.
    TouchControls::Configure(ETouchControls::On, ETouchCamera::Stick, layout, false);
    Frame();
    {
        Pos rs = Right(0.62f, 0.80f);
        const float radius = 0.135f * 0.85f * g_displayH;
        Finger(SDL_EVENT_FINGER_DOWN, 1, rs.x, rs.y);
        Finger(SDL_EVENT_FINGER_MOTION, 1, rs.x, rs.y - radius * 2);
        auto s = Pad();
        CHECK(s.thumbRX == 0 && s.thumbRY == 32767 && s.thumbLX == 0);
        Finger(SDL_EVENT_FINGER_UP, 1, rs.x, rs.y);
        // The free area does nothing in this mode.
        Finger(SDL_EVENT_FINGER_DOWN, 2, g_displayW * 0.5f, g_displayH * 0.3f);
        g_nowNs += 16'000'000;
        Finger(SDL_EVENT_FINGER_MOTION, 2, g_displayW * 0.5f + 80, g_displayH * 0.3f);
        CHECK(NeutralExceptNothing(Pad()));
        Finger(SDL_EVENT_FINGER_UP, 2, 0, 0);
    }

    // 3. Policy Off.
    TouchControls::Configure(ETouchControls::Off, ETouchCamera::Swipe, layout, false);
    Frame();
    {
        bool active = true;
        Pos a = Right(0.26f, 0.82f);
        Finger(SDL_EVENT_FINGER_DOWN, 1, a.x, a.y);
        auto s = Pad(&active);
        CHECK(!active && NeutralExceptNothing(s));
        Finger(SDL_EVENT_FINGER_UP, 1, a.x, a.y);
    }

    // 4. Policy Auto: hidden while a physical gamepad is connected; virtual ones do not count.
    TouchControls::Configure(ETouchControls::Auto, ETouchCamera::Swipe, layout, false);
    Frame();
    {
        bool active = false;
        Pos a = Right(0.26f, 0.82f);
        Finger(SDL_EVENT_FINGER_DOWN, 1, a.x, a.y);
        CHECK(Pad(&active).analogButtons[0] == 0xFF && active);

        g_gamepadCount = 1; g_gamepadVirtual = true;
        Simple(SDL_EVENT_GAMEPAD_ADDED);
        CHECK(Pad(&active).analogButtons[0] == 0xFF && active);

        g_gamepadVirtual = false;
        Simple(SDL_EVENT_GAMEPAD_ADDED);
        auto s = Pad(&active);
        CHECK(!active && NeutralExceptNothing(s));
        Frame();

        // Touches while hidden are not tracked, so nothing is stuck when it comes back.
        Finger(SDL_EVENT_FINGER_DOWN, 2, a.x, a.y);
        g_gamepadCount = 0;
        Simple(SDL_EVENT_GAMEPAD_REMOVED);
        s = Pad(&active);
        CHECK(active && NeutralExceptNothing(s));
        Finger(SDL_EVENT_FINGER_UP, 1, a.x, a.y);
        Finger(SDL_EVENT_FINGER_UP, 2, a.x, a.y);
    }

    // 5. Letterboxed display: a 4:3 image centred in a 20:9 window. Touch positions are
    //    relative to the window, the controls to the image.
    TouchControls::Configure(ETouchControls::On, ETouchCamera::Swipe, layout, false);
    g_surfaceW = 2400.0f; g_surfaceH = 1080.0f; g_displayW = 1440.0f; g_displayH = 1080.0f;
    Frame();
    {
        Pos a = Right(0.26f, 0.82f); // uses the narrow-screen horizontal unit (4:3 < 16:10)
        Finger(SDL_EVENT_FINGER_DOWN, 1, a.x, a.y);
        auto s = Pad();
        CHECK(s.analogButtons[0] == 0xFF);
        Finger(SDL_EVENT_FINGER_UP, 1, a.x, a.y);

        // The same window-relative spot WITHOUT the letterbox correction would be 480 px off.
        Finger(SDL_EVENT_FINGER_DOWN, 1, a.x + 480.0f, a.y);
        s = Pad();
        CHECK(s.analogButtons[0] == 0);
        Finger(SDL_EVENT_FINGER_UP, 1, a.x + 480.0f, a.y);

        // A touch on the black bar (left of the image) hits nothing but may swipe the camera.
        Finger(SDL_EVENT_FINGER_DOWN, 2, -300.0f, 500.0f);
        CHECK(NeutralExceptNothing(Pad()));
        Finger(SDL_EVENT_FINGER_UP, 2, -300.0f, 500.0f);

        Pos ls = Left(0.26f, 0.70f);
        Finger(SDL_EVENT_FINGER_DOWN, 3, ls.x, ls.y);
        Finger(SDL_EVENT_FINGER_MOTION, 3, ls.x - 2000.0f, ls.y);
        s = Pad();
        CHECK(s.thumbLX == -32767 && s.thumbLY == 0);
        Finger(SDL_EVENT_FINGER_UP, 3, ls.x, ls.y);
    }
    // Surface smaller than the display (bogus caller): treated as "no letterbox".
    g_surfaceW = 0.0f; g_surfaceH = 0.0f; g_displayW = 1920.0f; g_displayH = 1080.0f;
    Frame();
    {
        SDL_Event event{};
        event.type = SDL_EVENT_FINGER_DOWN;
        event.tfinger.fingerID = 9;
        Pos a = Right(0.26f, 0.82f);
        event.tfinger.x = a.x / 1920.0f; event.tfinger.y = a.y / 1080.0f;
        TouchControls::OnEvent(event);
        CHECK(Pad().analogButtons[0] == 0xFF);
        event.type = SDL_EVENT_FINGER_UP;
        TouchControls::OnEvent(event);
    }
    g_surfaceW = 1920.0f; g_surfaceH = 1080.0f;

    // 6. Layout editor started from the launcher: move A, enlarge it, save, reload.
    CHECK(!fs::exists(layout));
    TouchControls::Configure(ETouchControls::Off, ETouchCamera::Swipe, layout, true);
    CHECK(TouchControls::IsEditing());
    Frame(); // lays out the toolbar
    {
        bool active = true;
        Pad(&active);
        CHECK(!active); // the pad is neutral while editing, even though the overlay is shown

        Pos a = Right(0.26f, 0.82f);
        Finger(SDL_EVENT_FINGER_DOWN, 1, a.x + 10, a.y + 5);     // grab off-centre
        Finger(SDL_EVENT_FINGER_MOTION, 1, 610.0f, 405.0f);       // centre lands at 600,400
        Finger(SDL_EVENT_FINGER_UP, 1, 610.0f, 405.0f);
        Frame();

        // Toolbar geometry mirrored from DrawEditor().
        const float bh = g_displayH * 0.11f, gap = g_displayH * 0.02f;
        const float total = bh + bh + bh * 2 + bh * 2 + gap * 3;
        const float x0 = (g_displayW - total) * 0.5f, y0 = g_displayH * 0.36f + bh * 0.5f;
        const float smaller = x0 + bh * 0.5f, bigger = x0 + bh + gap + bh * 0.5f;
        const float reset = x0 + bh * 2 + gap * 2 + bh, done = x0 + bh * 4 + gap * 3 + bh;
        (void)smaller; (void)reset;

        for (int i = 0; i < 3; i++) { Finger(SDL_EVENT_FINGER_DOWN, 2, bigger, y0); Finger(SDL_EVENT_FINGER_UP, 2, bigger, y0); }
        Frame();
        CHECK(!TouchControls::ConsumeExitRequest());
        Finger(SDL_EVENT_FINGER_DOWN, 2, done, y0);
        Finger(SDL_EVENT_FINGER_UP, 2, done, y0);
        CHECK(!TouchControls::IsEditing());
        CHECK(TouchControls::ConsumeExitRequest());
        CHECK(!TouchControls::ConsumeExitRequest());
        CHECK(fs::exists(layout));
        CHECK(!fs::exists(fs::path(layout.string() + ".tmp")));

        std::ifstream file(layout);
        std::string line; bool found = false; int lines = 0;
        while (std::getline(file, line))
        {
            lines++;
            if (line.rfind("a ", 0) == 0)
            {
                found = true;
                char edge = 0; float ox = 0, y = 0, scale = 0;
                CHECK(sscanf(line.c_str(), "a %c %f %f %f", &edge, &ox, &y, &scale) == 4);
                CHECK(edge == 'L');
                CHECK(std::abs(ox - 600.0f / 1080.0f) < 0.001f);
                CHECK(std::abs(y - 400.0f / 1080.0f) < 0.001f);
                CHECK(std::abs(scale - 1.3f) < 0.001f);
            }
        }
        CHECK(found && lines == 16);
    }

    // Reload: A is now at 600,400 with a 1.3x radius; its old spot is free.
    TouchControls::Configure(ETouchControls::On, ETouchCamera::Stick, layout, false);
    Frame();
    {
        Finger(SDL_EVENT_FINGER_DOWN, 1, 600.0f, 400.0f);
        CHECK(Pad().analogButtons[0] == 0xFF);
        Finger(SDL_EVENT_FINGER_UP, 1, 600.0f, 400.0f);
        const float reach = 0.068f * 1.3f * 1080.0f * 1.25f;
        Finger(SDL_EVENT_FINGER_DOWN, 1, 600.0f + reach - 2, 400.0f);
        CHECK(Pad().analogButtons[0] == 0xFF);
        Finger(SDL_EVENT_FINGER_UP, 1, 0, 0);
        Finger(SDL_EVENT_FINGER_DOWN, 1, 600.0f + reach + 3, 400.0f);
        CHECK(Pad().analogButtons[0] == 0);
        Finger(SDL_EVENT_FINGER_UP, 1, 0, 0);
        Pos old = Right(0.26f, 0.82f);
        Finger(SDL_EVENT_FINGER_DOWN, 1, old.x, old.y);
        CHECK(NeutralExceptNothing(Pad()));
        Finger(SDL_EVENT_FINGER_UP, 1, 0, 0);
    }

    // 7. Hostile layout file: garbage, NaN, huge values, unknown ids. Must not crash, and
    //    the values are clamped.
    {
        std::ofstream file(layout, std::ios::trunc);
        file << "a R nan 0.5 1\n" << "b X 0.1 0.1 1\n" << "x R 1e30 -5 99\n" << "nosuch L 0.1 0.1 1\n"
             << "y\n" << "\n" << std::string(5000, 'z') << "\n" << "lt L 0.5 0.5 inf\n" << "rt R 0.2 0.2 0.01\n";
    }
    TouchControls::Configure(ETouchControls::On, ETouchCamera::Swipe, layout, false);
    Frame();
    {
        // "a" kept its default (the NaN line is rejected).
        Pos a = Right(0.26f, 0.82f);
        Finger(SDL_EVENT_FINGER_DOWN, 1, a.x, a.y);
        CHECK(Pad().analogButtons[0] == 0xFF);
        Finger(SDL_EVENT_FINGER_UP, 1, 0, 0);
        // "rt" moved to R 0.2 0.2 with the minimum scale (0.5).
        Pos rt = Right(0.2f, 0.2f);
        Finger(SDL_EVENT_FINGER_DOWN, 1, rt.x, rt.y);
        CHECK(Pad().analogButtons[7] == 0xFF);
        Finger(SDL_EVENT_FINGER_UP, 1, 0, 0);
        // "lt" kept its default (inf rejected).
        Pos lt = Left(0.12f, 0.14f);
        Finger(SDL_EVENT_FINGER_DOWN, 1, lt.x, lt.y);
        CHECK(Pad().analogButtons[6] == 0xFF);
        Finger(SDL_EVENT_FINGER_UP, 1, 0, 0);
    }

    // 8. Editor Reset restores the defaults; editing with a tiny/odd display does not crash.
    TouchControls::Configure(ETouchControls::On, ETouchCamera::Stick, layout, true);
    Frame();
    {
        const float bh = g_displayH * 0.11f, gap = g_displayH * 0.02f;
        const float total = bh + bh + bh * 2 + bh * 2 + gap * 3;
        const float x0 = (g_displayW - total) * 0.5f, y0 = g_displayH * 0.36f + bh * 0.5f;
        const float reset = x0 + bh * 2 + gap * 2 + bh, done = x0 + bh * 4 + gap * 3 + bh;
        Finger(SDL_EVENT_FINGER_DOWN, 1, reset, y0);
        Finger(SDL_EVENT_FINGER_UP, 1, reset, y0);
        // Drag a control far outside the screen: clamped inside.
        Pos b = Right(0.12f, 0.66f);
        Finger(SDL_EVENT_FINGER_DOWN, 1, b.x, b.y);
        Finger(SDL_EVENT_FINGER_MOTION, 1, 99999.0f, -99999.0f);
        Finger(SDL_EVENT_FINGER_UP, 1, 0, 0);
        Frame();
        Finger(SDL_EVENT_FINGER_DOWN, 1, done, y0);
        Finger(SDL_EVENT_FINGER_UP, 1, done, y0);
        CHECK(!TouchControls::IsEditing());
        CHECK(TouchControls::ConsumeExitRequest());
    }
    TouchControls::Configure(ETouchControls::On, ETouchCamera::Swipe, layout, false);
    Frame();
    {
        Pos rt = Right(0.12f, 0.14f); // back at its default after Reset
        Finger(SDL_EVENT_FINGER_DOWN, 1, rt.x, rt.y);
        CHECK(Pad().analogButtons[7] == 0xFF);
        Finger(SDL_EVENT_FINGER_UP, 1, 0, 0);
        // B was clamped into the top-right corner.
        Finger(SDL_EVENT_FINGER_DOWN, 1, g_displayW - 40.0f, 40.0f);
        CHECK(Pad().analogButtons[1] == 0xFF);
        Finger(SDL_EVENT_FINGER_UP, 1, 0, 0);
    }
    for (auto size : { Pos{ 1.0f, 1.0f }, Pos{ 320.0f, 4000.0f }, Pos{ 8000.0f, 200.0f }, Pos{ 0.0f, 0.0f } })
    {
        g_displayW = g_surfaceW = size.x; g_displayH = g_surfaceH = size.y;
        TouchControls::Configure(ETouchControls::On, ETouchCamera::Stick, layout, true);
        if (size.x >= 1.0f) Frame(); else TouchControls::Draw(0.0f, 0.0f);
        for (int i = 0; i < 12; i++)
        {
            Finger(SDL_EVENT_FINGER_DOWN, i, size.x * 0.1f * i, size.y * 0.09f * i);
            Finger(SDL_EVENT_FINGER_MOTION, i, size.x * 0.5f, size.y * 0.5f);
        }
        if (size.x >= 1.0f) Frame();
        for (int i = 0; i < 12; i++) Finger(SDL_EVENT_FINGER_UP, i, 0, 0);
        Pad();
    }

    // 9. Unwritable layout location: saving fails silently.
    {
        g_displayW = g_surfaceW = 1920.0f; g_displayH = g_surfaceH = 1080.0f;
        const fs::path blocker = dir / "blocker";
        { std::ofstream f(blocker); f << "x"; }
        TouchControls::Configure(ETouchControls::On, ETouchCamera::Swipe, blocker / "sub" / "layout.cfg", true);
        Frame();
        const float bh = g_displayH * 0.11f, gap = g_displayH * 0.02f;
        const float total = bh + bh + bh * 2 + bh * 2 + gap * 3;
        const float x0 = (g_displayW - total) * 0.5f, y0 = g_displayH * 0.36f + bh * 0.5f;
        const float done = x0 + bh * 4 + gap * 3 + bh;
        Finger(SDL_EVENT_FINGER_DOWN, 1, done, y0);
        CHECK(!TouchControls::IsEditing());
        CHECK(TouchControls::ConsumeExitRequest());
    }

    ImGui::DestroyContext();
    fs::remove_all(dir);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
