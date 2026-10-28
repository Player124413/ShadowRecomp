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

The link may point at a disc image, an archive (`.tar.gz`, `.tgz`, `.tar.xz`, `.zip`) or a single file:

| What you upload | Size | What the job does |
| --- | --- | --- |
| your `.iso` / `.gcm` image | ~1.4 GB | takes `sys/main.dol` out of it (`gameimport.py extract`) and runs `tools/gen.py` |
| `generated/` | ~200 MB | uses it as is (fastest) |
| your disc folder (`sys/main.dol` + `files/`) | ~1.4 GB | runs `tools/gen.py` on it here |
| `main.dol` alone - as a file, or inside an archive | ~5 MB | runs `tools/gen.py` on it here (the smallest upload) |

Dolphin-compressed images (`.rvz`, `.wia`, `.gcz`) are not read here: Dolphin's tool is not on the runner. Convert once
to ISO in Dolphin (right-click the game → Convert File… → Format: ISO) and link that.

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
stick_mode=0          # 0 fixed (the ring stays where it is drawn), 1 floating (under the thumb)
deadzone=15
haptics=1
w=stick,0,0.1500,0.7200,0.1250,0.450,0,1     # kind,action,x,y,radius,opacity,style,visible
w=button,0,0.8850,0.7350,0.0560,0.550,0,1
```

`x`/`y` are 0..1 of the screen, the radius is of the screen's short side, alpha 0..1, style 0 round / 1 diamond /
2 pill; kinds are `button`, `stick`, `menu`, `edit`.

The sticks are measured from the point the thumb lands on, so a touch never starts at a deflection, and the ring only
moves if you choose a floating stick. The knob follows the thumb with a short trail and springs back to the centre, the
deadzone is drawn, and a press changes a button's size and fill - the overlay animates, the input never waits for the
animation. The game itself runs immersive: the status bar and the navigation buttons are hidden while you play (swipe
from an edge to bring them back for a moment), so nothing sits on top of the picture or takes a tap from a control.
`version=1` files written by an earlier build are read as fixed sticks. The file is plain text: hand-editing and copying between phones
both work. The parser and its test are platform-free (`tools/android/`):

```sh
g++ -std=c++20 -Iruntime tools/android/touch_layout_test.cpp runtime/touch_layout.cpp -o t && ./t
```

While a cutscene plays - a pre-rendered movie or an in-engine story scene - the overlay shows one `SKIP` control (bottom
centre). For a movie it jumps the stream to its tail; for an in-engine scene it sets the game's own skip. Either way it
works whether or not you have seen that scene before: the `Skip any cutscene` setting only decides whether a plain
`START` press does the same, and it starts on for phones (a settings file written by an older build is migrated). The
render height is capped at 720 lines while a movie streams, so the emulated video decoder gets the frame time instead of
the GPU.

A connected Bluetooth/USB gamepad hides the overlay (`hide_with_gamepad=1`) and drives the game directly; a physical
stick always wins over a thumb on the glass.

## Performance

The target is a locked 60 fps, so the port is built for it rather than tuned afterwards:

* the render resolution is the launcher's slider (Auto / 50 % / 75 % / Native) and the presets start at 540p
  (performance), 720p (balanced) or 900p (quality; Native is on the slider for anyone who wants it);
* adaptive resolution then follows the frame times actually delivered (`runtime/dynamic_resolution.cpp`): when frames
  are late the render height comes down in steps of 5 %, and it goes back up one step at a time while there is a
  margin. It is on for Android (`SHADOW_DRS=0` turns it off for an A/B), logs `[drs] ... fps, render scale ...` every
  5 s, and never changes anything in the middle of a frame;
* frame pacing sleeps to the frame's deadline instead of spinning the game's frame wait (a phone core burned per
  frame is heat, and heat is a lost frame budget a few minutes in) — `FR_NO_PACE=1` on a device turns it off to compare;
* ETC2 is the mobile texture format, and the port uses it: console textures that were BC1 (CMPR) are re-encoded to
  ETC2 at load time and uploaded at 4 bits per pixel instead of 32, with an EAC alpha block only when the texture has
  alpha (`SHADOW_ETC2=1` forces it for all RGB sources, `=0` turns it off). The APK requires a GLES3 / Vulkan-class
  device, where ETC2 is guaranteed;
* Dawn renders through **OpenGL ES**. Vulkan was the default of the upstream layer this port is built on and is exactly
  what used to draw the world black; the port now asks for GLES on every phone (`SHADOW_BACKEND=vulkan` exists only for a
  diagnostic run, and Aurora keeps Vulkan as a last-resort fallback if a phone's GL driver refuses to start). libc++ is
  linked statically, and the audio stream keeps SDL's own device buffering.
* **Automatic quality** (`runtime/perf_auto.cpp`, "Keep the frame rate automatically" on the Screen page, on for a fresh
  Android install): if a stage stays below the frame rate even with adaptive resolution at its floor, the game's glow
  pass is dropped, then the resolution floor goes from 50 % to 40 %; both come back when the phone holds the frame rate
  again. It ignores one-off hitches, changes at most once a minute, and logs every step (`[auto] 47 fps: glow off`).

The launcher's settings travel as command-line arguments (`--disc`, `--perf`, `--render-scale`, `--touch`,
`--edit-touch`, `--device-info`), so everything the phone does can be replayed on the desktop build verbatim.

## When the picture is wrong: the two diagnostics built in

A phone has no console, so the port answers questions with the screen instead:

* **The frame-rate overlay names the graphics API and the adapter** it is actually using (`OpenGL ES - Adreno (TM)
  740`), and what the automatic quality has taken away (`auto quality: glow off`). The API is not a setting: it is
  OpenGL ES on every phone.
* **Settings > System > Bug reports**:
  * **Show log on the screen** draws the tail of the log over the picture, with error lines pinned in red at the top (a
    refused pipeline, a shader that failed to compile, a texture format the driver does not support). A screenshot of it
    is a bug report we can act on.
  * **Draw both sides** and **Ignore the depth buffer** rebuild the pipelines with culling off / the depth test off. If
    the missing world appears with either one, that is which part of the renderer is at fault, and it takes one tap.
  * **Skip full-screen copies** and **Hide haze copies** drop the two things the game draws on top of a finished world
    (its 2D copy of the screen, and the haze pass that samples one). A world that is being covered rather than not drawn
    appears as soon as one of them is on - that tells us the copy, not the geometry, is the broken part.
  * The overlay also counts **3D draws per second** (the world) against **2D draws per second** (the HUD): a stage that
    is drawing thousands of world triangles and showing black has a different cause from one that submits none.

## Installing an update (never uninstall)

A new APK installs **over** the one on the phone: open it from the notification or the file manager and press
**Update**. The disc image, the settings and the saves are in the app's private folder and are left alone, so there is
nothing to import again.

That works only because every build is signed with the same key and every build has a bigger version code:

* `android/shadow-key.p12` + `android/shadow-signing.properties` - the port's own key, committed on purpose. It is not a
  secret, it protects nothing, and it cannot pass for another app; its only job is to be the *same* key on every build.
  (Gradle's debug key - what the CI used before - is generated fresh on every machine, which is why every update used to
  end in "App not installed" and an uninstall, taking the disc image with it.) To use your own key instead, put a
  `android/keystore.properties` next to it with `storeFile`, `storeType`, `storePassword`, `keyAlias`, `keyPassword`; it
  wins. To regenerate the port's key, `python3 tools/android/make_keystore.py --force` - and expect one more uninstall.
* `versionCode` comes from the CI run number (`-Pshadow.versionCode=...` in the workflow), so every artifact is newer than
  the previous one. Locally, `./build.sh apk --version-code 42` or `SHADOW_VERSION_CODE=42` does the same. The launcher's
  footer shows the version it is running, so an update can be told from a fresh install.

If Android still refuses with "App not installed", the installed app was signed with a different key (an APK from before
this change), and one uninstall is unavoidable: export the log first, and copy the disc image out if you need it.

## When the game closes by itself: send the log

A phone has no console and no cable, so the port keeps the answer on the phone and hands it over in two taps:

* **The launcher** has a *Something went wrong?* section: **Send the log…** opens the phone's share sheet (a messenger,
  mail, Drive - anything that takes a file) with the newest log attached, and **Copy the log** puts the last 400 lines
  into the clipboard to paste into a message. If the previous run ended badly the section says so first, and the file sent
  is that run's report.
* **In the game:** *MENU → System → Logs → Send the log / Copy the log*.

What is kept:

| File | Written when |
| --- | --- |
| `files/logs/shadow-<date>.log` | every run, from the first line (`SHADOW_LOG_PATH`) |
| `files/crashes/crash-<date>.txt` | the previous run did not finish **and** its log ends with the runtime's own fault line (`[crash] host signal 11 (SIGSEGV)`, `FATAL`, `abort`) - plus the guest backtrace it wrote |
| `files/crashes/stop-<date>.txt` | the previous run did not finish with no error at all: Android closed the app in the background, or the phone ran out of memory - a different answer, and worth knowing |
| `files/crashes/fatal-<date>.txt` | the renderer refused something and stopped the game on purpose |
| `files/crashes/java-<date>.txt` | the Android/Java side threw an uncaught exception (`JavaCrash.java`), with its stack trace |
| `files/bugs/<date>/` | the bug report key: screenshot, game state, settings, log |

How the port knows: every run drops a `run.open` marker in the app's folder and removes it on the way out (including the
fast-exit path in `sys_exit_guarded`, which never runs the normal teardown). A marker found at the next start is the
previous run failing to finish - `runtime/crash_report.cpp`.

## Known limits

* Only the USA first release (`GUPE8P`) works: the recompiled code is that build's.
* 32-bit-only devices are not supported and not built.
* The launcher is plain Android widgets (no AndroidX): fewer dependencies, smaller APK. The log leaves the app through
  `LogProvider.java`, a plain `ContentProvider` written for exactly that, not through AndroidX's FileProvider.
* `android/PORT-STATUS.md` keeps the running list of what is left.
