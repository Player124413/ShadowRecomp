# ShadowRecomp for Android (64-bit)

One APK, one Gradle project, no game files in it. The launcher asks the player for their own
Shadow the Hedgehog (USA) disc image (`GUPE8P`), copies it into the app's private folder once, and the game then reads
that image **in place** (nothing is unpacked, so it costs exactly the image's size).

Two halves:

* **`app/src/main/java/dev/shadowrecomp/`** — the launcher.
  `LauncherActivity` (import the disc, graphics preset, render resolution, touch controls on/off, Play),
  `ShadowActivity` (the game: SDL3's `SDLActivity` with Aurora's surface), `DiscImport` (SAF copy + header check),
  `DeviceInfo` (chip / cores / RAM for the `auto` preset).
* **`../../CMakeLists.txt`** — the native half, built by Gradle's `externalNativeBuild` into `libmain.so`.
  SDL3's Java classes and Aurora's `dev.encounter.aurora.AuroraSurface` are copied into `app/src/main/java` by CMake
  when it configures, so they are not duplicated in the repository.

## Requirements

| What | Why | Notes |
| --- | --- | --- |
| JDK 17+ | Gradle | `build.sh` finds one |
| Android SDK, platform 35 + build-tools 35 | Gradle | `ANDROID_HOME` or `android/local.properties` |
| NDK r26 or newer (r27 used here) | the native build | `shadow.ndk` in `gradle.properties` |
| CMake 3.25+ | the native build | the SDK ships one |
| Python 3 | `tools/gen.py` (the recompiled game code) | run once, from the repository root |
| ~15 GB of disk | Dawn, SDL3 and the game code | the first build downloads and compiles a lot |

## Build

```sh
cd android
./build.sh              # or: ./build.sh release
```

`build.sh` checks the pieces above, creates `local.properties` from `ANDROID_HOME` if it can, fetches Gradle into
`android/.gradle-dist` when there is none, and prints the APK's path. Without it:

```sh
python3 tools/gen.py               # once, from the repository root
cd android && gradle :app:assembleDebug
# Android Studio: File > Open > (this) android/ folder, then Run
```

### Without a disc yet: the launcher-only build

If you want to see the launcher, the touch layout or the settings on a phone before you have run `tools/gen.py`, the
build can link empty stand-ins for the game's functions instead of the real recompiled code:

```sh
cd android
SHADOW_STUB_GAME_CODE=1 ./build.sh          # -Pshadow.stubGameCode=ON is passed to Gradle for you
```

That APK installs and runs, but it cannot play a disc: every guest function returns immediately, so "Play" gets you a
black screen or an idle game loop. It is for checking the packaging (and it is what the CI workflow builds on every
push, so the Gradle/NDK/SDL3 path is never silently broken). Build again without `SHADOW_STUB_GAME_CODE` once
`generated/` is real.

Install and watch the log:

```sh
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb logcat -s ShadowRecomp SDL
```

## ABIs

`gradle.properties` → `shadow.abis=arm64-v8a`. That is the target: every 64-bit phone, and the only Android ABI Dawn
ships a prebuilt library for. Adding `x86_64` (the emulator) makes the first build compile Dawn from source, which is
long and wants about 10 GB — do it only if you need it.

## Build on GitHub Actions

`.github/workflows/android-apk.yml` builds on every push that touches the port, in three jobs:

| Job | What it does | Needs |
| --- | --- | --- |
| `checks` | the disc-free tests (`tools/android/`): ETC2 encoder against an independent decoder, the touch layout, the Java sources | nothing |
| `compile` | compiles **every** native file for `arm64-v8a` with the real NDK. `tools/android/stub_generated.py` writes a stand-in `generated/`, so nothing needs the disc; the link step is deliberately not attempted | nothing |
| `apk` | the installable APK | the game code, or the `main.dol` it comes from |

The APK job cannot invent the game: `generated/` is code from your own `main.dol`. You can hand it a link at the moment
you start the build, so nothing has to be stored in the repository:

1. Put the file on Hugging Face (a model, dataset or space repo all work) or anywhere that serves it directly.
2. **Actions → Android APK → Run workflow**, and paste the link into **game_code_url**.
3. When it finishes, download the artifact **`shadowrecomp-android-arm64-debug`**.

The link may point at a `.tar.gz`, `.tgz`, `.tar.xz` or `.zip`, or at a single file. These are all accepted:

| What you upload | Size | What the job does |
| --- | --- | --- |
| `generated/` | ~200 MB | uses it as is (fastest) |
| your disc folder (`sys/main.dol` + `files/`) | ~1.4 GB | runs `tools/gen.py` on it here |
| `main.dol` alone - as a file, or inside an archive | ~5 MB | runs `tools/gen.py` on it here (usually the easiest) |

On Hugging Face, use the **direct** link, not the file page: `.../resolve/main/main.dol` (the "Download" button gives
exactly that) rather than `.../blob/main/main.dol`. A file page answers with HTML, and the build stops with a message
saying so instead of pretending. For a private repo, a link with `?download=true` and a token also works, but remember
that the URL you paste is visible in the run's log.

```sh
# the smallest thing to upload - about 5 MB, and it is all tools/gen.py needs:
tar -czf main.dol.tar.gz -C "<disc folder>/sys" main.dol     # or just upload main.dol itself
# or, if you have already run tools/gen.py:
tar -czf generated.tar.gz generated/
```

`main.dol` is checked against the SHA-1 of the USA release (`118ef49e...`) before anything is built, so a PAL or
Japanese dump fails loudly instead of producing an APK that crashes on the phone.

If you would rather not paste the link every time, save it as the repository secret `SHADOW_GEN_URL`; a value typed into
**Run workflow** wins over it. Do not put the disc or `generated/` in the repository: `generated/` is the game's compiled
code.

Without a link the job builds a **launcher-only** APK instead (`tools/android/stub_generated.py --definitions`): the same
packaging path with empty stand-ins, so the Gradle/NDK/SDL3 side is still tested on every push. It installs and shows the
launcher but cannot play.

## First run

1. The launcher opens. **Import disc image…** → pick your own ISO/GCM. It is checked on the spot (GameCube magic,
   game id `GUPE8P`, first USA release) and copied to `files/shadow.iso`, with progress.
2. Pick a graphics preset — *Auto* reads the chip, cores and RAM and starts from the tier that fits.
3. **Play**.

## Touch controls

On by default. `MENU` opens the in-game settings menu; `EDIT` opens the **control editor**: drag any control with a
finger, tap one to change its size, opacity, shape and the button it presses, add or remove controls, change the whole
picture's size / opacity / left-handed mode, and save it as a new layout. Presets: *classic*, *left_handed*, *minimal*.
The result is `touch_layouts.ini` in the app's folder:

```
enabled=1
active=classic
scale=100
opacity=100
hide_with_gamepad=1
[classic]
stick_mode=1          # 0 fixed, 1 floating (it appears where the thumb lands)
deadzone=15
haptics=1
w=stick,0,0.1500,0.7200,0.1250,0.450,0,1     # kind,action,x,y,radius,opacity,style,visible
w=button,0,0.8850,0.7350,0.0560,0.550,0,1
```

`x`/`y` are 0..1 of the screen, the radius is of the screen's short side, alpha 0..1, style 0 round / 1 diamond /
2 pill; kinds are `button`, `stick`, `menu`, `edit`. The file is plain text: hand-editing and copying between phones
both work. The parser and its test are platform-free (`tools/android/`):

```sh
g++ -std=c++20 -Iruntime tools/android/touch_layout_test.cpp runtime/touch_layout.cpp -o t && ./t
```

A connected Bluetooth/USB gamepad hides the overlay (`hide_with_gamepad=1`) and drives the game directly; a physical
stick always wins over a thumb on the glass.

## Performance

The target is a locked 60 fps, so the port is built for it rather than tuned afterwards:

* the render resolution is the launcher's slider (Auto / 50 % / 75 % / Native) and the presets start at 540p
  (performance), 720p (balanced) or native (quality);
* frame pacing sleeps to the frame's deadline instead of spinning the game's frame wait (a phone core burned per
  frame is heat, and heat is a lost frame budget a few minutes in) — `FR_NO_PACE=1` on a device turns it off to compare;
* ETC2 is the mobile texture format, and the port uses it: console textures that were BC1 (CMPR) are re-encoded to
  ETC2 at load time and uploaded at 4 bits per pixel instead of 32, with an EAC alpha block only when the texture has
  alpha (`SHADOW_ETC2=1` forces it for all RGB sources, `=0` turns it off). The APK requires a GLES3 / Vulkan-class
  device, where ETC2 is guaranteed;
* Dawn renders through Vulkan, libc++ is linked statically, and the audio stream keeps SDL's own device buffering.

The launcher's settings travel as command-line arguments (`--disc`, `--perf`, `--render-scale`, `--touch`,
`--edit-touch`, `--device-info`), so everything the phone does can be replayed on the desktop build verbatim.

## Known limits

* Only the USA first release (`GUPE8P`) works: the recompiled code is that build's.
* 32-bit-only devices are not supported and not built.
* The launcher is plain Android widgets (no AndroidX): fewer dependencies, smaller APK.
* `android/PORT-STATUS.md` keeps the running list of what is left.
