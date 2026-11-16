cmake_policy(SET CMP0077 NEW)

set(MSDF_ATLAS_BUILD_STANDALONE OFF)
set(MSDF_ATLAS_USE_SKIA OFF)
set(MSDF_ATLAS_NO_ARTERY_FONT ON)
set(MSDFGEN_DISABLE_PNG ON)

set(SDL_TEST_LIBRARY OFF)

set(SDLMIXER_DEPS_SHARED OFF)
set(SDLMIXER_VENDORED OFF)
set(SDLMIXER_FLAC OFF)
set(SDLMIXER_GME OFF)
set(SDLMIXER_MOD OFF)
set(SDLMIXER_MP3 OFF)
set(SDLMIXER_MIDI OFF)
set(SDLMIXER_OPUS OFF)
set(SDLMIXER_VORBIS_STB ON)
set(SDLMIXER_VORBIS_VORBISFILE OFF)
set(SDLMIXER_WAVPACK OFF)

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(PLUME_SDL3_VULKAN_ENABLED ON CACHE BOOL "")
endif()

if (WIN32)
    set(PLUME_D3D12_AGILITY_SDK_ENABLED ON CACHE BOOL "")
endif()

if (ANDROID)
    # The Android build does not use vcpkg, so FreeType (font rasterizer of Dear ImGui
    # and msdfgen) is built from source. It is configured without its optional
    # dependencies: the game's fonts need neither PNG glyphs nor WOFF2, and gzip support
    # falls back to the zlib copy bundled with FreeType.
    include(FetchContent)

    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
      freetype
      GIT_REPOSITORY https://github.com/freetype/freetype.git
      GIT_TAG VER-2-14-3
      GIT_SHALLOW TRUE
    )

    FetchContent_MakeAvailable(freetype)

    if (NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()

    # msdf-atlas-gen would otherwise insist on a vcpkg toolchain.
    set(MSDF_ATLAS_USE_VCPKG OFF)
endif()

add_subdirectory("${SHADOW_RECOMP_THIRDPARTY_ROOT}/msdf-atlas-gen")

# Discord has no Android IPC transport; the rich presence calls are stubbed there.
if (NOT ANDROID)
    add_subdirectory("${SHADOW_RECOMP_THIRDPARTY_ROOT}/discord-rpc")
endif()

add_subdirectory("${SHADOW_RECOMP_THIRDPARTY_ROOT}/o1heap")
add_subdirectory("${SHADOW_RECOMP_THIRDPARTY_ROOT}/SDL")
add_subdirectory("${SHADOW_RECOMP_THIRDPARTY_ROOT}/SDL_mixer")
add_subdirectory("${SHADOW_RECOMP_THIRDPARTY_ROOT}/plume")

if (ANDROID)
    # Lets the game load a Vulkan driver other than the one that ships with the device
    # (Mesa Turnip on Adreno GPUs). See ShadowRecomp/os/android/vulkan_driver_android.h.
    add_subdirectory("${SHADOW_RECOMP_THIRDPARTY_ROOT}/libadrenotools")

    # plume loads Vulkan through volk, which dlopen()s "libvulkan.so". Routing that one
    # call through the game is all it takes to substitute the driver, and it keeps the
    # plume submodule unmodified. The replacement forwards every other library to the
    # real dlopen().
    target_compile_definitions(plume PRIVATE dlopen=ShadowRecomp_AndroidVulkanDlopen)
endif()
