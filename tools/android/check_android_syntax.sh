#!/usr/bin/env bash
# Compile the runtime sources for Android *here*, in seconds, without an NDK, without CMake and without waiting for CI.
#
# Why: the arm64 build is the only build where "a path is wide on Windows" and "libc++ has no std::jthread" turn into
# errors, and finding that out through a seven-minute CI run is a slow way to work. This does the same thing with the
# host compiler: `-fsyntax-only -D__ANDROID__` over runtime/*.cpp. It cannot replace the real NDK build (libc++ differs
# from libstdc++, and the target ABI cannot be linked), but every mistake of the "this only compiles on Windows" kind
# shows up immediately.
#
# Third-party headers the runtime uses are looked for in SHADOW_SDL3_INCLUDE, SHADOW_IMGUI_INCLUDE, SHADOW_XXHASH_INCLUDE
# and SHADOW_DAWN_INCLUDE (or in a few conventional locations). SHADOW_DAWN_INCLUDE wants Dawn's *generated* headers too
# (dawn/webgpu_cpp.h and dawn/webgpu.h are produced by Dawn's own generator, they are not in its git tree): a
# space-separated list is accepted, e.g. SHADOW_DAWN_INCLUDE="/tmp/dawn_gen/include /tmp/dawn-src/include". Whatever is missing means the files that need it are
# *skipped* and listed, not failed: the point is the runtime, and a machine without the renderer's sources can still
# check most of it. tools/android/stubs holds the two small hand-written stand-ins (libpng, stb_truetype) that the
# texture pack code needs, because those are not fetched into the tree.
#
#   tools/android/check_android_syntax.sh            # check, print a summary, exit 1 on a real error
#   CXX=clang++ tools/android/check_android_syntax.sh
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
CXX=${CXX:-g++}
INCLUDES=(-I"$ROOT/runtime" -I"$ROOT/generated" -I"$ROOT/third_party/aurora/include" -I"$ROOT/third_party/aurora/lib"
          -I"$ROOT/tools/android/stubs")

add_dir() {
  local name=$1 dir=${2:-}
  if [ -n "$dir" ] && [ -d "$dir" ]; then
    INCLUDES+=("-I$dir")
    return 0
  fi
  return 1
}

# The renderer's dependencies. A missing one is not a failure, only a skip (see the header comment).
MISSING=()
add_dir SDL3 "${SHADOW_SDL3_INCLUDE:-}" || \
  add_dir SDL3 /tmp/sdl3/include || MISSING+=("SDL3 (SHADOW_SDL3_INCLUDE)")
add_dir imgui "${SHADOW_IMGUI_INCLUDE:-}" || \
  add_dir imgui /tmp/imgui || MISSING+=("Dear ImGui (SHADOW_IMGUI_INCLUDE)")
add_dir xxhash "${SHADOW_XXHASH_INCLUDE:-}" || \
  add_dir xxhash /tmp/xxhash || MISSING+=("xxhash (SHADOW_XXHASH_INCLUDE)")
# Aurora logs through fmt; its headers are only needed to compile, so any fmt 11 tree will do.
add_dir fmt "${SHADOW_FMT_INCLUDE:-}" || \
  add_dir fmt /tmp/fmt/include || MISSING+=("fmt (SHADOW_FMT_INCLUDE)")
dawn_ok=0
for dir in ${SHADOW_DAWN_INCLUDE:-} /tmp/bt/_deps/dawn-src/include; do
  add_dir dawn "$dir" && dawn_ok=1
done
[ "$dawn_ok" = 1 ] || MISSING+=("Dawn (SHADOW_DAWN_INCLUDE)")
# Dawn generates dawn/webgpu_cpp.h at build time; the stub only says where the real one lives.
if [ -d /tmp/dawnstub ]; then INCLUDES+=("-I/tmp/dawnstub"); fi

if [ ! -f "$ROOT/generated/funcs.h" ]; then
  echo "generated/ is missing: writing the stand-ins (as the compile job does)"
  python3 "$ROOT/tools/android/stub_generated.py" >/dev/null || exit 1
fi

# Errors that mean "a header this machine does not have", not "this code is wrong".
# Dawn's webgpu_cpp.h is generated during its own build, so on a machine without it every renderer-facing file stops
# with "'wgpu' does not name a type" - that is a missing dependency too, not a mistake in the code.
SKIP_RE='SDL3/|imgui\.h|imgui_internal\.h|xxhash\.h|fmt/|webgpu/|dawn/|absl/|android/log\.h|jni\.h|wgpu'

failed=0
skipped=0
checked=0
for source in "$ROOT"/runtime/*.cpp; do
  name=$(basename "$source")
  # TARGET_PC / AURORA / WEBGPU_DAWN are what aurora_core sets PUBLIC, and the runtime includes its headers: without
  # them GX_TF_R8_PC and friends do not exist and every renderer file would "fail" for the wrong reason.
  output=$("$CXX" -std=c++20 -fsyntax-only -D__ANDROID__ -DTARGET_PC -DAURORA -DWEBGPU_DAWN \
           "${INCLUDES[@]}" "$source" 2>&1)
  if [ -z "$(printf '%s' "$output" | grep -E 'error:')" ]; then
    checked=$((checked + 1))
    continue
  fi
  if printf '%s' "$output" | grep -E 'error:' | grep -qE "$SKIP_RE"; then
    skipped=$((skipped + 1))
    continue
  fi
  failed=$((failed + 1))
  echo "FAIL $name"
  printf '%s\n' "$output" | grep -E 'error:' | head -5 | sed 's/^/     /'
done

echo
echo "checked: $checked   skipped (dependency not available here): $skipped   failed: $failed"
if [ ${#MISSING[@]} -gt 0 ]; then
  printf 'not available: %s\n' "${MISSING[*]}"
  echo "(install/beside this repository and point the SHADOW_*_INCLUDE variables at them to check more)"
fi
exit $((failed > 0 ? 1 : 0))
