#include "vulkan_driver_android.h"

#include <os/android/logger_android.h>
#include <os/android/storage_android.h>
#include <os/logger.h>
#include <os/android/android_settings.h>

#include <adrenotools/driver.h>
#include <miniz.h>
#include <nlohmann/json.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <vector>

// The bundled drivers are the Mesa Turnip builds published by the UnleashedRecomp-Android
// project; tools/android/fetch-drivers.sh downloads them into the APK assets under exactly
// these names. How each one was built, and the TU_DEBUG presets applied to them below, come
// from that project's testing with its own game. None of it has been re-measured with
// Shadow the Hedgehog.
//
// The general-purpose build: Mesa 26.1 with fixes for Adreno 7xx.
static constexpr const char *BUNDLED_DRIVER_NAME = "vulkan.unleashed26_1_wfm_a732.so";
// The asset must NOT live under "turnip/": SDL_IOFromFile resolves relative paths against
// internal storage BEFORE the APK asset system, and the driver is extracted to the internal
// "turnip/" directory under the same name - the extracted copy would permanently shadow the
// packaged asset and driver updates shipped in the APK would never be seen again.
static constexpr const char *BUNDLED_DRIVER_ASSET = "bundled_driver/vulkan.unleashed26_1_wfm_a732.so";
static constexpr const char *VAUZI_710_DRIVER_NAME = "vulkan.vauzi710_v2_7.so";
static constexpr const char *VAUZI_710_DRIVER_ASSET = "bundled_driver/vulkan.vauzi710_v2_7.so";
static constexpr const char *EXPERIMENTAL_A725_DRIVER_NAME = "vulkan.wb26_2_rp_pair_ccu_color_a725.so";
static constexpr const char *EXPERIMENTAL_A725_DRIVER_ASSET = "bundled_driver/vulkan.wb26_2_rp_pair_ccu_color_a725.so";
// Mesa 26.3 development snapshot with Qualcomm UBWC detection in u_gralloc, for
// Adreno 8xx (Snapdragon 8 Elite). Meant to run with TU_DEBUG=sysmem,nobin.
static constexpr const char *ADRENO_8XX_DRIVER_NAME = "vulkan.mesa26_3_gralloc_a8xx.so";
static constexpr const char *ADRENO_8XX_DRIVER_ASSET = "bundled_driver/vulkan.mesa26_3_gralloc_a8xx.so";
static constexpr const char *ADRENO_8XX_TU_DEBUG_PRESET = "sysmem,nobin";
static constexpr const char *DEFAULT_DRIVER_NAME = "vulkan.unleashed26_1_wfm_a732.so";
static constexpr const char *LAST_IMPORTED_DRIVER_FILE = "last_imported_driver.txt";
static constexpr const char *VULKAN_STARTUP_STATE_FILE = "vulkan_startup_state.txt";
static constexpr const char *BUNDLED_DRIVER_STAMP_FILE = "bundled_drivers.stamp";

struct BundledDriver
{
    const char *name;  // File name in the internal "turnip/" directory.
    const char *asset; // Path of the APK asset it is extracted from.
};

static constexpr BundledDriver BUNDLED_DRIVERS[] =
{
    { BUNDLED_DRIVER_NAME, BUNDLED_DRIVER_ASSET },
    { VAUZI_710_DRIVER_NAME, VAUZI_710_DRIVER_ASSET },
    { EXPERIMENTAL_A725_DRIVER_NAME, EXPERIMENTAL_A725_DRIVER_ASSET },
    { ADRENO_8XX_DRIVER_NAME, ADRENO_8XX_DRIVER_ASSET },
};

enum class VulkanStartupPhase
{
    CustomPending,
    SafeFallbackPending,
    RecoveryExhausted,
};

struct VulkanStartupState
{
    VulkanStartupPhase phase{};
    EAndroidVulkanDriver configuredDriver{};
    EAndroidRenderMode configuredRenderMode{};
};

static bool g_vulkanStartupPrepared;
static bool g_vulkanStartupStateActive;
static bool g_vulkanStartupSuccessReported;
static bool g_forceSafeVulkanFallback;
static bool g_armCustomVulkanStartup;
static EAndroidVulkanDriver g_runtimeVulkanDriver;
static EAndroidRenderMode g_runtimeRenderMode;

// What the user configured. Set by AndroidSetVulkanDriverSelection() before the renderer
// starts, so this file does not depend on the config system.
static EAndroidVulkanDriver g_configuredVulkanDriver = EAndroidVulkanDriver::Auto;
static EAndroidRenderMode g_configuredRenderMode = EAndroidRenderMode::Auto;

// Result of the one custom-loader attempt of this process (see AndroidGetCustomVulkanLoader).
static bool g_customLoaderResolved;
static void *g_customLibVulkan;

// Set when this launch wrote imported driver files into the "turnip/" directory, where they
// may have replaced a bundled driver of the same name.
static bool g_driverFilesImported;

static std::string GetTurnipDir()
{
    const std::filesystem::path &internalDir = os::android::GetInternalFilesDir();
    if (internalDir.empty())
        return {};

    return (internalDir / "turnip/").string();
}

enum class EAndroidGpuFamily
{
    Adreno,
    Xclipse,
    Other,
    Unknown,
};

// Identifies the GPU family from the Vulkan/EGL HAL system properties (the HAL library is
// named vulkan.<ro.hardware.vulkan>.so) without loading any driver. The bundled Turnip build
// only works on Adreno; attempting it on other GPUs crashes the first launch and relies on
// boot recovery to reach the system driver on the second one.
static EAndroidGpuFamily DetectGpuFamily(std::string &description)
{
    char vulkanProp[PROP_VALUE_MAX]{};
    char eglProp[PROP_VALUE_MAX]{};
    __system_property_get("ro.hardware.vulkan", vulkanProp);
    __system_property_get("ro.hardware.egl", eglProp);

    description = {};
    if (vulkanProp[0] != '\0')
        description += vulkanProp;

    if (eglProp[0] != '\0' && strcmp(vulkanProp, eglProp) != 0)
    {
        if (!description.empty())
            description += "/";

        description += eglProp;
    }

    std::string lowered = description;
    for (char &c : lowered)
        c = char(tolower(uint8_t(c)));

    if (lowered.empty())
        return EAndroidGpuFamily::Unknown;

    if (lowered.find("adreno") != std::string::npos)
        return EAndroidGpuFamily::Adreno;

    // Samsung Xclipse (RDNA-based) ships its Vulkan HAL as vulkan.samsung.so.
    if (lowered.find("samsung") != std::string::npos || lowered.find("sgpu") != std::string::npos ||
        lowered.find("xclipse") != std::string::npos)
    {
        return EAndroidGpuFamily::Xclipse;
    }

    return EAndroidGpuFamily::Other;
}

