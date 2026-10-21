# Android port — where it stands

What the port is: one APK for 64-bit phones (arm64-v8a) and emulators (x86_64), a launcher that imports the player's
own disc image and picks a graphics tier, and touch controls that the player edits by hand, over the running picture,
with a finger. No game files are in the repository.

## Done

| Area | Where | Notes |
| --- | --- | --- |
| Build system | `CMakeLists.txt` (`ANDROID` branch), `android/` | `libmain.so` + `libSDL3.so`, Windows sources excluded, Gradle `assembleDebug` |
| Launcher | `android/app/src/main/java/dev/shadowrecomp/LauncherActivity.java` | disc import (SAF), header check, presets, render scale, touch on/off, Play |
| Disc import | `DiscImport.java` | copies to `files/shadow.iso` once, progress, no storage permission |
| Game activity | `ShadowActivity.java` + `dev.encounter.aurora.AuroraSurface` | SDL3 activity, Aurora's surface, arguments from the launcher |
| Disc reading | `runtime/disc.cpp`, `runtime/first_run_android.cpp` | image read in place (no unpack), `GUPE8P` checked natively too |
| Touch input | `runtime/touch.cpp`, `runtime/input.cpp` | SDL3 fingers → pad bits/sticks; merged with gamepad and keyboard |
| Touch layout format | `runtime/touch_layout.{h,cpp}` | presets, `touch_layouts.ini`, hit testing; portable test in `tools/android/` |
| Touch editor | `runtime/touch.cpp` (`EDIT` control) | drag / size / opacity / shape / rebind / add / remove / new layout / presets |
| Graphics tiers | `runtime/perf.{h,cpp}` | auto from chip/cores/RAM, quality/balanced/performance, `--perf`/`--render-scale` |
| Platform layer | `runtime/plat.h`, `plat_posix.cpp` | one `base_dir()/data_path()/open()/sleep`; Win32 spellings shimmed on POSIX |
| Build one command | `android/build.sh` | JDK/SDK/Gradle discovery, `tools/gen.py` check, APK path printed |
| Frame pacing | `runtime/frame_rate.cpp` | sleeps to the frame deadline on Android instead of spinning (FR_NO_PACE=1 A/B) |
| Fonts | `runtime/game_menu.cpp` | the phone's own Roboto when the Windows fonts are absent (menu stays readable) |
| ETC2 textures | `third_party/aurora/lib/gfx/etc2_encode.cpp`, `etc2_transcode.cpp`, `texture.cpp` | CMPR (BC1) textures are decoded to RGBA8 as before and then re-encoded to ETC2 instead of being uploaded as 32-bit RGBA8: 4 bits per pixel instead of 32, plus an EAC alpha block only when the texture uses alpha. Falls back to RGBA8 when the adapter has no `TextureCompressionETC2` or the format is not worth the loss. `SHADOW_ETC2=1` forces every RGB source through it, `=0` turns it off; the running total is logged (`aurora::etc2`) |
| Adaptive resolution | `runtime/dynamic_resolution.{h,cpp}`, `runtime/gx_lle.cpp` | render height follows delivered frame times (on for Android), 720-line cap while a movie streams |
| CI | `.github/workflows/android-apk.yml`, `tools/android/stub_generated.py`, `tools/android/ci_errors.py` | GitHub Actions, three jobs: the disc-free tests; a full arm64 compile of the native port without the disc; and an APK that is really built and uploaded on every push - the real one when `SHADOW_GEN_URL` holds your `generated/` tarball, otherwise a launcher-only one that links stand-ins for the game code (so the packaging path is never untested). Failures come back as check-run annotations carrying the include chain, the failing command and the linker's complaints |

## Verified here, and what still needs a device

**The arm64 build is green in CI.** On every push, GitHub's machines configure the project with the real NDK (r27,
`arm64-v8a`, API 26), compile every translation unit of `runtime/`, Aurora and the Android glue with the stub
`generated/`, and then - in the APK job - link `libmain.so` against `libSDL3.so` and assemble, sign (debug) and upload a
9.5 MB APK. What that first real compile and link found, all fixed:

* Bionic has no `getcontext`/`makecontext`/`swapcontext`: `runtime/sched.cpp` has a pthread + `sem_t` fiber backend.
* Android's libc++ has no `std::jthread`, `std::stop_token` or `std::stop_callback`: `third_party/aurora/lib/thread.hpp`
  provides `thread::StopToken`/`thread::StopCallback` over a shared stop flag, with jthread semantics (`request_stop()`
  runs the callbacks, the destructor requests stop and joins).
* Windows-only spellings the runtime uses (`_byteswap_*`, `GetSystemTimeAsFileTime`, `CompareFileTime`) are in the POSIX
  layer.
* `std::filesystem::path::native()` is narrow on POSIX and wide on Windows, and several maps are keyed by `wstring`:
  `texture_pack.cpp` now asks for `wstring()` everywhere it builds a key (`RT_SW` for the separators).
