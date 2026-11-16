#include "touch_controls.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

namespace
{
    // XINPUT_GAMEPAD_* digital bits (see input/input.h).
    constexpr uint16_t PAD_DPAD_UP = 0x0001;
    constexpr uint16_t PAD_DPAD_DOWN = 0x0002;
    constexpr uint16_t PAD_DPAD_LEFT = 0x0004;
    constexpr uint16_t PAD_DPAD_RIGHT = 0x0008;
    constexpr uint16_t PAD_START = 0x0010;
    constexpr uint16_t PAD_BACK = 0x0020;

    // Indices into PadState::analogButtons (bAnalogBtnE in input/input.h).
    constexpr int ANALOG_A = 0;
    constexpr int ANALOG_B = 1;
    constexpr int ANALOG_X = 2;
    constexpr int ANALOG_Y = 3;
    constexpr int ANALOG_BLACK = 4;
    constexpr int ANALOG_WHITE = 5;
    constexpr int ANALOG_LT = 6;
    constexpr int ANALOG_RT = 7;

    enum class EControlKind
    {
        DigitalButton, // Sets a bit in PadState::buttons.
        AnalogButton,  // Sets an entry of PadState::analogButtons.
        LeftStick,
        RightStick
    };

    enum EControl
    {
        CONTROL_LEFT_STICK,
        CONTROL_RIGHT_STICK,
        CONTROL_A,
        CONTROL_B,
        CONTROL_X,
        CONTROL_Y,
        CONTROL_BLACK,
        CONTROL_WHITE,
        CONTROL_LT,
        CONTROL_RT,
        CONTROL_START,
        CONTROL_BACK,
        CONTROL_DPAD_UP,
        CONTROL_DPAD_DOWN,
        CONTROL_DPAD_LEFT,
        CONTROL_DPAD_RIGHT,
        CONTROL_COUNT
    };

    // Where a control sits. Horizontal distances are measured from the nearest screen edge
    // in units of the screen HEIGHT (see HorizontalUnit), so the layout keeps its shape on
    // any aspect ratio instead of stretching with the screen width.
    struct Placement
    {
        bool fromRight; // Anchored to the right edge instead of the left one.
        float offsetX;  // Distance of the centre from that edge, in screen heights.
        float y;        // Centre, as a fraction of the screen height.
        float scale;    // Multiplier applied to the control's base radius.
    };

    struct ControlDef
    {
        const char* id;    // Name in the layout file. Never rename.
        const char* label; // Drawn on the control.
        EControlKind kind;
        int target;        // Button bit or analog index.
        float radius;      // Base radius, in screen heights.
        Placement defaults;
    };

    constexpr float FACE_RADIUS = 0.068f;
    constexpr float SMALL_RADIUS = 0.052f;
    constexpr float DPAD_RADIUS = 0.048f;
    constexpr float STICK_RADIUS = 0.135f;

