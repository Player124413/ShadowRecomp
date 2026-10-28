#pragma once
// Crash and log reporting for a phone, where nobody can read a console and nothing can be attached with a cable.
//
// What this answers, in the order a player hits it:
//   "it just closed"       -> start() finds the marker the previous run left behind, reads the end of that run's log and
//                             keeps it as crashes/crash-<time>.txt (a real crash: the log ends with the runtime's own
//                             SIGSEGV/SIGBUS line and guest backtrace) or crashes/stop-<time>.txt (no error at all: Android
//                             closed the app in the background, or the phone ran out of memory).
//   "here, look at my log" -> tail_text() is what the menu's "Copy the log" row puts in the clipboard, and the share
//                             sheet gets the newest file itself (LogSharing.java); neither needs adb.
//
// The signal handler itself is not repeated here: runtime/rt_sys.cpp already installs one and writes the guest backtrace
// into the log, which is exactly what makes the kept log worth reading.
#include <string>

namespace crash_report {

// Call once, early (main.cpp, after the data directory is known).
void start();

// Call on a clean exit (main.cpp, and sys_exit_guarded): the next start must not think this run crashed.
void finish();

// True when the previous run ended in a real crash (not merely closed by the system).
bool previous_run_crashed();

// One sentence about the previous run, or empty when it ended normally.
const std::string& previous_run();

// The last `max_lines` lines of this run's log (from the log file; the in-memory ring when there is no file yet).
std::string tail_text(size_t max_lines);

// A deliberate abort: keeps the log with `reason` at the top, as crashes/fatal-<time>.txt.
void fatal(const char* reason);

}  // namespace crash_report