// Auto selects the Adreno 8xx driver only on SoCs it was validated on. The GPU model is not
// known before a driver is loaded, so the SoC model (Android 12+) stands in for it.
// SM8750 = Snapdragon 8 Elite (Adreno 830).
static bool IsValidatedAdreno8xxSoc(std::string &socModel)
{
    char socProp[PROP_VALUE_MAX]{};
    __system_property_get("ro.soc.model", socProp);
    socModel = socProp;
    return strncmp(socProp, "SM8750", 6) == 0;
}

static const char *VulkanDriverName(EAndroidVulkanDriver driver)
{
    switch (driver)
    {
        case EAndroidVulkanDriver::System:   return "System";
        case EAndroidVulkanDriver::Bundled:  return "Bundled";
        case EAndroidVulkanDriver::Vauzi710: return "Vauzi710";
        case EAndroidVulkanDriver::ExperimentalA725: return "ExperimentalA725";
        case EAndroidVulkanDriver::Adreno8xx: return "Adreno8xx";
        case EAndroidVulkanDriver::Imported: return "Imported";
        case EAndroidVulkanDriver::Auto:
        default:                             return "Auto";
    }
}

static const char *RenderModeName(EAndroidRenderMode mode)
{
    switch (mode)
    {
        case EAndroidRenderMode::GMEM:   return "GMEM";
        case EAndroidRenderMode::Sysmem: return "Sysmem";
        case EAndroidRenderMode::Auto:
        default:                         return "Auto";
    }
}

static const char *StartupPhaseName(VulkanStartupPhase phase)
{
    switch (phase)
    {
        case VulkanStartupPhase::CustomPending:       return "custom_pending";
        case VulkanStartupPhase::SafeFallbackPending: return "safe_fallback_pending";
        case VulkanStartupPhase::RecoveryExhausted:   return "recovery_exhausted";
        default:                                      return "unknown";
    }
}

static std::filesystem::path GetVulkanStartupStatePath()
{
    const std::filesystem::path &internalDir = os::android::GetInternalFilesDir();
    return internalDir.empty() ? std::filesystem::path() : internalDir / VULKAN_STARTUP_STATE_FILE;
}

static bool SyncDirectory(const std::filesystem::path &directory)
{
    int directoryFd = open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (directoryFd < 0)
        return false;

    const bool ok = fsync(directoryFd) == 0;
    close(directoryFd);
    return ok;
}

// The state is replaced, never edited in place. fsync(temp) + rename + fsync(parent)
// guarantees that a process/OS kill yields either the complete old state or complete new
// state, never a half-written marker that could make recovery non-deterministic.
static bool WriteVulkanStartupState(const VulkanStartupState &state)
{
    const std::filesystem::path statePath = GetVulkanStartupStatePath();
    if (statePath.empty())
        return false;

    std::filesystem::path tempPath = statePath;
    tempPath += ".tmp";

    char contents[256];
    const int length = snprintf(contents, sizeof(contents),
        "version=1\nphase=%s\nconfigured_driver=%u\nconfigured_render_mode=%u\n",
        StartupPhaseName(state.phase), unsigned(state.configuredDriver), unsigned(state.configuredRenderMode));
    if (length <= 0 || size_t(length) >= sizeof(contents))
        return false;

    int file = open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (file < 0)
        return false;

    size_t written = 0;
    bool ok = true;
    while (written < size_t(length))
    {
        const ssize_t result = write(file, contents + written, size_t(length) - written);
        if (result > 0)
        {
            written += size_t(result);
            continue;
        }

        if (result < 0 && errno == EINTR)
            continue;

        ok = false;
        break;
    }

    if (ok)
        ok = fsync(file) == 0;
    if (close(file) != 0)
        ok = false;

    if (!ok || rename(tempPath.c_str(), statePath.c_str()) != 0)
    {
        unlink(tempPath.c_str());
        return false;
    }

    return SyncDirectory(statePath.parent_path());
}

static bool RemoveVulkanStartupState()
{
    const std::filesystem::path statePath = GetVulkanStartupStatePath();
    if (statePath.empty())
        return false;

    if (unlink(statePath.c_str()) != 0 && errno != ENOENT)
        return false;

    // Also discard an interrupted pre-rename write. It is never considered state by readers.
    std::filesystem::path tempPath = statePath;
    tempPath += ".tmp";
    unlink(tempPath.c_str());
    return SyncDirectory(statePath.parent_path());
}

static bool ReadVulkanStartupState(VulkanStartupState &state, bool &stateFileExists)
{
    const std::filesystem::path statePath = GetVulkanStartupStatePath();
    stateFileExists = false;
    if (statePath.empty())
        return false;

    FILE *file = fopen(statePath.c_str(), "rb");
    if (file == nullptr)
        return false;

    stateFileExists = true;
    char contents[256]{};
    const size_t bytesRead = fread(contents, 1, sizeof(contents) - 1, file);
    const bool readError = ferror(file) != 0;
    fclose(file);
    if (readError || bytesRead == 0 || bytesRead == sizeof(contents) - 1)
        return false;

    unsigned version = 0;
    unsigned configuredDriver = 0;
    unsigned configuredRenderMode = 0;
    char phase[64]{};
    if (sscanf(contents,
            "version=%u\nphase=%63s\nconfigured_driver=%u\nconfigured_render_mode=%u",
            &version, phase, &configuredDriver, &configuredRenderMode) != 4 ||
        version != 1 ||
        configuredDriver > unsigned(EAndroidVulkanDriver::Imported) ||
        configuredRenderMode > unsigned(EAndroidRenderMode::Sysmem))
    {
        return false;
    }

    if (strcmp(phase, "custom_pending") == 0)
        state.phase = VulkanStartupPhase::CustomPending;
    else if (strcmp(phase, "safe_fallback_pending") == 0)
        state.phase = VulkanStartupPhase::SafeFallbackPending;
    else if (strcmp(phase, "recovery_exhausted") == 0)
        state.phase = VulkanStartupPhase::RecoveryExhausted;
    else
        return false;

    state.configuredDriver = EAndroidVulkanDriver(configuredDriver);
    state.configuredRenderMode = EAndroidRenderMode(configuredRenderMode);
    return true;
}

static bool StartupSelectionMatches(const VulkanStartupState &state)
{
    return state.configuredDriver == g_configuredVulkanDriver &&
        state.configuredRenderMode == g_configuredRenderMode;
}

static void ForceSafeVulkanFallback(const VulkanStartupState &previousState, const char *reason)
{
    VulkanStartupState recoveryState
    {
        VulkanStartupPhase::SafeFallbackPending,
        previousState.configuredDriver,
        previousState.configuredRenderMode,
    };

    g_forceSafeVulkanFallback = true;
    g_runtimeVulkanDriver = EAndroidVulkanDriver::System;
    g_runtimeRenderMode = EAndroidRenderMode::Auto;
    g_vulkanStartupStateActive = WriteVulkanStartupState(recoveryState);

    LOGF_WARNING("Vulkan boot recovery: {} (previous phase={}, configured driver={}, render mode={}). "
        "Forcing this startup to System driver + RenderMode Auto; user Config is unchanged.",
        reason, StartupPhaseName(previousState.phase), VulkanDriverName(previousState.configuredDriver),
        RenderModeName(previousState.configuredRenderMode));

    if (!g_vulkanStartupStateActive)
    {
        LOG_ERROR("Vulkan boot recovery: failed to persist the consumed recovery state atomically.");
        // Best-effort consumption still prevents an unwritable stale custom_pending file
        // from forcing the same recovery on every launch.
        RemoveVulkanStartupState();
    }
}