    constexpr std::array<ControlDef, CONTROL_COUNT> CONTROLS =
    { {
        { "left_stick",  "",      EControlKind::LeftStick,     0,              STICK_RADIUS, { false, 0.26f, 0.70f, 1.0f } },
        { "right_stick", "",      EControlKind::RightStick,    0,              STICK_RADIUS * 0.85f, { true, 0.62f, 0.80f, 1.0f } },
        { "a",           "A",     EControlKind::AnalogButton,  ANALOG_A,       FACE_RADIUS,  { true,  0.26f, 0.82f, 1.0f } },
        { "b",           "B",     EControlKind::AnalogButton,  ANALOG_B,       FACE_RADIUS,  { true,  0.12f, 0.66f, 1.0f } },
        { "x",           "X",     EControlKind::AnalogButton,  ANALOG_X,       FACE_RADIUS,  { true,  0.40f, 0.66f, 1.0f } },
        { "y",           "Y",     EControlKind::AnalogButton,  ANALOG_Y,       FACE_RADIUS,  { true,  0.26f, 0.50f, 1.0f } },
        { "black",       "BK",    EControlKind::AnalogButton,  ANALOG_BLACK,   SMALL_RADIUS, { true,  0.31f, 0.13f, 1.0f } },
        { "white",       "WH",    EControlKind::AnalogButton,  ANALOG_WHITE,   SMALL_RADIUS, { false, 0.31f, 0.13f, 1.0f } },
        { "lt",          "LT",    EControlKind::AnalogButton,  ANALOG_LT,      FACE_RADIUS,  { false, 0.12f, 0.14f, 1.0f } },
        { "rt",          "RT",    EControlKind::AnalogButton,  ANALOG_RT,      FACE_RADIUS,  { true,  0.12f, 0.14f, 1.0f } },
        { "start",       "START", EControlKind::DigitalButton, PAD_START,      SMALL_RADIUS, { true,  0.52f, 0.10f, 1.0f } },
        { "back",        "BACK",  EControlKind::DigitalButton, PAD_BACK,       SMALL_RADIUS, { false, 0.52f, 0.10f, 1.0f } },
        { "dpad_up",     "^",     EControlKind::DigitalButton, PAD_DPAD_UP,    DPAD_RADIUS,  { false, 0.66f, 0.62f, 1.0f } },
        { "dpad_down",   "v",     EControlKind::DigitalButton, PAD_DPAD_DOWN,  DPAD_RADIUS,  { false, 0.66f, 0.84f, 1.0f } },
        { "dpad_left",   "<",     EControlKind::DigitalButton, PAD_DPAD_LEFT,  DPAD_RADIUS,  { false, 0.55f, 0.73f, 1.0f } },
        { "dpad_right",  ">",     EControlKind::DigitalButton, PAD_DPAD_RIGHT, DPAD_RADIUS,  { false, 0.77f, 0.73f, 1.0f } },
    } };

    constexpr float MIN_SCALE = 0.5f;
    constexpr float MAX_SCALE = 2.5f;
    constexpr float SCALE_STEP = 0.1f;

    // Extra reach around a button, as a multiple of its radius. Fingers are not precise.
    constexpr float HIT_SLOP = 1.25f;

    // Camera swipe: finger speed (screen heights per second) that maps to full deflection,
    // and how long the last deflection is held after the finger stops reporting motion.
    constexpr float SWIPE_FULL_SPEED = 1.1f;
    constexpr uint64_t SWIPE_HOLD_NS = 70ull * 1000 * 1000;

    constexpr int MAX_FINGERS = 10;
    constexpr int NO_CONTROL = -1;

    enum class EFingerRole
    {
        None,
        Control, // Holding a button or a stick.
        Camera,  // Swiping the camera.
        Editor   // Dragging a control in the layout editor.
    };

    struct Finger
    {
        bool active = false;
        SDL_FingerID id = 0;
        EFingerRole role = EFingerRole::None;
        int control = NO_CONTROL;
        float x = 0.0f; // Current position in pixels.
        float y = 0.0f;
        float grabX = 0.0f; // Editor: offset from the control's centre when grabbed.
        float grabY = 0.0f;
        uint64_t lastMotionNs = 0;
    };

    // Toolbar of the layout editor, laid out in Draw() and hit-tested in the event handler.
    enum EEditorButton
    {
        EDITOR_SMALLER,
        EDITOR_BIGGER,
        EDITOR_RESET,
        EDITOR_DONE,
        EDITOR_BUTTON_COUNT
    };

    constexpr const char* EDITOR_LABELS[EDITOR_BUTTON_COUNT] = { "-", "+", "Reset", "Save" };

    struct State
    {
        std::mutex mutex;

        ETouchControls policy = ETouchControls::Auto;
        ETouchCamera camera = ETouchCamera::Swipe;
        std::filesystem::path layoutFile;

        std::array<Placement, CONTROL_COUNT> layout{};
        std::array<Finger, MAX_FINGERS> fingers{};

