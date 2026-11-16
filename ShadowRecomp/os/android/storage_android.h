#pragma once

#include <filesystem>

// Storage layout of the Android build.
//
// GameActivity publishes every path through environment variables before libmain.so is
// loaded (see GameActivity.publishEnvironment), so all of these are valid from the very
// first static initializer and never need JNI. They return an empty path only when the
// library is loaded by something other than GameActivity.
namespace os::android
{
    // Root for game files, mods, config and saves, as chosen in the launcher
    // (AppStorage.activeGameRoot). Fixed for the lifetime of the process.
    const std::filesystem::path& GetDataRoot();

    // True when the launcher's storage choice resolved to a usable directory.
    bool HasDataRoot();

    // App-private internal files directory (/data/user/0/<package>/files).
    const std::filesystem::path& GetInternalFilesDir();

    // App-specific external files directory (/storage/emulated/0/Android/data/<package>/files).
    // Reachable from a PC over USB without root; logs and driver imports live here.
    // Falls back to the internal files directory when external storage is unavailable.
    const std::filesystem::path& GetExternalFilesDir();

    // App-specific media directory (/storage/emulated/0/Android/media/<package>), which
    // on-device file managers can browse on Android 11+. Empty if unavailable.
    const std::filesystem::path& GetExternalMediaDir();

    // Directory the APK's native libraries were extracted to; libadrenotools loads its
    // hook libraries from here.
    const std::filesystem::path& GetNativeLibraryDir();

    // versionName of the installed APK, for the log header.
    const char* GetAppVersion();
}
