#include "paths.h"
#include <os/process.h>

std::filesystem::path g_executableRoot = os::process::GetExecutablePath().remove_filename();
std::filesystem::path g_userPath = BuildUserPath();

bool CheckPortable()
{
    return std::filesystem::exists(g_executableRoot / "portable.txt");
}

std::filesystem::path BuildUserPath()
{
#ifndef __ANDROID__
    // Not on Android: the launcher edits config.toml at one fixed location (see below).
    if (CheckPortable())
        return g_executableRoot;
#endif

    std::filesystem::path userPath;

#if defined(_WIN32)
    PWSTR knownPath = NULL;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &knownPath) == S_OK)
        userPath = std::filesystem::path{ knownPath } / USER_DIRECTORY;

    CoTaskMemFree(knownPath);
#elif defined(__ANDROID__)
    // An app has no home directory. Settings and saves live next to the game files, in
    // the storage root chosen in the launcher (which patches config.toml at this path).
    if (os::android::HasDataRoot())
        userPath = os::android::GetDataRoot() / ".config" / USER_DIRECTORY;
#elif defined(__linux__)
    const char* homeDir = getenv("HOME");
    if (homeDir == nullptr)
    {
        homeDir = getpwuid(getuid())->pw_dir;
    }

    if (homeDir != nullptr)
    {
        // Prefer to store in the .config directory if it exists. Use the home directory otherwise.
        std::filesystem::path homePath = homeDir;
        std::filesystem::path configPath = homePath / ".config";
        if (std::filesystem::exists(configPath))
            userPath = configPath / USER_DIRECTORY;
        else
            userPath = homePath / ("." USER_DIRECTORY);
    }
#else
    static_assert(false, "GetUserPath() not implemented for this platform.");
#endif

    return userPath;
}

const std::filesystem::path& GetUserPath()
{
    return g_userPath;
}
