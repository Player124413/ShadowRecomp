#!/usr/bin/env bash
#
# Downloads the Mesa Turnip Vulkan drivers that the APK bundles for Adreno GPUs into
# android-apk/app/src/main/assets/bundled_driver/.
#
# The binaries (about 68 MB) are not kept in this repository. They are the builds published
# by the UnleashedRecomp-Android project, pinned to one commit and verified against the
# SHA-256 checksums below; see driver/README.md in that repository for how each was built.
#
# An APK built without them still works: the game then uses the device's own Vulkan
# driver, or one imported through the launcher.
#
# Usage: tools/android/fetch-drivers.sh [destination directory]
#
# SHADOW_RECOMP_DRIVER_BASE_URL overrides where the files are downloaded from (the file
# names are appended), for mirrors and offline builds.

set -euo pipefail

SOURCE_REPOSITORY="SansNope/UnleashedRecomp-Android"
SOURCE_COMMIT="f65c40ba2788ebd7318361670bcbb82cfdef6ae8"
BASE_URL="${SHADOW_RECOMP_DRIVER_BASE_URL:-https://raw.githubusercontent.com/${SOURCE_REPOSITORY}/${SOURCE_COMMIT}/driver}"

# File names are part of the game's driver selection (ShadowRecomp/os/android/vulkan_driver_android.cpp).
DRIVERS=(
    "vulkan.unleashed26_1_wfm_a732.so        5b9025389c2b9a79b59caf009d6aeaff8537aeafa29c85b6e9dd200a7c99c62a"
    "vulkan.vauzi710_v2_7.so                 636c7d20cbcaec8ce00f3ee88f4fa0ed8c2312d59dbb9664a7b63e98a387fdb6"
    "vulkan.wb26_2_rp_pair_ccu_color_a725.so 87f371a26a4678a092efa0d209be550ccaae0d18c96436aec181d558e53b22b0"
    "vulkan.mesa26_3_gralloc_a8xx.so         64ad4fbab81e787eb4373bccc4d02604e1b8a87bd2dc7e850bc9f7e91f881221"
)

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
destination="${1:-${repository_root}/android-apk/app/src/main/assets/bundled_driver}"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d ' ' -f 1
    else
        shasum -a 256 "$1" | cut -d ' ' -f 1
    fi
}

mkdir -p "${destination}"

for entry in "${DRIVERS[@]}"; do
    read -r name expected <<< "${entry}"
    target="${destination}/${name}"

    if [ -f "${target}" ] && [ "$(sha256_of "${target}")" = "${expected}" ]; then
        echo "${name}: up to date"
        continue
    fi

    # Whatever is there is not the pinned build; do not leave it for the APK to pick up.
    rm -f "${target}"

    echo "${name}: downloading"
    partial="${target}.part"
    trap 'rm -f "${partial}"' EXIT
    curl --fail --location --silent --show-error --retry 3 --output "${partial}" "${BASE_URL}/${name}"

    actual="$(sha256_of "${partial}")"
    if [ "${actual}" != "${expected}" ]; then
        echo "${name}: checksum mismatch" >&2
        echo "  expected ${expected}" >&2
        echo "  actual   ${actual}" >&2
        exit 1
    fi

    mv -f "${partial}" "${target}"
    trap - EXIT
done

echo "Vulkan drivers are in ${destination}"