        // Size of the area the overlay is laid out and drawn in (the ImGui display), and of
        // the whole window surface, in pixels; refreshed by Draw(). They differ when a fixed
        // aspect ratio letterboxes the game: the display is then centred in the surface.
        float width = 1920.0f;
        float height = 1080.0f;
        float surfaceWidth = 1920.0f;
        float surfaceHeight = 1080.0f;

        int physicalGamepads = 0;

        // Camera swipe output, already in stick units (-1..1, +Y up).
        float swipeX = 0.0f;
        float swipeY = 0.0f;
        uint64_t swipeNs = 0;

        bool editing = false;
        bool editorFromLauncher = false;
        int selected = NO_CONTROL;
        std::array<ImVec4, EDITOR_BUTTON_COUNT> editorRects{}; // x0, y0, x1, y1
    };

    State g_state;
    std::atomic<bool> g_configured{ false };
    std::atomic<bool> g_exitRequested{ false };

    void ResetLayout(State& state)
    {
        for (int i = 0; i < CONTROL_COUNT; i++)
            state.layout[i] = CONTROLS[i].defaults;
    }

    // The layout is designed for screens at least 16:10 wide. On narrower ones (tablets)
    // the horizontal distances shrink in proportion, so the two halves do not collide.
    float HorizontalUnit(const State& state)
    {
        constexpr float DESIGN_ASPECT = 1.6f;
        const float aspect = state.width / std::max(state.height, 1.0f);
        return state.height * std::min(1.0f, aspect / DESIGN_ASPECT);
    }

    ImVec2 ControlCentre(const State& state, int control)
    {
        const Placement& placement = state.layout[control];
        const float offset = placement.offsetX * HorizontalUnit(state);
        return { placement.fromRight ? state.width - offset : offset, placement.y * state.height };
    }

    float ControlRadius(const State& state, int control)
    {
        return CONTROLS[control].radius * state.layout[control].scale * state.height;
    }

    bool IsControlEnabled(const State& state, int control)
    {
        if (CONTROLS[control].kind == EControlKind::RightStick)
            return state.camera == ETouchCamera::Stick;

        return true;
    }

    bool IsVisibleLocked(const State& state)
    {
        if (state.editing)
            return true;

        switch (state.policy)
        {
        case ETouchControls::On:
            return true;
        case ETouchControls::Off:
            return false;
        case ETouchControls::Auto:
        default:
            return state.physicalGamepads == 0;
        }
    }

    // The control under a point, preferring the one whose centre is closest. Sticks are
    // matched by their full radius, buttons with some slop.
    int HitTest(const State& state, float x, float y)
    {
        int best = NO_CONTROL;
        float bestDistance = 0.0f;

        for (int i = 0; i < CONTROL_COUNT; i++)
        {
            if (!IsControlEnabled(state, i))
                continue;

            const ImVec2 centre = ControlCentre(state, i);
            const float dx = x - centre.x;
            const float dy = y - centre.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const bool isStick = CONTROLS[i].kind == EControlKind::LeftStick || CONTROLS[i].kind == EControlKind::RightStick;
            const float reach = ControlRadius(state, i) * (isStick ? 1.15f : HIT_SLOP);

            if (distance <= reach && (best == NO_CONTROL || distance < bestDistance))
            {
                best = i;
                bestDistance = distance;
            }
        }

        return best;
    }

    Finger* FindFinger(State& state, SDL_FingerID id)
    {
        for (auto& finger : state.fingers)
        {
            if (finger.active && finger.id == id)
                return &finger;
        }

        return nullptr;
    }

    Finger* AllocateFinger(State& state, SDL_FingerID id)
    {
        if (Finger* existing = FindFinger(state, id))
            return existing;

        for (auto& finger : state.fingers)
        {
            if (!finger.active)
            {
                finger = {};
                finger.active = true;
                finger.id = id;
                return &finger;
            }
        }

        return nullptr;
    }

