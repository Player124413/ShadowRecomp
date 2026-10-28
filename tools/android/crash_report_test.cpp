// The verdict runtime/crash_report.cpp puts on a previous run, played through real log tails - no phone, no filesystem.
//
// Why this is worth a test: "the game closed" is answered from these lines alone, and both mistakes are bad. Calling a
// system kill a crash sends the player looking for a bug that is not there; calling a crash a system kill throws away the
// one report that would explain it. The cases below are the shapes a real log ends in.
//
//   g++ -std=c++20 -O2 -Wall -Wextra -Iruntime tools/android/crash_report_test.cpp -o /tmp/crash_report_test && /tmp/crash_report_test
#include "crash_report_core.h"

#include <cstdio>
#include <string>
#include <vector>

using crash_report_core::Verdict;

namespace {
int g_failed = 0;

void check(const char* what, Verdict got, Verdict want) {
  if (got == want) {
    printf("ok    %s\n", what);
    return;
  }
  ++g_failed;
  const char* names[] = {"crashed", "clean", "stopped"};
  printf("FAIL  %s: got %s, wanted %s\n", what, names[int(got)], names[int(want)]);
}

std::vector<std::string> lines(std::initializer_list<const char*> l) {
  std::vector<std::string> out;
  for (const char* s : l) out.emplace_back(s);
  return out;
}

// A real boot: the runtime's lines, then the SIGSEGV that rt_sys.cpp wrote from inside the handler.
std::vector<std::string> boot_then_sigsegv() {
  std::vector<std::string> l;
  for (int i = 0; i < 500; ++i) l.push_back("[gx] frame " + std::to_string(i));
  l.push_back("[auto] 47 fps: glow off");
  l.push_back("");
  l.push_back("[crash] host signal 11 (SIGSEGV)");
  l.push_back("guest backtrace: 80003154 -> 8004A1C8 -> 800F0020");
  return l;
}
}  // namespace

int main() {
  // ---- a real crash: the handler's own line, with the guest backtrace after it
  check("SIGSEGV in the runtime", crash_report_core::judge(boot_then_sigsegv()), Verdict::Crashed);
  check("SIGBUS in the runtime", crash_report_core::judge(lines({"[gx] draw", "[crash] host signal 7 (SIGBUS)"})),
        Verdict::Crashed);
  check("host exception (Windows spelling of the same handler)",
        crash_report_core::judge(lines({"[gx] draw", "[crash] host exception C0000005 at 00007FF6"})), Verdict::Crashed);

  // ---- a deliberate stop
  check("Aurora FATAL", crash_report_core::judge(lines({"[gx] present", "FATAL: WebGPU validation error in draws[3]"})),
        Verdict::Crashed);
  check("std::terminate", crash_report_core::judge(lines({"terminate called after throwing an instance of 'std::bad_alloc'"})),
        Verdict::Crashed);
  check("an assertion", crash_report_core::judge(lines({"Assertion failed: stride % 64 == 0"})), Verdict::Crashed);

  // ---- Android closed the app: the log simply stops, mid-frame, with no fault line
  check("killed while drawing (the log just stops)", crash_report_core::judge(lines({"[gx] frame 900", "[gx] draw 12"})),
        Verdict::Stopped);
  check("killed with an empty log", crash_report_core::judge(lines({})), Verdict::Stopped);
  check("killed right after a report note", crash_report_core::judge(lines({"[report] the previous run did not finish"})),
        Verdict::Stopped);

  // ---- a run that reached its own exit path: nothing to report
  check("a clean fast exit", crash_report_core::judge(lines({"[gx] frame 900", "[exit] fast exit (code 0)"})), Verdict::Clean);

  // ---- only the end counts: a fault line 400 lines back belongs to an earlier, already-reported run
  {
    std::vector<std::string> l = boot_then_sigsegv();
    for (int i = 0; i < 300; ++i) l.push_back("[gx] frame later " + std::to_string(i));
    check("a fault line far back is not this run's end", crash_report_core::judge(l), Verdict::Stopped);
  }

  // ---- what the files are called: the launcher tells a crash from a system stop by the name alone
  check("a crash is kept as crash-*.txt", crash_report_core::report_prefix(Verdict::Crashed) == std::string("crash-") ? Verdict::Clean : Verdict::Crashed,
        Verdict::Clean);
  check("a system stop is kept as stop-*.txt", crash_report_core::report_prefix(Verdict::Stopped) == std::string("stop-") ? Verdict::Clean : Verdict::Crashed,
        Verdict::Clean);

  if (g_failed == 0) printf("\nall crash-report cases passed\n");
  return g_failed == 0 ? 0 : 1;
}
