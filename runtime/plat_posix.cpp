// POSIX backend for plat.h: the Android host (and Linux, for developer builds).
//
// Every function here has a Windows twin in plat_win.cpp; the runtime calls plat:: and never a platform header. The
// Win32 names at the bottom (GetFileAttributesA, CreateDirectoryA, ...) exist so the runtime's older code, written
// for the PC, keeps working unchanged: they are the same handful of calls the game always made, on top of plat::.
#include "plat.h"

#ifndef _WIN32

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/resource.h>
#endif

namespace plat {
namespace {
std::string g_base;         // set once (main.cpp: --data-dir, or the folder next to the "exe")
bool g_base_set = false;
std::string g_android_info;

// "\\" is a legal character in a POSIX file name, so it is never meant as anything but a separator here.
std::string native(const std::string& p) {
  std::string out = p;
  for (auto& ch : out)
    if (ch == '\\') ch = '/';
  return out;
}
}  // namespace

std::string base_dir() {
  if (g_base_set) return g_base;
  if (const char* env = getenv("SHADOW_DATA_DIR")) {
    g_base = native(env);
  } else {
    char cwd[4096];
    g_base = getcwd(cwd, sizeof cwd) ? cwd : ".";
    // developer builds: the folder this binary runs from, if it has the game's files next to it
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) {
      self[n] = 0;
      g_base = parent(self);
    }
  }
  g_base_set = true;
  return g_base;
}

void set_base_dir(const std::string& dir) {
  if (dir.empty() || g_base_set) return;
  g_base = native(dir);
  while (g_base.size() > 1 && g_base.back() == '/') g_base.pop_back();
  g_base_set = true;
}

std::string join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (b[0] == '/') return b;
  if (a.back() == '/' || a.back() == '\\') return a + b;
  return a + RT_S + b;
}

std::string parent(const std::string& path) {
  const size_t cut = path.find_last_of("/\\");
  if (cut == std::string::npos) return ".";
  if (cut == 0) return "/";
  return path.substr(0, cut);
}

std::string slash(std::string path) {
  for (auto& ch : path)
    if (ch == '\\') ch = '/';
  return path;
}

std::string data_path(const char* name) { return join(base_dir(), name); }

bool exists(const std::string& path) {
  struct stat st{};
  return stat(native(path).c_str(), &st) == 0;
}