static void PrepareVulkanStartup()
{
    if (g_vulkanStartupPrepared)
        return;

    g_vulkanStartupPrepared = true;
    g_runtimeVulkanDriver = g_configuredVulkanDriver;
    g_runtimeRenderMode = g_configuredRenderMode;

    VulkanStartupState previousState{};
    bool stateFileExists = false;
    const bool validState = ReadVulkanStartupState(previousState, stateFileExists);

    if (stateFileExists && (!validState || previousState.phase == VulkanStartupPhase::CustomPending))
    {
        if (!validState)
        {
            previousState =
            {
                VulkanStartupPhase::CustomPending,
                g_configuredVulkanDriver,
                g_configuredRenderMode,
            };
            ForceSafeVulkanFallback(previousState, "an invalid/incomplete pending marker was left by the previous launch");
        }
        else
        {
            ForceSafeVulkanFallback(previousState, "the previous custom-driver Vulkan startup did not reach a usable swapchain");
        }
        return;
    }

    if (!validState)
    {
        // Arm only immediately before an actual custom-driver dlopen, after paths and the
        // selected .so have been validated. System mode needs no crash marker.
        g_armCustomVulkanStartup = g_runtimeVulkanDriver != EAndroidVulkanDriver::System;
        return;
    }

    g_vulkanStartupStateActive = true;
    if (previousState.phase == VulkanStartupPhase::SafeFallbackPending)
    {
        // The safe System+Auto attempt itself failed. Consume it without forcing the same
        // fallback forever. A successful startup still clears this exhausted state.
        VulkanStartupState exhaustedState
        {
            VulkanStartupPhase::RecoveryExhausted,
            previousState.configuredDriver,
            previousState.configuredRenderMode,
        };
        if (!WriteVulkanStartupState(exhaustedState))
            LOG_ERROR("Vulkan boot recovery: failed to persist recovery_exhausted state atomically.");

        previousState = exhaustedState;
        LOGF_WARNING("Vulkan boot recovery: the one-time System + RenderMode Auto recovery did not "
            "reach a usable swapchain. Automatic recovery is now exhausted for driver={} / render mode={} "
            "to prevent a restart loop; trying the configured selection without modifying Config.",
            VulkanDriverName(previousState.configuredDriver), RenderModeName(previousState.configuredRenderMode));
    }

    if (!StartupSelectionMatches(previousState))
    {
        LOGF("Vulkan boot recovery: configured selection changed from driver={} / render mode={} to "
            "driver={} / render mode={}; enabling crash recovery for the new selection.",
            VulkanDriverName(previousState.configuredDriver), RenderModeName(previousState.configuredRenderMode),
            VulkanDriverName(g_configuredVulkanDriver), RenderModeName(g_configuredRenderMode));
        g_armCustomVulkanStartup = g_runtimeVulkanDriver != EAndroidVulkanDriver::System;
    }
    else
    {
        LOGF_WARNING("Vulkan boot recovery: recovery remains exhausted for driver={} / render mode={}; "
            "no additional forced fallback will be scheduled until this startup succeeds or the selection changes.",
            VulkanDriverName(previousState.configuredDriver), RenderModeName(previousState.configuredRenderMode));
    }
}

static bool ArmCustomVulkanStartup()
{
    if (!g_armCustomVulkanStartup)
        return true;

    VulkanStartupState state
    {
        VulkanStartupPhase::CustomPending,
        g_configuredVulkanDriver,
        g_configuredRenderMode,
    };

    if (!WriteVulkanStartupState(state))
    {
        // Never enter an untracked custom-driver startup: failure to create the durable
        // marker deterministically degrades this launch to the safe system path.
        LOG_ERROR("Vulkan boot recovery: unable to arm the custom startup marker; forcing System driver + RenderMode Auto for this launch.");
        g_forceSafeVulkanFallback = true;
        g_runtimeVulkanDriver = EAndroidVulkanDriver::System;
        g_runtimeRenderMode = EAndroidRenderMode::Auto;
        g_armCustomVulkanStartup = false;
        RemoveVulkanStartupState();
        return false;
    }

    g_armCustomVulkanStartup = false;
    g_vulkanStartupStateActive = true;
    LOGF("Vulkan boot recovery: armed custom startup marker (driver={}, render mode={}); "
        "it will be cleared only after device + usable swapchain creation.",
        VulkanDriverName(state.configuredDriver), RenderModeName(state.configuredRenderMode));
    return true;
}

// Several times the size of the largest driver. A file that claims more is not a driver,
// and is not read into memory to find that out.
static constexpr uint64_t MAX_WHOLE_FILE_SIZE = 256ull * 1024 * 1024;

static bool ReadWholeFile(const std::filesystem::path &path, std::vector<uint8_t> &data)
{
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;

    // The size is asked of the file itself, and only of a regular one. Seeking to the end
    // answers for anything that can be opened: for a directory ext4 reports 2^63 - 1, and
    // a buffer of that size is an allocation failure that ends the process.
    struct stat info {};
    if (fstat(fileno(file), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 || uint64_t(info.st_size) > MAX_WHOLE_FILE_SIZE)
    {
        fclose(file);
        return false;
    }

    data.resize(size_t(info.st_size));
    bool ok = fread(data.data(), 1, data.size(), file) == data.size();
    fclose(file);
    return ok;
}

// Writes through a temporary file + rename so a mid-write kill can't leave a truncated
// driver behind that would then be loaded on every subsequent launch.
static bool WriteWholeFile(const std::filesystem::path &path, const void *data, size_t size)
{
    std::filesystem::path tempPath = path;
    tempPath += ".tmp";

    FILE *file = fopen(tempPath.c_str(), "wb");
    if (file == nullptr)
        return false;

    bool ok = fwrite(data, 1, size, file) == size;
    fclose(file);

    if (!ok)
    {
        remove(tempPath.c_str());
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(tempPath, path, ec);
    if (ec)
        return false;

    // Match the permissions a manually pushed driver would have; files created by the
    // app default to 0600, which is unnecessarily tight for something the loader mmaps.
    chmod(path.c_str(), 0755);
    return true;
}

static void WriteTextFileIfMissing(const std::filesystem::path &path, const char *contents)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
        return;

    WriteWholeFile(path, contents, strlen(contents));
}

static void WriteTextFile(const std::filesystem::path &path, const char *contents)
{
    // Most callers write the same text on every launch; leave a file that already has it alone.
    const size_t length = strlen(contents);
    std::vector<uint8_t> current;
    if (ReadWholeFile(path, current) && current.size() == length && memcmp(current.data(), contents, length) == 0)
        return;

    WriteWholeFile(path, contents, length);
}

enum class EAssetRead
{
    Ok,
    Missing, // The APK has no such asset (or an empty one).
    Failed,  // The asset exists but could not be read.
};

// Reads a file bundled in the APK assets (SDL routes relative paths to the asset manager).
static EAssetRead ReadAssetFile(const char *assetPath, std::vector<uint8_t> &data)
{
    SDL_IOStream *rw = SDL_IOFromFile(assetPath, "rb");
    if (rw == nullptr)
        return EAssetRead::Missing;

    Sint64 size = SDL_GetIOSize(rw);
    if (size <= 0)
    {
        SDL_CloseIO(rw);
        return size == 0 ? EAssetRead::Missing : EAssetRead::Failed;
    }

    data.resize(size_t(size));
    bool ok = SDL_ReadIO(rw, data.data(), data.size()) == size_t(size);
    SDL_CloseIO(rw);
    return ok ? EAssetRead::Ok : EAssetRead::Failed;
}

static uint64_t DriverFingerprint(const std::vector<uint8_t> &driver)
{
    // FNV-1a is only a compact log identifier for distinguishing A/B assets. Package
    // integrity still comes from the APK signature and the published SHA-256 checksum.
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : driver)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }

    return hash;
}

