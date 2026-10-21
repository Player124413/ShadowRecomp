#!/usr/bin/env bash
# Builds the Android APK (64-bit) without Android Studio.
#
#   ./build.sh                 # debug APK (installable as-is)
#   ./build.sh release         # release APK (unsigned unless android/keystore.properties exists)
#   ./build.sh --install       # debug APK, then adb install -r
#   ./build.sh clean           # remove build outputs
#
# What it does: checks the recompiled game code exists (tools/gen.py), finds a JDK 17+ and an Android SDK, installs
# Gradle into android/.gradle-dist if there is none, then runs Gradle - which drives CMake and the NDK for libmain.so
# and libSDL3.so.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"

task="debug"
install_after=0
for arg in "$@"; do
  case "$arg" in
    debug|release|clean) task="$arg" ;;
    --install|-i) install_after=1 ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) printf 'unknown argument: %s\n' "$arg" >&2; exit 2 ;;
  esac
done

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- 1. the game code the native build links against --------------------------------------------
if [ ! -f "$root/generated/files.cmake" ]; then
  say "generated/ is missing: running tools/gen.py"
  [ -f "$root/tools/gen.py" ] || die "tools/gen.py not found - is this the repository root?"
  (cd "$root" && python3 tools/gen.py) || die "tools/gen.py failed (it needs your own disc image's main.dol)"
fi

# ---- 1b. a launcher-only build, for checking the APK pipeline without a disc ---------------------
# SHADOW_STUB_GAME_CODE=1 links empty stand-ins for the game code: the app installs and shows its launcher, but cannot
# play. Useful to see that the whole Gradle/NDK/SDL path works before running tools/gen.py.
if [ "${SHADOW_STUB_GAME_CODE:-0}" = "1" ] && [ "$task" != "clean" ]; then
  say "launcher-only build: the game code is a stand-in and the app cannot play a disc"
  (cd "$root" && python3 tools/android/stub_generated.py --definitions) || die "could not write the stand-in game code"
  SHADOW_GRADLE_ARGS="${SHADOW_GRADLE_ARGS:-} -Pshadow.stubGameCode=ON"
  export SHADOW_GRADLE_ARGS
fi

# ---- 2. a JDK -----------------------------------------------------------------------------------
if [ -z "${JAVA_HOME:-}" ] || [ ! -x "${JAVA_HOME}/bin/javac" ]; then
  JAVA_HOME=""
  for candidate in \
      "${ANDROID_STUDIO_JBR:-}" \
      "/usr/lib/jvm/java-21-openjdk-amd64" "/usr/lib/jvm/java-17-openjdk-amd64" "/usr/lib/jvm/default-java" \
      "/Applications/Android Studio.app/Contents/jbr/Contents/Home" "$HOME"/.jdks/*; do
    if [ -n "$candidate" ] && [ -x "${candidate}/bin/javac" ]; then JAVA_HOME="$candidate"; break; fi
  done
  if [ -z "$JAVA_HOME" ] && command -v javac >/dev/null 2>&1; then
    JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")"
  fi
fi
if [ "$task" != "clean" ]; then
  [ -n "$JAVA_HOME" ] && [ -x "$JAVA_HOME/bin/javac" ] || die "no JDK 17+ found. Install one or set JAVA_HOME."
  say "JDK: $JAVA_HOME"
  export JAVA_HOME
fi

# ---- 3. the Android SDK -------------------------------------------------------------------------
if [ ! -f "$here/local.properties" ]; then
  for sd in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$HOME/Android/Sdk" "$HOME/Library/Android/sdk"; do
    if [ -n "$sd" ] && [ -d "$sd" ]; then
      printf 'sdk.dir=%s\n' "$sd" > "$here/local.properties"
      break
    fi
  done
fi
if [ "$task" != "clean" ]; then
  [ -f "$here/local.properties" ] ||
    die "no Android SDK found. Set ANDROID_HOME, or copy local.properties.example to local.properties."
  say "SDK: $(sed -n 's/^sdk.dir=//p' "$here/local.properties")"
fi

# ---- 4. Gradle ----------------------------------------------------------------------------------
GRADLE_VERSION="8.7"
gradle_bin="$(command -v gradle || true)"
if [ -z "$gradle_bin" ] && [ -x "$here/.gradle-dist/gradle-$GRADLE_VERSION/bin/gradle" ]; then
  gradle_bin="$here/.gradle-dist/gradle-$GRADLE_VERSION/bin/gradle"
fi
if [ -z "$gradle_bin" ] && [ "$task" != "clean" ]; then
  say "Gradle is not installed: fetching $GRADLE_VERSION into android/.gradle-dist (once)"
  mkdir -p "$here/.gradle-dist"
  url="https://services.gradle.org/distributions/gradle-$GRADLE_VERSION-bin.zip"
  if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 -o "$here/.gradle-dist/gradle.zip" "$url"
  else
    wget -O "$here/.gradle-dist/gradle.zip" "$url"
  fi
  (cd "$here/.gradle-dist" && unzip -q -o gradle.zip && rm -f gradle.zip)
  gradle_bin="$here/.gradle-dist/gradle-$GRADLE_VERSION/bin/gradle"
fi
[ -n "$gradle_bin" ] || die "no Gradle (and no way to fetch one)"

# ---- 5. build -----------------------------------------------------------------------------------
cd "$here"
case "$task" in
  clean)
    if [ -n "$gradle_bin" ]; then "$gradle_bin" --no-daemon clean; else rm -rf app/build build; fi
    rm -rf app/src/main/jniLibs
    say "build outputs removed"
    ;;
  debug|release)
    if [ "$task" = "debug" ]; then target=":app:assembleDebug"; else target=":app:assembleRelease"; fi
    abis="$(sed -n 's/^shadow.abis=//p' gradle.properties | tr -d ' ')"
    say "ABIs: $abis"
    if [ "$abis" != "arm64-v8a" ]; then
      say "note: Dawn has a prebuilt Android library for arm64 only;"
      say "      any other ABI makes the first build compile Dawn from source (long, ~10 GB)."
    fi
    # Extra Gradle arguments when someone needs them, e.g. SHADOW_GRADLE_ARGS=-Pshadow.cmake=3.22.1 (CI does this for the
    # CMake version the SDK actually ships).
    # shellcheck disable=SC2086
    "$gradle_bin" --no-daemon "$target" ${SHADOW_GRADLE_ARGS:-}
    apk="$(find app/build/outputs/apk -name "*-$task.apk" | head -1 || true)"
    [ -n "$apk" ] || die "the build finished but no APK was found"
    say "APK: $here/$apk"
    if [ "$install_after" = "1" ]; then
      command -v adb >/dev/null 2>&1 || die "adb not found (platform-tools)"
      adb install -r "$here/$apk"
      say "installed"
    else
      say "install with: adb install -r $here/$apk"
    fi
    ;;
esac
