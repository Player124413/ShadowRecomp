#include <os/android/lifecycle_android.h>

#include <os/android/logger_android.h>

#include <condition_variable>
#include <mutex>

namespace os::android
{
    static std::mutex s_pauseMutex;
    static std::condition_variable s_pauseChanged;
    static bool s_paused;

    void SetAppPaused(bool paused)
    {
        {
            std::lock_guard lock(s_pauseMutex);
            if (s_paused == paused)
                return;

            s_paused = paused;
        }

        // Frames stop on purpose while paused; do not let the watchdog call that a hang.
        SetWatchdogSuspended(paused);
        s_pauseChanged.notify_all();
    }

    bool IsAppPaused()
    {
        std::lock_guard lock(s_pauseMutex);
        return s_paused;
    }

    bool WaitWhileAppPaused()
    {
        std::unique_lock lock(s_pauseMutex);
        if (!s_paused)
            return false;

        s_pauseChanged.wait(lock, [] { return !s_paused; });
        return true;
    }
}
