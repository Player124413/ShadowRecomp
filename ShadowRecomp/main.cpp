#include "stdafx.h"
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/function.h>
#include <gpu/video.h>
#include <cpu/guest_thread.h>
#include <input/input.h>
#include <file.h>
#include <image.h>
#include <user/paths.h>
#include <kernel/io/file_system.h>
#include <audio/audio.h>
#include <ui/game_window.h>
#include <os/logger.h>
#include <os/process.h>
#include <os/registry.h>

#include <xbe.h>
#include <iostream>
#include <filesystem>
#include <mod/mod_loader.h>
#include <preload_executable.h>
#include <patches/unix_patches.h>
#include <patches/reloaded_mod_patches.h>
#include <patches/code_patches.h>
#include <patches/multiplayer_mod_patches.h>
#include <audio/embedded_player.h>
#include <ui/installer_wizard.h>
#include <install/installer.h>
#include <install/update_checker.h>
#include <gpu/frame_pacing/timer_resolution.h>
#include <patches/pause_patches.h>
#include <patches/bloom_patches.h>
#include <misc/rich_presence/shadow_rpc.h>
#include <os/startup_timeline.h>

#ifdef __ANDROID__
// Renames main() to SDL_main(), the entry point SDLActivity calls on its own thread.
#include <SDL3/SDL_main.h>

#ifndef main
#error "main() must be renamed to SDL_main() on Android: do not define SDL_MAIN_HANDLED."
#endif

#include <os/android/install_android.h>
#include <os/android/jni_android.h>
#include <os/android/storage_android.h>
#include <os/android/vulkan_driver_android.h>
#include <ui/touch_controls.h>
#endif


#if defined(_WIN32) && defined(SHADOW_RECOMP_D3D12)
static std::array<std::string_view, 3> g_D3D12RequiredModules =
{
    "D3D12/D3D12Core.dll",
    "dxcompiler.dll",
    "dxil.dll"
};
#endif

Memory g_memory;
Heap g_userHeap;

// return a modifiable pointer inside this structure that corresponds to a virtual address
uint8_t* GetAddr(uint32_t xbeVA, const XbeHeader& header, const XbeSectionHeader* sectionHeaders)
{
    for (uint32_t i = 0; i < header.numSections; i++)
    {
        uint32_t virtStart = sectionHeaders[i].virtualAddress;
        uint32_t virtSize = sectionHeaders[i].virtualSize;

        if (xbeVA >= virtStart && xbeVA < (virtStart + virtSize))
        {
            uint32_t offsetInSection = xbeVA - virtStart;
            uint8_t* sectionBaseHost = reinterpret_cast<uint8_t*>(g_memory.Translate(virtStart));
            return sectionBaseHost + offsetInSection;
        }
    }

    return nullptr; // Not found
}

TLS* m_TLS;

uint32_t LoadXbeModule(const std::filesystem::path& path)
{
    auto module = LoadFile(path);

    if (module.empty())
    {
        LOGN_ERROR("Failed to load XBE file, please make sure game files are present.");
        assert("Failed to load XBE file, please make sure game files are present." && false);
        return 0;
    }


    const auto* header = reinterpret_cast<const XbeHeader*>(module.data());
    const uint32_t sectionHeadersOffset = header->sectionHeadersAddress - header->baseAddress;
    const auto* sectionHeaders = reinterpret_cast<const XbeSectionHeader*>(module.data() + sectionHeadersOffset);

    uint32_t firstSectionVA = sectionHeaders[0].virtualAddress;
    uint32_t headersSize = firstSectionVA - header->baseAddress;

    //Copy headers to their expected VA 
    void* headersDest = g_memory.Translate(header->baseAddress);
    memcpy(headersDest, module.data(), headersSize);

    for (uint32_t i = 0; i < header->numSections; i++)
    {
        auto srcData = module.data() + sectionHeaders[i].rawAddress;
        auto destData = g_memory.Translate(sectionHeaders[i].virtualAddress);
        memcpy(destData, srcData, sectionHeaders[i].rawSize);
    }

    void* addr = GetAddr(header->tlsAddress, *header, sectionHeaders);
    if (addr)
    {
        m_TLS = new TLS;
        memcpy(m_TLS, addr, sizeof(*m_TLS));
    }

    return decode_entry_point(header->m_entry, header->sizeOfImage);
}

