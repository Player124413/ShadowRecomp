#include <os/android/install_android.h>

#include <install/installer.h>
#include <os/android/jni_android.h>
#include <os/android/logger_android.h>
#include <os/logger.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>

namespace os::android
{
    static constexpr const char* MessageBoxTitle = "Shadow Recompiled";

    // How deep a disc image or default.xbe may sit inside the staging area: one level for
    // the launcher's import batch, plus the wrapper folders a ZIP typically adds.
    static constexpr int MaxSearchDepth = 6;

    static std::string ToLower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

    static std::string FileNameLower(const std::filesystem::path& path)
    {
        return ToLower(path.filename().string());
    }

    // The launcher writes every import as "<name>.part" and renames it once complete, so
    // anything still carrying that suffix is an interrupted copy and must be ignored.
    static bool IsUnpublished(const std::filesystem::path& path)
    {
        return ToLower(path.extension().string()) == ".part";
    }

    struct StagedSource
    {
        std::filesystem::path path;       // Disc image, or the directory holding default.xbe.
        std::filesystem::path executable; // That default.xbe as it is spelled on disk, if any.
        bool found = false;
        bool anyPublishedEntry = false;
    };

    // Depth-first search for the first installable source. An extracted disc (a directory
    // with default.xbe) wins over a disc image at the same level because it needs no parsing.
    static void FindSource(const std::filesystem::path& directory, int depth, StagedSource& result)
    {
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        std::vector<std::filesystem::path> directories;

        for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::filesystem::path& entry = it->path();
            if (IsUnpublished(entry))
                continue;

            result.anyPublishedEntry = true;

            std::error_code typeEc;
            if (it->is_directory(typeEc))
                directories.push_back(entry);
            else if (it->is_regular_file(typeEc))
                files.push_back(entry);
        }

        // Directory iteration order is unspecified; sort so the choice is deterministic.
        std::sort(files.begin(), files.end());
        std::sort(directories.begin(), directories.end());

        for (const auto& file : files)
        {
            if (FileNameLower(file) == "default.xbe")
            {
                result.path = directory;
                result.executable = file;
                result.found = true;
                return;
            }
        }

        for (const auto& file : files)
        {
            if (ToLower(file.extension().string()) == ".iso")
            {
                result.path = file;
                result.found = true;
                return;
            }
        }

        if (depth >= MaxSearchDepth)
            return;

        for (const auto& child : directories)
        {
            FindSource(child, depth + 1, result);
            if (result.found)
                return;
        }
    }

    static void RemoveStaging(const std::filesystem::path& staging)
    {
        std::error_code ec;
        std::filesystem::remove_all(staging, ec);
        if (ec)
            LOGFN_WARNING("Failed to remove the staging directory {}: {}", staging.string(), ec.message());
    }

    static void ShowFailure(const char* resourceName, const char* fallback, const std::string& detail)
    {
        const std::string message = LocaliseMessage(resourceName, fallback, detail);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, MessageBoxTitle, message.c_str(), nullptr);
    }

    StagedInstallResult InstallStagedGame(const std::filesystem::path& dataRoot, const std::filesystem::path& gameDirectory)
    {
        const std::filesystem::path staging = dataRoot / "to_install";

        std::error_code ec;
        if (!std::filesystem::is_directory(staging, ec))
            return StagedInstallResult::NothingStaged;

        StagedSource source;
        FindSource(staging, 0, source);

        if (!source.found)
        {
            // Only interrupted copies (or nothing at all) are left: not an error, and an
            // already installed game must still start.
            if (!source.anyPublishedEntry)
                return StagedInstallResult::NothingStaged;

            LOGN_ERROR("The staging directory contains no Xbox disc image and no folder with default.xbe.");
            RemoveStaging(staging);
            ShowFailure("installer_source_missing",
                "The staged files contain no Xbox disc image (.iso) and no folder with default.xbe. Import the game again from the launcher.", {});
            return StagedInstallResult::Failed;
        }

        // The installer reads an extracted disc without regard to case, except that it
        // first looks for the executable under this exact name. App storage is
        // case-sensitive, so a rip that spells it DEFAULT.XBE would be turned down.
        if (!source.executable.empty() && source.executable.filename() != "default.xbe")
        {
            std::error_code renameEc;
            std::filesystem::rename(source.executable, source.path / "default.xbe", renameEc);
            if (renameEc)
                LOGFN_WARNING("Failed to rename {}: {}", source.executable.string(), renameEc.message());
        }

        LOGFN("Installing the staged game source: {}", source.path.string());
        SetLogLocation("installing the game");
        SetInstallProgress("", 0.0f);

        std::string errorMessage;

        // Checked up front: running out of space half-way would only be reported as a
        // generic write failure, and here the staged source is worth keeping.
        Installer::Sources parsedSources;
        if (Installer::ParseSources(source.path, parsedSources, errorMessage))
        {
            std::filesystem::path spacePath = gameDirectory;
            std::error_code spaceEc;
            while (spacePath.has_parent_path() && spacePath != spacePath.parent_path() && !std::filesystem::exists(spacePath, spaceEc))
                spacePath = spacePath.parent_path();

            const std::filesystem::space_info space = std::filesystem::space(spacePath, spaceEc);
            if (!spaceEc && space.available < parsedSources.totalSize)
            {
                constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
                const std::string detail = fmt::format("Not enough free space: {:.2f} GiB required, {:.2f} GiB available.",
                    double(parsedSources.totalSize) / GiB, double(space.available) / GiB);

                LOGFN_ERROR("Staged installation aborted. {}", detail);
                parsedSources = {};
                FinishInstallProgress();
                ShowFailure("installer_failed", "Could not install the game files:", detail);
                return StagedInstallResult::Failed;
            }
        }
        else
        {
            LOGFN_ERROR("The staged game source is not usable: {}", errorMessage);
            parsedSources = {};
            FinishInstallProgress();
            RemoveStaging(staging);
            ShowFailure("installer_failed", "Could not install the game files:", errorMessage);
            return StagedInstallResult::Failed;
        }

        // Release the disc image before the installer opens it again.
        parsedSources = {};
        errorMessage.clear();

        int lastPermille = -1;
        const bool installed = Installer::Install(source.path, gameDirectory, [&](const Installer::Progress& progress)
        {
            // The installer reports every 16 MiB; forward only visible changes to Java.
            const int permille = int(progress.ratio * 1000.0f);
            if (permille != lastPermille)
            {
                lastPermille = permille;
                SetInstallProgress(progress.currentFileName, progress.ratio);
            }
        }, errorMessage);

        FinishInstallProgress();

        // Whatever the outcome, the staged copy has served its purpose: on success it is a
        // multi-gigabyte duplicate, and on failure it would fail the same way every launch.
        RemoveStaging(staging);

        if (!installed)
        {
            LOGFN_ERROR("Staged game installation failed: {}", errorMessage);
            ShowFailure("installer_failed", "Could not install the game files:", errorMessage);
            return StagedInstallResult::Failed;
        }

        LOGN("Installed the staged game source successfully.");
        return StagedInstallResult::Installed;
    }

    void ShowGameMissingMessage()
    {
        const std::string message = LocaliseMessage("installer_game_missing",
            "Game files not found. Use Install game in the launcher to import your Xbox disc image (.iso), a ZIP archive or an extracted disc folder.");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, MessageBoxTitle, message.c_str(), nullptr);
    }
}
