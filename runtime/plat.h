#pragma once
// One platform header for the whole runtime: Windows (the original target) and Android (and other POSIX hosts).
//
// The runtime was written for Windows and called Win32 directly: GetModuleFileNameA for the folder next to the exe,
// GetFileAttributesA, CreateDirectoryA, FindFirstFileA, MoveFileExA, timeBeginPeriod, Sleep, VirtualAlloc. The Android
// port keeps that code readable: every one of those calls has a small portable implementation (the compatibility
// section at the bottom of this file, plat_posix.cpp) so the runtime keeps a single codebase, and new code uses the
// plat:: functions above it.
//
// Where things live:
//   Windows:  settings, saves, mods, logs and the disc sit next to shadow.exe      (plat::base_dir())
//   Android:  the same files sit in the app's private folder, passed by the launcher as --data-dir
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// The path separator the runtime builds paths with: "\\" on Windows, "/" everywhere else. Written as RT_S "name" (the
// compiler joins the pieces), so `dir + RT_S "shadow_settings.ini"` is valid on both. Every literal that used to start
// with "\\" or hold "\\a\\b" now uses this.
#ifdef _WIN32
#define RT_S "\\"
#define RT_SW L"\\"        // the same separator as a wide literal (std::filesystem paths on Windows are wide)
#else
#define RT_S "/"
#define RT_SW L"/"
#endif

namespace plat {
// ---- locations -----------------------------------------------------------------------------------------------------
std::string base_dir();                          // see above: where the game keeps its own files
void set_base_dir(const std::string& dir);       // the launcher's --data-dir (Android); ignored once used
std::string join(const std::string& a, const std::string& b);
std::string parent(const std::string& path);     // the folder part of a path ("a/b/c.txt" -> "a/b")
std::string slash(std::string path);             // '\\' -> RT_S (a no-op on Windows)
std::string data_path(const char* name);         // base_dir() + RT_S + name

// ---- files and folders ---------------------------------------------------------------------------------------------
bool exists(const std::string& path);
bool is_dir(const std::string& path);
bool make_dir(const std::string& path);          // one level, true if it exists afterwards
bool make_dirs(const std::string& path);         // and every missing parent
uint64_t mtime(const std::string& path);         // last write time, 0 if missing (live ini polling)
std::vector<std::string> list_dir(const std::string& path);   // entry names, without "." and ".."
FILE* open(const std::string& path, const char* mode);         // UTF-8 paths; "\\" accepted as a separator
bool remove_file(const std::string& path);
bool rename_file(const std::string& from, const std::string& to);   // replaces `to`

// ---- process and timing --------------------------------------------------------------------------------------------
void set_process_priority_above_normal();
void set_high_priority_thread(bool high);        // the game (render) thread
void set_timer_resolution(int ms);               // Windows timeBeginPeriod; no-op elsewhere
void sleep_ms(int ms);
uint64_t now_ms();
void* reserve_memory(size_t bytes);              // zeroed, read/write (guest RAM)

// ---- identity ------------------------------------------------------------------------------------------------------
const char* os_name();                           // "windows", "android", "linux", "macos"
bool is_android();

// ---- Android glue (no-ops on the desktop) --------------------------------------------------------------------------
// The launcher (android/app, Java) tells the native side about the device and about what the player pressed there.
const std::string& android_info();               // key=value; pairs set by the launcher (soc, cores, ram_mb, gpu, abi)
void android_set_info(const std::string& kv);
void android_toast(const char* text);            // a short Android toast / a log line on the desktop
void android_keep_screen_on(bool on);
void android_set_requested_orientation(int landscape);   // 0 = device default, 1 = landscape, 2 = portrait

}  // namespace plat

// ---------------------------------------------------------------------------------------------------------------------
// Win32 compatibility for non-Windows hosts (plat_posix.cpp). Only what the runtime actually calls. The Windows build
// uses the real thing, so nothing here changes behaviour there.
// ---------------------------------------------------------------------------------------------------------------------
#ifndef _WIN32
#include <ctime>
#include <strings.h>   // strcasecmp
#define MAX_PATH 1024
typedef unsigned long DWORD;
typedef void* HANDLE;

// Windows spellings the runtime still uses
#define FALSE 0
#define TRUE 1
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
inline int localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : -1; }   // as on Windows

#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#define MOVEFILE_REPLACE_EXISTING 0x00000001u
#define GetFileExInfoStandard 0

struct WIN32_FIND_DATAA {
  DWORD dwFileAttributes;
  DWORD nFileSizeHigh, nFileSizeLow;
  char cFileName[MAX_PATH];
};

struct FILETIME {
  DWORD dwLowDateTime, dwHighDateTime;
};

struct WIN32_FILE_ATTRIBUTE_DATA {
  DWORD dwFileAttributes;
  FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
  DWORD nFileSizeHigh, nFileSizeLow;
};

unsigned long GetModuleFileNameA(void* module, char* buf, unsigned long size);   // "<base_dir>/shadow.exe"
DWORD GetFileAttributesA(const char* path);
bool GetFileAttributesExA(const char* path, int level, WIN32_FILE_ATTRIBUTE_DATA* out);
bool CreateDirectoryA(const char* path, void* security);
bool CopyFileA(const char* from, const char* to, int fail_if_exists);
bool MoveFileExA(const char* from, const char* to, unsigned flags);
uint64_t GetTickCount64();
// MSVC intrinsics the runtime was written with (gx_lle.cpp byte-swaps 32-bit words): the compiler's own builtins on
// clang and gcc, same names and types, so the call sites stay as they are.
inline uint32_t _byteswap_ulong(uint32_t v) { return __builtin_bswap32(v); }
inline uint16_t _byteswap_ushort(uint16_t v) { return __builtin_bswap16(v); }
inline uint64_t _byteswap_uint64(uint64_t v) { return __builtin_bswap64(v); }
void Sleep(unsigned ms);
void timeBeginPeriod(unsigned ms);
void timeEndPeriod(unsigned ms);
void* VirtualAlloc(void* addr, size_t size, unsigned type, unsigned protect);
bool VirtualFree(void* addr, size_t size, unsigned type);
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04

void GetSystemTimeAsFileTime(FILETIME* out);                          // UTC, 100 ns units since 1601, as on Windows
int CompareFileTime(const FILETIME* a, const FILETIME* b);            // -1 / 0 / 1

HANDLE FindFirstFileA(const char* pattern, WIN32_FIND_DATAA* out);   // pattern may end in '*'
bool FindNextFileA(HANDLE h, WIN32_FIND_DATAA* out);
bool FindClose(HANDLE h);

FILE* _wfopen(const char* path, const wchar_t* mode);   // the wide-name open, on a UTF-8 path
int _fseeki64(FILE* f, long long off, int whence);
long long _ftelli64(FILE* f);
int _wremove(const char* path);
#endif