static bool IsSafeDriverFileName(const std::string &driverName);

// Imports an adrenotools-style driver package: a zip whose meta.json names the entry
// library ("libraryName") plus the driver binaries - the layout ExynosTools ships for
// Samsung Xclipse and AdrenoToolsDrivers releases use. Every regular file is extracted
// flat (basename only, so hostile entry paths cannot escape) into the turnip dir; the
// adrenotools linker namespace resolves the package's dependent libraries from there.
// Zips without meta.json fall back to selecting the single .so inside.
static bool ImportDriverPackage(const std::filesystem::path &zipPath, const std::filesystem::path &turnipDir, std::string &selectedName)
{
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.string().c_str(), 0))
    {
        LOGF_ERROR("Driver import: {} is not a readable zip archive.", zipPath.filename().string());
        return false;
    }

    struct ZipEntry
    {
        std::string baseName;
        mz_uint index;
    };

    std::vector<ZipEntry> entries;
    std::string libraryName;
    std::string onlySoName;
    uint32_t soCount = 0;

    const mz_uint fileCount = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < fileCount; i++)
    {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory)
            continue;

        std::string baseName = std::filesystem::path(stat.m_filename).filename().string();
        if (baseName.empty())
            continue;

        if (baseName.size() > 3 && baseName.compare(baseName.size() - 3, 3, ".so") == 0)
        {
            ++soCount;
            onlySoName = baseName;
        }

        entries.push_back({ std::move(baseName), i });
    }

    for (const ZipEntry &entry : entries)
    {
        if (entry.baseName != "meta.json")
            continue;

        size_t metaSize = 0;
        void *meta = mz_zip_reader_extract_to_heap(&zip, entry.index, &metaSize, 0);
        if (meta != nullptr)
        {
            // The package is untrusted input: parse without exceptions and take the name
            // only if the document has the expected shape. An exception here would end the
            // process, and the package would do the same on every later launch.
            const auto parsed = nlohmann::json::parse(
                static_cast<const char *>(meta), static_cast<const char *>(meta) + metaSize, nullptr, false);
            if (parsed.is_object())
            {
                const auto name = parsed.find("libraryName");
                if (name != parsed.end() && name->is_string())
                    libraryName = name->get<std::string>();
            }

            mz_free(meta);
        }
        break;
    }

    if (libraryName.empty() && soCount == 1)
        libraryName = onlySoName;

    if (!IsSafeDriverFileName(libraryName))
    {
        LOGF_ERROR("Driver import: {} has no usable meta.json libraryName and does not contain exactly one .so.",
            zipPath.filename().string());
        mz_zip_reader_end(&zip);
        return false;
    }

    bool ok = true;
    for (const ZipEntry &entry : entries)
    {
        size_t size = 0;
        void *data = mz_zip_reader_extract_to_heap(&zip, entry.index, &size, 0);
        if (data == nullptr || !WriteWholeFile(turnipDir / entry.baseName, data, size))
        {
            LOGF_ERROR("Driver import: failed to extract {} from {}.", entry.baseName, zipPath.filename().string());
            ok = false;
        }

        if (data != nullptr)
            mz_free(data);

        if (!ok)
            break;
    }

    mz_zip_reader_end(&zip);
    if (ok)
        selectedName = libraryName;

    return ok;
}

static std::string LowerCaseExtension(const std::filesystem::path &path)
{
    std::string extension = path.extension().string();
    for (char &c : extension)
        c = char(tolower(uint8_t(c)));

    return extension;
}

// Users can drop an arbitrary Turnip build (plain .so, extracted from the release zip)
// or a whole driver package zip (see ImportDriverPackage) into <external>/driver_import/.
// It is copied byte-for-byte to internal storage and selected via driver_name.txt.
//
// Each file is first moved to driver_import/installed/ and only then read: whatever
// happens while it is processed, it is not picked up again on the next launch. A file that
// turns out to be unusable ends up in driver_import/rejected/.
static void ScanDriverImportDir(const std::filesystem::path &importDir, const std::filesystem::path &turnipDir)
{
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator it(importDir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc))
            continue;

        const std::string extension = LowerCaseExtension(it->path());
        if (extension == ".so" || extension == ".zip")
            candidates.push_back(it->path());
    }

    // The last one processed stays selected; make that independent of directory order.
    std::sort(candidates.begin(), candidates.end());

    const std::filesystem::path installedDir = importDir / "installed";
    const std::filesystem::path rejectedDir = importDir / "rejected";

    for (const std::filesystem::path &candidate : candidates)
    {
        const std::string fileName = candidate.filename().string();

        // Whatever comes of it, files in the driver directory may be replaced from here on.
        g_driverFilesImported = true;

        std::filesystem::create_directories(installedDir, ec);
        std::filesystem::path source = installedDir / fileName;
        std::filesystem::rename(candidate, source, ec);

        // Could not be moved: use it where it is and delete it afterwards instead.
        const bool claimed = !ec;
        if (!claimed)
            source = candidate;

        std::string selectedName;
        bool ok = false;

        if (LowerCaseExtension(candidate) == ".so")
        {
            // The loader looks drivers up by a name ending in ".so", in lower case.
            selectedName = candidate.stem().string() + ".so";

            std::vector<uint8_t> driver;
            if (!ReadWholeFile(source, driver))
                LOGF_ERROR("Driver import: failed to read {}", fileName);
            else if (!WriteWholeFile(turnipDir / selectedName, driver.data(), driver.size()))
                LOGF_ERROR("Driver import: failed to install {} to internal storage.", fileName);
            else
                ok = true;
        }
        else
        {
            ok = ImportDriverPackage(source, turnipDir, selectedName);
        }

        if (ok)
        {
            WriteTextFile(turnipDir / "driver_name.txt", selectedName.c_str());
            WriteTextFile(turnipDir / LAST_IMPORTED_DRIVER_FILE, selectedName.c_str());
            LOGF("Driver import: installed {} and selected {}.", fileName, selectedName);
        }

        if (!claimed)
        {
            std::filesystem::remove(source, ec);
        }
        else if (!ok)
        {
            std::filesystem::create_directories(rejectedDir, ec);
            std::filesystem::rename(source, rejectedDir / fileName, ec);
            if (ec)
                std::filesystem::remove(source, ec);
        }
    }
}

