#include "sys.h"
#include "rt_sched.h"
#include "hw.h"
#include "gx_lle.h"
#include "input.h"
#include "settings_ui.h"
#include "first_run.h"
#include "perf.h"
#include "touch.h"
#include "crash_report.h"
#include "plat.h"
#ifdef _WIN32
#include <shellapi.h>
#endif
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <cstdio>
#ifndef _WIN32
#include <unistd.h>
#endif
#if defined(__ANDROID__)
// On Android there is no process to start: SDL3's Java activity loads libmain.so and calls SDL_main, so the entry
// point below is renamed by this header (it is the only way SDL finds it).
#include <SDL3/SDL_main.h>
#endif

extern void f_80003154(Cpu&);

double g_base_scale = 1.0;
bool g_quiet = false;
int g_boot_stage = 0;
bool g_play_stage = false;
bool g_boot_story = false;

// The extracted disc (a folder with sys/main.dol) or a disc image: --disc wins; otherwise look next to the program, so
// a plain double-click works from any working directory (play\shadow.exe finds ..\..\Shadow).
// Android: the launcher starts the game with --disc <file or folder>, both in the app's own folder.
static bool has_disc(const std::string& path) {
  return plat::exists(plat::join(path, "sys" RT_S "main.dol")) || plat::exists(plat::join(path, "sys" RT_S "boot.bin")) ||
         (plat::exists(path) && !plat::is_dir(path));   // an .iso / .gcm image
}

static std::string find_disc() {
  const std::string dir = plat::base_dir();
  const std::string cands[] = {plat::join(dir, "Shadow"), plat::join(dir, "game" RT_S "Shadow"), plat::join(dir, "disc.iso"),
                               plat::join(dir, ".." RT_S "Shadow"), plat::join(dir, ".." RT_S ".." RT_S "Shadow"),
                               "../Shadow"};
  for (const auto& c : cands)
    if (has_disc(c)) return c;
  return plat::join(dir, "Shadow");
}

// UTC time for file names, same shape as the Windows SYSTEMTIME call the log used.
static void local_stamp(int out[6]) {
  const time_t t = time(nullptr);
  struct tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  out[0] = tm.tm_year + 1900;
  out[1] = tm.tm_mon + 1;
  out[2] = tm.tm_mday;
  out[3] = tm.tm_hour;
  out[4] = tm.tm_min;
  out[5] = tm.tm_sec;
}

// The log a crash report survives in: <base>\\logs\\shadow-<time>.log when no console is attached (a Windows-subsystem
// program has no stderr at all when double-clicked; Android keeps its own logcat copy and gets the file too).
static void redirect_log_to_file() {
  const std::string dir = plat::data_path("logs");
  plat::make_dirs(dir);
  int t[6];
  local_stamp(t);
  char path[1024];
  snprintf(path, sizeof path, "%s%sshadow-%04d%02d%02d-%02d%02d%02d.log", dir.c_str(), RT_S, t[0], t[1], t[2], t[3], t[4], t[5]);
  fprintf(stderr, "logging to %s\n", path);
  if (!freopen(path, "w", stderr)) fprintf(stdout, "could not open log %s\n", path);
#ifdef _WIN32
  _putenv_s("SHADOW_LOG_PATH", path);   // bug_capture.cpp reads the log tail back from it
#else
  setenv("SHADOW_LOG_PATH", path, 1);
#endif
}

