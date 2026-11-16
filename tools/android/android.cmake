# Android cross build support, included by the top-level CMakeLists.txt.
#
# A cross build cannot run what it compiles, and two things in this project run at build
# time: file_to_c (embeds resources) and the DirectX Shader Compiler (HLSL -> SPIR-V).
# XboxRecomp2 runs at configure time too, but already selects a binary for the build
# machine (ShadowRecompLib/cmake/recompile/select.cmake).

# Provides SHADOW_RECOMP_HOST_FILE_TO_C and DIRECTX_DXC_TOOL for the build machine.
# Call before the tools and game directories are added.
function(shadow_recomp_android_host_tools)
    # --- file_to_c ---------------------------------------------------------------------
    set(SHADOW_RECOMP_HOST_FILE_TO_C "" CACHE FILEPATH
        "file_to_c executable for the build machine. Built automatically when empty.")

    if (SHADOW_RECOMP_HOST_FILE_TO_C)
        if (NOT EXISTS "${SHADOW_RECOMP_HOST_FILE_TO_C}")
            message(FATAL_ERROR "SHADOW_RECOMP_HOST_FILE_TO_C is set to ${SHADOW_RECOMP_HOST_FILE_TO_C}, which doesn't exist.")
        endif()

        set(file_to_c "${SHADOW_RECOMP_HOST_FILE_TO_C}")
    else()
        set(host_tools_dir "${CMAKE_BINARY_DIR}/host-tools")
        set(host_tools_args -G "${CMAKE_GENERATOR}" -DCMAKE_BUILD_TYPE=Release)

        # The NDK build is often driven with a ninja that is not on PATH.
        if (CMAKE_MAKE_PROGRAM)
            list(APPEND host_tools_args "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
        endif()

        message(STATUS "Android: building host tools in ${host_tools_dir}")

        # This has to pick the build machine's own compiler. CMake exports the compilers
        # of the running (Android) configure to its child processes as CC, CXX and ASM, and
        # a toolchain file may come from the environment as well; all of that would turn
        # this into a second Android build.
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env --unset=CC --unset=CXX --unset=ASM --unset=CMAKE_TOOLCHAIN_FILE
                "${CMAKE_COMMAND}" -S "${SHADOW_RECOMP_TOOLS_ROOT}/android/host" -B "${host_tools_dir}" ${host_tools_args}
            RESULT_VARIABLE configure_result
            OUTPUT_VARIABLE configure_output
            ERROR_VARIABLE configure_output
        )

        if (NOT configure_result EQUAL 0)
            message(FATAL_ERROR
                "Android: could not configure the host tools with the build machine's compiler:\n${configure_output}\n"
                "Install a host C/C++ compiler, or build tools/android/host yourself and pass "
                "-DSHADOW_RECOMP_HOST_FILE_TO_C=<path to file_to_c>.")
        endif()

        execute_process(
            COMMAND "${CMAKE_COMMAND}" --build "${host_tools_dir}" --target file_to_c --config Release
            RESULT_VARIABLE build_result
            OUTPUT_VARIABLE build_output
            ERROR_VARIABLE build_output
        )

        if (NOT build_result EQUAL 0)
            message(FATAL_ERROR "Android: could not build the host tools:\n${build_output}")
        endif()

        set(file_to_c "${host_tools_dir}/bin/file_to_c${CMAKE_HOST_EXECUTABLE_SUFFIX}")
        if (NOT EXISTS "${file_to_c}")
            message(FATAL_ERROR "Android: the host tools build did not produce ${file_to_c}.")
        endif()
    endif()

    # Without arguments file_to_c prints its usage and succeeds. Finding out here that it
    # cannot run (wrong architecture, missing runtime) beats finding out from the first
    # resource the build tries to embed.
    execute_process(
        COMMAND "${file_to_c}"
        RESULT_VARIABLE run_result
        OUTPUT_QUIET
        ERROR_QUIET
    )

    if (NOT run_result STREQUAL "0")
        message(FATAL_ERROR
            "Android: ${file_to_c} does not run on this machine (${run_result}). "
            "It has to be built for the build machine, not for Android: build tools/android/host "
            "with a host compiler and pass -DSHADOW_RECOMP_HOST_FILE_TO_C=<path to file_to_c>.")
    endif()

    message(STATUS "Android: host file_to_c: ${file_to_c}")
    set(SHADOW_RECOMP_FILE_TO_C_EXECUTABLE "${file_to_c}" PARENT_SCOPE)

    # --- DirectX Shader Compiler ---------------------------------------------------------
    # XShaderRecomp ships prebuilt compilers; pick the one for the build machine. An
    # explicit -DDIRECTX_DXC_TOOL=... (a ;-list is a command with arguments) wins.
    if (NOT DIRECTX_DXC_TOOL)
        set(dxc_root "${SHADOW_RECOMP_TOOLS_ROOT}/XShaderRecomp/thirdparty/dxc-bin")

        cmake_host_system_information(RESULT host_arch QUERY OS_PLATFORM)
        string(TOLOWER "${host_arch}" host_arch)

        if (host_arch MATCHES "^(x86_64|amd64)$")
            set(dxc_arch "x64")
        elseif (host_arch MATCHES "^(aarch64|arm64)$")
            set(dxc_arch "arm64")
        else()
            message(FATAL_ERROR "Android: no prebuilt DirectX Shader Compiler for a ${host_arch} build machine. Pass -DDIRECTX_DXC_TOOL=<dxc>.")
        endif()

        if (CMAKE_HOST_WIN32)
            set(dxc_executable "${dxc_root}/bin/${dxc_arch}/dxc.exe")
            set(dxc_command "${dxc_executable}")
        elseif (CMAKE_HOST_APPLE)
            set(dxc_executable "${dxc_root}/bin/${dxc_arch}/dxc-macos")
            set(dxc_command "${CMAKE_COMMAND}" -E env "DYLD_LIBRARY_PATH=${dxc_root}/lib/${dxc_arch}" "${dxc_executable}")
        else()
            set(dxc_executable "${dxc_root}/bin/${dxc_arch}/dxc-linux")
            set(dxc_command "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${dxc_root}/lib/${dxc_arch}" "${dxc_executable}")
        endif()

        if (NOT EXISTS "${dxc_executable}")
            message(FATAL_ERROR
                "Android: ${dxc_executable} is missing. Check out the tools/XShaderRecomp submodule, "
                "or pass -DDIRECTX_DXC_TOOL=<dxc for the build machine>.")
        endif()

        set(DIRECTX_DXC_TOOL "${dxc_command}" CACHE STRING "DirectX Shader Compiler command for the build machine" FORCE)
    endif()

    message(STATUS "Android: host DirectX Shader Compiler: ${DIRECTX_DXC_TOOL}")
    set(SHADOW_RECOMP_HOST_DXC_TOOL "${DIRECTX_DXC_TOOL}" PARENT_SCOPE)