    void ReleaseAllFingers(State& state)
    {
        for (auto& finger : state.fingers)
            finger = {};

        state.swipeX = 0.0f;
        state.swipeY = 0.0f;
    }

    bool IsControlHeld(const State& state, int control)
    {
        for (const auto& finger : state.fingers)
        {
            if (finger.active && finger.role == EFingerRole::Control && finger.control == control)
                return true;
        }

        return false;
    }

    // Deflection of a stick from the finger holding it: x right, y UP, each in -1..1.
    bool StickDeflection(const State& state, int control, float& outX, float& outY)
    {
        for (const auto& finger : state.fingers)
        {
            if (!finger.active || finger.role != EFingerRole::Control || finger.control != control)
                continue;

            const ImVec2 centre = ControlCentre(state, control);
            const float radius = std::max(ControlRadius(state, control), 1.0f);
            float dx = (finger.x - centre.x) / radius;
            float dy = (finger.y - centre.y) / radius;
            const float length = std::sqrt(dx * dx + dy * dy);

            if (length > 1.0f)
            {
                dx /= length;
                dy /= length;
            }

            outX = dx;
            outY = -dy; // Screen Y grows downwards, stick Y grows upwards.
            return true;
        }

        outX = 0.0f;
        outY = 0.0f;
        return false;
    }

    // SDL reports finger positions as fractions of the window, while the overlay lives in
    // the display area, which may be a smaller rectangle centred in it. The result is in
    // display pixels and lies outside of 0..size for touches on the bars around the image.
    float FingerX(const State& state, const SDL_TouchFingerEvent& finger)
    {
        return finger.x * state.surfaceWidth - (state.surfaceWidth - state.width) * 0.5f;
    }

    float FingerY(const State& state, const SDL_TouchFingerEvent& finger)
    {
        return finger.y * state.surfaceHeight - (state.surfaceHeight - state.height) * 0.5f;
    }

    int16_t ToAxis(float value)
    {
        const float clamped = std::clamp(value, -1.0f, 1.0f);
        return int16_t(std::lround(clamped * 32767.0f));
    }

    bool PointInRect(const ImVec4& rect, float x, float y)
    {
        return x >= rect.x && x <= rect.z && y >= rect.y && y <= rect.w;
    }

    // --- Layout file -----------------------------------------------------------------
    // One control per line: <id> <L|R> <offsetX> <y> <scale>. Unknown ids and malformed
    // lines are skipped, so a layout survives controls being added in later versions.

    void LoadLayout(State& state)
    {
        ResetLayout(state);

        if (state.layoutFile.empty())
            return;

        std::ifstream file(state.layoutFile);
        if (!file.is_open())
            return;

        std::string line;
        while (std::getline(file, line))
        {
            std::istringstream stream(line);
            std::string id;
            std::string edge;
            Placement placement{};

            if (!(stream >> id >> edge >> placement.offsetX >> placement.y >> placement.scale))
                continue;

            if (edge != "L" && edge != "R")
                continue;

            if (!std::isfinite(placement.offsetX) || !std::isfinite(placement.y) || !std::isfinite(placement.scale))
                continue;

            placement.fromRight = edge == "R";
            placement.offsetX = std::clamp(placement.offsetX, 0.0f, 4.0f);
            placement.y = std::clamp(placement.y, 0.0f, 1.0f);
            placement.scale = std::clamp(placement.scale, MIN_SCALE, MAX_SCALE);

            for (int i = 0; i < CONTROL_COUNT; i++)
            {
                if (id == CONTROLS[i].id)
                {
                    state.layout[i] = placement;
                    break;
                }
            }
        }
    }