int main(int argc, char** argv) {
  // Settings and saves live next to the program on the PC and in the app's folder on Android (--data-dir, first).
  for (int i = 1; i + 1 < argc; ++i)
    if (!strcmp(argv[i], "--data-dir")) plat::set_base_dir(argv[i + 1]);
  if (plat::is_android()) {
    // the phone has one folder for everything the player adds: settings, saves, mods, logs and the disc
    plat::make_dirs(plat::base_dir());
  }
  std::string root = find_disc();
  bool have_disc_arg = false;
  bool no_launcher = getenv("SHADOW_NO_LAUNCHER") != nullptr;
  bool builtin_launcher = false;   // --builtin-launcher: the old in-window launcher (launcher.cpp)
  bool with_launcher = false;      // --with-launcher: also open the launcher window (ShadowLauncher.exe / .pyw) as before
  bool edit_touch = false;         // --edit-touch: the launcher's "Edit the touch controls": start with the editor open
  const char* perf_preset = nullptr;   // --perf auto|quality|balanced|performance (the Android launcher's choice)
  int perf_scale = 0;              // --render-scale 0 auto / 1 50 % / 2 75 % / 3 native
  settings::load();   // shadow_settings.ini; command-line flags below override it
  gxlle::g_render_height = settings::g.height;
  gxlle::g_widescreen = settings::g.widescreen;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--disc") && i + 1 < argc) { root = argv[++i]; have_disc_arg = true; }
    else if (!strcmp(argv[i], "--trace-mmio")) g_trace_mmio = true;
    else if (!strcmp(argv[i], "--watch")) sys_set_watch(true);
    else if (!strcmp(argv[i], "--trace-sched")) g_trace_sched = true;
    else if (!strcmp(argv[i], "--trace-dsp")) hw::g_trace_dsp = true;
    else if (!strcmp(argv[i], "--trace-audio")) hw::g_audio_trace = true;
    else if (!strcmp(argv[i], "--trace-gx")) gxlle::g_trace = true;
    else if (!strcmp(argv[i], "--timescale") && i + 1 < argc) g_base_scale = atof(argv[++i]);
    else if (!strcmp(argv[i], "--trace-disc")) g_trace_disc = true;
    else if (!strcmp(argv[i], "--skip-movies")) g_skip_movies = true;
    else if (!strcmp(argv[i], "--mash")) { g_mash = true; g_skip_movies = true; }
    else if (!strcmp(argv[i], "--fast")) { g_mash = true; g_skip_movies = !getenv("TEST_PLAY_MOVIES"); }   // boot -> NEW GAME -> stage load, movies skipped
    else if (!strcmp(argv[i], "--watch-ea") && i + 1 < argc) g_watch_ea = (uint32_t)strtoul(argv[++i], nullptr, 16) & RT_RAM_MASK;
    else if (!strcmp(argv[i], "--press") && i + 1 < argc) input_add_script(argv[++i]);
    else if (!strcmp(argv[i], "--scale") && i + 1 < argc) settings::g.height = gxlle::g_render_height = 480 * atoi(argv[++i]);
    else if (!strcmp(argv[i], "--height") && i + 1 < argc) settings::g.height = gxlle::g_render_height = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--quiet")) g_quiet = true;
    else if (!strcmp(argv[i], "--no-launcher")) no_launcher = true;
    else if (!strcmp(argv[i], "--builtin-launcher")) builtin_launcher = true;
    else if (!strcmp(argv[i], "--with-launcher")) with_launcher = true;
    else if (!strcmp(argv[i], "--hdr")) settings::g.hdr = true;   // testing (not saved)
    else if (!strcmp(argv[i], "--stage") && i + 1 < argc) { g_boot_stage = atoi(argv[++i]); g_mash = true; g_skip_movies = !getenv("TEST_PLAY_MOVIES"); }   // TEST_PLAY_MOVIES: boot movies play (#149 skip test)
    // the launcher's Free Mode / Continue: stage N started the way Select Mode starts it, for players - boot screens
    // auto-confirmed only until the start (as --story), movies normal, memory card writes ON (--stage blocks them)
    else if (!strcmp(argv[i], "--play-stage") && i + 1 < argc) { g_boot_stage = atoi(argv[++i]); g_play_stage = true; g_mash = true; }
    else if (!strcmp(argv[i], "--widescreen")) settings::g.widescreen = gxlle::g_widescreen = true;
    else if (!strcmp(argv[i], "--story")) { g_boot_story = true; g_mash = true; }   // the launcher's Story button: continue the save's story
    // Android: the launcher's device report (soc=..., cores=..., ram_mb=...) for the performance presets (perf.cpp)
    else if (!strcmp(argv[i], "--device-info") && i + 1 < argc) plat::android_set_info(argv[++i]);
    // Android launcher: the graphics preset, the render-resolution slider, the touch overlay and "edit the controls"
    else if (!strcmp(argv[i], "--perf") && i + 1 < argc) perf_preset = argv[++i];
    else if (!strcmp(argv[i], "--render-scale") && i + 1 < argc) perf_scale = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--touch") && i + 1 < argc) touch::set_enabled(atoi(argv[++i]) != 0);
    else if (!strcmp(argv[i], "--edit-touch")) edit_touch = true;
  }
  if (perf_preset) perf::apply(perf_preset);   // after settings::load(): the launcher's preset wins over the file
  if (perf_scale) perf::apply_render_scale(perf_scale);
  if (edit_touch) touch::open_editor();        // the overlay's editor is the control editor; there is no other
  if (getenv("TEST_STORY")) { g_boot_story = true; g_boot_stage = 0; g_mash = true; }   // testing: wsassert's --stage copy, booted as --story
  g_time_scale = g_base_scale;

#ifdef _WIN32
  {   // #151: tabbed out / minimised, Windows 11 power throttling (EcoQoS: efficiency cores, low clocks) and timer
      // throttling slowed the game thread enough to starve the audio queue (stutter). This process opts out of both.
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    pt.StateMask = 0;   // controlled + off = never throttled
    if (!getenv("TEST_ALLOW_THROTTLE")) SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);   // tests: A/B
  }
