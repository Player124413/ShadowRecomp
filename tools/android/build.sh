#!/usr/bin/env bash
#
# Builds the Android APK from a checkout: the native libraries with CMake and the NDK,
# then the app with Gradle. See docs/ANDROID.md for the prerequisites.
#
# Usage: tools/android/build.sh [release|relwithdebinfo|debug] [extra CMake arguments...]
#
#   tools/android/build.sh
#   tools/android/build.sh relwithdebinfo -DSHADOW_RECOMP_XBOXRECOMP2=binary
#
# Environment:
#   ANDROID_NDK_HOME       NDK to build with. Defaults to the version android-apk/app/build.gradle
#                          pins, inside ANDROID_HOME (or ANDROID_SDK_ROOT).
#   ANDROID_KEYSTORE_FILE, ANDROID_KEYSTORE_PASSWORD, ANDROID_KEY_ALIAS, ANDROID_KEY_PASSWORD
#                          Sign a release APK. Without them a debug-signed APK is built,
#                          which installs fine but cannot update a release-signed one.

set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repository_root}"

configuration="${1:-release}"
if [ "$#" -gt 0 ]; then
    shift
fi

case "${configuration}" in
    release | relwithdebinfo | debug) ;;
    *)
        echo "Unknown configuration '${configuration}': expected release, relwithdebinfo or debug." >&2
        exit 1
        ;;
esac

preset="android-${configuration}"

if [ ! -f ShadowRecompLib/private/default.xbe ]; then
    echo "ShadowRecompLib/private/default.xbe is missing: copy it from your own copy of the game (docs/BUILDING.md)." >&2
    exit 1
fi

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
    ndk_version="$(sed -n 's/^ *ndkVersion "\(.*\)" *$/\1/p' android-apk/app/build.gradle)"

    if [ -n "${sdk}" ] && [ -d "${sdk}/ndk/${ndk_version}" ]; then
        export ANDROID_NDK_HOME="${sdk}/ndk/${ndk_version}"
    else
        echo "Android NDK ${ndk_version} was not found. Install it (sdkmanager \"ndk;${ndk_version}\") and set" >&2
        echo "ANDROID_HOME to the SDK, or set ANDROID_NDK_HOME to the NDK directory." >&2
        exit 1
    fi
fi

echo "== Native libraries (${preset}, NDK ${ANDROID_NDK_HOME})"
cmake --preset "${preset}" "$@"
cmake --build "out/build/${preset}" --target android-jnilibs

echo "== Vulkan drivers"
tools/android/fetch-drivers.sh

echo "== APK"
if [ -n "${ANDROID_KEYSTORE_FILE:-}" ]; then
    gradle_task="assembleRelease"
else
    gradle_task="assembleDebug"
fi

(cd android-apk && ./gradlew --no-daemon "${gradle_task}")

find android-apk/app/build/outputs/apk -name '*.apk' -print