    void SaveLayout(const State& state)
    {
        if (state.layoutFile.empty())
            return;

        std::error_code ec;
        std::filesystem::create_directories(state.layoutFile.parent_path(), ec);

        // Written to a temporary file first so an interrupted save cannot truncate the layout.
        std::filesystem::path temporary = state.layoutFile;
        temporary += ".tmp";

        {
            std::ofstream file(temporary, std::ios::trunc);
            if (!file.is_open())
                return;

            for (int i = 0; i < CONTROL_COUNT; i++)
            {
                const Placement& placement = state.layout[i];
                char line[128];
                snprintf(line, sizeof(line), "%s %s %.4f %.4f %.3f\n", CONTROLS[i].id,
                    placement.fromRight ? "R" : "L", placement.offsetX, placement.y, placement.scale);
                file << line;
            }

            if (!file.good())
                return;
        }

        std::filesystem::rename(temporary, state.layoutFile, ec);
        if (ec)
            std::filesystem::remove(temporary, ec);
    }

    // --- Layout editor ---------------------------------------------------------------

    void MoveControl(State& state, int control, float x, float y)
    {
        const float radius = ControlRadius(state, control);
        x = std::clamp(x, radius * 0.5f, std::max(state.width - radius * 0.5f, radius * 0.5f));
        y = std::clamp(y, radius * 0.5f, std::max(state.height - radius * 0.5f, radius * 0.5f));

        Placement& placement = state.layout[control];
        placement.fromRight = x > state.width * 0.5f;
        placement.offsetX = (placement.fromRight ? state.width - x : x) / std::max(HorizontalUnit(state), 1.0f);
        placement.y = y / state.height;
    }

    void OnEditorButton(State& state, int button)
    {
        switch (button)
        {
        case EDITOR_SMALLER:
        case EDITOR_BIGGER:
            if (state.selected != NO_CONTROL)
            {
                float& scale = state.layout[state.selected].scale;
                scale = std::clamp(scale + (button == EDITOR_BIGGER ? SCALE_STEP : -SCALE_STEP), MIN_SCALE, MAX_SCALE);
            }
            break;

        case EDITOR_RESET:
            ResetLayout(state);
            state.selected = NO_CONTROL;
            break;

        case EDITOR_DONE:
            SaveLayout(state);
            state.editing = false;
            state.selected = NO_CONTROL;
            ReleaseAllFingers(state);
            if (state.editorFromLauncher)
            {
                state.editorFromLauncher = false;
                g_exitRequested = true;
            }
            break;
        }
    }

    void EditorFingerDown(State& state, Finger& finger)
    {
        for (int i = 0; i < EDITOR_BUTTON_COUNT; i++)
        {
            if (PointInRect(state.editorRects[i], finger.x, finger.y))
            {
                OnEditorButton(state, i);
                return;
            }
        }

        const int control = HitTest(state, finger.x, finger.y);
        state.selected = control;
        if (control == NO_CONTROL)
            return;

        const ImVec2 centre = ControlCentre(state, control);
        finger.role = EFingerRole::Editor;
        finger.control = control;
        finger.grabX = finger.x - centre.x;
        finger.grabY = finger.y - centre.y;
    }

    // --- Gameplay input --------------------------------------------------------------

    void GameFingerDown(State& state, Finger& finger, uint64_t timestampNs)
    {
        const int control = HitTest(state, finger.x, finger.y);
        if (control != NO_CONTROL)
        {
            finger.role = EFingerRole::Control;
            finger.control = control;
            return;
        }

        // A touch on the free area swipes the camera. Only one finger does at a time.
        if (state.camera != ETouchCamera::Swipe)
            return;

        for (const auto& other : state.fingers)
        {
            if (other.active && other.role == EFingerRole::Camera)
                return;
        }

        finger.role = EFingerRole::Camera;
        finger.lastMotionNs = timestampNs;
    }

