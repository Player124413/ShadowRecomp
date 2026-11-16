# Host-side tests for the Android layer.
#
# The Android-only sources are built for the build machine, with the Android, SDL and
# libadrenotools interfaces they call replaced by test doubles, and driven through
# situations that are awkward to produce on a device: a driver that takes the app down at
# startup, a broken driver package, an interrupted import, a full disk, a frozen frame loop.
# Nothing here needs the NDK, a device or the game files.
#
#   cmake -S tools/android/tests -B out/build/android-tests
#   cmake --build out/build/android-tests
#   ctest --test-dir out/build/android-tests --output-on-failure
#
# Needs a host C++20 compiler (GCC or Clang), Python 3 and the submodules. Without a JDK
# the Java tests are left out; without curl or 7z, the tests of links and of .7z archives.

cmake_minimum_required(VERSION 3.20)

project(ShadowRecompAndroidTests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(SHADOW_RECOMP_TESTS_SANITIZE "Build the tests with AddressSanitizer and UndefinedBehaviorSanitizer" ON)

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(GAME_ROOT "${REPOSITORY_ROOT}/ShadowRecomp")
set(THIRDPARTY_ROOT "${REPOSITORY_ROOT}/thirdparty")
set(FMT_INCLUDE "${REPOSITORY_ROOT}/tools/XboxRecomp/thirdparty/fmt/include")
set(XXHASH_ROOT "${REPOSITORY_ROOT}/tools/XboxRecomp/thirdparty/xxHash")
set(VOLK_INCLUDE "${THIRDPARTY_ROOT}/plume/contrib/volk")
set(VULKAN_HEADERS_INCLUDE "${THIRDPARTY_ROOT}/plume/contrib/Vulkan-Headers/include")

foreach (required
        "${THIRDPARTY_ROOT}/SDL/include/SDL3/SDL.h"
        "${THIRDPARTY_ROOT}/imgui/imgui.cpp"
        "${FMT_INCLUDE}/fmt/format.h"
        "${XXHASH_ROOT}/xxhash.c"
        "${VOLK_INCLUDE}/volk.h"
        "${VULKAN_HEADERS_INCLUDE}/vulkan/vulkan_core.h")
    if (NOT EXISTS "${required}")
        message(FATAL_ERROR "${required} is missing. Run: git submodule update --init --recursive")
    endif()
endforeach()

# The same release the game is built with (ShadowRecomp/CMakeLists.txt).
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

enable_testing()

# Common setup: prelude.h stands in for the game's forced precompiled header, and fake/
# comes first so that its headers win over the real ones of the same name.
function(android_test_executable name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "NO_ADDRESS_SANITIZER" "" "SOURCES")

    add_executable(${name} ${arg_SOURCES})
    target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/fake"
        "${GAME_ROOT}"
        "${THIRDPARTY_ROOT}/SDL/include"
        "${FMT_INCLUDE}"
    )
    target_compile_options(${name} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/prelude.h\">"
        -Wall -Wextra
    )

    if (SHADOW_RECOMP_TESTS_SANITIZE)
        set(sanitizers "undefined")
        if (NOT arg_NO_ADDRESS_SANITIZER)
            set(sanitizers "address,undefined")
        endif()

        target_compile_options(${name} PRIVATE -fsanitize=${sanitizers} -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(${name} PRIVATE -fsanitize=${sanitizers})
    endif()
endfunction()

# --- On-screen controls ---------------------------------------------------------------
# ui/touch_controls.cpp with the real Dear ImGui (no rendering backend) and stubbed SDL.
android_test_executable(touch_test SOURCES
    touch_test.cpp
    "${GAME_ROOT}/ui/touch_controls.cpp"
    "${THIRDPARTY_ROOT}/imgui/imgui.cpp"
    "${THIRDPARTY_ROOT}/imgui/imgui_draw.cpp"
    "${THIRDPARTY_ROOT}/imgui/imgui_tables.cpp"
    "${THIRDPARTY_ROOT}/imgui/imgui_widgets.cpp"
)
target_include_directories(touch_test PRIVATE "${THIRDPARTY_ROOT}/imgui")
set_source_files_properties(
    "${THIRDPARTY_ROOT}/imgui/imgui.cpp"
    "${THIRDPARTY_ROOT}/imgui/imgui_draw.cpp"
    "${THIRDPARTY_ROOT}/imgui/imgui_tables.cpp"
    "${THIRDPARTY_ROOT}/imgui/imgui_widgets.cpp"
    PROPERTIES COMPILE_OPTIONS "-w"
)
add_test(NAME touch_controls COMMAND touch_test)

# --- Staged game installation ---------------------------------------------------------
# os/android/install_android.cpp with a scripted installer (fake/install/installer.h).
android_test_executable(install_test SOURCES
    install_test.cpp
    "${GAME_ROOT}/os/android/install_android.cpp"
)
add_test(NAME staged_install COMMAND install_test)

# --- Vulkan driver selection, import and startup recovery ------------------------------
# The loader keeps its state for the lifetime of the process, so every launch of the app
# is one run of driver_harness; driver_test.py strings launches together.
android_test_executable(driver_harness SOURCES
    driver_harness.cpp
    "${GAME_ROOT}/os/android/vulkan_driver_android.cpp"
    "${GAME_ROOT}/os/android/storage_android.cpp"
    "${THIRDPARTY_ROOT}/miniz/miniz.c"
)
set_source_files_properties("${THIRDPARTY_ROOT}/miniz/miniz.c" PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(driver_harness PRIVATE
    "${THIRDPARTY_ROOT}/libadrenotools/include"
    "${THIRDPARTY_ROOT}/miniz"
)
target_link_libraries(driver_harness PRIVATE nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
# The harness hands itself out as the "driver": its vkGetInstanceProcAddr must be visible to dlsym().
set_target_properties(driver_harness PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME vulkan_driver COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/driver_test.py" $<TARGET_FILE:driver_harness>)

# --- Log file, crash reports and the hang watchdog --------------------------------------
# Without AddressSanitizer: it has its own ideas about SIGSEGV, which this test raises on purpose.
android_test_executable(logger_harness NO_ADDRESS_SANITIZER SOURCES
    logger_harness.cpp
    "${GAME_ROOT}/os/android/logger_android.cpp"
    "${GAME_ROOT}/os/android/storage_android.cpp"
)
target_link_libraries(logger_harness PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
add_test(NAME logger COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/logger_test.py" $<TARGET_FILE:logger_harness>)
set_tests_properties(logger PROPERTIES TIMEOUT 300)

# --- Background pipeline compilation -----------------------------------------------------
# gpu/background_work_queue.h: the threads that create pipelines while the game runs.
android_test_executable(work_queue_test SOURCES work_queue_test.cpp)
target_link_libraries(work_queue_test PRIVATE Threads::Threads)
add_test(NAME background_work_queue COMMAND work_queue_test)

# --- Which Vulkan drivers the game accepts ------------------------------------------------
android_test_executable(requirements_test SOURCES requirements_test.cpp)
add_test(NAME vulkan_requirements COMMAND requirements_test)

# --- The graphics driver's pipeline cache, kept on disk ---------------------------------
# gpu/vulkan_pipeline_cache.cpp against a stand-in driver: the harness defines the three
# volk entry points the code uses. Every launch of the app is one run of the harness.
android_test_executable(pipeline_cache_harness SOURCES
    pipeline_cache_harness.cpp
    "${GAME_ROOT}/gpu/vulkan_pipeline_cache.cpp"
    "${XXHASH_ROOT}/xxhash.c"
)
set_source_files_properties("${XXHASH_ROOT}/xxhash.c" PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(pipeline_cache_harness PRIVATE
    "${VOLK_INCLUDE}"
    "${VULKAN_HEADERS_INCLUDE}"
    "${XXHASH_ROOT}"
)
target_link_libraries(pipeline_cache_harness PRIVATE Threads::Threads)
add_test(NAME pipeline_cache COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/pipeline_cache_test.py" $<TARGET_FILE:pipeline_cache_harness>)

# --- Build tools (Python) ---------------------------------------------------------------
# fetch-xbe.py gets default.xbe out of a disc image, an archive, a folder or a link;
# stub-recompiler.py stands in for XboxRecomp2 when the pipeline is tested without the game.
add_test(NAME fetch_xbe COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/fetch_xbe_test.py")
add_test(NAME stub_recompiler COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/stub_recompiler_test.py")
set_tests_properties(fetch_xbe stub_recompiler PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# --- Launcher (Java) --------------------------------------------------------------------
# The launcher classes without Android dependencies: the Xbox disc image check, the game
# ZIP extractor and the copy of an installation to another storage location.
find_package(Java 11 COMPONENTS Development)
if (Java_FOUND)
    set(java_classes "${CMAKE_CURRENT_BINARY_DIR}/java-classes")
    set(launcher_java "${REPOSITORY_ROOT}/android-apk/app/src/main/java/org/shadowrecomp/android")
    set(test_java "${CMAKE_CURRENT_SOURCE_DIR}/java/org/shadowrecomp/android")
    set(java_sources
        "${launcher_java}/InstallerArchive.java"
        "${launcher_java}/LocalizedIOException.java"
        "${launcher_java}/StorageMigration.java"
        "${launcher_java}/XboxDiscImage.java"
        "${test_java}/LauncherFilesTest.java"
        "${test_java}/XboxDiscImageTest.java"
    )
    add_custom_command(
        OUTPUT "${java_classes}/compiled.stamp"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${java_classes}"
        COMMAND "${Java_JAVAC_EXECUTABLE}" -encoding UTF-8 -d "${java_classes}" ${java_sources}
        COMMAND "${CMAKE_COMMAND}" -E touch "${java_classes}/compiled.stamp"
        DEPENDS ${java_sources}
        VERBATIM
    )
    add_custom_target(java_tests ALL DEPENDS "${java_classes}/compiled.stamp")
    add_test(NAME xbox_disc_image COMMAND "${Java_JAVA_EXECUTABLE}" -cp "${java_classes}" org.shadowrecomp.android.XboxDiscImageTest)
    add_test(NAME launcher_files COMMAND "${Java_JAVA_EXECUTABLE}" -cp "${java_classes}" org.shadowrecomp.android.LauncherFilesTest)
else()
    message(STATUS "No JDK found: the launcher's Java tests are skipped.")
endif()
