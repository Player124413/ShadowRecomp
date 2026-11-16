// One simulated run of the app for ShadowRecomp/os/android/logger_android.cpp, built for
// the build machine. logger_test.py starts it once per scenario and reads the log files.
//
//   logger_harness <mode> [text for the first log line]
#include <os/android/logger_android.h>
#include <os/logger.h>
#include <android/log.h>
#include <sys/system_properties.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static std::atomic<int> g_logcatLines{ 0 };
extern "C" int __android_log_write(int, const char*, const char*) { g_logcatLines++; return 0; }
extern "C" int __android_log_print(int, const char*, const char*, ...) { g_logcatLines++; return 0; }
extern "C" int __system_property_get(const char* name, char* value)
{
    snprintf(value, PROP_VALUE_MAX, "%s", strcmp(name, "ro.product.model") == 0 ? "TestPhone" : "");
    return int(strlen(value));
}

__attribute__((noinline, no_sanitize("undefined"))) static void Crash()
{
    volatile int* p = nullptr;
    *p = 1;
}

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "";
    const char* arg = argc > 2 ? argv[2] : "";

    os::logger::Init();
    LOGFN("hello from run {}", arg);
    LOG_WARNING("a warning");
    LOGN_ERROR("an error");

    if (mode == "plain")
    {
        os::android::SetLogLocation("in game");
        os::android::SetLogLocation("in game"); // unchanged: logged once
        os::android::SetLogLocation("");
    }
    else if (mode == "stderr")
    {
        fprintf(stderr, "vulkan error one\n");
        fprintf(stderr, "partial ");
        fprintf(stderr, "line two\n");
        usleep(300 * 1000); // the reader thread is asynchronous
    }
    else if (mode == "crash")
    {
        os::android::SetLogLocation("loading the Vulkan driver");
        Crash();
    }
    else if (mode == "abort")
    {
        os::android::SetLogLocation("in game");
        abort();
    }
    else if (mode == "hang")
    {
        for (int i = 0; i < 30; i++) { os::android::LogHeartbeat(); usleep(20 * 1000); }
        sleep(8);                       // frames stop
        for (int i = 0; i < 120; i++) { os::android::LogHeartbeat(); usleep(20 * 1000); }
    }
    else if (mode == "suspended")
    {
        for (int i = 0; i < 30; i++) { os::android::LogHeartbeat(); usleep(20 * 1000); }
        os::android::SetWatchdogSuspended(true);
        sleep(8);                       // backgrounded: no frames, not a hang
        os::android::SetWatchdogSuspended(false);
        for (int i = 0; i < 150; i++) { os::android::LogHeartbeat(); usleep(20 * 1000); }
    }
    else if (mode == "flood")
    {
        // 48 MiB of messages, then a crash that must still be recorded.
        const std::string line(1000, 'x');
        for (int i = 0; i < 48 * 1024; i++) os::logger::Log(line, os::logger::ELogType::None, nullptr);
        os::android::SetLogLocation("after the flood");
        Crash();
    }
    else if (mode == "startup")
    {
        usleep(500 * 1000);
        os::android::LogStartupPhase("first step");
        usleep(80 * 1000);
        os::android::LogStartupPhase("second step");
        os::android::LogStartupEvent("first frame presented");
        // Longer than the line buffer: cut, never overflowed.
        os::android::LogStartupPhase(std::string(1000, 'p'));
        os::android::LogStartupEvent(std::string(1000, 'e'));
    }
    else if (mode == "lengths")
    {
        // Lines around the size of the logger's stack buffer, with and without a function name.
        for (int n = 900; n <= 1100; n++)
        {
            const std::string text = "<" + std::string(size_t(n), char('a' + n % 26)) + ">";
            os::logger::Log(text, os::logger::ELogType::None, n % 2 ? "function_name" : nullptr);
        }
        os::logger::Log(std::string(), os::logger::ELogType::None, nullptr);
        os::logger::Log(std::string(), os::logger::ELogType::Warning, "f");
        os::logger::Log("<" + std::string(200000, 'z') + ">", os::logger::ELogType::Error, "big");
    }
    else if (mode == "threads")
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; t++)
            threads.emplace_back([t]() { for (int i = 0; i < 2000; i++) LOGFN("thread {} line {}", t, i); });
        for (auto& thread : threads) thread.join();
    }
    printf("logcat_lines=%d\n", g_logcatLines.load());
    fflush(stdout);
    _Exit(0);
}