    void CameraFingerMotion(State& state, Finger& finger, float x, float y, uint64_t timestampNs)
    {
        // Clamp the interval: event timestamps can be coalesced or arrive in bursts.
        const uint64_t elapsedNs = timestampNs > finger.lastMotionNs ? timestampNs - finger.lastMotionNs : 0;
        const float seconds = std::clamp(float(double(elapsedNs) / 1e9), 0.004f, 0.05f);

        const float speedX = ((x - finger.x) / state.height) / seconds;
        const float speedY = ((y - finger.y) / state.height) / seconds;
        const float targetX = std::clamp(speedX / SWIPE_FULL_SPEED, -1.0f, 1.0f);
        const float targetY = std::clamp(-speedY / SWIPE_FULL_SPEED, -1.0f, 1.0f);

        // Light smoothing takes the jitter out of per-event speeds.
        state.swipeX = state.swipeX * 0.5f + targetX * 0.5f;
        state.swipeY = state.swipeY * 0.5f + targetY * 0.5f;
        state.swipeNs = timestampNs;
        finger.lastMotionNs = timestampNs;
    }

    void RefreshPhysicalGamepads(State& state)
    {
        int count = 0;
        int physical = 0;

        if (SDL_JoystickID* gamepads = SDL_GetGamepads(&count))
        {
            for (int i = 0; i < count; i++)
            {
                if (!SDL_IsJoystickVirtual(gamepads[i]))
                    physical++;
            }

            SDL_free(gamepads);
        }

        // The overlay just became hidden: let go of everything it was holding.
        if (physical > 0 && state.physicalGamepads == 0 && state.policy == ETouchControls::Auto && !state.editing)
            ReleaseAllFingers(state);

        state.physicalGamepads = physical;
    }

    // --- Drawing ---------------------------------------------------------------------

    void DrawLabel(ImDrawList* drawList, const ImVec2& centre, float radius, const char* label, ImU32 colour)
    {
        if (label == nullptr || label[0] == '\0')
            return;

        ImFont* font = ImGui::GetFont();
        const size_t length = strlen(label);

        // Short labels fill the button; longer ones shrink to stay inside it.
        float fontSize = radius * (length <= 2 ? 0.95f : 0.62f);
        ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
        const float maxWidth = radius * 1.7f;
        if (size.x > maxWidth && size.x > 0.0f)
        {
            fontSize *= maxWidth / size.x;
            size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
        }

        drawList->AddText(font, fontSize, { centre.x - size.x * 0.5f, centre.y - size.y * 0.5f }, colour, label);
    }

    void DrawControl(ImDrawList* drawList, const State& state, int control)
    {
        const ControlDef& def = CONTROLS[control];
        const ImVec2 centre = ControlCentre(state, control);
        const float radius = ControlRadius(state, control);
        const bool held = !state.editing && IsControlHeld(state, control);
        const bool selected = state.editing && state.selected == control;
        const float thickness = std::max(1.5f, state.height * 0.003f);

        const ImU32 fill = held ? IM_COL32(255, 255, 255, 110) : IM_COL32(20, 20, 20, 70);
        const ImU32 outline = selected ? IM_COL32(215, 38, 46, 255) : IM_COL32(255, 255, 255, held ? 230 : 130);
        const ImU32 text = IM_COL32(255, 255, 255, held ? 255 : 190);

        if (def.kind == EControlKind::LeftStick || def.kind == EControlKind::RightStick)
        {
            drawList->AddCircleFilled(centre, radius, IM_COL32(20, 20, 20, 50), 48);
            drawList->AddCircle(centre, radius, outline, 48, thickness);

            float x = 0.0f;
            float y = 0.0f;
            if (!state.editing)
                StickDeflection(state, control, x, y);

            const ImVec2 knob = { centre.x + x * radius * 0.6f, centre.y - y * radius * 0.6f };
            drawList->AddCircleFilled(knob, radius * 0.42f, held ? IM_COL32(255, 255, 255, 150) : IM_COL32(255, 255, 255, 80), 32);
        }
        else
        {
            drawList->AddCircleFilled(centre, radius, fill, 32);
            drawList->AddCircle(centre, radius, outline, 32, thickness);
            DrawLabel(drawList, centre, radius, def.label, text);
        }
    }

