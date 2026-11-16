#pragma once

#include <string_view>

// Android-only diagnostics on top of os::logger. Every log line also goes to
// Android/data/<package>/files/log.txt, which a tester can copy off the device without
// root or adb; these hooks add what makes that file useful for "it froze" reports.
namespace os::android
{
    // Per-frame liveness ping. It feeds a watchdog thread that dumps the state of every
    // thread to log.txt when frames stop being presented.
    void LogHeartbeat();

    // Pauses the watchdog while the game is intentionally frozen in the background, so the
    // missing frames are not reported as a hang.
    void SetWatchdogSuspended(bool suspended);

    // Records what the app is doing ("installing the game", "in game", ...). The value is
    // written to log.txt on every change and repeated by the crash handler, which tells a
    // driver that dies during startup apart from a crash in the middle of a session.
    void SetLogLocation(const std::string_view location);

    // Startup timeline ("[startup]" lines of log.txt), what a "the game takes long to
    // start" report is read off.
    //
    // LogStartupPhase() ends one step of the startup sequence: it records how long the step
    // took, counted from the end of the previous one (from the load of the game library for
    // the first), and how long ago Launch game was pressed in the launcher.
    // LogStartupEvent() records only the latter, for what happens alongside that sequence:
    // the first frame, the end of work left to background threads.
    void LogStartupPhase(const std::string_view phase);
    void LogStartupEvent(const std::string_view event);
}
