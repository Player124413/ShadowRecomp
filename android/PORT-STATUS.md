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

## Verified here, and what still needs a device

Checked in the development sandbox (no NDK, no JDK, no Gradle):

* every `runtime/*.cpp` compiles with `-D__ANDROID__` against SDL3 3.2.28 / ImGui 1.91.5 headers, except the files that
  need Dawn's `webgpu_cpp.h` and libpng (not available there);
* the link surface was audited with `nm`: the Android-compiled objects reference only SDL, Aurora and the fourteen
  Win32 spellings that `plat_posix.cpp` defines (CopyFileA, CreateDirectoryA, FindFirst/NextFileA, FindClose,
  GetFileAttributes(A/ExA), GetModuleFileNameA, MoveFileExA, VirtualAlloc/Free, _wfseeki64, _wftelli64, _wfopen);
* the CMake `ANDROID` branch configures end to end (`cmake -DCMAKE_TOOLCHAIN_FILE=<fake>` with stubbed Aurora/SDL), so
  `libmain.so` + `libSDL3.so` reaching `jniLibs/` and the Java copy step both work;
* the Java parses (javalang) and matches SDL3's `SDLActivity` API (`getLibraries`, `getArguments`, `createSDLSurface`);
* `tools/android/touch_layout_test.cpp` passes.

The first real build (`android/build.sh`) and the first boot are still to be done on a machine with the NDK; treat that
as the next step, and expect small portability fixes in Dawn's Android linkage and in SDL3's Java package copy.

## Next, in order

1. **On-device build pass.** `android/build.sh`, then the first boot, then fix what the device says (logcat: `SDL`,
   `ShadowRecomp`).
2. **ETC2.** Right now textures are unpacked to RGBA8 before upload (CMPR/BC1 → RGBA8 in `texture_convert.cpp`). On a
   phone that is 4x the texture bandwidth of a console, and bandwidth is the mobile bottleneck. The plan: decode
   BC1 → RGBA8 → re-encode to ETC2 (a BC1→ETC2 transcoder or a small encoder) at load time and upload as
   `ETC2RGB8`/`ETC2RGBA8` where WebGPU exposes it, keeping RGBA8 as the fallback for the desktop and for odd formats.
   This is the biggest single remaining performance item and the only one that touches the renderer.
3. **Frame pacing and thermals.** The main loop spins in the game's own frame wait; on a phone that is heat, and heat
   is lost frames after a few minutes. A short sleep when the frame is ahead of its deadline (and a check that
   `present` blocks on the display) is the fix — small, but it must be measured on a device, not guessed.
4. **Audio buffer.** The DSP block size should follow the device's fast/slow output path instead of the desktop
   constant, so cheap phones do not underrun while the game thread is busy.
5. **Touch polish.** Per-widget anchoring for notched screens; a "left thumb area" hint on first run; the editor's
   panel is drawn bottom-centre and may cover controls on short screens.
6. **Launcher polish.** A first-run screen that explains what to import, and a "check the image" action.

## Testing without a phone

* `tools/android/touch_layout_test.cpp` — the layout format, presets, round trip, damaged files, hit testing:
  `g++ -std=c++20 -Iruntime tools/android/touch_layout_test.cpp runtime/touch_layout.cpp -o t && ./t`
* The launcher passes only command-line arguments (`--disc`, `--perf`, `--render-scale`, `--touch`, `--edit-touch`,
  `--device-info`), so everything it does can be replayed on the desktop build verbatim.
