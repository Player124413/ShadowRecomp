// See crash_report.h.
#include "crash_report.h"

#include "build_info.h"
#include "crash_report_core.h"
#include "log_ring.h"
#include "plat.h"
#include "sys.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace crash_report {
namespace {

std::string g_previous;       // a sentence about the previous run (empty = it ended normally)
bool g_previous_crashed = false;
std::string g_dir;            // <data>/crashes
std::string g_marker;         // <data>/run.open, present while this run is running

// "2026-10-28_19-31-04": local time, sorts correctly, no colons (a Windows path cannot have one either).
void stamp(char* out, size_t n) {
  const time_t t = time(nullptr);
  struct tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  snprintf(out, n, "%04d-%02d-%02d_%02d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
           tm.tm_sec);
}

// The last `max_lines` lines of a file. The in-memory ring (log_ring) keeps 400 lines, which says what broke but not what
// led to it; the log file on disk is the whole run, and reading its end costs nothing.
std::vector<std::string> file_tail(const std::string& path, size_t max_lines) {
  std::vector<std::string> out;
  FILE* f = fopen(path.c_str(), "rb");
  if (f == nullptr) return out;
  std::vector<char> buf;
  if (fseek(f, 0, SEEK_END) == 0) {
    const long size = ftell(f);
    const long want = 256 * 1024;   // the last 256 KB: more than anyone reads
    fseek(f, size > want ? size - want : 0, SEEK_SET);
    buf.resize((size_t)want);
    buf.resize(fread(buf.data(), 1, buf.size(), f));
    if (size > want) {   // a line cut in half at the start of the window would read as garbage
      auto nl = std::find(buf.begin(), buf.end(), '\n');
      if (nl != buf.end()) buf.erase(buf.begin(), nl + 1);
    }
  }
  fclose(f);
  size_t start = 0;
  for (size_t i = 0; i <= buf.size(); ++i) {
    if (i == buf.size() || buf[i] == '\n') {
      if (i > start) out.emplace_back(buf.data() + start, i - start);
      start = i + 1;
    }
  }
  if (out.size() > max_lines) out.erase(out.begin(), out.end() - (long)max_lines);
  return out;
}

void write_lines(const std::string& path, const std::string& header, const std::vector<std::string>& lines) {
  FILE* f = fopen(path.c_str(), "wb");
  if (f == nullptr) return;
  fwrite(header.data(), 1, header.size(), f);
  for (const auto& l : lines) {
    fwrite(l.data(), 1, l.size(), f);
    fputc('\n', f);
  }
  fclose(f);
}

std::string header_for(const std::string& first_line) {
  std::string h = first_line + "\n";
  h += "build: " + std::string(build_info::text()) + "\n";
  h += "device: " + plat::android_info() + "\n";
  const char* log = getenv("SHADOW_LOG_PATH");
  h += "log: " + std::string(log != nullptr && *log ? log : "(none)") + "\n\n";
  return h;
}

std::string marker_line(int which) {
  FILE* f = fopen(g_marker.c_str(), "rb");
  if (f == nullptr) return {};
  std::string lines[2];
  char buf[1024];
  for (int i = 0; i < 2 && fgets(buf, sizeof buf, f) != nullptr; ++i) {
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    lines[i] = s;
  }
  fclose(f);
  return lines[which];
}

// The folder the reports go into; also called from fatal(), which the desktop build reaches without start().
const std::string& ensure_dir() {
  if (g_dir.empty()) {
    g_dir = plat::data_path("crashes");
    plat::make_dirs(g_dir);
  }
  return g_dir;
}

void write_marker() {
  char t[64];
  stamp(t, sizeof t);
  const char* log = getenv("SHADOW_LOG_PATH");
  if (FILE* f = fopen(g_marker.c_str(), "wb")) {
    fprintf(f, "%s\n%s\n", t, log != nullptr ? log : "");
    fclose(f);
  }
}

}  // namespace

void start() {
  // The phone is the reason this exists: on a PC the log is already a file the player can attach, and a marker lying next
  // to shadow.exe would be one more thing the test scenarios have to know about.
  if (!plat::is_android()) return;
  ensure_dir();
  g_marker = plat::data_path("run.open");

  // A marker from an earlier run means that run never reached finish(). Keep its log where the launcher can offer it, and
  // read the end of it to say what happened - a crash leaves the runtime's own signal line, a system kill leaves nothing.
  if (plat::exists(g_marker)) {
    const std::string previous_log = marker_line(1);
    std::vector<std::string> tail;
    if (!previous_log.empty()) tail = file_tail(previous_log, 400);
    const crash_report_core::Verdict v = crash_report_core::judge(tail);
    char t[64];
    stamp(t, sizeof t);
    const std::string name = std::string(crash_report_core::report_prefix(v)) + t + ".txt";
    const std::string out = g_dir + RT_S + name;
    write_lines(out, header_for(std::string("ShadowRecomp's previous run did not finish: ") + crash_report_core::reason(v) + "."), tail);
    if (v == crash_report_core::Verdict::Clean && tail.empty()) {   // an old build's leftover: nothing to say
      remove(out.c_str());
    } else {
      g_previous = std::string("the previous run did not finish (") + crash_report_core::reason(v) + "); its log is saved as crashes" RT_S + name;
      g_previous_crashed = v == crash_report_core::Verdict::Crashed;
    }
    remove(g_marker.c_str());
  }

  write_marker();
  if (!g_previous.empty()) logf("[report] %s\n", g_previous.c_str());
}

void finish() {
  if (!g_marker.empty()) remove(g_marker.c_str());   // this run ended on purpose: nothing for the next start to report
}

bool previous_run_crashed() { return g_previous_crashed; }
const std::string& previous_run() { return g_previous; }

std::string tail_text(size_t max_lines) {
  std::vector<std::string> lines;
  const char* path = getenv("SHADOW_LOG_PATH");
  if (path != nullptr && *path) lines = file_tail(path, max_lines);
  if (lines.empty()) lines = log_ring::lines(max_lines);
  std::string out;
  for (const auto& l : lines) { out += l; out += '\n'; }
  return out;
}

void fatal(const char* reason) {
  char t[64];
  stamp(t, sizeof t);
  const std::string out = ensure_dir() + RT_S "fatal-" + t + ".txt";
  std::vector<std::string> lines;
  const char* path = getenv("SHADOW_LOG_PATH");
  if (path != nullptr && *path) lines = file_tail(path, 400);
  if (lines.empty()) lines = log_ring::lines(400);
  write_lines(out, header_for(std::string("ShadowRecomp stopped because of an error: ") + (reason != nullptr ? reason : "?") +
                              "\nThe reason line names what the renderer refused; the log follows."),
               lines);
  fflush(stderr);
}

}  // namespace crash_report
