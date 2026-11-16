# Android

An Android build of Shadow Recompiled: the game runs natively on arm64 devices through
Vulkan, with a launcher app that installs the game files and manages drivers, mods and
settings.

> [!WARNING]
> The Android port is experimental. At the time of writing it has not been run on a
> device: expect it to need fixes, and report what you find with `log.txt` attached
> (see [Logs](#logs)).

- [Requirements](#requirements)
- [Using the app](#using-the-app)
- [Building](#building)
- [GitHub Actions](#github-actions)
- [Differences from the desktop versions](#differences-from-the-desktop-versions)

## Requirements

- A 64-bit ARM (arm64-v8a) device with Android 10 or newer.
- A GPU with a Vulkan 1.2 driver.
    - Qualcomm Adreno: the app carries its own Mesa Turnip drivers, so an outdated
      system driver is not a problem.
    - Mali, PowerVR, Xclipse: the system driver is used, or one you import.
- About 2.4 GiB of free storage for the game, plus room for the disc image while it is
  being installed.
- Your own copy of Shadow the Hedgehog for the original Xbox, as with every other
  platform.

## Using the app

The app opens with a launcher. Nothing native is loaded until you press **Launch game**,
so a bad setting can always be changed back from there.

### Installing the game

1. Press **Install game (.iso / .zip / folder)** and pick one of:
    - an Xbox disc image (`.iso`),
    - a ZIP archive that holds the disc image or the extracted disc,
    - the folder of an extracted disc (the one that contains `default.xbe`).
2. The launcher copies the source into the app's storage.
3. Press **Launch game**. Before the first start the files are verified against the
   retail hashes and installed, the same way the desktop installer does it. This takes
   a few minutes, after which the copy made in step 2 is deleted.

GameCube and PlayStation 2 copies cannot be used.

### Storage

**Choose game storage** selects where the game, mods, settings and saves live: the
app's private internal storage, its folder on shared storage, or an SD card. Existing
data can be copied over when you switch.

Inside the selected location:

| Path                                | Contents                                   |
| ----------------------------------- | ------------------------------------------ |
| `game/`                             | The installed game (`game/default.xbe`, …) |
| `mods/`                             | Mods, one folder each                      |
| `.config/ShadowRecomp/config.toml`  | Settings                                   |
| `.config/ShadowRecomp/save/`        | Save data (`Save.dat`)                     |

Android hides app folders from most file managers. **Open game folder**, **Open saves
folder** and **Open logs** open them in the system Files app instead, and the same
folders appear there under *Shadow Recompiled* when another app asks you to pick a
file.

> [!IMPORTANT]
> Uninstalling the app deletes these folders, saves included. Copy `save/` somewhere
> else first if you want to keep it.

### Graphics drivers

**Vulkan driver** chooses what the game renders with:

| Option                                        | Meaning                                              |
| --------------------------------------------- | ---------------------------------------------------- |
| Auto (recommended)                            | An imported driver if there is one. Otherwise Bundled Turnip on Adreno GPUs (the Adreno 8xx build on Snapdragon 8 Elite) and the system driver on all other GPUs |
| System                                        | Always the driver that came with the device          |
| Bundled Turnip                                | The general-purpose Turnip build                     |
| Adreno 710 (Vauzi)                            | Turnip build for Adreno 710 / 720 / 722              |
| A725 Performance (experimental; sysmem+nobin) | Turnip build for Adreno 725                          |
| Adreno 8xx (Snapdragon 8 Elite; sysmem+nobin) | Turnip build for Adreno 8xx                          |
| Imported                                      | A driver you imported with **Import .so / .zip**     |

Imported drivers can be a bare `vulkan.*.so` or an AdrenoTools / ExynosTools package
(`.zip` with a `meta.json`).

A driver can also be copied by hand into the `driver_import/` folder that **Driver
folder** shows. It is picked up the next time the game starts and moved to
`driver_import/installed/`, or to `driver_import/rejected/` if it could not be used;
`log.txt` says why. The `readme.txt` in that folder lists a few diagnostic options for
Turnip.

**Turnip render mode** switches Turnip between tiled (GMEM) and direct (Sysmem)
rendering; leave it on Auto unless you see graphical corruption or poor performance.
The two drivers marked `sysmem+nobin` always render directly and ignore this setting.

If a driver crashes or hangs while the game starts, the next launch falls back to the
system driver on its own. If the problem repeats, select **System**.

### Controls

Controllers work as they do on other platforms, over USB or Bluetooth.

Without one, an on-screen gamepad is shown. **On-screen controls** sets whether it is
shown always, never, or only while no controller is connected. **Camera control**
chooses between dragging on the free part of the screen and a second on-screen stick.
**Arrange touch controls** starts the game in a layout editor where every control can be
moved and resized.

### Mods

**Manage mods** lists the mods found in `mods/`, lets you enable them and set their
load order, and toggles the built-in codes. When two mods replace the same file, the one
lower in the list wins. **Install a mod (.zip / folder)** copies a mod from a ZIP archive
or a folder into `mods/`. Changes apply the next time the game starts.

> [!NOTE]
> Mods that replace textures with DXT/BC compressed ones may not display correctly on
> GPUs without BC texture support, which includes most Mali and PowerVR devices. The
> game's own textures are not affected.

### Audio

If the sound crackles or stutters, set **Audio preset** to Compatibility. Low latency
uses a smaller buffer and may crackle on slower devices.

### Updates

**Check for updates** looks at the releases of the GitHub repository the APK was built
from. An update is only installed if it is signed with the same key as the installed
app, which is why builds from different sources cannot update one another.

### Logs

Every run writes `log.txt`, and keeps the previous one as `log_prev.txt`. **Open logs**
shows them; on a computer they are in `Android/data/org.shadowrecomp.android/files/`.

## Building

The game is cross-compiled with the Android NDK into `libmain.so` by the same CMake
project as the desktop versions, then packaged by the Gradle project in `android-apk/`.
Build on x86-64 Linux, which is what the CI workflow does.

### 1. Prerequisites

Follow steps 1 and 2 of [BUILDING.md](BUILDING.md) first: clone with submodules and
copy `default.xbe` to `./ShadowRecompLib/private/`. If the game is still in a disc
image, this takes the file out of it and checks that it is a dump the project supports:

```bash
tools/android/fetch-xbe.py "/path/to/Shadow the Hedgehog.iso"
```

Then install:

- [CMake] 3.28 or newer and [Ninja].
- A C/C++ compiler for your own machine (`gcc` or `clang`). One build tool has to run
  during the build, so it is built for the machine and not for Android.
- JDK 17.
- The Android SDK with NDK `29.0.14206865`, SDK Platform 35 and Build-Tools 34.0.0.
  With the command line tools:

  ```bash
  sdkmanager "ndk;29.0.14206865" "platforms;android-35" "build-tools;34.0.0"
  ```
- [Rust], optionally. Without it, a prebuilt [XboxRecomp2] is downloaded
  (see [BUILDING.md](BUILDING.md#4-recompiler)).

Set `ANDROID_HOME` to the SDK directory. vcpkg is not used by the Android build.

### 2. Build

```bash
tools/android/build.sh
```

This configures and builds the native libraries, downloads the Turnip drivers and builds
the APK. The result is printed at the end and is found under
`android-apk/app/build/outputs/apk/`.

The script takes the configuration as its first argument (`release`, `relwithdebinfo` or
`debug`); anything after that is passed on to CMake:

```bash
tools/android/build.sh relwithdebinfo -DSHADOW_RECOMP_XBOXRECOMP2=binary
```

### Building step by step

The script only runs these commands:

```bash
# The presets use the NDK that ANDROID_NDK_HOME points to.
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/29.0.14206865"

# 1. Native libraries. `android-jnilibs` builds libmain.so and copies it, together with
#    the driver loader's hook libraries and without debug information, into
#    android-apk/app/src/main/jniLibs/.
cmake . --preset android-release
cmake --build ./out/build/android-release --target android-jnilibs

# 2. Turnip drivers for Adreno GPUs (about 68 MB, kept out of the repository).
#    Optional: without them the APK uses the system driver.
tools/android/fetch-drivers.sh

# 3. The APK.
cd android-apk
./gradlew assembleDebug
```

> [!NOTE]
> The available presets are `android-debug`, `android-relwithdebinfo` and
> `android-release`.

### Signing

`assembleDebug` signs with the debug key of the machine it runs on. That is fine for
your own device, but Android refuses to install an update signed with a different key,
so APKs meant for other people should be signed with a key you keep:

```bash
keytool -genkeypair -v -keystore release.keystore -alias shadowrecomp \
  -keyalg RSA -keysize 4096 -validity 10000

export ANDROID_KEYSTORE_FILE=/path/to/release.keystore
export ANDROID_KEYSTORE_PASSWORD=...
export ANDROID_KEY_ALIAS=shadowrecomp
export ANDROID_KEY_PASSWORD=...
./gradlew assembleRelease
```

`tools/android/build.sh` builds a signed release APK when `ANDROID_KEYSTORE_FILE` is set
and a debug-signed one otherwise.

### Options

CMake:

- `-DSHADOW_RECOMP_HOST_FILE_TO_C=/path/to/file_to_c`: use a `file_to_c` you built for
  your machine yourself (`cmake -S tools/android/host -B <dir> && cmake --build <dir>`)
  instead of having the configure step build it.
- `-DDIRECTX_DXC_TOOL=/path/to/dxc`: use another DirectX Shader Compiler for the build
  machine. By default the prebuilt one from `tools/XShaderRecomp` is used, which exists
  for x86-64 Linux, x86-64 and ARM64 Windows, and macOS.
- `-DSHADOW_RECOMP_ANDROID_JNILIBS_DIR=<dir>`: where `android-jnilibs` copies the
  libraries.
- The XboxRecomp2 options from [BUILDING.md](BUILDING.md#4-recompiler).

Gradle (`./gradlew -P<name>=<value> …`):

- `androidRevision`: number added to the version for Android-only re-releases of the
  same game version (`1.0.0-android.2`).
- `updateRepository`: `owner/name` of the GitHub repository whose releases the launcher
  checks for updates. Defaults to the repository building the APK on GitHub Actions;
  empty disables update checks.

### Looking up a crash address

When the game crashes, `log.txt` ends with where it happened as an offset into the
library, for example `pc=0x7b3c1d4a10 (libmain.so+0x48a10)`. The libraries in the APK
have no debug information, but the ones in the build tree do. This only gives the right
answer with the build tree that the APK was made from:

```bash
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-addr2line" -Cfi \
  -e out/build/android-release/ShadowRecomp/libmain.so 0x48a10
```

### Tests

The Android-specific code (driver selection and startup recovery, driver import, the
staged game installation, the on-screen controls, logging, and the launcher's handling
of archives, disc images and storage copies) and the build tools in `tools/android`
have tests that run on the build machine. They need neither the NDK nor a device nor
the game files:

```bash
cmake -S tools/android/tests -B out/build/android-tests
cmake --build out/build/android-tests
ctest --test-dir out/build/android-tests --output-on-failure
```

## GitHub Actions

`.github/workflows/build-android.yml` builds the APK on GitHub. All it needs from you is
the game, as a link to your disc image.

### Building in your fork

1. Open the **Actions** tab of the repository and pick **build-android** on the left.
2. Press **Run workflow**, paste the link to your disc image of Shadow the Hedgehog
   (Xbox) into the first field and press the green **Run workflow** button.
3. When the run has finished, the APK is under **Artifacts** at the bottom of its page,
   for three days.

> [!NOTE]
> GitHub only shows the **Run workflow** button for workflows that are in the default
> branch of the repository.

The run downloads the disc image, takes `default.xbe` out of it, checks that it is a
dump of the game the project supports and deletes everything else. XboxRecomp2 then
turns the executable into C++ and the build described above follows. The APK holds the
recompiled game code and none of the game's files: those are installed on the device
from your disc image ([Installing the game](#installing-the-game)).

The link can point to:

- an `.iso`, either a bare file system image or a full disc dump;
- a `.zip` or `.7z` with such an image, or with the extracted files of the game.

It has to download the file itself. Share links of Google Drive, Dropbox and Yandex Disk
are understood; other file hosts that show a web page first are not.

The link is kept out of the log of the run. To not paste it every time, save it as the
repository secret `GAME_ISO_URL` (**Settings > Secrets and variables > Actions**) and
leave the field empty.

> [!IMPORTANT]
> The artifacts of a public repository can be downloaded by anyone who is signed in to
> GitHub.

If a run fails, the lines of the log that explain why are on the summary page of the
run, and the complete logs are attached to it as an artifact.

### Testing the workflow without the game

Tick **stub_build** instead of giving a link. The run then goes through every step with
a placeholder: a made-up disc image is downloaded and unpacked like a real one, and
`tools/android/stub-recompiler.py` takes the place of XboxRecomp2 and writes empty
functions. The result is a real APK, named `-stub`; starting the game in it only shows a
message that the build has no game code. This is for checking the workflow, the
toolchain and the packaging after a change; it tells nothing about the recompiled game.

`.github/workflows/build-android-dev.yml` does this on every push to the branch the port
is being developed on, or a build of the game if the `GAME_ISO_URL` secret is set. It is
a development aid and can be deleted together with that branch.

The same build can be made locally:

```bash
tools/android/tests/make_test_iso.py /tmp/test.iso
tools/android/fetch-xbe.py /tmp/test.iso --no-verify
tools/android/build.sh release -DSHADOW_RECOMP_XBOXRECOMP2_EXECUTABLE=$PWD/tools/android/stub-recompiler.py
```

### Releases and signing

The release workflow calls `build-android.yml` and attaches the APK to the release as
`ShadowRecomp-Android-arm64-<version>.apk`. There is no field for a link then: the game
comes from the `GAME_ISO_URL` secret or, failing that, from the asset repository of the
other build workflows (`ASSET_REPO`, `ASSET_REPO_TOKEN`).

Signing is optional and takes four more secrets:

| Secret                      | Value                                              |
| --------------------------- | -------------------------------------------------- |
| `ANDROID_KEYSTORE_BASE64`   | The keystore file, encoded with `base64 -w0`       |
| `ANDROID_KEYSTORE_PASSWORD` | Its password                                       |
| `ANDROID_KEY_ALIAS`         | The alias of the key inside it                     |
| `ANDROID_KEY_PASSWORD`      | The password of that key                           |

Without them the APK is signed with a debug key that changes on every run. Such builds
install, but cannot update one another.

## Differences from the desktop versions

- The game always runs fullscreen, in landscape.
- The launcher replaces the in-game installer and the update notification.
- Shaders are never compiled on the device: the game only has the shaders that were
  compiled into it at build time. One that is missing from them, which a mod can cause,
  is drawn with the generic fixed-function shader and reported in `log.txt`.
- Discord Rich Presence is not available.
- Only arm64-v8a is supported.

[CMake]: https://cmake.org/download/
[Ninja]: https://ninja-build.org/
[Rust]: https://rustup.rs/
[XboxRecomp2]: https://github.com/Sewer56/XboxRecomp2