static void ProcessDriverImportDir(const std::filesystem::path &turnipDir)
{
    const std::filesystem::path &externalDir = os::android::GetExternalFilesDir();
    if (externalDir.empty())
        return;

    std::error_code ec;
    std::filesystem::path importDir = externalDir / "driver_import";
    std::filesystem::create_directories(importDir, ec);

    // Rewritten every launch so the instructions always match the installed version.
    WriteTextFile(importDir / "readme.txt",
        "Optional: drop a Vulkan driver here as a plain .so file (e.g. a Mesa\n"
        "Turnip libvulkan_freedreno.so) or as a whole driver-package .zip with\n"
        "a meta.json (AdrenoTools packages, ExynosTools for Samsung Xclipse).\n"
        "On the next launch it is installed byte-for-byte and selected.\n"
        "Processed files move to the installed/ subfolder, or to rejected/ if\n"
        "they could not be used (log.txt says why). The launcher's\n"
        "\"Import .so / .zip\" button does the same thing.\n"
        "\n"
        "TURNIP OPTIONS: create a tu_debug.txt file in THIS folder to set\n"
        "Turnip's TU_DEBUG options; it overrides the Render Mode setting.\n"
        "Examples (one per run):\n"
        "  nolrz\n"
        "  sysmem\n"
        "  noubwc\n"
        "\"flushall\" flushes after every draw and is extremely slow; it is for\n"
        "diagnostics only. Delete tu_debug.txt to return control to the Render\n"
        "Mode setting. An fd_dev_features.txt here likewise sets Mesa's\n"
        "FD_DEV_FEATURES (the app sets enable_tp_ubwc_flag_hint=1 on its own on\n"
        "Xiaomi HyperOS 3 and later).\n"
        "\n"
        "LOGS: the app writes log.txt to the PARENT folder (one level up from\n"
        "this driver_import/ folder). If the game freezes, close it and send\n"
        "that log.txt: it records what each thread was doing when frames\n"
        "stopped. The previous run is kept as log_prev.txt.\n"
        "\n"
        "NON-ADRENO DEVICES (Mali / PowerVR / Xclipse): the bundled drivers are\n"
        "Adreno-only, so the game uses the system Vulkan driver unless a driver\n"
        "is imported. On Mali you can experiment with a PanVK build (a plain\n"
        ".so). On Samsung Xclipse, an ExynosTools package zip from\n"
        "https://github.com/WearyConcern1165/ExynosTools can be used instead of\n"
        "the plain system driver.\n");

    ScanDriverImportDir(importDir, turnipDir);

    // Twin folder under Android/media: on-device file managers can browse it on
    // Android 11+ (unlike Android/data), so phone-only users can drop drivers there.
    const std::filesystem::path &mediaDir = os::android::GetExternalMediaDir();
    if (!mediaDir.empty())
    {
        std::filesystem::path mediaImportDir = mediaDir / "driver_import";
        std::filesystem::create_directories(mediaImportDir, ec);
        WriteTextFile(mediaImportDir / "readme.txt",
            "Optional: drop a Vulkan driver here as a plain .so file or as a whole\n"
            "driver-package .zip (AdrenoTools/ExynosTools format). This folder is\n"
            "scanned on launch exactly like Android/data/<app>/files/driver_import/;\n"
            "processed files move to the installed/ or rejected/ subfolder. See the\n"
            "readme.txt there for the Turnip options. The log file (log.txt) is\n"
            "available through the launcher's log button.\n");
        ScanDriverImportDir(mediaImportDir, turnipDir);
    }
}

// What an app-owned bundled-driver slot held when it was last compared with the APK.
struct BundledDriverRecord
{
    bool inApk = false; // The APK ships this driver.
    uint64_t size = 0;
    uint64_t fingerprint = 0;
};

// Brings one app-owned bundled-driver slot up to date with the APK, byte for byte. A
// different imported driver remains selected through driver_name.txt. Returns false if the
// slot could not be brought up to date.
static bool InstallBundledDriverAsset(const std::filesystem::path &turnipDir,
    const BundledDriver &bundled, BundledDriverRecord &record)
{
    record = {};

    std::filesystem::path driverPath = turnipDir / bundled.name;

    std::vector<uint8_t> driver;
    switch (ReadAssetFile(bundled.asset, driver))
    {
        case EAssetRead::Ok:
            break;

        case EAssetRead::Missing:
            // APK built without the bundled driver - nothing to provision.
            return true;

        case EAssetRead::Failed:
        default:
            LOGF_ERROR("Failed to read the bundled Vulkan driver {} from the APK.", bundled.name);
            return false;
    }

    record.inApk = true;
    record.size = driver.size();
    record.fingerprint = DriverFingerprint(driver);

    LOGF("Bundled Vulkan driver asset {}: {} bytes, fingerprint {:016x}.",
        bundled.name, driver.size(), record.fingerprint);

    std::vector<uint8_t> installedDriver;
    const bool alreadyInstalled = ReadWholeFile(driverPath, installedDriver) && installedDriver == driver;
    if (!alreadyInstalled)
    {
        if (!WriteWholeFile(driverPath, driver.data(), driver.size()))
        {
            LOG_ERROR("Failed to extract the bundled Vulkan driver to internal storage.");
            return false;
        }

        LOGF("Updated bundled Vulkan driver at {}.", driverPath.string());
    }

    return true;
}

// The comparison above reads all bundled drivers out of the APK and their copies from
// internal storage: well over a hundred megabytes on every launch, before the game has
// shown anything, to confirm what was already true the last time. Its outcome can only
// change when the APK is replaced or a copy is, so it is written down together with the
// stamp of the APK (see os::android::GetApkStamp) and the size of every copy:
//
//   apk=<stamp>
//   <driver file name>=<size>:<fingerprint>     or "=-" for a driver the APK does not ship
//
// While the stamp still describes what is installed, the comparison is skipped.
static std::string ComposeBundledDriverStamp(const char *apkStamp, const BundledDriverRecord *records)
{
    std::string stamp = fmt::format("apk={}\n", apkStamp);
    for (size_t i = 0; i < std::size(BUNDLED_DRIVERS); i++)
    {
        if (records[i].inApk)
            stamp += fmt::format("{}={}:{:016x}\n", BUNDLED_DRIVERS[i].name, records[i].size, records[i].fingerprint);
        else
            stamp += fmt::format("{}=-\n", BUNDLED_DRIVERS[i].name);
    }

    return stamp;
}