#endif
  plat::set_timer_resolution(1);
  // When run from a console/launcher without redirection, send the log to <base>\logs\shadow-<time>.log so a crash
  // report survives the window closing. Also when started by double-click: a Windows-subsystem program then has no
  // stderr at all.
  {
    bool console = false;
#ifdef _WIN32
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    console = err != nullptr && err != INVALID_HANDLE_VALUE && _isatty(_fileno(stderr));
#else
    console = isatty(fileno(stderr)) != 0;
#endif
    if (!console || getenv("SHADOW_LOG_TO_FILE")) redirect_log_to_file();
  }
  // A crash is a log nobody can read on a phone: report on the previous run's log and leave this run's marker behind
  // (crash_report.cpp), so "it closed by itself" has an answer in the launcher. Started here, right after the log exists,
  // so a failure anywhere after this point - including the boot itself - is caught.
  crash_report::start();
  // #164 (user: "tabbing out still causes massive lag and audio stutters"): reproduced only with something else busy -
  // a background test game (focus_test.py: 125 fps focused -> 88 tabbed out, audio under-runs). Once the window is not
  // in front, Windows favours the foreground app and our game competes as an equal with every other busy process. The
  // player's game now runs above normal priority, and quiet test games below normal, so they never slow real play.
  // TEST_NORMAL_PRIORITY=1: both normal (A/B).
  if (!getenv("TEST_NORMAL_PRIORITY") && !g_quiet) plat::set_process_priority_above_normal();
#ifdef _WIN32
  // The launcher (launcher.cpp): the window opens on it and the game boots when Play is pressed. Tests and tools
  // (--stage, --fast, --quiet, --no-launcher) go straight into the game.
  const bool plain_start = !no_launcher && !g_quiet && !g_mash && g_boot_stage == 0 && !g_boot_story;
  if (getenv("SHADOW_FORCE_LAUNCHER") || (plain_start && builtin_launcher)) {   // FORCE: tests (quiet window)
    g_disc_root = root;
    launcher::run();
  } else if (plain_start && with_launcher) {
    // #131 (user, 2026-10-20: "scrap launcher and move to the in-game one only"): started directly, the game boots and
    // opens no launcher; settings are in the in-game menu. --with-launcher opens the old launcher window next to it.
    // the launcher is its own window (launcher\shadow_launcher.pyw, Python + Tk): it edits shadow_settings.ini live while
    // the game runs. Opened next to the game when the game is started directly; a second copy just raises the first.
    const std::string dir = plat::base_dir();
    // the launcher: ShadowLauncher.exe (tools/launcher_web, one exe, nothing to install) beside the game or in launcher\,
    // else the Python ones (forge_launcher.pyw, then shadow_launcher.pyw). --from-game: an open launcher stays the only one.
    std::string launcher_exe = plat::join(dir, "ShadowLauncher.exe");
    if (!plat::exists(launcher_exe)) launcher_exe = plat::join(plat::join(dir, "launcher"), "ShadowLauncher.exe");
    std::string script = plat::join(plat::join(dir, "launcher"), "forge_launcher.pyw");
    if (!plat::exists(script)) script = plat::join(plat::join(dir, "launcher"), "shadow_launcher.pyw");
    if (plat::exists(launcher_exe)) {
      ShellExecuteA(nullptr, "open", launcher_exe.c_str(), "--from-game", dir.c_str(), SW_SHOWNORMAL);
    } else if (plat::exists(script)) {
      const std::string args = "\"" + script + "\"";
      ShellExecuteA(nullptr, "open", "pythonw", args.c_str(), dir.c_str(), SW_SHOWNORMAL);
    }
  }
#else
  // Android: the app's launcher (android/app, Java) asks for the disc and starts the game itself; nothing to do here.
  (void)no_launcher; (void)builtin_launcher; (void)with_launcher;
#endif
  // first start without a disc (release folder): ask for the player's own disc, check it, unpack to <base>\Shadow
  // (Windows: the import window. Android: the launcher imports it; if it did not, say so and stop.)
  if (!first_run::disc_present(root)) {
    if (plat::is_android()) {
      fprintf(stderr,
              "[boot] no game files: the launcher imports the player's own Shadow the Hedgehog (USA) disc image. "
              "Start the game from the launcher and choose the disc image there.\n");
      plat::android_toast("No game files yet - open the launcher and add your disc image.");
      return 2;
    }
    if ((root = first_run::run(g_quiet)).empty()) return 0;
  }
  (void)have_disc_arg;
  extern void profiler_start_if_requested();
  profiler_start_if_requested();
  if (!boot_init(root)) {
    // The game came up far enough to have a log but not to run: keep that log with the reason (crash_report.cpp).
    crash_report::fatal("the game could not start");
    crash_report::finish();
    return 1;
  }
  static Cpu cpu;
  memset(&cpu, 0, sizeof cpu);
  cpu.msr = 0x2030;
  cpu.budget = 4000;
  cpu.lr = 0;
  sched_init(cpu);
  sys_install_crash_handlers();
  // The game thread (this one) runs the recompiled code: put it on the fast cores (Android: a phone decides by heat).
  plat::set_high_priority_thread(true);
  logf("[boot] starting guest at 80003154\n");
  f_80003154(cpu);
  logf("[boot] guest returned\n");
  crash_report::finish();
  return 0;
}
