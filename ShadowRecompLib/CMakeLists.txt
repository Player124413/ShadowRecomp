project("ShadowRecompLib")

add_compile_options(
    -fno-strict-aliasing
)

if (WIN32)
    add_compile_options(/fp:precise)

    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(/clang:-ffp-contract=off)
    endif()
else()
    add_compile_options(
            -ffp-model=precise
            -ffp-contract=off
            -fno-math-errno
    )
endif()

# Note(sewer): Recompiled functions are huge.
#
# Especially since we have to emulate, a lot of things like eflags, stacks, etc.
# every original instruction is bloated a fair bit.
#
# This sometimes makes Clang's (LLVM's) default analysis limits give up on
# functions a little too soon.
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(SHADOW_RECOMP_LLVM_LIMITS
        -dse-memoryssa-scanlimit=100000
        -dse-memoryssa-walklimit=100000
        -dse-memoryssa-path-check-limit=100000
        -dse-memoryssa-defs-per-block-limit=100000
        -dse-memoryssa-otherbb-cost=1
        -memssa-check-limit=100000
        -memdep-block-number-limit=100000
        -memdep-block-scan-limit=100000
        -gvn-max-num-insns=100000
        -gvn-max-num-deps=100000
        -gvn-max-block-speculations=100000
        -available-load-scan-limit=100000
    )
    foreach(limit IN LISTS SHADOW_RECOMP_LLVM_LIMITS)
        # clang and clang-cl both accept `-mllvm`.
        add_compile_options("SHELL:-mllvm ${limit}")
    endforeach()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/recompile.cmake")

set(OPTIONAL_SOURCES "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/shader/shader_cache.cpp")
    list(APPEND OPTIONAL_SOURCES "shader/shader_cache.cpp")
endif()

add_library(ShadowRecompLib 
    ${SHADOW_RECOMP_X86_RECOMPILED_SOURCES}
    "shader/shader_cache.h"
    ${OPTIONAL_SOURCES}
)

if (ANDROID)
    # The NDK compiles everything with -g, the desktop release builds have no debug
    # information at all. The recompiled game is by far the largest part of the build and
    # nobody steps through generated code, so in release builds it is compiled like on
    # desktop. Its function names stay in the symbol table of the unstripped library.
    target_compile_options(ShadowRecompLib PRIVATE $<$<CONFIG:Release>:-g0>)
endif()

target_include_directories(ShadowRecompLib PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_precompile_headers(ShadowRecompLib PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/x86/x86_recomp_shared.h")

# Speed hack: x87 emulation in double instead of long double.
# MSVC long double is already 64-bit, so this only affects GCC/Clang.
option(X87_DOUBLE_PRECISION "x87 emulation in double instead of long double" ON)
if (X87_DOUBLE_PRECISION)
    target_compile_definitions(ShadowRecompLib PUBLIC X87_DOUBLE_PRECISION)
endif()
