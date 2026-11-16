// Test of ShadowRecomp/os/android/install_android.cpp on the build machine. The installer
// itself is scripted (fake/install/installer.h): what is tested is how staged sources are
// found, what is kept or cleaned up afterwards, and what the user is told.
#include <install/installer.h>
#include <os/android/install_android.h>
#include <os/android/jni_android.h>
#include <os/android/logger_android.h>
#include <os/logger.h>
#include <SDL3/SDL.h>

#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;
using os::android::StagedInstallResult;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// --- Scripted collaborators ----------------------------------------------------------
struct Script
{
    bool parseOk = true;
    uint64_t totalSize = 1000;
    bool installOk = true;
    std::string error = "scripted failure";
} g_script;

struct Record
{
    int parseCalls = 0, installCalls = 0, messageBoxes = 0, progressCalls = 0, finishCalls = 0;
    fs::path parsedPath, installedPath, outputDir;
    std::string lastMessage, lastResource, lastDetail, location;
    Uint32 lastFlags = 0;
    bool stagingExistedDuringInstall = false;
    bool executableSpelledRight = false;
    int openSources = 0; // Sources objects alive (the disc image must be closed before Install)
    bool sourceOpenDuringInstall = false;
} g_rec;

static fs::path g_staging;

bool Installer::ParseSources(const fs::path& sourcePath, Sources& sources, std::string& errorMessage)
{
    g_rec.parseCalls++;
    g_rec.parsedPath = sourcePath;
    if (!g_script.parseOk) { errorMessage = g_script.error; return false; }
    sources.game = std::unique_ptr<int>(new int(1));
    sources.totalSize = g_script.totalSize;
    return true;
}

bool Installer::Install(const fs::path& sourcePath, const fs::path& outputDir, const ProgressCallback& callback, std::string& errorMessage)
{
    g_rec.installCalls++;
    g_rec.installedPath = sourcePath;
    g_rec.outputDir = outputDir;
    g_rec.stagingExistedDuringInstall = fs::exists(sourcePath);
    if (fs::is_directory(sourcePath))
    {
        int spellings = 0;
        for (const auto& entry : fs::directory_iterator(sourcePath))
        {
            std::string lower = entry.path().filename().string();
            for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (lower == "default.xbe") spellings++;
        }
        g_rec.executableSpelledRight = spellings == 1 && fs::is_regular_file(sourcePath / "default.xbe");
    }
    Progress p;
    for (int i = 0; i <= 2000; i++) { p.ratio = float(i) / 2000.0f; p.currentFileName = "file.bin"; callback(p); }
    if (!g_script.installOk) { errorMessage = g_script.error; return false; }
    fs::create_directories(outputDir);
    std::ofstream(outputDir / "default.xbe") << "xbe";
    return true;
}

namespace os::logger
{
    void Log(const std::string_view, ELogType, const char*) {}
}

namespace os::android
{
    std::string LocaliseMessage(const char* resourceName, const char* fallback, const std::string& detail)
    {
        g_rec.lastResource = resourceName;
        g_rec.lastDetail = detail;
        return detail.empty() ? std::string(fallback) : std::string(fallback) + "\n" + detail;
    }
    void SetInstallProgress(const char*, float ratio) { g_rec.progressCalls++; CHECK(ratio >= 0.0f && ratio <= 1.0f); }
    void FinishInstallProgress() { g_rec.finishCalls++; }
    void SetLogLocation(const std::string_view location) { g_rec.location = std::string(location); }
}

extern "C" bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char*, const char* message, SDL_Window*)
{
    g_rec.messageBoxes++;
    g_rec.lastFlags = flags;
    g_rec.lastMessage = message;
    return true;
}

// --- Helpers ---------------------------------------------------------------------------
static fs::path g_root;

static void Reset()
{
    fs::remove_all(g_root);
    fs::create_directories(g_root);
    g_script = {};
    g_rec = {};
    g_staging = g_root / "to_install";
}

static void Touch(const fs::path& path, const char* content = "x")
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << content;
}

static StagedInstallResult Run() { return os::android::InstallStagedGame(g_root, g_root / "game"); }

