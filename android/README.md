# ShadowRecomp for Android (64-bit)

One APK, one Gradle project. It contains **no game files**: the launcher asks the player for their own
Shadow the Hedgehog (USA) disc image (`GUPE8P`), copies it into the app's private folder, and the game reads that
image in place (nothing is unpacked).

Two parts:

* `app/src/main/java/dev/shadowrecomp/` — the launcher. `LauncherActivity` (import the disc, graphics preset, render
  resolution, touch controls on/off, Play), `ShadowActivity` (the game: SDL3's `SDLActivity` with Aurora's surface),
  `DiscImport` (SAF copy + header check), `DeviceInfo` (chip / cores / RAM for the `auto` preset).
* the native side — `../../CMakeLists.txt`, built by `externalNativeBuild` into `libmain.so`. SDL3's Java classes and
  Aurora's `dev.encounter.aurora.AuroraSurface` are copied into `app/src/main/java` by CMake at configure time.

## What you need

* JDK 17, Android SDK (platform 35, build-tools 35), NDK **27**, CMake 3.25 (the SDK ships one),
  Python 3 (for `tools/gen.py`).
* The recompiled game code: `python tools/gen.py` in the repository root (it is never committed).

## Build

```sh
python tools/gen.py                 # once, from the repository root

cd android
# local.properties is not committed: point it at your SDK
echo "sdk.dir=$HOME/Android/Sdk" > local.properties

gradle :app:assembleDebug          # or: ./gradlew (add the wrapper once), or Android Studio > Open android/
```

The APK lands in `app/build/outputs/apk/debug/app-debug.apk`. `arm64-v8a` (every 64-bit phone) and `x86_64`
(emulator) are built; 32-bit ABIs are deliberately not built.

Install and watch the log:

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb logcat -s ShadowRecomp SDL  # the game logs to logcat and to <files>/logs/
```

## First run

1. The launcher opens. **Import disc image…** → pick your own ISO/GCM. It is checked on the spot (GameCube magic,
   game id `GUPE8P`, first USA revision) and copied to `files/shadow.iso`.
2. Pick a graphics preset (auto is fine — it reads the chip, cores and RAM and starts from the tier that fits).
3. **Play**. The first frame boots exactly like the desktop build.

## Touch controls

They are on by default. `MENU` opens the in-game settings menu, `EDIT` opens the **control editor**: drag a control
anywhere with a finger, pick it to change its size, opacity, shape and the button it presses; add and remove controls;
change the whole picture's size / opacity / left-handed mode; save as a new layout. Presets: *classic*,
*left_handed*, *minimal*. The result is `touch_layouts.ini` in the app's folder:

```
active=classic
scale=100
opacity=100
[classic]
stick_mode=1          # 0 fixed, 1 floating (it appears where the thumb lands)
deadzone=15
haptics=1
w=stick,0,0.1500,0.7200,0.1250,0.450,0,1     # kind,action,x,y,radius,opacity,style,visible
w=button,0,0.8850,0.7350,0.0560,0.550,0,1
```

x/y are 0..1 of the screen, the radius is of the screen's short side, alpha 0..1, style 0 round / 1 diamond / 2 pill.
The file can be edited by hand or copied between phones; the portable parser and its test live in
`runtime/touch_layout.cpp` and `tools/android/touch_layout_test.cpp`
(`g++ -std=c++20 -Iruntime tools/android/touch_layout_test.cpp runtime/touch_layout.cpp -o t && ./t`).

A connected Bluetooth/USB gamepad hides the overlay (`hide_with_gamepad=1`) and is used directly; the sticks and
buttons of a physical pad always win over the glass.

## Performance

The target is a locked 60 fps, and the port is built for it rather than tuned afterwards:

* render resolution is the launcher's slider (Auto / 50 % / 75 % / Native) and the presets start from 540p
  (performance), 720p (balanced) or native (quality);
* ETC2 is the mobile texture format (required by the GLES3 / Vulkan feature level the APK asks for);
* Dawn renders through Vulkan; the frame is paced to the display and the audio thread never waits on the game;
* `--perf`, `--render-scale`, `--touch` and `--device-info` are the command line the launcher uses — the same code
  paths the desktop build has, so a bug in one is a bug in both.

## Known limits

* Only the USA first release (`GUPE8P`) is supported: the recompiled code is that build's.
* 32-bit-only devices are not supported and not built.
* The launcher is deliberately plain Android widgets (no AndroidX): fewer dependencies, smaller APK.
