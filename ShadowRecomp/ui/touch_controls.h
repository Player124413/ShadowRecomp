#pragma once

#include <os/android/android_settings.h>

#include <cstdint>
#include <filesystem>

union SDL_Event;

// On-screen gamepad for touch devices.
//
// The overlay turns SDL finger events into the state of an original Xbox controller,
// which input::GetState() merges into player 1. It is drawn with ImGui on top of the
// game and can be rearranged in a built-in layout editor.
//
// Threading: finger events arrive on whichever thread SDL dispatches them from (the Java
// UI thread on Android), the pad state is polled by game threads and Draw() runs on the
// thread that owns ImGui. Every entry point may be called from any thread.
namespace TouchControls
{
    // State of the virtual controller, in the conventions of XINPUT_GAMEPAD_OG.
    struct PadState
    {
        uint16_t buttons = 0;          // XINPUT_GAMEPAD_* bits: D-pad, Start, Back, thumb clicks.
        uint8_t analogButtons[8] = {}; // A, B, X, Y, Black, White, left trigger, right trigger.
        int16_t thumbLX = 0;
        int16_t thumbLY = 0;
        int16_t thumbRX = 0;
        int16_t thumbRY = 0;
    };

    // Applies the user's settings and loads the saved layout.
    //   layoutFile  - where the layout is stored; created on the first save.
    //   startEditor - open the layout editor right away (requested from the launcher).
    void Configure(ETouchControls policy, ETouchCamera camera, const std::filesystem::path& layoutFile, bool startEditor);

    // Feeds an SDL event to the overlay. Call for every event (from the SDL event watch).
    void OnEvent(const SDL_Event& event);

    // Fills `state` with the virtual controller and returns true while the overlay is the
    // active input source. Returns false (and leaves `state` neutral) when it is hidden or
    // the layout editor is open.
    bool GetPadState(PadState& state);

    // Draws the overlay; call once per ImGui frame. The overlay fills the ImGui display,
    // which may be smaller than the window when a fixed aspect ratio letterboxes the game;
    // pass the size of the whole window surface in pixels so touches still line up.
    void Draw(float surfaceWidth, float surfaceHeight);

    // True while the layout editor is open. The game should ignore the pad meanwhile.
    bool IsEditing();

    // True once, after the user leaves an editor session that the launcher started. The
    // app is expected to exit so the user lands back in the launcher.
    bool ConsumeExitRequest();
}