static bool ParseBundledDriverStamp(const std::string &stamp, BundledDriverRecord *records)
{
    // The first line holds the APK stamp; the caller checks it by composing the text again.
    size_t lineEnd = stamp.find('\n');
    for (size_t i = 0; i < std::size(BUNDLED_DRIVERS); i++)
    {
        if (lineEnd == std::string::npos)
            return false;

        const size_t lineStart = lineEnd + 1;
        lineEnd = stamp.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            return false;

        const std::string line = stamp.substr(lineStart, lineEnd - lineStart);
        const std::string key = std::string(BUNDLED_DRIVERS[i].name) + "=";
        if (line.compare(0, key.size(), key) != 0)
            return false;

        const char *value = line.c_str() + key.size();
        unsigned long long size = 0;
        unsigned long long fingerprint = 0;
        if (strcmp(value, "-") == 0)
            records[i] = {};
        else if (sscanf(value, "%llu:%llx", &size, &fingerprint) == 2)
            records[i] = { true, uint64_t(size), uint64_t(fingerprint) };
        else
            return false;
    }

    return true;
}

// True if the stamp file describes this APK and every copy it lists is still in place.
static bool BundledDriversAreCurrent(const std::filesystem::path &turnipDir, const char *apkStamp,
    BundledDriverRecord *records)
{
    std::vector<uint8_t> contents;
    if (!ReadWholeFile(turnipDir / BUNDLED_DRIVER_STAMP_FILE, contents) || contents.size() > 4096)
        return false;

    const std::string stamp(contents.begin(), contents.end());
    if (!ParseBundledDriverStamp(stamp, records) || stamp != ComposeBundledDriverStamp(apkStamp, records))
        return false;

    for (size_t i = 0; i < std::size(BUNDLED_DRIVERS); i++)
    {
        if (!records[i].inApk)
            continue;

        struct stat info {};
        if (stat((turnipDir / BUNDLED_DRIVERS[i].name).c_str(), &info) != 0 ||
            !S_ISREG(info.st_mode) || uint64_t(info.st_size) != records[i].size)
        {
            return false;
        }
    }

    return true;
}

static void RemoveBundledDriverStamp(const std::filesystem::path &turnipDir)
{
    unlink((turnipDir / BUNDLED_DRIVER_STAMP_FILE).c_str());
}

static void InstallBundledDriversIfNeeded(const std::filesystem::path &turnipDir)
{
    // Without a usable stamp of the APK nothing can be skipped.
    const char *apkStamp = os::android::GetApkStamp();
    const bool hasApkStamp = apkStamp[0] != '\0' && strlen(apkStamp) < 256 && strpbrk(apkStamp, "\r\n") == nullptr;

    BundledDriverRecord records[std::size(BUNDLED_DRIVERS)];

    if (hasApkStamp && !g_driverFilesImported && BundledDriversAreCurrent(turnipDir, apkStamp, records))
    {
        for (size_t i = 0; i < std::size(BUNDLED_DRIVERS); i++)
        {
            if (records[i].inApk)
            {
                LOGF("Bundled Vulkan driver {}: {} bytes, fingerprint {:016x} (compared with this APK on an earlier launch).",
                    BUNDLED_DRIVERS[i].name, records[i].size, records[i].fingerprint);
            }
        }
    }
    else
    {
        // The stamp goes first: should this launch end half-way, the next one must not find
        // a stamp that vouches for files which were being replaced.
        RemoveBundledDriverStamp(turnipDir);

        bool upToDate = true;
        for (size_t i = 0; i < std::size(BUNDLED_DRIVERS); i++)
            upToDate = InstallBundledDriverAsset(turnipDir, BUNDLED_DRIVERS[i], records[i]) && upToDate;

        if (upToDate && hasApkStamp)
        {
            const std::string stamp = ComposeBundledDriverStamp(apkStamp, records);
            WriteWholeFile(turnipDir / BUNDLED_DRIVER_STAMP_FILE, stamp.data(), stamp.size());
        }
    }

    WriteTextFileIfMissing(turnipDir / "driver_name.txt", BUNDLED_DRIVER_NAME);
}

static void EnsureVulkanDriverInstalled(const std::string &turnipDirString)
{
    std::filesystem::path turnipDir(turnipDirString);

    std::error_code ec;
    std::filesystem::create_directories(turnipDir, ec);

    ProcessDriverImportDir(turnipDir);
    InstallBundledDriversIfNeeded(turnipDir);
}

static std::string GetCustomDriverName(const std::string &turnipDir)
{
    std::string path = turnipDir + "driver_name.txt";
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr)
        return DEFAULT_DRIVER_NAME;

    char buffer[256]{};
    size_t bytesRead = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);

    while (bytesRead > 0 && (buffer[bytesRead - 1] == '\n' || buffer[bytesRead - 1] == '\r' || buffer[bytesRead - 1] == ' '))
        --bytesRead;

    buffer[bytesRead] = '\0';
    return (bytesRead > 0) ? std::string(buffer) : DEFAULT_DRIVER_NAME;
}

static std::string GetImportedDriverName(const std::string &turnipDir)
{
    const std::filesystem::path turnipPath(turnipDir);
    char buffer[256]{};

    FILE *file = fopen((turnipPath / LAST_IMPORTED_DRIVER_FILE).c_str(), "rb");
    if (file != nullptr)
    {
        size_t bytesRead = fread(buffer, 1, sizeof(buffer) - 1, file);
        fclose(file);

        while (bytesRead > 0 &&
            (buffer[bytesRead - 1] == '\n' || buffer[bytesRead - 1] == '\r' || buffer[bytesRead - 1] == ' '))
        {
            --bytesRead;
        }

        buffer[bytesRead] = '\0';
        if (bytesRead > 0)
            return buffer;
    }

    return {};
}

static bool IsSafeDriverFileName(const std::string &driverName)
{
    const std::filesystem::path path(driverName);
    return !driverName.empty() &&
        path == path.filename() &&
        path.extension() == ".so";
}

static bool ReadTrimmedTextFile(const std::filesystem::path &path, char *buffer, size_t bufferSize)
{
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;

    size_t bytesRead = fread(buffer, 1, bufferSize - 1, file);
    fclose(file);

    while (bytesRead > 0 && (buffer[bytesRead - 1] == '\n' || buffer[bytesRead - 1] == '\r' || buffer[bytesRead - 1] == ' '))
        --bytesRead;

    buffer[bytesRead] = '\0';
    return bytesRead > 0;
}

