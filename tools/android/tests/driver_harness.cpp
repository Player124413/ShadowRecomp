// One simulated launch of the app for ShadowRecomp/os/android/vulkan_driver_android.cpp,
// built for the build machine. driver_test.py strings launches together.
//
//   driver_harness <driver 0..6> <render mode 0..2> <reach swapchain 0|1>
//
// Directories and the APK stamp come from the SHADOWRECOMP_* variables (the real
// storage_android.cpp reads them), system properties from PROP_<name with dots as
// underscores>, APK assets from TEST_ASSETS_DIR; TEST_ASSET_READ_FAILS makes every asset
// unreadable. Prints what the launch did as key=value lines on stdout.
#include <os/android/android_settings.h>
#include <os/android/logger_android.h>
#include <os/android/vulkan_driver_android.h>
#include <os/logger.h>
#include <adrenotools/driver.h>
#include <SDL3/SDL.h>
#include <sys/system_properties.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>

static std::string g_openedDriver = "-";
static std::string g_openedDriverDir;
static std::string g_openedHookDir;
static int g_openCalls = 0;
static int g_assetOpens = 0;
static std::string g_location;

// The loader looks this symbol up in what adrenotools returns.
extern "C" __attribute__((visibility("default"))) void vkGetInstanceProcAddr() {}

extern "C" void* adrenotools_open_libvulkan(int, int featureFlags, const char* tmpLibDir, const char* hookLibDir,
    const char* customDriverDir, const char* customDriverName, const char*, void**)
{
    g_openCalls++;
    g_openedDriver = customDriverName ? customDriverName : "(null)";
    g_openedDriverDir = customDriverDir ? customDriverDir : "(null)";
    g_openedHookDir = hookLibDir ? hookLibDir : "(null)";
    if (featureFlags != ADRENOTOOLS_DRIVER_CUSTOM || tmpLibDir != nullptr) g_openedDriver += "(bad-args)";
    if (getenv("TEST_ADRENOTOOLS_FAILS")) return nullptr;
    return dlopen(nullptr, RTLD_NOW); // the harness itself, which exports vkGetInstanceProcAddr
}

extern "C" int __system_property_get(const char* name, char* value)
{
    std::string key = std::string("PROP_") + name;
    for (char& c : key) if (c == '.') c = '_';
    const char* found = getenv(key.c_str());
    value[0] = '\0';
    if (found) { strncpy(value, found, PROP_VALUE_MAX - 1); value[PROP_VALUE_MAX - 1] = '\0'; }
    return int(strlen(value));
}

// SDL's file IO: on Android a relative path resolves to an APK asset.
struct SDL_IOStream { FILE* file; };
extern "C" SDL_IOStream* SDL_IOFromFile(const char* path, const char* mode)
{
    g_assetOpens++;
    const char* assets = getenv("TEST_ASSETS_DIR");
    if (!assets) return nullptr;
    FILE* file = fopen((std::string(assets) + "/" + path).c_str(), mode);
    return file ? new SDL_IOStream{ file } : nullptr;
}
extern "C" Sint64 SDL_GetIOSize(SDL_IOStream* io)
{
    long at = ftell(io->file); fseek(io->file, 0, SEEK_END); long size = ftell(io->file); fseek(io->file, at, SEEK_SET);
    return size;
}
extern "C" size_t SDL_ReadIO(SDL_IOStream* io, void* ptr, size_t size)
{
    return getenv("TEST_ASSET_READ_FAILS") ? 0 : fread(ptr, 1, size, io->file);
}
extern "C" bool SDL_CloseIO(SDL_IOStream* io) { fclose(io->file); delete io; return true; }

namespace os::logger
{
    void Log(const std::string_view str, ELogType type, const char*)
    {
        fprintf(stderr, "[%s] %.*s\n", type == ELogType::Error ? "E" : type == ELogType::Warning ? "W" : "I", int(str.size()), str.data());
    }
}

namespace os::android
{
    void SetLogLocation(const std::string_view location) { g_location = std::string(location); }
}

int main(int argc, char** argv)
{
    if (argc != 4) return 2;
    const auto driver = EAndroidVulkanDriver(atoi(argv[1]));
    const auto mode = EAndroidRenderMode(atoi(argv[2]));
    const bool reachSwapchain = atoi(argv[3]) != 0;

    unsetenv("TU_DEBUG");
    unsetenv("FD_DEV_FEATURES");

    // Before the selection is known nothing must be resolved.
    if (AndroidIsCustomVulkanDriverActive()) return 3;
    AndroidMarkVulkanStartupSuccessful(); // harmless before startup

    AndroidSetVulkanDriverSelection(driver, mode);

    // Not the Vulkan loader: passes straight through, resolves nothing.
    void* self = ShadowRecomp_AndroidVulkanDlopen(nullptr, RTLD_NOW);
    if (self == nullptr || g_openCalls != 0) return 4;

    void* first = ShadowRecomp_AndroidVulkanDlopen("libvulkan.so", RTLD_NOW);
    const bool custom = AndroidIsCustomVulkanDriverActive();
    // volk may ask again (and plume creates more than one interface): resolved once.
    void* second = ShadowRecomp_AndroidVulkanDlopen("libvulkan.so.1", RTLD_NOW);
    if (custom && (first == nullptr || second != first)) return 5;
    if (g_openCalls > 1) return 6;
    if (AndroidGetCustomVulkanLibrary() != (custom ? first : nullptr)) return 7;

    if (reachSwapchain)
    {
        AndroidMarkVulkanStartupSuccessful();
        AndroidMarkVulkanStartupSuccessful(); // called every frame
    }

    const char* tu = getenv("TU_DEBUG");
    const char* fd = getenv("FD_DEV_FEATURES");
    printf("custom=%d\n", custom ? 1 : 0);
    printf("driver=%s\n", g_openedDriver.c_str());
    printf("driver_dir=%s\n", g_openedDriverDir.c_str());
    printf("hook_dir=%s\n", g_openedHookDir.c_str());
    printf("tu_debug=%s\n", tu ? tu : "-");
    printf("fd_dev_features=%s\n", fd ? fd : "-");
    printf("location=%s\n", g_location.c_str());
    printf("asset_opens=%d\n", g_assetOpens);
    fflush(stdout);
    // A launch that never reaches the swapchain dies without any cleanup.
    _Exit(0);
}
