#pragma once

#include <mod/mod_loader.h>

#ifdef __ANDROID__
#include <os/android/storage_android.h>
#endif

#define USER_DIRECTORY "ShadowRecomp"

#ifndef GAME_INSTALL_DIRECTORY
#define GAME_INSTALL_DIRECTORY "."
#endif

extern std::filesystem::path g_executableRoot;

inline std::filesystem::path GetGamePath()
{
#ifdef __ANDROID__
    // The storage root the user picked in the launcher. GameActivity publishes it before
    // this library is loaded, so it is already valid in static initializers.
    if (os::android::HasDataRoot())
        return os::android::GetDataRoot();
#endif

    return GAME_INSTALL_DIRECTORY;
}

bool CheckPortable();
std::filesystem::path BuildUserPath();
const std::filesystem::path& GetUserPath();

inline std::filesystem::path GetSavePath(bool checkForMods)
{
    if (checkForMods && !ModLoader::s_saveFilePath.empty())
        return ModLoader::s_saveFilePath.parent_path();
    else
        return GetUserPath() / "save";
}

inline std::filesystem::path GetSaveFilePath(bool checkForMods)
{
    if (checkForMods && !ModLoader::s_saveFilePath.empty())
        return ModLoader::s_saveFilePath;
    else
        return GetSavePath(false) / "Save.dat";
}
