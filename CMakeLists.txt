cmake_minimum_required(VERSION 3.25)
project(ShadowRecomp C CXX)
if(WIN32)
  enable_language(RC)          # the exe icon; Android has no resource compiler
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# --- Aurora (GX renderer, window, input) --------------------------------------------------
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
file(TO_CMAKE_PATH "$ENV{USERPROFILE}/miniconda3" _conda)   # keep a user conda out of the package search
set(CMAKE_IGNORE_PREFIX_PATH "${_conda}" CACHE STRING "" FORCE)
if(ANDROID)
  # libSDL3.so, as SDL3's own Android project does: its Java activity loads it by name before libmain.so. Built from
  # source here (Aurora insists on that on Android), so no SDL3 SDK is needed on the machine.
  set(AURORA_SDL3_PROVIDER vendor CACHE STRING "" FORCE)
  set(AURORA_SDL3_LINKAGE shared CACHE STRING "" FORCE)
endif()

# --- sqlite3, before Aurora gets to choose ---------------------------------------------------------------
# Aurora's pipeline and texture caches include <sqlite3.h>. It tries an installed package, then pkg-config, then the
# SQLite amalgamation - but on Android none of the first two can be right: the NDK ships no sqlite3 and a pkg-config hit
# on the build machine would be the host's copy. Aurora only creates the `sqlite3` target when none exists, so the
# deterministic answer is to build the amalgamation for the target ABI here, first.
include(FetchContent)
function(shadow_fetch_sqlite3 target_name)
  FetchContent_Declare(shadow_sqlite3
    URL https://sqlite.org/2026/sqlite-amalgamation-3510300.zip
    URL_HASH SHA256=acb1e6f5d832484bf6d32b681e858c38add8b2acdfd42ac5df24b8afb46552b4
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(shadow_sqlite3)
  add_library(${target_name} STATIC ${shadow_sqlite3_SOURCE_DIR}/sqlite3.c)
  target_include_directories(${target_name} PUBLIC ${shadow_sqlite3_SOURCE_DIR})
  if(ANDROID)
    # Bionic keeps dlopen in libc, but naming it is harmless and the maths library is not implicit in a static archive.
    target_link_libraries(${target_name} PUBLIC ${CMAKE_DL_LIBS} m)
  endif()
  message(STATUS "shadow: sqlite3 built from the amalgamation in ${shadow_sqlite3_SOURCE_DIR}")
endfunction()

if(ANDROID AND NOT TARGET sqlite3)
  shadow_fetch_sqlite3(sqlite3)
endif()

add_subdirectory(third_party/aurora ${CMAKE_BINARY_DIR}/aurora EXCLUDE_FROM_ALL)

# Anywhere else, trust what Aurora resolved, but only if it really has the header: `target_link_libraries(.. sqlite3)`
# treats a missing target as a plain library name, so a half-found package silently becomes -lsqlite3 and the sources
# that include <sqlite3.h> fail one by one instead of the configure step saying so.
set(_shadow_sqlite_ok FALSE)
if(TARGET sqlite3)
  get_target_property(_shadow_sqlite_include_dirs sqlite3 INTERFACE_INCLUDE_DIRECTORIES)
  foreach(_dir IN LISTS _shadow_sqlite_include_dirs)
    if(EXISTS "${_dir}/sqlite3.h")
      set(_shadow_sqlite_ok TRUE)
    endif()
  endforeach()
  message(STATUS "shadow: sqlite3 target from aurora (include dirs: ${_shadow_sqlite_include_dirs})")
endif()
if(NOT _shadow_sqlite_ok)
  if(TARGET sqlite3)
    # An imported/alias target cannot be given include directories; build our own and hand the header to the targets
    # that need it, which is enough because the link name is already right.
    shadow_fetch_sqlite3(shadow_sqlite3)
    foreach(_target aurora_core aurora_gx)
      if(TARGET ${_target})
        target_include_directories(${_target} PRIVATE ${shadow_sqlite3_SOURCE_DIR})
      endif()
    endforeach()
  else()
    shadow_fetch_sqlite3(sqlite3)
  endif()
endif()

# generated/ is produced by tools/gen.py from the player's own disc image and is never committed. Without it there is
# nothing to simulate, so say that plainly instead of failing on a missing include or an unknown function.
if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/generated/files.cmake)
  message(FATAL_ERROR "generated/files.cmake is missing.\n"
    "  Run:  python tools/gen.py     (from the repository root, once)\n"
    "  It needs Python 3 and the uncompiled main.dol of your own Shadow the Hedgehog (USA) disc, which the tool \n"
    "  takes from the disc folder or from --dol; the result (the recompiled game code) stays out of git.")
