#include <os/android/storage_android.h>

#include <cstdlib>

namespace os::android
{
    static std::filesystem::path PathFromEnvironment(const char* name)
    {
        const char* value = getenv(name);
        return (value != nullptr && value[0] != '\0') ? std::filesystem::path(value) : std::filesystem::path();
    }

    const std::filesystem::path& GetDataRoot()
    {
        static const std::filesystem::path path = PathFromEnvironment("SHADOWRECOMP_DATA_ROOT");
        return path;
    }

    bool HasDataRoot()
    {
        return !GetDataRoot().empty();
    }

    const std::filesystem::path& GetInternalFilesDir()
    {
        static const std::filesystem::path path = PathFromEnvironment("SHADOWRECOMP_INTERNAL_DIR");
        return path;
    }

    const std::filesystem::path& GetExternalFilesDir()
    {
        static const std::filesystem::path path = []()
        {
            std::filesystem::path external = PathFromEnvironment("SHADOWRECOMP_EXTERNAL_DIR");
            return external.empty() ? GetInternalFilesDir() : external;
        }();
        return path;
    }

    const std::filesystem::path& GetExternalMediaDir()
    {
        static const std::filesystem::path path = PathFromEnvironment("SHADOWRECOMP_MEDIA_DIR");
        return path;
    }

    const std::filesystem::path& GetNativeLibraryDir()
    {
        static const std::filesystem::path path = PathFromEnvironment("SHADOWRECOMP_NATIVE_LIB_DIR");
        return path;
    }

    const std::filesystem::path& GetCacheDir()
    {
        static const std::filesystem::path path = []()
        {
            std::filesystem::path cache = PathFromEnvironment("SHADOWRECOMP_CACHE_DIR");
            if (cache.empty() && !GetInternalFilesDir().empty())
                cache = GetInternalFilesDir() / "cache";

            return cache;
        }();
        return path;
    }

    const char* GetAppVersion()
    {
        const char* value = getenv("SHADOWRECOMP_APP_VERSION");
        return (value != nullptr && value[0] != '\0') ? value : "unknown";
    }

    const char* GetApkStamp()
    {
        const char* value = getenv("SHADOWRECOMP_APK_STAMP");
        return value != nullptr ? value : "";
    }
}