* A cross build must not adopt the build machine's sqlite3 or zstd: `third_party/aurora/extern/CMakeLists.txt` skips
  system/pkg-config lookups when `CMAKE_CROSSCOMPILING`, and the top-level `CMakeLists.txt` builds the SQLite
  amalgamation for the target ABI before Aurora looks for one.
* Aurora's surface lock calls SDL's `Android_LockActivityMutex`, which SDL3 does not export (its shared library is
  limited by a linker version script): `runtime/android_jni.cpp` provides the two symbols over a mutex of its own - a
  documented compromise, since SDL's own mutex is internal.
* `settings::init_fonts()` lived in `runtime/launcher.cpp`, which the Android build does not compile (the launcher
  there is Java): the Android definition lives in `settings_ui.cpp` and sets up the in-game menu's fonts.

Checked in the development sandbox (no NDK, no JDK, no Gradle):

* `tools/android/check_android_syntax.sh` compiles every `runtime/*.cpp` with `-D__ANDROID__` on the host compiler in
  about 30 seconds (56 files checked, 13 skipped for Dawn's generated headers, which are not present here). That is the
  fast loop for the "this only compiles on Windows" kind of mistake;
* the Java parses (javalang) and matches SDL3's `SDLActivity` API (`getLibraries`, `getArguments`, `createSDLSurface`);
* `tools/android/touch_layout_test.cpp` passes, and the CI job runs it;
* the ETC2 encoder's blocks decode correctly in `texture2ddecoder`, an independent implementation: 37 dB on a smooth
  gradient, 55 dB on an alpha ramp, ~10-12 dB on UI art and noise (that is the format, not a bug - see below). CI runs
  the same check.

**Not verified yet: the first boot on a phone.** Everything above is build-time. What the device has to answer is in
the list below (logcat tags `SDL`, `ShadowRecomp`).

## Next, in order

1. **First boot on a device.** Install the APK (own disc + `SHADOW_GEN_URL`, or `android/build.sh` on a machine with the
   NDK), watch logcat, and fix what the phone says: Dawn's Vulkan surface on Android, audio device, frame pacing.
2. **ETC2 quality: the extra modes.** The encoder uses ETC1's two modes (individual and differential) plus EAC alpha.
   ETC2's T, H and planar modes are what etcpak uses, and the gap is measurable: on synthetic 128x128 images, decoded by
   the same independent decoder, ours is 41.7 dB against etcpak's 49.8 dB on a gradient and 11.6 vs 14.7 dB on
   two-tone art. Worth doing (UI art in this game is exactly the two-tone case), not urgent: the default path only
   re-encodes textures that were *already* BC1 on the console, so it adds no new loss. `tools/android/etc2_check.py`
   guards the current numbers, so a mode can be added and measured before it is trusted.
3. **Audio buffer.** The DSP block size should follow the device's fast/slow output path instead of the desktop
   constant, so cheap phones do not underrun while the game thread is busy.
4. **Touch polish.** Per-widget anchoring for notched screens; a "left thumb area" hint on first run; the editor's
   panel is drawn bottom-centre and may cover controls on short screens.
5. **Launcher polish.** A first-run screen that explains what to import, and a "check the image" action.

## Testing without a phone

* `tools/android/stub_generated.py` writes a stand-in `generated/`. Without `--definitions` it is declarations only,
  which is enough to *compile* the port (that is the CI compile job); with `--definitions` it also writes empty, weak
  bodies plus the empty function tables, which is enough to *link* and package an APK - that is the launcher-only
  build, and it is what the CI APK job does when there is no disc code.
* `android/build.sh` with `SHADOW_STUB_GAME_CODE=1` does the same thing through Gradle, one command, no disc needed.
* `tools/android/check_android_syntax.sh` - the 30-second host-side compile check described above;
  `SHADOW_SDL3_INCLUDE`, `SHADOW_IMGUI_INCLUDE`, `SHADOW_XXHASH_INCLUDE`, `SHADOW_FMT_INCLUDE` and `SHADOW_DAWN_INCLUDE`
  widen it when those headers are available.
* `tools/android/etc2_check.cpp` + `etc2_check.py` — the ETC2 encoder against an independent decoder:
  `g++ -std=c++20 -Ithird_party/aurora/lib tools/android/etc2_check.cpp third_party/aurora/lib/gfx/etc2_encode.cpp -o t
  && ./t blocks.bin && python3 tools/android/etc2_check.py blocks.bin` (needs `pip install texture2ddecoder`).
* `tools/android/touch_layout_test.cpp` — the layout format, presets, round trip, damaged files, hit testing:
  `g++ -std=c++20 -Iruntime tools/android/touch_layout_test.cpp runtime/touch_layout.cpp -o t && ./t`
* The launcher passes only command-line arguments (`--disc`, `--perf`, `--render-scale`, `--touch`, `--edit-touch`,
  `--device-info`), so everything it does can be replayed on the desktop build verbatim.
