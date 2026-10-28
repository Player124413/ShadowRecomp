#pragma once
// The decision at the heart of runtime/crash_report.cpp: read the end of the previous run's log and say what happened.
//
// Kept apart, and pure, so it can be played through in CI without a phone (tools/android/crash_report_test.cpp) - the same
// arrangement as perf_auto_core.h. The distinction it makes is the one that matters when a player says "it closed":
//
//   Crashed  the runtime's own fault line is in the log: SIGSEGV/SIGBUS/... from rt_sys.cpp's handler, the Aurora FATAL
//            that stops the game deliberately, or an abort/assert. Its guest backtrace is right there, so the report is
//            worth reading.
//   Clean    the run reached an exit path ("[exit] ..."). The marker was left behind by a build without the marker code,
//            or by a path that never called finish(): nothing to report.
//   Stopped  the log simply stops. Android closed the app in the background or the low-memory killer ended it; there is
//            no fault line to read, and saying "it crashed" would be a lie.
#include <string>
#include <vector>

namespace crash_report_core {

enum class Verdict { Crashed, Clean, Stopped };

// How many lines from the end are examined: the fault line is always within the last few of them.
constexpr size_t kLookBack = 200;

inline bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

inline Verdict judge(const std::vector<std::string>& tail) {
  std::string joined;
  for (size_t i = tail.size() > kLookBack ? tail.size() - kLookBack : 0; i < tail.size(); ++i) joined += tail[i] + "\n";
  // "[crash] host signal 11 (SIGSEGV)" / "[crash] host exception C0000005" is rt_sys.cpp's handler, written from inside
  // the fault - and the guest backtrace follows it. (crash_report.cpp's own notes use "[report]", not "[crash]", so a note
  // about an earlier crash cannot be mistaken for a fault in this run.)
  if (contains(joined, "[crash] host")) return Verdict::Crashed;
  if (contains(joined, "FATAL")) return Verdict::Crashed;   // Apollo/Aurora's deliberate stop
  if (contains(joined, "Aborting") || contains(joined, "abort()") || contains(joined, "Assertion")) return Verdict::Crashed;
  if (contains(joined, "terminate called")) return Verdict::Crashed;
  if (contains(joined, "[exit]")) return Verdict::Clean;
  return Verdict::Stopped;
}

// The file name (without the folder) the verdict is kept under: "crash-" for a crash, "stop-" for a system kill.
inline const char* report_prefix(Verdict v) { return v == Verdict::Crashed ? "crash-" : "stop-"; }

// One sentence for the launcher and the log, so the answer is readable without opening the file.
inline const char* reason(Verdict v) {
  return v == Verdict::Crashed ? "the game crashed"
       : v == Verdict::Stopped ? "Android stopped the app (in the background, or out of memory); the log just stops"
                               : "the game was closed";
}

}  // namespace crash_report_core
