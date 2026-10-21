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
add_subdirectory(third_party/aurora ${CMAKE_BINARY_DIR}/aurora EXCLUDE_FROM_ALL)

# sqlite3 backs Aurora's pipeline and texture caches. Aurora looks for it in three ways - an installed package, a
# pkg-config file, or fetching the SQLite amalgamation - but if all three come up empty, `target_link_libraries(... sqlite3)`
# does not fail at configure time: the name is then taken as a plain library (`-lsqlite3`) and the sources that include
# <sqlite3.h> fail to compile with "file not found". That is what happened on the Android CI machine. So check here and
# fetch the amalgamation ourselves in that case; the include directory is then ours to set.
if(TARGET sqlite3)
  get_target_property(_shadow_sqlite_include sqlite3 INTERFACE_INCLUDE_DIRECTORIES)
  message(STATUS "shadow: sqlite3 target present (include dirs: ${_shadow_sqlite_include})")
else()
  message(STATUS "shadow: aurora produced no sqlite3 target - fetching the SQLite amalgamation")
  include(FetchContent)
  FetchContent_Declare(shadow_sqlite3
    URL https://sqlite.org/2026/sqlite-amalgamation-3510300.zip
    URL_HASH SHA256=acb1e6f5d832484bf6d32b681e858c38add8b2acdfd42ac5df24b8afb46552b4
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
  )
  FetchContent_MakeAvailable(shadow_sqlite3)
  add_library(sqlite3 STATIC ${shadow_sqlite3_SOURCE_DIR}/sqlite3.c)
  target_include_directories(sqlite3 PUBLIC ${shadow_sqlite3_SOURCE_DIR})
  message(STATUS "shadow: sqlite3 fetched into ${shadow_sqlite3_SOURCE_DIR}")
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