int main()
{
    g_root = fs::temp_directory_path() / "shadow_install_test";

    // 1. No staging directory at all.
    Reset();
    CHECK(Run() == StagedInstallResult::NothingStaged);
    CHECK(g_rec.parseCalls == 0 && g_rec.installCalls == 0 && g_rec.messageBoxes == 0);

    // 2. "to_install" exists but is a regular file.
    Reset();
    Touch(g_root / "to_install");
    CHECK(Run() == StagedInstallResult::NothingStaged);
    CHECK(fs::exists(g_root / "to_install"));

    // 3. Empty staging directory: nothing to do, and it is not an error.
    Reset();
    fs::create_directories(g_staging);
    CHECK(Run() == StagedInstallResult::NothingStaged);
    CHECK(g_rec.messageBoxes == 0);

    // 4. Only an interrupted copy (.part): ignored and left in place for the launcher.
    Reset();
    // The launcher's protocol: a single file is "<name>.part", a ZIP/folder batch is a
    // DIRECTORY "import-<uuid>.part" that is renamed once everything is in it.
    Touch(g_staging / "import-1234.part" / "Shadow.iso");
    Touch(g_staging / "import-5678.PART" / "disc" / "default.xbe");
    Touch(g_staging / "Shadow.iso.part");
    CHECK(Run() == StagedInstallResult::NothingStaged);
    CHECK(g_rec.parseCalls == 0 && g_rec.messageBoxes == 0);
    CHECK(fs::exists(g_staging / "import-1234.part" / "Shadow.iso"));
    CHECK(fs::exists(g_staging / "Shadow.iso.part"));

    // 5. A disc image, upper-case extension, one batch folder deep.
    Reset();
    Touch(g_staging / "batch1" / "Shadow The Hedgehog.ISO");
    CHECK(Run() == StagedInstallResult::Installed);
    CHECK(g_rec.parseCalls == 1 && g_rec.installCalls == 1 && g_rec.messageBoxes == 0);
    CHECK(g_rec.installedPath == g_staging / "batch1" / "Shadow The Hedgehog.ISO");
    CHECK(g_rec.parsedPath == g_rec.installedPath);
    CHECK(g_rec.outputDir == g_root / "game");
    CHECK(g_rec.stagingExistedDuringInstall);
    CHECK(!fs::exists(g_staging));                       // the duplicate is removed
    CHECK(fs::exists(g_root / "game" / "default.xbe"));
    CHECK(g_rec.finishCalls == 1);
    CHECK(g_rec.progressCalls >= 2 && g_rec.progressCalls <= 1002); // throttled to permille steps (+ the initial one)
    CHECK(g_rec.location == "installing the game");

    // 6. An extracted disc inside ZIP wrapper folders; default.xbe in mixed case.
    Reset();
    Touch(g_staging / "b" / "Shadow" / "disc" / "Default.XBE");
    Touch(g_staging / "b" / "Shadow" / "disc" / "data" / "a.bin");
    CHECK(Run() == StagedInstallResult::Installed);
    CHECK(g_rec.installedPath == g_staging / "b" / "Shadow" / "disc");
    // The installer looks the executable up by its exact lower-case name.
    CHECK(g_rec.executableSpelledRight);

    // 7. A folder with default.xbe wins over an image next to it; an .iso.part is ignored.
    Reset();
    Touch(g_staging / "b" / "default.xbe");
    Touch(g_staging / "b" / "game.iso");
    CHECK(Run() == StagedInstallResult::Installed);
    CHECK(g_rec.installedPath == g_staging / "b");
    CHECK(g_rec.executableSpelledRight);

    Reset();
    Touch(g_staging / "a" / "broken.iso.part");
    Touch(g_staging / "b" / "good.iso");
    CHECK(Run() == StagedInstallResult::Installed);
    CHECK(g_rec.installedPath == g_staging / "b" / "good.iso");

    // 8. A directory whose NAME ends in .iso or is "default.xbe" is not a source.
    Reset();
    fs::create_directories(g_staging / "b" / "folder.iso");
    fs::create_directories(g_staging / "b" / "default.xbe");
    Touch(g_staging / "b" / "folder.iso" / "inner" / "real.iso");
    CHECK(Run() == StagedInstallResult::Installed);
    CHECK(g_rec.installedPath == g_staging / "b" / "folder.iso" / "inner" / "real.iso");

    // 9. Too deep: beyond the search limit nothing is found -> reported and cleaned up.
    Reset();
    Touch(g_staging / "1" / "2" / "3" / "4" / "5" / "6" / "7" / "default.xbe");
    CHECK(Run() == StagedInstallResult::Failed);
    CHECK(g_rec.parseCalls == 0 && g_rec.messageBoxes == 1);
    CHECK(g_rec.lastResource == "installer_source_missing");
    CHECK(g_rec.lastFlags == SDL_MESSAGEBOX_ERROR);
    CHECK(!fs::exists(g_staging));
    // ...and exactly at the limit it still is.
    Reset();
    Touch(g_staging / "1" / "2" / "3" / "4" / "5" / "6" / "default.xbe");
    CHECK(Run() == StagedInstallResult::Installed);

    // 10. Published files, none usable.
    Reset();
    Touch(g_staging / "b" / "readme.txt");
    Touch(g_staging / "b" / "game.bin");
    CHECK(Run() == StagedInstallResult::Failed);
    CHECK(g_rec.lastResource == "installer_source_missing" && g_rec.messageBoxes == 1);
    CHECK(!fs::exists(g_staging));
    CHECK(!fs::exists(g_root / "game"));

    // 11. The source cannot be parsed (not an Xbox image): staging removed, error shown.
    Reset();
    Touch(g_staging / "b" / "movie.iso");
    g_script.parseOk = false;
    g_script.error = "not an Xbox disc image";
    CHECK(Run() == StagedInstallResult::Failed);
    CHECK(g_rec.parseCalls == 1 && g_rec.installCalls == 0 && g_rec.messageBoxes == 1);
    CHECK(g_rec.lastResource == "installer_failed" && g_rec.lastDetail == "not an Xbox disc image");
    CHECK(g_rec.lastMessage.find("not an Xbox disc image") != std::string::npos);
    CHECK(!fs::exists(g_staging));
    CHECK(g_rec.finishCalls == 1);

    // 12. Not enough free space: reported, and the staged source is KEPT for a retry.
    Reset();
    Touch(g_staging / "b" / "game.iso");
    g_script.totalSize = ~0ull / 2;
    CHECK(Run() == StagedInstallResult::Failed);
    CHECK(g_rec.installCalls == 0 && g_rec.messageBoxes == 1);
    CHECK(g_rec.lastResource == "installer_failed");
    CHECK(g_rec.lastDetail.find("Not enough free space") != std::string::npos);
    CHECK(fs::exists(g_staging / "b" / "game.iso"));
    CHECK(g_rec.finishCalls == 1);
    // After freeing space the same staged source installs.
    g_script.totalSize = 1000; g_rec = {};
    CHECK(Run() == StagedInstallResult::Installed);
    CHECK(!fs::exists(g_staging));

    // 13. The installer itself fails (bad hash, I/O error): staging removed, error shown.
    Reset();
    Touch(g_staging / "b" / "game.iso");
    g_script.installOk = false;
    g_script.error = "hash mismatch in data/a.bin";
    CHECK(Run() == StagedInstallResult::Failed);
    CHECK(g_rec.installCalls == 1 && g_rec.messageBoxes == 1);
    CHECK(g_rec.lastDetail == "hash mismatch in data/a.bin");
    CHECK(!fs::exists(g_staging));
    CHECK(g_rec.finishCalls == 1);

    // 14. The game directory's parents do not exist yet (first launch on fresh storage):
    //     the free-space probe walks up to an existing directory instead of failing.
    Reset();
    Touch(g_staging / "b" / "game.iso");
    {
        const fs::path deep = g_root / "not" / "yet" / "there" / "game";
        CHECK(os::android::InstallStagedGame(g_root, deep) == StagedInstallResult::Installed);
        CHECK(fs::exists(deep / "default.xbe"));
    }

    // 15. Several batches: deterministic choice (sorted), the rest is discarded with it.
    Reset();
    Touch(g_staging / "20260102" / "second.iso");
    Touch(g_staging / "20260101" / "first.iso");
    CHECK(Run() == StagedInstallResult::Installed);
    CHECK(g_rec.installedPath == g_staging / "20260101" / "first.iso");
    CHECK(!fs::exists(g_staging));

    // 16. The "game missing" message.
    g_rec = {};
    os::android::ShowGameMissingMessage();
    CHECK(g_rec.messageBoxes == 1 && g_rec.lastResource == "installer_game_missing" && g_rec.lastFlags == SDL_MESSAGEBOX_INFORMATION);

    fs::remove_all(g_root);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
