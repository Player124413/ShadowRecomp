#include <os/process.h>

#include <os/android/storage_android.h>

#include <climits>
#include <unistd.h>

// There is no meaningful executable on Android: the process image is the system's
// app_process and the game is a shared library inside the APK. The desktop code only uses
// the executable's location as the anchor for everything it reads and writes (it makes it
// the working directory and looks for the game, mods and portable.txt next to it), so the
// "executable" reported here is a virtual file inside the storage root the user chose in
// the launcher. That makes the rest of the code base behave exactly as it does on a PC.
std::filesystem::path os::process::GetExecutablePath()
{
    const std::filesystem::path& root = os::android::GetDataRoot();
    return root.empty() ? std::filesystem::path() : root / "ShadowRecomp";
}

std::filesystem::path os::process::GetExecutableRoot()
{
    return GetExecutablePath().remove_filename();
}

std::filesystem::path os::process::GetWorkingDirectory()
{
    char cwd[PATH_MAX] = {};
    char* res = getcwd(cwd, sizeof(cwd));
    if (res != nullptr)
    {
        return std::filesystem::path(std::u8string_view((const char8_t*)(cwd)));
    }
    else
    {
        return std::filesystem::path();
    }
}

bool os::process::SetWorkingDirectory(const std::filesystem::path& path)
{
    return chdir(path.c_str()) == 0;
}

bool os::process::StartProcess(const std::filesystem::path& path, const std::vector<std::string>& args, std::filesystem::path work)
{
    // An Android app cannot re-execute itself. App::Restart() falls through to App::Exit(),
    // which returns the user to the launcher.
    (void)path;
    (void)args;
    (void)work;
    return false;
}

void os::process::CheckConsole()
{
    // No console on Android; all logging goes to logcat and log.txt (see logger_android.cpp).
    g_consoleVisible = false;
}

void os::process::ShowConsole()
{
    // Unsupported on Android.
}

#error TEMPORARY: deliberate failure to rehearse the failure report of the workflow