void HostStartup()
{
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    input::Init();

    if (g_memory.base == nullptr)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), Localise("System_MemoryAllocationFailed").c_str(), GameWindow::s_pWindow);
        std::_Exit(1);
    }


    g_userHeap.Init();
}

extern bool isHostThread;


bool QuickBootConfig = false;
int Ts_StageID = -1; // test spawn stage select
std::vector<int> Ts_EventIDs; // events/cutscenes to play directly on boot
bool Ts_EventAll = false; // will play all events

static constexpr size_t EventListMaxSize = 256;

static void ParseEventList(const char* arg)
{
    std::string list = arg;

    for (char* token = strtok(list.data(), ","); token; token = strtok(nullptr, ","))
    {
        int first, last;
        int read = sscanf(token, "%d-%d", &first, &last);

        if (read < 1)
            break;

        if (read == 1)
            last = first;

        if (last < first)
            std::swap(first, last);

        for (int id = first; id <= last && Ts_EventIDs.size() < EventListMaxSize; id++)
            Ts_EventIDs.push_back(id);
    }
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    //SetProcessAffinityMask(GetCurrentProcess(), 1);
    TimerResolution::SetHighestResolution();

    // Higher priority == faster wake-up during frame pacing.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#elif defined(__linux__)
    //cpu_set_t cpuset;
    //CPU_ZERO(&cpuset);
    //CPU_SET(0, &cpuset);
    //sched_setaffinity(0, sizeof(cpuset), &cpuset);
#endif

    os::process::CheckConsole();

    if (!os::registry::Init())
        LOGN_WARNING("OS does not support registry");


    os::logger::Init();
    STARTUP_PHASE("game code started and log opened");

#ifdef __ANDROID__
    // Everything is read from and written to the storage root chosen in the launcher.
    if (!os::android::HasDataRoot())
    {
        const auto message = os::android::LocaliseMessage("storage_native_unavailable",
            "The selected game storage is unavailable or not writable. Reconnect the SD card or choose another storage location in the launcher.");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Shadow Recompiled", message.c_str(), nullptr);
        std::_Exit(1);
    }

#ifdef X86_RECOMP_STUB
    {
        // Built with tools/android/stub-recompiler.py to test the build pipeline: every
        // guest function is empty, so there is no game to start.
        LOGN_ERROR("This build was made without the game's code (stub recompiler) and cannot run the game.");
        const auto message = os::android::LocaliseMessage("stub_build",
            "This is a test build made without the game's code, so it cannot start the game. Install a build made from the game's executable.");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Shadow Recompiled", message.c_str(), nullptr);
        std::_Exit(1);
    }
#endif

    {
        // The user path is nested two levels below the storage root, and Config::Save()
        // only creates its last component (and throws if the parent is missing). The
        // launcher normally creates it, but GameActivity can also be started directly.
        std::error_code userPathEc;
        std::filesystem::create_directories(GetUserPath(), userPathEc);
        if (userPathEc)
            LOGFN_ERROR("Could not create {}: {}", GetUserPath().string(), userPathEc.message());
    }
#endif

    PreloadContext preloadContext;
    preloadContext.PreloadExecutable();

    LOGN("Starting Shadow Recomp");

    std::filesystem::path xbePath;
    xbePath = GameExecutable;

    bool forceInstaller = false;
    bool useDefaultWorkingDirectory = false;
    bool forceInstallationCheck = false;
    bool graphicsApiRetry = false;
    const char* sdlVideoDriver = nullptr;

    for (uint32_t i = 1; i < argc; i++)
    {
        forceInstaller = forceInstaller || (strcmp(argv[i], "--install") == 0);
        useDefaultWorkingDirectory = useDefaultWorkingDirectory || (strcmp(argv[i], "--use-cwd") == 0);
        forceInstallationCheck = forceInstallationCheck || (strcmp(argv[i], "--install-check") == 0);
        graphicsApiRetry = graphicsApiRetry || (strcmp(argv[i], "--graphics-api-retry") == 0);
        QuickBootConfig = QuickBootConfig || (strcmp(argv[i], "--quickboot") == 0);
        Video::s_dumpPipelineCacheHeader = Video::s_dumpPipelineCacheHeader || (strcmp(argv[i], "--dump-pipeline-cache") == 0);

        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--level") == 0)
        {
            if ((i + 1) < argc)
                Ts_StageID = atoi(argv[++i]);
            else
                LOGN_WARNING("No argument was specified for --level. Option will be ignored.");
        }

        if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--event") == 0)
        {
            if ((i + 1) < argc)
            {
                const char* arg = argv[++i];

                if (_stricmp(arg, "all") == 0)
                {
                    Ts_EventAll = true;
                }
                else
                {
                    ParseEventList(arg);

                    if (Ts_EventIDs.empty())
                        LOGN_WARNING("No valid event id was specified for --event. Option will be ignored.");
                }
            }
            else
            {
                LOGN_WARNING("No argument was specified for --event. Option will be ignored.");
            }
        }

        if (strcmp(argv[i], "--sdl-video-driver") == 0)
        {
            if ((i + 1) < argc)
                sdlVideoDriver = argv[++i];
            else
                LOGN_WARNING("No argument was specified for --sdl-video-driver. Option will be ignored.");
        }
    }

    if (Ts_StageID != -1 && (Ts_EventAll || !Ts_EventIDs.empty()))
    {
        Ts_EventAll = false;
        Ts_EventIDs.clear();
    }

    if (!useDefaultWorkingDirectory)
    {
        // Set the current working directory to the executable's path.
        std::error_code ec;
        std::filesystem::current_path(os::process::GetExecutableRoot(), ec);
    }

    Config::Load();
    STARTUP_PHASE("configuration loaded");

#ifdef __ANDROID__
    // The game always covers the screen; a config.toml copied from a PC must not make
    // the window code try to leave fullscreen.
    Config::Fullscreen = true;

    AndroidSetVulkanDriverSelection(Config::VulkanDriver, Config::RenderMode);

    {
        // The launcher asks for the touch layout editor with a marker file; consume it.
        std::error_code markerEc;
        const std::filesystem::path editMarker = GetGamePath() / "touch_layout_edit.txt";
        const bool editTouchLayout = std::filesystem::exists(editMarker, markerEc);
        if (editTouchLayout)
            std::filesystem::remove(editMarker, markerEc);

        TouchControls::Configure(Config::TouchControls, Config::TouchCamera, GetUserPath() / "touch_layout.cfg", editTouchLayout);
    }

    // The launcher stages the game source (disc image or extracted disc); install it
    // before anything looks for the game. See os/android/install_android.h.
    const os::android::StagedInstallResult stagedInstall = os::android::InstallStagedGame(GetGamePath(), MainGameFolder);
    STARTUP_PHASE(stagedInstall == os::android::StagedInstallResult::NothingStaged ? "checked for a game to install" : "game install attempted");
#endif

    bool isGameInstalled = std::filesystem::exists(GameExecutable);
    bool runInstallerWizard = forceInstaller || !isGameInstalled;

    if (forceInstallationCheck)
    {
        os::process::ShowConsole();
        int lastDecile = -1;
        std::string errorMessage;
        bool isInstallValid = Installer::CheckInstall(MainGameFolder, [&](const Installer::Progress& progress)
        {
            int decile = int(progress.ratio * 10.0f);
            if (decile != lastDecile)
            {
                LOGFN("Checking files: {}%", decile * 10);
                lastDecile = decile;
            }
        }, errorMessage);

        char resultText[512];
        snprintf(resultText, sizeof(resultText), Localise(isInstallValid ? "IntegrityCheck_Success" : "IntegrityCheck_Failed").c_str(), errorMessage.c_str());
        fprintf(isInstallValid ? stdout : stderr, "%s\n", resultText);
        SDL_ShowSimpleMessageBox(isInstallValid ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), resultText, GameWindow::s_pWindow);
        return isInstallValid ? 0 : 1;
    }

    if (runInstallerWizard)
    {
#ifdef __ANDROID__
        // There is no installer wizard on Android. A failed staged install has already
        // told the user what went wrong; otherwise point them at the launcher.
        if (stagedInstall != os::android::StagedInstallResult::Failed)
            os::android::ShowGameMissingMessage();

        std::_Exit(1);
#endif

        if (!Video::CreateHostDevice(sdlVideoDriver, graphicsApiRetry))
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), Localise("Video_BackendError").c_str(), GameWindow::s_pWindow);
            std::_Exit(1);
        }

        g_userHeap.Init();

        if (!InstallerWizard::Run(GetGamePath()))
        {
            std::_Exit(0);
        }
    }