endfunction()

# Call after the tools directory is added. XShaderRecomp's own setup describes the
# DirectX Shader Compiler for the system it believes it is building for; two things have
# to be put right for Android.
function(shadow_recomp_android_finish_tools)
    # 1. The compiler that runs at build time is the build machine's.
    set(DIRECTX_DXC_TOOL "${SHADOW_RECOMP_HOST_DXC_TOOL}" CACHE STRING "DirectX Shader Compiler command for the build machine" FORCE)

    # 2. Nothing can link the desktop libdxcompiler into an Android library, and no Android
    #    build of it exists. The game never compiles shaders at run time on Android
    #    (SHADOW_RECOMP_NO_RUNTIME_SHADER_COMPILER); a static stand-in resolves the
    #    references XShaderRecomp still carries. See os/android/dxc_stub_android.cpp.
    set(stub_dir "${CMAKE_BINARY_DIR}/android-dxc-stub")

    add_library(ShadowRecompDxcStub STATIC "${CMAKE_SOURCE_DIR}/ShadowRecomp/os/android/dxc_stub_android.cpp")
    set_target_properties(ShadowRecompDxcStub PROPERTIES
        OUTPUT_NAME "dxcompiler_stub"
        ARCHIVE_OUTPUT_DIRECTORY "${stub_dir}"
    )

    if (TARGET Microsoft::DirectXShaderCompiler)
        # Whatever links the imported compiler target now gets the stand-in instead. The
        # game target depends on ShadowRecompDxcStub, so the archive exists by link time.
        set_target_properties(Microsoft::DirectXShaderCompiler PROPERTIES
            IMPORTED_LOCATION "${stub_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}dxcompiler_stub${CMAKE_STATIC_LIBRARY_SUFFIX}"
        )
    endif()
endfunction()