endif()
include(generated/files.cmake)

# A launcher-only build: `python tools/android/stub_generated.py --definitions` writes empty stand-ins for the game's
# recompiled functions, which makes the app link and install without anyone's disc. It cannot play anything - every
# guest function returns at once - but it is how the APK pipeline itself (Gradle, the NDK, SDL3's Java classes, the
# 16 KB page alignment) gets built and tested without the game. The stand-in source refuses to compile unless this
# option is on, so a real build cannot pick it up by accident.
option(SHADOW_STUB_GAME_CODE "Link stand-ins for the game code (launcher-only build; cannot play a disc)" OFF)
if(SHADOW_STUB_GAME_CODE)
  set(_shadow_stub_source ${CMAKE_CURRENT_LIST_DIR}/generated/stub_definitions.cpp)
  if(NOT EXISTS ${_shadow_stub_source})
    message(FATAL_ERROR "SHADOW_STUB_GAME_CODE is on but generated/stub_definitions.cpp is missing.\n"
      "  Run:  python tools/android/stub_generated.py --definitions")
  endif()
  list(APPEND GEN_SOURCES ${_shadow_stub_source})
  add_compile_definitions(SHADOW_STUB_GAME_CODE=1)
  message(STATUS "shadow: launcher-only build, the game code is a stand-in (no disc can be played)")