// Select a deterministic TU_DEBUG default from the restart-required Render Mode option.
// Auto and GMEM both leave sysmem disabled; GMEM is explicit while Auto leaves room for a
// future device policy. An external driver_import/tu_debug.txt (editable over MTP/file
// managers without root) remains the sole diagnostic override and is never rewritten here.
// "flushall" is Mesa's expensive full-flush diagnostic. See
// https://docs.mesa3d.org/drivers/freedreno.html for the full list of TU_DEBUG options.
static void ApplyRenderMode(EAndroidRenderMode renderMode, bool allowDiagnosticOverride,
    const char *driverTuDebugPreset = nullptr)
{
    char buffer[256]{};

    const char *modeName;
    const char *defaultTuDebug;
    if (driverTuDebugPreset != nullptr)
    {
        modeName = "Driver preset";
        defaultTuDebug = driverTuDebugPreset;
    }
    else switch (renderMode)
    {
        case EAndroidRenderMode::GMEM:
            modeName = "GMEM";
            defaultTuDebug = "none";
            break;

        case EAndroidRenderMode::Sysmem:
            modeName = "Sysmem";
            defaultTuDebug = "sysmem";
            break;

        case EAndroidRenderMode::Auto:
        default:
            modeName = "Auto";
            defaultTuDebug = "none";
            break;
    }

    setenv("TU_DEBUG", defaultTuDebug, 1);
    LOGF("Android render mode: {} (default TU_DEBUG=\"{}\").", modeName, defaultTuDebug);

    if (!allowDiagnosticOverride)
    {
        LOG("External TU_DEBUG override is disabled for this driver startup policy.");
        return;
    }

    const std::filesystem::path &externalDir = os::android::GetExternalFilesDir();
    std::filesystem::path externalPath = externalDir / "driver_import" / "tu_debug.txt";

    if (externalDir.empty() || !ReadTrimmedTextFile(externalPath, buffer, sizeof(buffer)))
        return;

    setenv("TU_DEBUG", buffer, 1);
    LOGF("Applied diagnostic TU_DEBUG override from {}: \"{}\" (overrides Render Mode {}).",
        externalPath.string(), buffer, modeName);
}

// Mesa feature flags via FD_DEV_FEATURES. Xiaomi HyperOS 3 ships a display stack whose
// composer misreads Turnip's UBWC layout, which shows as display glitches; Mesa's
// enable_tp_ubwc_flag_hint feature fixes the interop, so it is applied automatically on
// HyperOS 3+. A driver_import/fd_dev_features.txt overrides the value verbatim for
// experiments on other devices (delete the file to return to automatic).
static void ApplyFdDevFeatures()
{
    char buffer[256]{};

    const std::filesystem::path &externalDir = os::android::GetExternalFilesDir();
    if (!externalDir.empty() &&
        ReadTrimmedTextFile(externalDir / "driver_import" / "fd_dev_features.txt", buffer, sizeof(buffer)))
    {
        setenv("FD_DEV_FEATURES", buffer, 1);
        LOGF("Applied FD_DEV_FEATURES override from fd_dev_features.txt: \"{}\".", buffer);
        return;
    }

    // HyperOS exposes its version through ro.mi.os.version.name (e.g. "OS3.0"); MIUI
    // devices and other vendors don't define it.
    char osVersion[PROP_VALUE_MAX]{};
    __system_property_get("ro.mi.os.version.name", osVersion);
    if (osVersion[0] == '\0')
        return;

    const char *digits = osVersion;
    while (*digits != '\0' && (*digits < '0' || *digits > '9'))
        digits++;

    if (atoi(digits) >= 3)
    {
        setenv("FD_DEV_FEATURES", "enable_tp_ubwc_flag_hint=1", 1);
        LOGF("HyperOS {} detected: FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1 (UBWC interop fix).",
            osVersion);
    }
}

static std::string GetNativeLibraryDir()
{
    return os::android::GetNativeLibraryDir().string();
}

void AndroidSetVulkanDriverSelection(EAndroidVulkanDriver driver, EAndroidRenderMode renderMode)
{
    g_configuredVulkanDriver = driver;
    g_configuredRenderMode = renderMode;
}

