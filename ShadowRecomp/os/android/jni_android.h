#pragma once

#include <string>

// Calls into GameActivity (android-apk/.../GameActivity.java). All of them may be used
// from any native thread once SDL has been initialized by SDLActivity, and they degrade to
// harmless fallbacks when the Java side is unavailable.
namespace os::android
{
    // Looks up an Android string resource by name, substituting `detail` for %1$s. Used
    // for messages the game has no locale entry for, in the language of the device.
    // Returns `fallback` (with `detail` appended on its own line, if any) when the resource
    // or the activity is unavailable.
    std::string LocaliseMessage(const char* resourceName, const char* fallback, const std::string& detail = {});

    // Shows or updates the native installer's progress dialog. `ratio` is 0..1.
    void SetInstallProgress(const char* fileName, float ratio);

    // Dismisses the progress dialog, if it is showing.
    void FinishInstallProgress();
}