endif()
file(GLOB RT_SOURCES ${CMAKE_CURRENT_LIST_DIR}/runtime/*.cpp)
# The exe icon and launcher art (runtime/shadow.ico, runtime/assets/*.png) come from the game, so they are not in git:
# a fresh clone builds with the neutral placeholders instead. An existing file is never replaced.
if(NOT ANDROID)
foreach(pair "shadow.ico=icon.ico" "assets/emblem.png=emblem.png" "assets/hero.png=hero.png")
  string(REPLACE "=" ";" pair "${pair}")
  list(GET pair 0 dst)
  list(GET pair 1 src)
  if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/runtime/${dst})
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/runtime/assets)
    file(COPY_FILE ${CMAKE_CURRENT_LIST_DIR}/tools/release/placeholder/${src} ${CMAKE_CURRENT_LIST_DIR}/runtime/${dst})
  endif()
endforeach()
endif()
# WIN32: a normal Windows program (no console window when double-clicked); runtime/shadow.rc embeds the icon.
# #137 build number: version (VERSION file) + configure time + short git hash -> one generated source file
set(SHADOW_VERSION "dev")
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/VERSION)
  file(READ ${CMAKE_CURRENT_LIST_DIR}/VERSION SHADOW_VERSION)
  string(STRIP "${SHADOW_VERSION}" SHADOW_VERSION)
endif()
string(TIMESTAMP SHADOW_BUILD_DATE "%Y-%m-%d %H:%M")
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE SHADOW_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT SHADOW_GIT_HASH)
  set(SHADOW_GIT_HASH "nogit")
else()
  execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                  OUTPUT_VARIABLE SHADOW_GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(SHADOW_GIT_DIRTY)
    set(SHADOW_GIT_HASH "${SHADOW_GIT_HASH}+")
  endif()
endif()
configure_file(${CMAKE_CURRENT_LIST_DIR}/runtime/build_info.cpp.in ${CMAKE_BINARY_DIR}/build_info.cpp @ONLY)

# ======================================================================================
# Android (64-bit only: arm64-v8a for phones, x86_64 for the emulator)
# ======================================================================================
# Built by Gradle (android/): the app's native side is libmain.so, which SDL3 loads and enters through SDL_main.
# Everything that is Windows-only (the Win32 launcher, the Win32 first-run window, the Win32 platform layer and the
# icon/resource script) is left out; runtime/first_run_android.cpp and plat_posix.cpp take over. The Java sources of
# SDL3 and of Aurora's surface are copied into the Gradle project here, so there is one source of truth for them.
if(ANDROID)
  set(SHADOW_ANDROID_DIR ${CMAKE_CURRENT_LIST_DIR}/android)
  set(SHADOW_JAVA_OUT ${SHADOW_ANDROID_DIR}/app/src/main/java)
  foreach(dir IN ITEMS "${AURORA_SDL3_JAVA_SOURCE_DIR}" "${AURORA_ANDROID_JAVA_SOURCE_DIR}")
    if(IS_DIRECTORY "${dir}")
      file(COPY "${dir}/" DESTINATION "${SHADOW_JAVA_OUT}")
    endif()
  endforeach()

  list(FILTER RT_SOURCES EXCLUDE REGEX "/runtime/(launcher|first_run|plat_win)\\.cpp$")
  add_library(main SHARED ${RT_SOURCES} ${GEN_SOURCES} ${CMAKE_BINARY_DIR}/build_info.cpp)
  target_include_directories(main PRIVATE runtime generated third_party/aurora/lib third_party/aurora/include)
  # -O3 and no frame pointers: the game is one thread of interdependent work and 60 fps is the target on every phone.
  # No fast-math: the emulated PPC code must round exactly like the console.
  target_compile_options(main PRIVATE -O3 -fno-strict-aliasing -fomit-frame-pointer -ffunction-sections -fdata-sections)
  # --gc-sections: only the game code that is reached is kept (the recompiled image is big).
  # max-page-size: Android 15+ can map libraries with 16 KB pages; without the flag such a device refuses to load them.
  target_link_options(main PRIVATE -Wl,--gc-sections -Wl,--as-needed -Wl,-z,max-page-size=16384)
  target_compile_definitions(main PRIVATE WEBGPU_DAWN)
  # -llog / -landroid / EGL / GLESv3 come out of the NDK's own sysroot: plain names are what the toolchain expects.
  target_link_libraries(main PRIVATE aurora_gx aurora_core aurora_vi ${AURORA_SDL3_TARGET}
      absl::btree absl::flat_hash_map Tracy::TracyClient dawn::webgpu_dawn sqlite3 PNG::PNG
      log android EGL GLESv3)

  # Gradle packages whatever is in jniLibs: both our libmain.so and libSDL3.so have to land there, or the app starts and
  # immediately dies in System.loadLibrary.
  set(_shadow_jnilibs ${SHADOW_ANDROID_DIR}/app/src/main/jniLibs/${CMAKE_ANDROID_ARCH_ABI})
  set_target_properties(main PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${_shadow_jnilibs})
  if(TARGET SDL3-shared)
    set_target_properties(SDL3-shared PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${_shadow_jnilibs})
  else()
    message(WARNING "SDL3-shared target not found: libSDL3.so may not be packaged into the APK")
  endif()
  return()
endif()

# ======================================================================================
# Windows
# ======================================================================================
add_executable(shadow WIN32 ${RT_SOURCES} ${GEN_SOURCES} runtime/shadow.rc ${CMAKE_BINARY_DIR}/build_info.cpp)
target_include_directories(shadow PRIVATE runtime generated third_party/aurora/lib third_party/aurora/include)
target_compile_options(shadow PRIVATE -O2 -gcodeview -gline-tables-only -fno-strict-aliasing -fno-asynchronous-unwind-tables -w)
target_link_options(shadow PRIVATE -Wl,/STACK:268435456 -Wl,/DEBUG -Wl,/ENTRY:mainCRTStartup)
target_compile_definitions(shadow PRIVATE WEBGPU_DAWN)
target_link_libraries(shadow PRIVATE aurora_gx aurora_core aurora_vi dwmapi dbghelp winmm
    absl::btree absl::flat_hash_map Tracy::TracyClient dawn::webgpu_dawn sqlite3 PNG::PNG)