    void DrawEditor(ImDrawList* drawList, State& state)
    {
        // Dim the game so the controls stand out while they are being arranged.
        drawList->AddRectFilled({ 0.0f, 0.0f }, { state.width, state.height }, IM_COL32(0, 0, 0, 110));

        const float buttonHeight = state.height * 0.11f;
        const float gap = state.height * 0.02f;
        const float widths[EDITOR_BUTTON_COUNT] = { buttonHeight, buttonHeight, buttonHeight * 2.0f, buttonHeight * 2.0f };

        float total = gap * (EDITOR_BUTTON_COUNT - 1);
        for (float width : widths)
            total += width;

        // Centred vertically: the default layout keeps the middle of the screen free.
        float x = (state.width - total) * 0.5f;
        const float y = state.height * 0.36f;
        ImFont* font = ImGui::GetFont();

        for (int i = 0; i < EDITOR_BUTTON_COUNT; i++)
        {
            const ImVec4 rect = { x, y, x + widths[i], y + buttonHeight };
            state.editorRects[i] = rect;

            const bool needsSelection = i == EDITOR_SMALLER || i == EDITOR_BIGGER;
            const bool enabled = !needsSelection || state.selected != NO_CONTROL;
            const ImU32 fill = i == EDITOR_DONE ? IM_COL32(215, 38, 46, 235) : IM_COL32(31, 35, 42, 235);

            drawList->AddRectFilled({ rect.x, rect.y }, { rect.z, rect.w }, fill, buttonHeight * 0.2f);
            drawList->AddRect({ rect.x, rect.y }, { rect.z, rect.w }, IM_COL32(255, 255, 255, 90), buttonHeight * 0.2f);

            const float fontSize = buttonHeight * 0.45f;
            const ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, EDITOR_LABELS[i]);
            drawList->AddText(font, fontSize, { rect.x + (widths[i] - size.x) * 0.5f, rect.y + (buttonHeight - size.y) * 0.5f },
                IM_COL32(255, 255, 255, enabled ? 255 : 90), EDITOR_LABELS[i]);

            x += widths[i] + gap;
        }
    }
}

void TouchControls::Configure(ETouchControls policy, ETouchCamera camera, const std::filesystem::path& layoutFile, bool startEditor)
{
    std::lock_guard lock(g_state.mutex);

    g_state.policy = policy;
    g_state.camera = camera;
    g_state.layoutFile = layoutFile;
    LoadLayout(g_state);
    ReleaseAllFingers(g_state);

    g_state.editing = startEditor;
    g_state.editorFromLauncher = startEditor;
    g_state.selected = NO_CONTROL;

    g_configured = true;
}

void TouchControls::OnEvent(const SDL_Event& event)
{
    if (!g_configured)
        return;

    switch (event.type)
    {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    {
        std::lock_guard lock(g_state.mutex);
        RefreshPhysicalGamepads(g_state);
        break;
    }

    // Fingers that were down when the app left the foreground never report an "up".
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    {
        std::lock_guard lock(g_state.mutex);
        ReleaseAllFingers(g_state);
        break;
    }

    case SDL_EVENT_FINGER_DOWN:
    {
        std::lock_guard lock(g_state.mutex);
        if (!IsVisibleLocked(g_state))
            break;

        Finger* finger = AllocateFinger(g_state, event.tfinger.fingerID);
        if (finger == nullptr)
            break;

        finger->role = EFingerRole::None;
        finger->control = NO_CONTROL;
        finger->x = FingerX(g_state, event.tfinger);
        finger->y = FingerY(g_state, event.tfinger);

        if (g_state.editing)
            EditorFingerDown(g_state, *finger);
        else
            GameFingerDown(g_state, *finger, event.tfinger.timestamp);

        break;
    }

    case SDL_EVENT_FINGER_MOTION:
    {
        std::lock_guard lock(g_state.mutex);
        Finger* finger = FindFinger(g_state, event.tfinger.fingerID);
        if (finger == nullptr)
            break;

        const float x = FingerX(g_state, event.tfinger);
        const float y = FingerY(g_state, event.tfinger);

        if (finger->role == EFingerRole::Camera)
            CameraFingerMotion(g_state, *finger, x, y, event.tfinger.timestamp);
        else if (finger->role == EFingerRole::Editor && finger->control != NO_CONTROL)
            MoveControl(g_state, finger->control, x - finger->grabX, y - finger->grabY);

        finger->x = x;
        finger->y = y;
        break;
    }

    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
    {
        std::lock_guard lock(g_state.mutex);
        if (Finger* finger = FindFinger(g_state, event.tfinger.fingerID))
        {
            if (finger->role == EFingerRole::Camera)
            {
                g_state.swipeX = 0.0f;
                g_state.swipeY = 0.0f;
            }

            *finger = {};
        }

        break;
    }

    default:
        break;
    }
}

