# CMakeList.txt : Top-level CMake project file, do global configuration
# and include sub-projects here.
#
cmake_minimum_required (VERSION 3.20)

# Android: the game is cross-compiled into libmain.so with the NDK's CMake toolchain file
# and packaged by the Gradle project in android-apk/ (see docs/ANDROID.md). The toolchain
# file is only loaded by project(), so until then the target is recognised by what an NDK
# build passes on the command line.
if (CMAKE_SYSTEM_NAME STREQUAL "Android" OR DEFINED ANDROID_ABI OR DEFINED ANDROID_PLATFORM OR CMAKE_TOOLCHAIN_FILE MATCHES "android\\.toolchain\\.cmake$")
    set(SHADOW_RECOMP_ANDROID_DEFAULT ON)
else()
    set(SHADOW_RECOMP_ANDROID_DEFAULT OFF)
endif()

option(SHADOW_RECOMP_ANDROID "Build the Android library (libmain.so). Does not use vcpkg." ${SHADOW_RECOMP_ANDROID_DEFAULT})

# The Android build has no vcpkg dependencies: FreeType is built from source and the
# desktop-only libraries (curl, nativefiledialog, dbus, the D3D12 SDK) are not used.
if (NOT SHADOW_RECOMP_ANDROID)
    if(NOT DEFINED ENV{VCPKG_ROOT})
        message(FATAL_ERROR "VCPKG_ROOT is not defined!")
    endif()

    include($ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake)

    if(VCPKG_TARGET_TRIPLET MATCHES "static")
        set(VCPKG_APPLOCAL_DEPS OFF)
    endif()
endif()

set(SHADOW_RECOMP_THIRDPARTY_ROOT ${CMAKE_SOURCE_DIR}/thirdparty)
set(SHADOW_RECOMP_TOOLS_ROOT ${CMAKE_SOURCE_DIR}/tools)
set(CMAKE_CXX_STANDARD 20)
set(BUILD_SHARED_LIBS OFF)
set(VCPKG_APPLOCAL_DEPS OFF)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo,Release>:ProgramDatabase>>")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project ("ShadowRecomp-ALL")

if (SHADOW_RECOMP_ANDROID AND NOT ANDROID)
    message(FATAL_ERROR
        "SHADOW_RECOMP_ANDROID needs the Android NDK toolchain. Use one of the android-* presets, "
        "or pass -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake (see docs/ANDROID.md).")
elseif (ANDROID AND NOT SHADOW_RECOMP_ANDROID)
    message(FATAL_ERROR "An Android toolchain is in use: configure with -DSHADOW_RECOMP_ANDROID=ON (see docs/ANDROID.md).")
endif()

if (SHADOW_RECOMP_ANDROID)
    # The recompiled game needs a 64-bit host, and the bundled Vulkan driver loader
    # (libadrenotools) only exists for arm64.
    if (NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR "Only the arm64-v8a ABI is supported on Android (got '${CMAKE_ANDROID_ARCH_ABI}').")
    endif()

    # Everything ends up in shared objects that Android loads from the APK: the search
    # path baked in for the build tree would be meaningless on a device.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_SKIP_RPATH ON)

    # Devices with 16 KB memory pages (Android 15 and later) refuse libraries whose
    # segments are aligned for 4 KB only. NDK r28 and later do this by default.
    add_link_options("LINKER:-z,max-page-size=16384")

    # Nothing here uses C++20 modules. Without this, SDL's only C++ source on Android is
    # scanned for them, which needs clang-scan-deps from the toolchain for no benefit.
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

    # Leave fmt's OS layer (fmt/os.h) out of the library: the game does not use it, and at
    # the revision pinned by tools/XboxRecomp it is the one fmt source that does not
    # compile against libc++ (it calls malloc() without including <cstdlib>).
    set(FMT_OS OFF CACHE BOOL "" FORCE)
endif()

# Ship symbols with release builds (for now)
if (MSVC)
    # To generate PDB needs DEBUG, but doing so disables REF, ICF (re-enabling).
    add_link_options("$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF>")
endif()

if (CMAKE_OSX_ARCHITECTURES)
    set(SHADOW_RECOMP_ARCHITECTURE ${CMAKE_OSX_ARCHITECTURES})
elseif(CMAKE_SYSTEM_PROCESSOR)
    set(SHADOW_RECOMP_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
else()
    set(SHADOW_RECOMP_ARCHITECTURE ${CMAKE_HOST_SYSTEM_PROCESSOR})
endif()
string(TOLOWER "${SHADOW_RECOMP_ARCHITECTURE}" SHADOW_RECOMP_ARCHITECTURE)
message(STATUS "Detected architecture: ${SHADOW_RECOMP_ARCHITECTURE}")

if (SHADOW_RECOMP_ARCHITECTURE STREQUAL "x86_64" OR SHADOW_RECOMP_ARCHITECTURE STREQUAL "amd64")
    add_compile_options(
        -march=sandybridge # You have a puter made after 2011, right?
        -mtune=znver4      # Most probably have one circa 2022-2023
    )
endif()

# Sewer says: Don't clear used registers before every return.
# Some toolchains (e.g. Nix's clang wrapper) turn this on by default.
# It adds a few `xor`s to every function: ~1.8% code size, with only speed loss.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-fzero-call-used-regs=skip SHADOW_RECOMP_HAS_ZERO_CALL_USED_REGS)
if (SHADOW_RECOMP_HAS_ZERO_CALL_USED_REGS)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-fzero-call-used-regs=skip>)
endif()


if (SHADOW_RECOMP_ANDROID)
    include("${SHADOW_RECOMP_TOOLS_ROOT}/android/android.cmake")
    shadow_recomp_android_host_tools()
endif()

add_subdirectory(${SHADOW_RECOMP_THIRDPARTY_ROOT})
add_subdirectory(${SHADOW_RECOMP_TOOLS_ROOT})

if (SHADOW_RECOMP_ANDROID)
    shadow_recomp_android_finish_tools()
endif()

# Include sub-projects.
add_subdirectory("ShadowRecompLib")
add_subdirectory ("ShadowRecomp")