// Resolves the custom driver once per process and returns the dlopen() handle of the
// libvulkan.so instance that routes to it, or nullptr to use the system driver.
static void *OpenCustomLibVulkan()
{
    // Applies to every driver path (custom and system) - the HyperOS UBWC interop
    // glitch is in the display stack, not in a particular driver build.
    ApplyFdDevFeatures();

    PrepareVulkanStartup();

    // Recovery bypasses custom-driver provisioning/import processing as well as dlopen. This
    // keeps the one-time fallback limited to the platform loader and deterministic Auto mode.
    if (g_forceSafeVulkanFallback)
    {
        // Should the driver that failed be a damaged copy of a bundled one, the launch after
        // this one compares the copies with the APK again and replaces it.
        if (const std::string recoveryTurnipDir = GetTurnipDir(); !recoveryTurnipDir.empty())
            RemoveBundledDriverStamp(recoveryTurnipDir);

        ApplyRenderMode(EAndroidRenderMode::Auto, false);
        LOGF_WARNING("Android Vulkan driver mode: System (boot recovery override; configured mode remains {}).",
            VulkanDriverName(g_configuredVulkanDriver));
        return nullptr;
    }

    std::string turnipDir = GetTurnipDir();
    if (turnipDir.empty())
    {
        LOG_ERROR("Internal storage path unavailable, cannot set up the custom Vulkan driver.");
        return nullptr;
    }

    EnsureVulkanDriverInstalled(turnipDir);

    std::string driverName;
    switch (g_runtimeVulkanDriver)
    {
        case EAndroidVulkanDriver::System:
            LOG("Android Vulkan driver mode: System.");
            return nullptr;

        case EAndroidVulkanDriver::Bundled:
            driverName = BUNDLED_DRIVER_NAME;
            LOG("Android Vulkan driver mode: Bundled.");
            break;

        case EAndroidVulkanDriver::Vauzi710:
            driverName = VAUZI_710_DRIVER_NAME;
            LOG("Android Vulkan driver mode: Adreno 710 (Vauzi v2.7).");
            break;

        case EAndroidVulkanDriver::ExperimentalA725:
            driverName = EXPERIMENTAL_A725_DRIVER_NAME;
            LOG("Android Vulkan driver mode: A725 Performance (experimental CCU COLOR pair).");
            break;

        case EAndroidVulkanDriver::Adreno8xx:
            driverName = ADRENO_8XX_DRIVER_NAME;
            LOG("Android Vulkan driver mode: Adreno 8xx (Mesa 26.3 + gralloc UBWC fix).");
            break;

        case EAndroidVulkanDriver::Imported:
            driverName = GetImportedDriverName(turnipDir);
            LOG("Android Vulkan driver mode: Imported.");
            break;

        case EAndroidVulkanDriver::Auto:
        default:
            // driver_name.txt names the last imported driver, or the bundled one when
            // nothing has been imported.
            driverName = GetCustomDriverName(turnipDir);
            LOG("Android Vulkan driver mode: Auto.");

            // Only replace the generic bundled default: an imported driver stays selected.
            if (driverName == BUNDLED_DRIVER_NAME)
            {
                std::string socModel;
                if (IsValidatedAdreno8xxSoc(socModel))
                {
                    driverName = ADRENO_8XX_DRIVER_NAME;
                    LOGF("Auto: SoC {} (Adreno 830) selects the Adreno 8xx driver.", socModel);
                }
            }
            break;
    }

    std::string gpuDescription;
    const EAndroidGpuFamily gpuFamily = DetectGpuFamily(gpuDescription);
    if (gpuFamily != EAndroidGpuFamily::Adreno && gpuFamily != EAndroidGpuFamily::Unknown)
    {
        // Auto only skips the *bundled* Turnip: an explicitly imported driver selected on a
        // non-Adreno device (e.g. a PanVK build on Mali) stays honored, as do the explicit
        // Bundled/Imported modes. Returning before ArmCustomVulkanStartup keeps this launch
        // out of the crash-recovery cycle entirely.
        if (g_runtimeVulkanDriver == EAndroidVulkanDriver::Auto && driverName == BUNDLED_DRIVER_NAME)
        {
            LOGF_WARNING("Detected non-Adreno GPU (\"{}\"): the bundled Turnip driver is Adreno-only, using the system Vulkan driver.",
                gpuDescription);

            if (gpuFamily == EAndroidGpuFamily::Xclipse)
            {
                LOG("Samsung Xclipse detected. A compatibility package zip from "
                    "https://github.com/WearyConcern1165/ExynosTools can be dropped into driver_import/ to use it instead of the system driver.");
            }

            return nullptr;
        }

        LOGF_WARNING("Custom Vulkan driver \"{}\" is selected on a non-Adreno GPU (\"{}\"); attempting to load it anyway.",
            driverName, gpuDescription);
    }

    if (!IsSafeDriverFileName(driverName))
    {
        LOGF_ERROR("Invalid or unavailable Vulkan driver selection: \"{}\". Falling back to the system driver.", driverName);
        return nullptr;
    }

    struct stat buf {};
    std::string driverPath = turnipDir + driverName;
    if (stat(driverPath.c_str(), &buf) != 0)
    {
        // Auto can recover from a removed imported driver without making the app
        // unbootable. Explicit Imported remains explicit and falls back to System.
        if (g_runtimeVulkanDriver == EAndroidVulkanDriver::Auto && driverName != BUNDLED_DRIVER_NAME)
        {
            driverName = BUNDLED_DRIVER_NAME;
            driverPath = turnipDir + driverName;
        }

        if (stat(driverPath.c_str(), &buf) != 0)
        {
            LOGF_ERROR("Selected Vulkan driver is missing: {}. Falling back to the system driver.", driverPath);
            return nullptr;
        }
    }

    std::string nativeLibraryDir = GetNativeLibraryDir();
    if (nativeLibraryDir.empty())
    {
        LOG_ERROR("The native library directory is unknown, cannot load a custom Vulkan driver.");
        return nullptr;
    }

    if (!ArmCustomVulkanStartup())
    {
        ApplyRenderMode(EAndroidRenderMode::Auto, false);
        return nullptr;
    }

    EAndroidRenderMode effectiveRenderMode = g_runtimeRenderMode;
    const char *driverTuDebugPreset = nullptr;
    if (effectiveRenderMode == EAndroidRenderMode::Auto &&
        g_runtimeVulkanDriver == EAndroidVulkanDriver::Vauzi710)
    {
        effectiveRenderMode = EAndroidRenderMode::Sysmem;
        LOG("Adreno 710 Vauzi driver: Auto render mode selects Sysmem as recommended by the driver author.");
    }

    if (g_runtimeVulkanDriver == EAndroidVulkanDriver::ExperimentalA725)
    {
        effectiveRenderMode = EAndroidRenderMode::Sysmem;
        driverTuDebugPreset = "sysmem,nobin";
        LOG("A725 Performance experimental driver: forcing TU_DEBUG=sysmem,nobin.");
    }

    // Keyed on the resolved file so Auto on a validated SoC gets the same preset.
    if (driverName == ADRENO_8XX_DRIVER_NAME)
    {
        effectiveRenderMode = EAndroidRenderMode::Sysmem;
        driverTuDebugPreset = ADRENO_8XX_TU_DEBUG_PRESET;
        LOGF("Adreno 8xx driver: forcing TU_DEBUG={}.", ADRENO_8XX_TU_DEBUG_PRESET);
    }

    ApplyRenderMode(effectiveRenderMode, driverTuDebugPreset == nullptr, driverTuDebugPreset);

    void *libVulkan = adrenotools_open_libvulkan(
        RTLD_NOW | RTLD_LOCAL,
        ADRENOTOOLS_DRIVER_CUSTOM,
        nullptr, // tmpLibDir: unused on API >= 29 (memfd is used instead)
        nativeLibraryDir.c_str(),
        turnipDir.c_str(),
        driverName.c_str(),
        nullptr,
        nullptr);

    if (libVulkan == nullptr)
    {
        LOG_ERROR("adrenotools_open_libvulkan failed, falling back to the default Vulkan driver.");
        return nullptr;
    }

    if (dlsym(libVulkan, "vkGetInstanceProcAddr") == nullptr)
    {
        LOG_ERROR("Custom Vulkan driver loaded but vkGetInstanceProcAddr symbol is missing.");
        return nullptr;
    }

    LOGF("Successfully loaded custom Vulkan driver ({}) via libadrenotools.", driverName);
    return libVulkan;
}

void *AndroidGetCustomVulkanLibrary()
{
    if (!g_customLoaderResolved)
    {
        // A driver that crashes while it is being loaded takes the process down right
        // here; the crash handler then reports this as the last known location.
        os::android::SetLogLocation("loading the Vulkan driver");
        g_customLibVulkan = OpenCustomLibVulkan();
        g_customLoaderResolved = true;
        os::android::SetLogLocation("starting the renderer");
    }

    return g_customLibVulkan;
}

// plume loads Vulkan through volk, which dlopen()s "libvulkan.so". The Android build
// compiles plume with dlopen redefined to this function (see thirdparty/CMakeLists.txt),
// which is the one seam that lets a custom driver in without patching the submodule.
// Everything that is not the Vulkan loader goes straight to the real dlopen().
void *ShadowRecomp_AndroidVulkanDlopen(const char *filename, int flags)
{
    if (filename != nullptr && (strcmp(filename, "libvulkan.so") == 0 || strcmp(filename, "libvulkan.so.1") == 0))
    {
        void *customLibVulkan = AndroidGetCustomVulkanLibrary();
        if (customLibVulkan != nullptr)
            return customLibVulkan;
    }

    return dlopen(filename, flags);
}

bool AndroidIsCustomVulkanDriverActive()
{
    return g_customLoaderResolved && g_customLibVulkan != nullptr;
}

void AndroidMarkVulkanStartupSuccessful()
{
    if (!g_vulkanStartupPrepared || g_vulkanStartupSuccessReported)
        return;

    if (!g_vulkanStartupStateActive)
    {
        g_vulkanStartupSuccessReported = true;
        return;
    }

    if (!RemoveVulkanStartupState())
    {
        LOG_ERROR("Vulkan boot recovery: device + swapchain are usable, but the startup marker could not be cleared.");
        return;
    }

    g_vulkanStartupStateActive = false;
    g_vulkanStartupSuccessReported = true;
    LOG("Vulkan boot recovery: device + usable swapchain created successfully; startup marker cleared.");
}