#if defined(_WIN32) && defined(SHADOW_RECOMP_D3D12)
    for (auto& dll : g_D3D12RequiredModules)
    {
        if (!std::filesystem::exists(g_executableRoot / dll))
        {
            char text[512];
            snprintf(text, sizeof(text), "missing graphics DLL", dll.data());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), text, GameWindow::s_pWindow);
            std::_Exit(1);
        }
    }
#endif

    constexpr double TimeBetweenUpdateChecksInSeconds = 24 * 60 * 60;
    time_t timeNow = std::time(nullptr);
    if (Config::CheckForUpdates && difftime(timeNow, Config::LastChecked) > TimeBetweenUpdateChecksInSeconds)
    {
        UpdateChecker::initialize();
        UpdateChecker::start();
        Config::LastChecked = timeNow;
        Config::Save();
    }

    if (Config::ShowConsole)
        os::process::ShowConsole();

    HostStartup();
    STARTUP_PHASE("input and guest heap initialized");

    ModLoader::Init();
    STARTUP_PHASE("mods loaded");

    //init mutexes for multi-core patch
    InitMultiCoreMutexesPatches();


    isGameInstalled = std::filesystem::exists(GameExecutable);
    if (!isGameInstalled)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), "Game files not found. Please reinstall.", GameWindow::s_pWindow);
        std::_Exit(1);
    }

    xbePath = GameExecutable;
    uint32_t entry = LoadXbeModule(xbePath);
    STARTUP_PHASE("game executable loaded");

    if (!runInstallerWizard)
    {
        if (!Video::CreateHostDevice(sdlVideoDriver, graphicsApiRetry))
        {
#ifdef __ANDROID__
            // The usual cause on Android is the selected driver, which is changed in the launcher.
            const auto message = os::android::LocaliseMessage("vulkan_startup_failed",
                "The graphics driver could not be started. Select another Vulkan driver in the launcher (try System or Auto) and start the game again.");
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), message.c_str(), GameWindow::s_pWindow);
#else
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), Localise("Video_BackendError").c_str(), GameWindow::s_pWindow);
#endif
            std::_Exit(1);
        }
    }



    Video::PrecompileShadersAndPipelines();

    PatchUnixStrings();
    ReloadedValuePatches();
    CodeValuePatches();
    MultiplayerPatchesInit();
    PausePatchesInit();
    BloomConfigValueChangedCallback(&Config::BloomIntensity);
    RichPresence::ShadowRpc::Start();
    STARTUP_PHASE("patches applied; starting the game");

    //launch the Start function of the game
    isHostThread = true;
    GuestThreadParams params = { entry, 0, 0, 0, 0 };
    GuestThread::Start(params);


    return 0;
}