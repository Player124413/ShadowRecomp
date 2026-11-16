#pragma once

#include <filesystem>

// First-launch game installation on Android.
//
// The desktop builds install the game with an in-game wizard that asks for the disc image
// through a native file dialog. Android has no such dialog for raw paths, so the launcher
// does the picking instead: it copies the user's Xbox disc image, extracted disc folder or
// ZIP into <storage root>/to_install ("staging"). On the next launch this module finds the
// staged source and runs the very same installer the wizard uses, which verifies every
// file against the known retail hashes while copying it into <storage root>/game.
namespace os::android
{
    enum class StagedInstallResult
    {
        NothingStaged, // No staged source; the caller decides based on the existing install.
        Installed,     // The staged source was installed and the staging area removed.
        Failed         // The user was told why; the staging area was cleaned up as needed.
    };

    // Installs a staged game source into `gameDirectory`, showing progress through
    // GameActivity. Must be called before the renderer starts.
    StagedInstallResult InstallStagedGame(const std::filesystem::path& dataRoot, const std::filesystem::path& gameDirectory);

    // Tells the user that no game is installed and how to import one, in a message box.
    void ShowGameMissingMessage();
}