bool TouchControls::GetPadState(PadState& state)
{
    state = {};

    if (!g_configured)
        return false;

    std::lock_guard lock(g_state.mutex);
    if (g_state.editing || !IsVisibleLocked(g_state))
        return false;

    for (int i = 0; i < CONTROL_COUNT; i++)
    {
        const ControlDef& def = CONTROLS[i];
        if (!IsControlEnabled(g_state, i))
            continue;

        switch (def.kind)
        {
        case EControlKind::DigitalButton:
            if (IsControlHeld(g_state, i))
                state.buttons |= uint16_t(def.target);
            break;

        case EControlKind::AnalogButton:
            if (IsControlHeld(g_state, i))
                state.analogButtons[def.target] = 0xFF;
            break;

        case EControlKind::LeftStick:
        {
            float x, y;
            if (StickDeflection(g_state, i, x, y))
            {
                state.thumbLX = ToAxis(x);
                state.thumbLY = ToAxis(y);
            }
            break;
        }

        case EControlKind::RightStick:
        {
            float x, y;
            if (StickDeflection(g_state, i, x, y))
            {
                state.thumbRX = ToAxis(x);
                state.thumbRY = ToAxis(y);
            }
            break;
        }
        }
    }

    if (g_state.camera == ETouchCamera::Swipe)
    {
        // A finger resting on the screen stops sending motion events; without this the
        // camera would keep turning at the last reported speed.
        const uint64_t now = SDL_GetTicksNS();
        if (now > g_state.swipeNs && now - g_state.swipeNs > SWIPE_HOLD_NS)
        {
            g_state.swipeX = 0.0f;
            g_state.swipeY = 0.0f;
        }

        state.thumbRX = ToAxis(g_state.swipeX);
        state.thumbRY = ToAxis(g_state.swipeY);
    }

    return true;
}

void TouchControls::Draw(float surfaceWidth, float surfaceHeight)
{
    if (!g_configured)
        return;

    std::lock_guard lock(g_state.mutex);

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    if (displaySize.x >= 1.0f && displaySize.y >= 1.0f)
    {
        g_state.width = displaySize.x;
        g_state.height = displaySize.y;

        // The display never exceeds the surface; this also covers a caller without one.
        g_state.surfaceWidth = std::max(surfaceWidth, displaySize.x);
        g_state.surfaceHeight = std::max(surfaceHeight, displaySize.y);
    }

    if (!IsVisibleLocked(g_state))
        return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    if (g_state.editing)
        DrawEditor(drawList, g_state);

    for (int i = 0; i < CONTROL_COUNT; i++)
    {
        if (IsControlEnabled(g_state, i))
            DrawControl(drawList, g_state, i);
    }
}

bool TouchControls::IsEditing()
{
    if (!g_configured)
        return false;

    std::lock_guard lock(g_state.mutex);
    return g_state.editing;
}

bool TouchControls::ConsumeExitRequest()
{
    return g_exitRequested.exchange(false);
}
