#pragma once

// Android-only settings shared by the config system (user/config.h), the platform layer
// (os/android) and the touch overlay. Kept free of other project headers on purpose.
//
// The enumerator NAMES are part of the config.toml format and are also written by the
// launcher (android-apk/.../LauncherActivity.java); keep both sides in sync.

#include <cstdint>

// Which Vulkan driver the game loads.
enum class EAndroidVulkanDriver : uint32_t
{
    Auto,             // Bundled Turnip on supported Adreno GPUs, the system driver elsewhere.
    System,           // Always the driver that ships with the device.
    Bundled,          // The universal Turnip build shipped in the APK.
    Vauzi710,         // Turnip build for Adreno 710/720/722.
    ExperimentalA725, // Experimental Turnip build for Adreno 725.
    Adreno8xx,        // Turnip build for Adreno 8xx (Snapdragon 8 Elite).
    Imported          // A driver the user imported through the launcher. Keep last:
                      // values read back from disk are range-checked against it.
};

// Turnip render pass strategy (TU_DEBUG). Ignored by non-Turnip drivers.
enum class EAndroidRenderMode : uint32_t
{
    Auto,
    GMEM,
    Sysmem
};

// Audio device buffer size preset.
enum class EAndroidAudioPreset : uint32_t
{
    Default,
    Compatibility, // Larger buffer: trades latency for robustness against crackling.
    LowLatency     // Smaller buffer.
};

// When the on-screen gamepad is shown.
enum class ETouchControls : uint32_t
{
    Auto, // Hidden while a physical controller is connected.
    On,
    Off
};

// How the camera is controlled by touch.
enum class ETouchCamera : uint32_t
{
    Swipe, // Drag anywhere on the free area of the screen.
    Stick, // A second on-screen stick.
    Off
};