bool is_dir(const std::string& path) {
  struct stat st{};
  return stat(native(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool make_dir(const std::string& path) {
  const std::string p = native(path);
  if (p.empty()) return false;
  if (is_dir(p)) return true;
  return mkdir(p.c_str(), 0775) == 0 || is_dir(p);
}

bool make_dirs(const std::string& path) {
  const std::string p = native(path);
  if (p.empty() || is_dir(p)) return !p.empty();
  // walk the path, creating each missing level
  for (size_t i = 1; i <= p.size(); ++i) {
    if (i != p.size() && p[i] != '/') continue;
    const std::string part = p.substr(0, i);
    if (part.empty() || is_dir(part)) continue;
    if (mkdir(part.c_str(), 0775) != 0 && errno != EEXIST) return false;
  }
  return is_dir(p);
}

uint64_t mtime(const std::string& path) {
  struct stat st{};
  if (stat(native(path).c_str(), &st) != 0) return 0;
  return (uint64_t)st.st_mtim.tv_sec * 10000000ull + (uint64_t)st.st_mtim.tv_nsec / 100ull;
}

std::vector<std::string> list_dir(const std::string& path) {
  std::vector<std::string> out;
  DIR* d = opendir(native(path).c_str());
  if (!d) return out;
  while (struct dirent* e = readdir(d)) {
    const char* n = e->d_name;
    if (!strcmp(n, ".") || !strcmp(n, "..")) continue;
    out.emplace_back(n);
  }
  closedir(d);
  return out;
}

FILE* open(const std::string& path, const char* mode) { return fopen(native(path).c_str(), mode); }

bool remove_file(const std::string& path) { return unlink(native(path).c_str()) == 0; }

bool rename_file(const std::string& from, const std::string& to) {
  return rename(native(from).c_str(), native(to).c_str()) == 0;
}

void set_process_priority_above_normal() {
#ifdef __ANDROID__
  setpriority(PRIO_PROCESS, 0, -8);   // the shell's normal is 0; -8 is allowed for our own process
#else
  nice(-8);
#endif
}

void set_high_priority_thread(bool high) {
#ifdef __ANDROID__
  // The game thread (the one that also runs the recompiled guest code) on the big cores. Little cores would halve
  // the frame rate; Android's scheduler sometimes parks a busy thread there when the device is warm.
  setpriority(PRIO_PROCESS, 0, high ? -12 : 0);
#else
  (void)high;
#endif
}

void set_timer_resolution(int ms) { (void)ms; }

void sleep_ms(int ms) {
  if (ms <= 0) return;
  struct timespec ts{ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, nullptr);
}

uint64_t now_ms() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

void* reserve_memory(size_t bytes) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}

const char* os_name() {
#if defined(__ANDROID__)
  return "android";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

bool is_android() {
#ifdef __ANDROID__
  return true;
#else
  return false;
#endif
}

const std::string& android_info() { return g_android_info; }
void android_set_info(const std::string& kv) {
  if (!kv.empty()) g_android_info = kv;
}

void android_toast(const char* text) {
#ifdef __ANDROID__
  __android_log_print(ANDROID_LOG_INFO, "ShadowRecomp", "%s", text ? text : "");
#else
  fprintf(stderr, "[toast] %s\n", text ? text : "");
#endif
}

void android_keep_screen_on(bool on) {
  (void)on;   // the game activity holds FLAG_KEEP_SCREEN_ON for as long as it is in front (ShadowActivity.java)
}

void android_set_requested_orientation(int landscape) { (void)landscape; }

}  // namespace plat

// ---------------------------------------------------------------------------------------------------------------------
// The Win32 calls the runtime still makes.
// ---------------------------------------------------------------------------------------------------------------------
namespace {
uint64_t file_time(const struct stat& st) {
  return (uint64_t)st.st_mtim.tv_sec * 10000000ull + (uint64_t)st.st_mtim.tv_nsec / 100ull;
}

struct FindState {
  DIR* dir = nullptr;
  std::string pattern;
  bool matches(const char* name) const {
    // a tiny wildcard match: '*' and '?' only (all the runtime uses)
    const char* p = pattern.c_str();
    const char* n = name;
    const char* star = nullptr;
    const char* mark = nullptr;
    while (*n) {
      if (*p == '?' || *p == *n) {
        ++p, ++n;
      } else if (*p == '*') {
        star = p++;
        mark = n;
      } else if (star) {
        p = star + 1;
        n = ++mark;
      } else {
        return false;
      }
    }
    while (*p == '*') ++p;
    return *p == 0;
  }
};
}  // namespace

unsigned long GetModuleFileNameA(void*, char* buf, unsigned long size) {
  const std::string exe = plat::join(plat::base_dir(), "shadow.exe");
  const unsigned long n = (unsigned long)std::min<size_t>(exe.size(), size ? size - 1 : 0);
  if (buf && size) {
    memcpy(buf, exe.data(), n);
    buf[n] = 0;
  }
  return n;
}

DWORD GetFileAttributesA(const char* path) {
  if (!path) return INVALID_FILE_ATTRIBUTES;
  struct stat st{};
  if (stat(plat::slash(path).c_str(), &st) != 0) return INVALID_FILE_ATTRIBUTES;
  return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

bool GetFileAttributesExA(const char* path, int, WIN32_FILE_ATTRIBUTE_DATA* out) {
  if (!path || !out) return false;
  struct stat st{};
  if (stat(plat::slash(path).c_str(), &st) != 0) return false;
  memset(out, 0, sizeof *out);
  out->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
  const uint64_t t = file_time(st);
  out->ftLastWriteTime.dwLowDateTime = (DWORD)(t & 0xFFFFFFFFu);
  out->ftLastWriteTime.dwHighDateTime = (DWORD)(t >> 32);
  out->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
  out->nFileSizeLow = (DWORD)((uint64_t)st.st_size & 0xFFFFFFFFu);
  return true;
}

bool CreateDirectoryA(const char* path, void*) { return path && plat::make_dir(path); }

bool CopyFileA(const char* from, const char* to, int fail_if_exists) {
  if (!from || !to) return false;
  FILE* in = plat::open(from, "rb");
  if (!in) return false;
  if (fail_if_exists && plat::exists(to)) {
    fclose(in);
    return false;
  }
  FILE* out = plat::open(to, "wb");
  if (!out) {
    fclose(in);
    return false;
  }
  char buf[1 << 16];
  size_t n = 0;
  bool ok = true;
  while ((n = fread(buf, 1, sizeof buf, in)) > 0)
    if (fwrite(buf, 1, n, out) != n) {
      ok = false;
      break;
    }
  fclose(in);
  ok = fclose(out) == 0 && ok;
  return ok;
}

bool MoveFileExA(const char* from, const char* to, unsigned) { return from && to && plat::rename_file(from, to); }

uint64_t GetTickCount64() { return plat::now_ms(); }

void Sleep(unsigned ms) { plat::sleep_ms((int)ms); }
void timeBeginPeriod(unsigned) {}
void timeEndPeriod(unsigned) {}

void* VirtualAlloc(void*, size_t size, unsigned, unsigned) { return plat::reserve_memory(size); }

bool VirtualFree(void* addr, size_t size, unsigned) {
  if (!addr) return false;
  return munmap(addr, size) == 0;
}

HANDLE FindFirstFileA(const char* pattern, WIN32_FIND_DATAA* out) {
  if (!pattern || !out) return nullptr;
  std::string p = plat::slash(pattern);
  const size_t cut = p.find_last_of('/');
  const std::string dir = cut == std::string::npos ? "." : (cut == 0 ? "/" : p.substr(0, cut));
  auto* st = new FindState();
  st->pattern = cut == std::string::npos ? p : p.substr(cut + 1);
  st->dir = opendir(dir.c_str());
  if (!st->dir) {
    delete st;
    return nullptr;
  }
  if (FindNextFileA((HANDLE)st, out)) return (HANDLE)st;
  FindClose((HANDLE)st);
  return nullptr;
}

bool FindNextFileA(HANDLE h, WIN32_FIND_DATAA* out) {
  auto* st = (FindState*)h;
  if (!st || !st->dir || !out) return false;
  while (struct dirent* e = readdir(st->dir)) {
    const char* name = e->d_name;
    if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
    if (!st->pattern.empty() && !st->matches(name)) continue;
    const std::string full = std::string(st->dir ? "" : "") + name;   // only the name is needed by the callers
    (void)full;
    struct stat sb{};
    // the runtime only needs the attributes and the size, and it has the folder itself
    memset(out, 0, sizeof *out);
    snprintf(out->cFileName, sizeof out->cFileName, "%s", name);
    if (fstatat(dirfd(st->dir), name, &sb, 0) == 0) {
      out->dwFileAttributes = S_ISDIR(sb.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
      out->nFileSizeHigh = (DWORD)((uint64_t)sb.st_size >> 32);
      out->nFileSizeLow = (DWORD)((uint64_t)sb.st_size & 0xFFFFFFFFu);
    }
    return true;
  }
  return false;
}

bool FindClose(HANDLE h) {
  auto* st = (FindState*)h;
  if (!st) return false;
  if (st->dir) closedir(st->dir);
  delete st;
  return true;
}

FILE* _wfopen(const char* path, const wchar_t* mode) {
  char m[8] = "rb";
  if (mode) {
    int i = 0;
    for (; i < 7 && mode[i]; ++i) m[i] = (char)mode[i];
    m[i] = 0;
  }
  return plat::open(path ? path : "", m);
}

int _fseeki64(FILE* f, long long off, int whence) { return fseeko(f, (off_t)off, whence); }
long long _ftelli64(FILE* f) { return (long long)ftello(f); }
int _wremove(const char* path) { return path ? remove(path) : -1; }

#endif   // !_WIN32
