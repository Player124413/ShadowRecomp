# Host build of the code generation tools the Android cross build runs.
#
# The game embeds its resources with file_to_c at build time. In a cross build the in-tree
# file_to_c target is an Android binary that the build machine cannot run, so
# tools/android/android.cmake configures and builds this project with the build machine's
# own compiler first. It can also be built by hand:
#
#   cmake -S tools/android/host -B out/build/host-tools
#   cmake --build out/build/host-tools
#
# and handed to the Android configure with -DSHADOW_RECOMP_HOST_FILE_TO_C=<path>.
cmake_minimum_required(VERSION 3.20)

project(ShadowRecompHostTools LANGUAGES C CXX)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(SHADOW_RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# file_to_c compresses what it embeds with zstd. The sources are the ones the game itself
# is built with, from the XShaderRecomp submodule, so nothing is downloaded here.
set(ZSTD_SOURCE_DIR "${SHADOW_RECOMP_ROOT}/tools/XShaderRecomp/thirdparty/zstd")
if (NOT EXISTS "${ZSTD_SOURCE_DIR}/build/cmake/CMakeLists.txt")
    message(FATAL_ERROR
        "${ZSTD_SOURCE_DIR} is missing. Check out the submodules first: git submodule update --init --recursive")
endif()

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)

add_subdirectory("${ZSTD_SOURCE_DIR}/build/cmake" zstd EXCLUDE_FROM_ALL)

add_subdirectory("${SHADOW_RECOMP_ROOT}/tools/file_to_c" file_to_c)

# A fixed location, whatever the generator, so the cross build can find the result.
set_target_properties(file_to_c PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(file_to_c PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin")
endforeach()
