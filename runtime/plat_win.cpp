// Windows backend for plat.h (the original host). Every function is what the runtime used to do inline.
#include "plat.h"

#ifdef _WIN32

#include <cstring>
#include <string>

namespace plat {
namespace {
std::string g_base;
bool g_base_set = false;

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

void read_exe_dir() {
  char exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string p = exe;
  const size_t cut = p.find_last_of("\\/");
  g_base = cut == std::string::npos ? "." : p.substr(0, cut);
}
}  // namespace

std::string base_dir() {
  if (!g_base_set) {
    read_exe_dir();
    g_base_set = true;
  }
  return g_base;
}

void set_base_dir(const std::string& dir) {
  if (dir.empty() || g_base_set) return;
  g_base = dir;
  g_base_set = true;
}

std::string join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (a.back() == '\\' || a.back() == '/') return a + b;
  return a + RT_S + b;
}

std::string parent(const std::string& path) {
  const size_t cut = path.find_last_of("\\/");
  if (cut == std::string::npos) return ".";
  if (cut == 0) return path.substr(0, 1);
  return path.substr(0, cut);
}

std::string slash(std::string path) {
  for (auto& ch : path)
    if (ch == '/') ch = '\\';
  return path;
}

std::string data_path(const char* name) { return join(base_dir(), name); }

bool exists(const std::string& path) {
  return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool is_dir(const std::string& path) {
  const DWORD a = GetFileAttributesA(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_dir(const std::string& path) { return !path.empty() && (CreateDirectoryA(path.c_str(), nullptr) || is_dir(path)); }

bool make_dirs(const std::string& path) {
  if (path.empty()) return false;
  if (is_dir(path)) return true;
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur += path[i];
    if (path[i] != '\\' && path[i] != '/') continue;
    if (cur.size() > 3) make_dir(cur.substr(0, cur.size() - 1));
  }
  return make_dir(path);
}

uint64_t mtime(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) return 0;
  return ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
}

std::vector<std::string> list_dir(const std::string& path) {
  std::vector<std::string> out;
  WIN32_FIND_DATAA fd{};
  HANDLE h = FindFirstFileA((path + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) out.emplace_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return out;
}

FILE* open(const std::string& path, const char* mode) {
  // The runtime's own paths are ANSI (they always were). A path with non-ANSI text (a player's folder name) is tried
  // again as UTF-8, which is what the launcher and the ini write.
  FILE* f = fopen(path.c_str(), mode);
  if (f) return f;
  const std::wstring wpath = wide(path);
  if (wpath.empty()) return nullptr;
  std::wstring wmode;
  for (const char* m = mode; *m; ++m) wmode += (wchar_t)*m;
  return _wfopen(wpath.c_str(), wmode.c_str());
}

bool remove_file(const std::string& path) { return DeleteFileA(path.c_str()) != 0; }

bool rename_file(const std::string& from, const std::string& to) {
  return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void set_process_priority_above_normal() {
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
}

void set_high_priority_thread(bool high) {
  SetThreadPriority(GetCurrentThread(), high ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_NORMAL);
}

void set_timer_resolution(int ms) { timeBeginPeriod((UINT)ms); }

void sleep_ms(int ms) { Sleep((DWORD)ms); }

uint64_t now_ms() { return GetTickCount64(); }

void* reserve_memory(size_t bytes) {
  return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

const char* os_name() { return "windows"; }
bool is_android() { return false; }

const std::string& android_info() {
  static const std::string empty;
  return empty;
}
void android_set_info(const std::string&) {}
void android_toast(const char* text) { OutputDebugStringA(text ? text : ""); }
void android_keep_screen_on(bool) {}
void android_set_requested_orientation(int) {}

}  // namespace plat

#endif   // _WIN32
