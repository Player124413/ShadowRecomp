project("ShadowRecomp")

if (WIN32)
    option(SHADOW_RECOMP_D3D12 "Add D3D12 support for rendering" ON)
endif()

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    option(SHADOW_RECOMP_FLATPAK "Configure the build for Flatpak compatibility." OFF)
endif()

function(BIN2C)
    cmake_parse_arguments(BIN2C_ARGS "" "TARGET_OBJ;SOURCE_FILE;DEST_FILE;ARRAY_NAME;COMPRESSION_TYPE" "" ${ARGN})

    if(NOT BIN2C_ARGS_TARGET_OBJ)
        message(FATAL_ERROR "TARGET_OBJ not specified.")
    endif()

    if(NOT BIN2C_ARGS_SOURCE_FILE)
        message(FATAL_ERROR "SOURCE_FILE not specified.")
    endif()

    if(NOT BIN2C_ARGS_DEST_FILE)
        set(BIN2C_ARGS_DEST_FILE "${BIN2C_ARGS_SOURCE_FILE}")
    endif()

    if(NOT BIN2C_ARGS_COMPRESSION_TYPE)
        set(BIN2C_ARGS_COMPRESSION_TYPE "none")
    endif()

    # A cross build (Android) cannot run the in-tree file_to_c target; the top-level
    # CMakeLists.txt provides one built for the build machine instead.
    if(SHADOW_RECOMP_FILE_TO_C_EXECUTABLE)
        set(BIN2C_COMMAND "${SHADOW_RECOMP_FILE_TO_C_EXECUTABLE}")
    else()
        set(BIN2C_COMMAND $<TARGET_FILE:file_to_c>)
    endif()

    add_custom_command(OUTPUT "${BIN2C_ARGS_DEST_FILE}.c"
        COMMAND ${BIN2C_COMMAND} "${BIN2C_ARGS_SOURCE_FILE}" "${BIN2C_ARGS_ARRAY_NAME}" "${BIN2C_ARGS_COMPRESSION_TYPE}" "${BIN2C_ARGS_DEST_FILE}.c" "${BIN2C_ARGS_DEST_FILE}.h"
        DEPENDS "${BIN2C_ARGS_SOURCE_FILE}"
        BYPRODUCTS "${BIN2C_ARGS_DEST_FILE}.h"
        COMMENT "Generating binary header for ${BIN2C_ARGS_SOURCE_FILE}..."
    )

    set_source_files_properties(${BIN2C_ARGS_DEST_FILE}.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_sources(${BIN2C_ARGS_TARGET_OBJ} PRIVATE ${BIN2C_ARGS_DEST_FILE}.c)
endfunction()

add_compile_options(
    -fno-strict-aliasing
    -Wno-switch
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-void-pointer-to-int-cast
    -Wno-int-to-void-pointer-cast
    -Wno-invalid-offsetof
    -Wno-null-arithmetic
    -Wno-null-conversion
    -Wno-tautological-undefined-compare
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

add_compile_definitions(
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR 
    _CRT_SECURE_NO_WARNINGS)

# On Android the entry point is SDL_main(), which SDL's Java activity looks up and calls
# on its own thread, so SDL_main.h must be left free to rename main() (see main.cpp).
if (NOT ANDROID)
    add_compile_definitions(SDL_MAIN_HANDLED)
endif()

set(SHADOW_RECOMP_PRECOMPILED_HEADERS
    "stdafx.h"
)

set(SHADOW_RECOMP_KERNEL_CXX_SOURCES
    "kernel/imports.cpp"
    "kernel/kernel.cpp"
    "kernel/heap.cpp"
    "kernel/memory.cpp"
    "kernel/io/file_system.cpp"
)


set(SHADOW_RECOMP_LOCALE_CXX_SOURCES
    "locale/achievement_locale.cpp"
    "locale/config_locale.cpp"
    "locale/locale.cpp"
)

if (WIN32)
    set(SHADOW_RECOMP_OS_CXX_SOURCES
        "os/win32/logger_win32.cpp"
        "os/win32/process_win32.cpp"
        "os/win32/media_win32.cpp"
        "os/win32/user_win32.cpp"
        "os/win32/version_win32.cpp"

    )
elseif (ANDROID)
    set(SHADOW_RECOMP_OS_CXX_SOURCES
        "os/android/install_android.cpp"
        "os/android/jni_android.cpp"
        "os/android/lifecycle_android.cpp"
        "os/android/logger_android.cpp"
        "os/android/media_android.cpp"
        "os/android/process_android.cpp"
        "os/android/storage_android.cpp"
        "os/android/stubs_android.cpp"
        "os/android/user_android.cpp"
        "os/android/vulkan_driver_android.cpp"
    )
elseif (CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(SHADOW_RECOMP_OS_CXX_SOURCES
        "os/linux/logger_linux.cpp"
        "os/linux/media_linux.cpp"
        "os/linux/process_linux.cpp"
        "os/linux/user_linux.cpp"
    )
endif()


set(SHADOW_RECOMP_CPU_CXX_SOURCES
    "cpu/guest_thread.cpp"
)

set(SHADOW_RECOMP_GPU_CXX_SOURCES
    "gpu/video.cpp"
    "gpu/vertexbuffer.cpp"
    "gpu/textures.cpp"
    "gpu/guest_convert.cpp"
    "gpu/ubershaders/fixed_function_pixel_shader_variants.cpp"
    "gpu/ubershaders/fixed_function_vertex_shader_variants.cpp"
    "gpu/frame_pacing/frame_pacer.cpp"
    "gpu/frame_pacing/timer_resolution.cpp"
    "gpu/vertexshader.cpp"
    "gpu/vertexdeclaration.cpp"
    "gpu/pixelshader.cpp"
    "gpu/pipeline.cpp"
    "gpu/pushbuffer.cpp"
    "gpu/resources.cpp"
    "gpu/swizzle.cpp"
    "gpu/FixedFunction.cpp"
    "gpu/renderstate.cpp"
    "gpu/imgui/imgui_common.cpp"
    "gpu/imgui/imgui_font_builder.cpp"
    "gpu/imgui/imgui_snapshot.cpp"
)

if (ANDROID)
    list(APPEND SHADOW_RECOMP_GPU_CXX_SOURCES "gpu/android_video.cpp")
endif()


set(SHADOW_RECOMP_APU_CXX_SOURCES
    "audio/audio.cpp"
    "audio/driver/sdl3_driver.cpp"
    "audio/embedded_player.cpp"
)

set(SHADOW_RECOMP_HID_CXX_SOURCES
    "input/input.cpp"
)

set(SHADOW_RECOMP_PATCHES_CXX_SOURCES
    "patches/start_patches.cpp"
    "patches/bloom_patches.cpp"
    "patches/fmv_patches.cpp"
    "patches/multi_core_patches.cpp"
    "patches/aspect_ratio_patches.cpp"
    "patches/audio_patches.cpp"
    "patches/fps_patches.cpp"
    "patches/collision_patches.cpp"
    "patches/animated_texture_patches.cpp"
    "patches/frontend_listener.cpp"
    "patches/pause_patches.cpp"
    "patches/prompt_patches.cpp"
    "patches/player_patches.cpp"
    "patches/motion_patches.cpp"
    "patches/misc_patches.cpp"
    "patches/vector_patches.cpp"
    "patches/multiplayer_mod_patches.cpp"
    "patches/reloaded_mod_patches.cpp"
    "patches/objects_patches.cpp"
    "patches/titlescreen_patches.cpp"
    "patches/unix_patches.cpp"
    "patches/gravity_patches.cpp"
    "patches/limit_break_patches.cpp"
    "patches/code_patches.cpp"
)


set(SHADOW_RECOMP_USER_CXX_SOURCES
    "user/achievement_data.cpp"
    "user/achievement_manager.cpp"
    "user/config.cpp"
    "user/registry.cpp"
    "user/paths.cpp"
)

set(SHADOW_RECOMP_INSTALL_CXX_SOURCES
    "install/directory_file_system.cpp"
    "install/hashes/game.cpp"
    "install/installer.cpp"
    "install/update_checker.cpp"
    "install/xiso_file_system.cpp"
)

set(SHADOW_RECOMP_UI_CXX_SOURCES
    "ui/achievement_menu.cpp"
    "ui/achievement_overlay.cpp"
    "ui/game_window.cpp"
    "ui/imgui_utils.cpp"
    "ui/button_guide.cpp"
    "ui/fader.cpp"
    "ui/options_menu.cpp"
    "ui/message_window.cpp"
    "ui/menu_common.cpp"
    "ui/multiplayer_hud.cpp"
    "ui/installer_wizard.cpp"
)

set(SHADOW_RECOMP_MOD_CXX_SOURCES
    "mod/mod_loader.cpp"
    "mod/IniFile.cpp"
)

set(SHADOW_RECOMP_MISC_CXX_SOURCES
    "misc/rich_presence/shadow_rpc.cpp"
    "misc/rich_presence/shadow/shadow.cpp"
    "misc/rich_presence/shadow/utilities/image_name_dictionary.cpp"
    "misc/rich_presence/shadow/utilities/menu_name_dictionary.cpp"
    "misc/rich_presence/shadow/utilities/mode_name_dictionary.cpp"
    "misc/rich_presence/shadow/utilities/stage_name_dictionary.cpp"
)

if (ANDROID)
    # The launcher app (android-apk/) installs the game and updates itself, and Android
    # has no Discord client to talk to. os/android/stubs_android.cpp keeps the interfaces
    # of these three desktop features so the code calling them stays the same.
    list(REMOVE_ITEM SHADOW_RECOMP_INSTALL_CXX_SOURCES "install/update_checker.cpp")
    list(REMOVE_ITEM SHADOW_RECOMP_UI_CXX_SOURCES "ui/installer_wizard.cpp")
    set(SHADOW_RECOMP_MISC_CXX_SOURCES "")

    # The on-screen gamepad.
    list(APPEND SHADOW_RECOMP_UI_CXX_SOURCES "ui/touch_controls.cpp")
endif()


set(SHADOW_RECOMP_THIRDPARTY_SOURCES
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/imgui/backends/imgui_impl_sdl3.cpp"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/imgui/imgui.cpp"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/imgui/imgui_demo.cpp"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/imgui/imgui_draw.cpp"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/imgui/imgui_tables.cpp"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/imgui/imgui_widgets.cpp"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/imgui/misc/freetype/imgui_freetype.cpp"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/implot/implot.cpp"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/implot/implot_demo.cpp"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/implot/implot_items.cpp"
    "${SHADOW_RECOMP_TOOLS_ROOT}/XShaderRecomp/thirdparty/smol-v/source/smolv.cpp"
)

if (ANDROID)
    # Reads the driver packages (.zip) imported through the launcher.
    list(APPEND SHADOW_RECOMP_THIRDPARTY_SOURCES "${SHADOW_RECOMP_THIRDPARTY_ROOT}/miniz/miniz.c")
endif()


set(SHADOW_RECOMP_THIRDPARTY_INCLUDES
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/concurrentqueue"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/ddspp"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/imgui"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/implot"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/rapidhash"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/stb"
    "${SHADOW_RECOMP_THIRDPARTY_ROOT}/unordered_dense/include"
    "${SHADOW_RECOMP_TOOLS_ROOT}/bc_diff"
    "${SHADOW_RECOMP_TOOLS_ROOT}/XShaderRecomp/thirdparty/smol-v/source"
)

if (ANDROID)
    list(APPEND SHADOW_RECOMP_THIRDPARTY_INCLUDES "${SHADOW_RECOMP_THIRDPARTY_ROOT}/miniz")
endif()

set_source_files_properties(${SHADOW_RECOMP_THIRDPARTY_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

set(SHADOW_RECOMP_CXX_SOURCES
    "app.cpp"
    "exports.cpp"
    "main.cpp"
    "misc_impl.cpp"
    "preload_executable.cpp"
    "sdl_listener.cpp"
    "stdafx.cpp"
    "version.cpp"
    
    ${SHADOW_RECOMP_KERNEL_CXX_SOURCES}
    ${SHADOW_RECOMP_LOCALE_CXX_SOURCES}
    ${SHADOW_RECOMP_OS_CXX_SOURCES}
    ${SHADOW_RECOMP_CPU_CXX_SOURCES}
    ${SHADOW_RECOMP_GPU_CXX_SOURCES}
    ${SHADOW_RECOMP_APU_CXX_SOURCES}
    ${SHADOW_RECOMP_HID_CXX_SOURCES}
    ${SHADOW_RECOMP_PATCHES_CXX_SOURCES}
    ${SHADOW_RECOMP_UI_CXX_SOURCES}
    ${SHADOW_RECOMP_USER_CXX_SOURCES}
    ${SHADOW_RECOMP_MOD_CXX_SOURCES}
    ${SHADOW_RECOMP_MISC_CXX_SOURCES}
    ${SHADOW_RECOMP_INSTALL_CXX_SOURCES}
    ${SHADOW_RECOMP_THIRDPARTY_SOURCES}
)

include("version.cmake")

set(VERSION_TXT "${PROJECT_SOURCE_DIR}/res/version.txt")

# Use version without commit info for tagged releases
if ("$ENV{GITHUB_REF_TYPE}" STREQUAL "tag")
    set(SHADOW_RECOMP_TAG_BUILD ON)
endif()

set(SHOW_GIT_INFO_AND_BUILD_TYPE 1)
if (SHADOW_RECOMP_TAG_BUILD AND ${CMAKE_BUILD_TYPE} MATCHES "Release")
    set(SHOW_GIT_INFO_AND_BUILD_TYPE 0)
endif()

GenerateVersionSources(
    OUTPUT_DIR ${PROJECT_SOURCE_DIR}
    VERSION_TXT ${VERSION_TXT}
    H_TEMPLATE "${PROJECT_SOURCE_DIR}/res/version.h.template"
    CXX_TEMPLATE "${PROJECT_SOURCE_DIR}/res/version.cpp.template"
    BUILD_TYPE ${CMAKE_BUILD_TYPE}
    SHOW_GIT_INFO ${SHOW_GIT_INFO_AND_BUILD_TYPE}
    SHOW_BUILD_TYPE ${SHOW_GIT_INFO_AND_BUILD_TYPE}
)

if (WIN32)
    # Create binary version number for Win32 integer attributes.
    CreateVersionString(
        VERSION_TXT ${VERSION_TXT}
        OUTPUT_CSV 1
        OUTPUT_VAR WIN32_VERSION_BINARY
    )

    # Create string version number for Win32 detailed attributes.
    CreateVersionString(
        VERSION_TXT ${VERSION_TXT}
        BUILD_TYPE ${CMAKE_BUILD_TYPE}
        SHOW_GIT_INFO ${SHOW_GIT_INFO_AND_BUILD_TYPE}
        SHOW_BUILD_TYPE ${SHOW_GIT_INFO_AND_BUILD_TYPE}
        OUTPUT_VAR WIN32_VERSION_STRING
    )

    # Set Win32 icon path.
    set(WIN32_ICON_PATH "${PROJECT_SOURCE_DIR}/../ShadowRecompResources/images/game_icon.ico")

    configure_file("res/Win32_/res.rc.template" "${CMAKE_BINARY_DIR}/res.rc" @ONLY)
    add_executable(ShadowRecomp ${SHADOW_RECOMP_CXX_SOURCES} "${CMAKE_BINARY_DIR}/res.rc")

     # Hide console for release configurations.
    if (${CMAKE_BUILD_TYPE} MATCHES "Release")
        target_link_options(ShadowRecomp PRIVATE "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")
    endif()

    # Link time increases build time in RelWithDebInfo by a lot without a cache
    if (CMAKE_INTERPROCEDURAL_OPTIMIZATION AND CMAKE_LINKER MATCHES "lld-link")
        target_link_options(ShadowRecomp PRIVATE
            "/lldltocache:${CMAKE_BINARY_DIR}/lto.cache"
            "/lldltocachepolicy:cache_size_bytes=4g:prune_after=72h"
        )
    endif()
elseif (ANDROID)
    # An Android app cannot start a bare executable. The game is a shared library that
    # SDL's Java activity loads (GameActivity.getLibraries() names it "main") and whose
    # SDL_main() it then calls.
    add_library(ShadowRecomp SHARED ${SHADOW_RECOMP_CXX_SOURCES})
    set_target_properties(ShadowRecomp PROPERTIES OUTPUT_NAME "main")

    # Bind calls between the game's own functions at link time. A shared library otherwise
    # routes every call to a function in another translation unit through the PLT so that
    # it could be overridden from outside, which nothing does and the recompiled code,
    # made of tens of thousands of functions calling each other, would pay for.
    target_link_options(ShadowRecomp PRIVATE "LINKER:-Bsymbolic-functions")
else()
    add_executable(ShadowRecomp ${SHADOW_RECOMP_CXX_SOURCES})
endif()

if (SHADOW_RECOMP_FLATPAK)
    target_compile_definitions(ShadowRecomp PRIVATE
        "GAME_INSTALL_DIRECTORY=\"/var/data\""
    )
endif()

if (SHADOW_RECOMP_D3D12)
    find_package(directx-headers CONFIG REQUIRED)
    find_package(directx12-agility CONFIG REQUIRED)
    target_compile_definitions(ShadowRecomp PRIVATE 
        SHADOW_RECOMP_D3D12
        D3D12MA_USING_DIRECTX_HEADERS
        D3D12MA_OPTIONS16_SUPPORTED
    )
endif()

#find_package(directx-dxc REQUIRED)
if (NOT ANDROID)
    find_package(CURL REQUIRED)
    find_package(nfd REQUIRED)
endif()

include(FetchContent)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
)

FetchContent_MakeAvailable(nlohmann_json)


if (SHADOW_RECOMP_D3D12)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/D3D12)
    add_custom_command(TARGET ShadowRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PROPERTY:Microsoft::DirectX12-Core,IMPORTED_LOCATION_RELEASE> ${CMAKE_CURRENT_BINARY_DIR}/D3D12
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PROPERTY:Microsoft::DirectX12-Layers,IMPORTED_LOCATION_DEBUG> ${CMAKE_CURRENT_BINARY_DIR}/D3D12
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PROPERTY:Microsoft::DirectXShaderCompiler,IMPORTED_LOCATION> $<TARGET_FILE_DIR:ShadowRecomp>
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PROPERTY:Microsoft::DXIL,IMPORTED_LOCATION> $<TARGET_FILE_DIR:ShadowRecomp>
       COMMAND_EXPAND_LISTS
    )
    
    find_file(DIRECTX_DXIL_LIBRARY "dxil.dll")
#    file(COPY ${DIRECTX_DXIL_LIBRARY} DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

    target_link_libraries(ShadowRecomp PRIVATE
        Microsoft::DirectX-Headers 
        Microsoft::DirectX-Guids 
        Microsoft::DirectX12-Agility
        Microsoft::DirectXShaderCompiler
        Microsoft::DXIL
        dxgi
    )
endif()

if (WIN32)
    target_link_libraries(ShadowRecomp PRIVATE
        comctl32
        dwmapi
        ntdll
        Shcore
        Synchronization
        winmm
        windowsapp
    )
endif()

# Discord Rich Presence is desktop only (stubbed on Android).
if (NOT ANDROID)
    target_link_libraries(ShadowRecomp PRIVATE discord-rpc)
endif()

target_link_libraries(ShadowRecomp PRIVATE
    fmt::fmt
    libzstd_static
    msdf-atlas-gen::msdf-atlas-gen
    o1heap
    XboxUtils
    SDL3::SDL3-static
    SDL3_mixer::SDL3_mixer
    tomlplusplus::tomlplusplus
    ShadowRecompLib
    xxHash::xxhash
    plume
    XShaderRecompLib
    nlohmann_json::nlohmann_json
)

if (ANDROID)
    target_link_libraries(ShadowRecomp PRIVATE
        adrenotools           # loads the bundled or an imported Vulkan driver
        Freetype::Freetype    # built from source, see thirdparty/CMakeLists.txt
        ShadowRecompDxcStub   # stands in for libdxcompiler, see tools/android/android.cmake
        android
        log
    )

    # No DirectX Shader Compiler exists for Android. Shaders come from the precompiled
    # shader cache and the fixed-function variants built below; see gpu/vertexshader.cpp.
    target_compile_definitions(ShadowRecomp PRIVATE SHADOW_RECOMP_NO_RUNTIME_SHADER_COMPILER)
else()
    # The installer wizard's file dialogs and the update checker's HTTP client.
    target_link_libraries(ShadowRecomp PRIVATE
        nfd::nfd
        CURL::libcurl
    )
endif()


target_compile_definitions(XShaderRecompLib PRIVATE SHADOW_RECOMP $<$<CONFIG:Release>:XSHADER_RECOMP_NOLOGGING>)

target_include_directories(ShadowRecomp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SHADOW_RECOMP_THIRDPARTY_INCLUDES}
)

target_compile_definitions(ShadowRecomp PRIVATE IMGUI_USER_CONFIG="gpu/imgui/imgui_config.h")

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    find_package(X11 REQUIRED)
    target_include_directories(ShadowRecomp PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(ShadowRecomp PRIVATE ${X11_LIBRARIES})
    find_package(Freetype REQUIRED)
    target_link_libraries(ShadowRecomp PRIVATE Freetype::Freetype)
    find_package(DBus1 CONFIG REQUIRED)
    target_link_libraries(ShadowRecomp PRIVATE dbus-1)

    set_target_properties(ShadowRecomp PROPERTIES BUILD_RPATH "$ORIGIN")
    add_custom_command(TARGET ShadowRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PROPERTY:Microsoft::DirectXShaderCompiler,IMPORTED_LOCATION> $<TARGET_FILE_DIR:ShadowRecomp>
        COMMAND_EXPAND_LISTS
    )
endif()

target_precompile_headers(ShadowRecomp PUBLIC ${SHADOW_RECOMP_PRECOMPILED_HEADERS})

function(compile_shader FILE_PATH TARGET_NAME)
    set(FILE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/gpu/shader/${FILE_PATH}.hlsl)
    cmake_path(GET FILE_PATH STEM VARIABLE_NAME)
    if (SHADOW_RECOMP_D3D12)
        add_custom_command(
            OUTPUT ${FILE_PATH}.dxil.h
            COMMAND ${DIRECTX_DXC_TOOL} -T ${TARGET_NAME} -HV 2021 -all-resources-bound -Wno-ignored-attributes -Fh ${FILE_PATH}.dxil.h ${FILE_PATH} -Vn g_${VARIABLE_NAME}_dxil
            DEPENDS ${FILE_PATH}
        )
        target_sources(ShadowRecomp PRIVATE ${FILE_PATH}.dxil.h)
    endif()
    add_custom_command(
        OUTPUT ${FILE_PATH}.spirv.h
        COMMAND ${DIRECTX_DXC_TOOL} -T ${TARGET_NAME} -HV 2021 -all-resources-bound -spirv -fvk-use-dx-layout ${ARGN} -Fh ${FILE_PATH}.spirv.h ${FILE_PATH} -Vn g_${VARIABLE_NAME}_spirv
        DEPENDS ${FILE_PATH}
    )
    target_sources(ShadowRecomp PRIVATE ${FILE_PATH}.spirv.h)
endfunction()

################################################################################
# Ubershaders: Composed of:
# - Generic fixed-function shaders (fallback)
# - Derived optimized fixed function shaders (variants - derived from generic)

include("${SHADOW_RECOMP_TOOLS_ROOT}/XShaderRecomp/XShaderRecomp/fixed_function/XShaderRecompFixedFunction.cmake")

xsr_add_fixed_function_shaders(
    TARGET ShadowRecomp
    DXC ${DIRECTX_DXC_TOOL}
    DXIL ${SHADOW_RECOMP_D3D12}
    DXIL_MACRO SHADOW_RECOMP_D3D12
    OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gpu/ubershaders/generated"
    PIXEL_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gpu/ubershaders/generated/fixed_function_pixel_shader"
    VERTEX_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gpu/ubershaders/generated/fixed_function_vertex_shader")

# END OF UBERSHADERS
################################################################################

function(compile_vertex_shader FILE_PATH)
    compile_shader(${FILE_PATH} vs_6_0 -fvk-invert-y)
endfunction()

function(compile_pixel_shader FILE_PATH)
    compile_shader(${FILE_PATH} ps_6_0)
endfunction()

compile_vertex_shader(PassthroughVertexShader)

compile_vertex_shader(copy_vs)
compile_pixel_shader(copy_color_ps)
compile_pixel_shader(copy_depth_ps)

compile_pixel_shader(gamma_correction_ps)
compile_pixel_shader(imgui_ps)
compile_vertex_shader(imgui_vs)


compile_vertex_shader(R2D_vs0)
compile_vertex_shader(R2D_vs1)

function(generate_aggregate_header INPUT_DIRECTORY OUTPUT_FILE)
    get_filename_component(ABS_OUTPUT_FILE "${OUTPUT_FILE}" ABSOLUTE)
    file(GLOB_RECURSE HEADER_FILES "${INPUT_DIRECTORY}/*.h")
    set(HEADER_CONTENT "#pragma once\n\n")

    foreach(HEADER_FILE IN LISTS HEADER_FILES)
        get_filename_component(ABS_HEADER_FILE "${HEADER_FILE}" ABSOLUTE)
        if (ABS_HEADER_FILE STREQUAL ABS_OUTPUT_FILE)
            continue()
        endif()
        file(RELATIVE_PATH RELATIVE_HEADER_FILE "${INPUT_DIRECTORY}" "${HEADER_FILE}")
        string(APPEND HEADER_CONTENT "#include \"${RELATIVE_HEADER_FILE}\"\n")
    endforeach()

    if (EXISTS "${OUTPUT_FILE}")
        file(READ "${OUTPUT_FILE}" EXISTING_CONTENT)
        if (EXISTING_CONTENT STREQUAL HEADER_CONTENT)
            return()
        endif()
    endif()

    file(WRITE "${OUTPUT_FILE}" "${HEADER_CONTENT}")
endfunction()

generate_aggregate_header(
    "${CMAKE_CURRENT_SOURCE_DIR}/api"
    "${CMAKE_CURRENT_SOURCE_DIR}/api/Shadow.h"
)

set(RESOURCES_SOURCE_PATH "${PROJECT_SOURCE_DIR}/../ShadowRecompResources")
set(RESOURCES_OUTPUT_PATH "${PROJECT_SOURCE_DIR}/res")

## Miscellaneous ##
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/bc_diff/button_bc_diff_gc.bin" DEST_FILE "${RESOURCES_OUTPUT_PATH}/bc_diff/button_bc_diff_gc.bin" ARRAY_NAME "g_button_bc_diff_gc" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/bc_diff/button_bc_diff_ps2.bin" DEST_FILE "${RESOURCES_OUTPUT_PATH}/bc_diff/button_bc_diff_ps2.bin" ARRAY_NAME "g_button_bc_diff_ps2" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/font/im_font_atlas.bin" DEST_FILE "${RESOURCES_OUTPUT_PATH}/font/im_font_atlas.bin" ARRAY_NAME "g_im_font_atlas" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/font/im_font_atlas.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/font/im_font_atlas.png" ARRAY_NAME "g_im_font_atlas_texture" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/common/Options.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/common/Options.png" ARRAY_NAME "g_optionsText" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/common/Options2.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/common/Options2.png" ARRAY_NAME "g_options_bg2" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/common/HPgauge.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/common/HPgauge.png" ARRAY_NAME "g_hpGauge" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/common/options_icons.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/common/options_icons.png" ARRAY_NAME "g_optionsIcons" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/common/TitleBg.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/common/TitleBg.png" ARRAY_NAME "g_options_bg" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/common/controllerBtn.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/common/controllerBtn.png" ARRAY_NAME "g_controller" COMPRESSION_TYPE "zstd")

BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/common/arrow.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/common/arrow.png" ARRAY_NAME "g_arrow" COMPRESSION_TYPE "zstd")

## 3P/4P HUD ##
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/common/mp_hud_ring.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/common/mp_hud_ring.png" ARRAY_NAME "g_mpHudRing" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/common/mp_hud_ammo.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/common/mp_hud_ammo.png" ARRAY_NAME "g_mpHudAmmo" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/common/mp_hud_ammo_types.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/common/mp_hud_ammo_types.png" ARRAY_NAME "g_mpHudAmmoTypes" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/common/mp_hud_digits.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/common/mp_hud_digits.png" ARRAY_NAME "g_mpHudDigits" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/common/mp_hud_meter.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/common/mp_hud_meter.png" ARRAY_NAME "g_mpHudMeter" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/common/mp_hud_mission.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/common/mp_hud_mission.png" ARRAY_NAME "g_mpHudMission" COMPRESSION_TYPE "zstd")

## Installer ##
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/installer/FoxLogo.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/installer/FoxLogo.png" ARRAY_NAME "g_foxLogo" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/installer/installer_bg.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/installer/installer_bg.png" ARRAY_NAME "g_installer_bg" COMPRESSION_TYPE "zstd")

## Game Icon ##
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/game_icon.bmp" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/game_icon.bmp" ARRAY_NAME "g_game_icon")

## Audio #
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/music/installer.ogg" DEST_FILE "${RESOURCES_OUTPUT_PATH}/music/installer.ogg" ARRAY_NAME "g_installer_music")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/sounds/sys_menu_ok.ogg" DEST_FILE "${RESOURCES_OUTPUT_PATH}/sounds/sys_menu_ok.ogg" ARRAY_NAME "g_sys_menu_ok")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/sounds/sys_menu_cursor.ogg" DEST_FILE "${RESOURCES_OUTPUT_PATH}/sounds/sys_menu_cursor.ogg" ARRAY_NAME "g_sys_menu_cursor")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/sounds/sys_menu_invalid.ogg" DEST_FILE "${RESOURCES_OUTPUT_PATH}/sounds/sys_menu_invalid.ogg" ARRAY_NAME "g_sys_menu_invalid")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/sounds/sys_menu_cancel.ogg" DEST_FILE "${RESOURCES_OUTPUT_PATH}/sounds/sys_menu_cancel.ogg" ARRAY_NAME "g_sys_menu_cancel")

## Achievements #
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/achievementBG.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/achievementBG.png" ARRAY_NAME "g_achievementBG" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/achievementBtn.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/achievementBtn.png" ARRAY_NAME "g_achievementBtn" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/achievements_pause.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/achievements_pause.png" ARRAY_NAME "g_pauseAchievements" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/pure_hero.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/pure_hero.png" ARRAY_NAME "g_purehero" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/neutral_hero.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/neutral_hero.png" ARRAY_NAME "g_neutralhero" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/neutral_dark.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/neutral_dark.png" ARRAY_NAME "g_neutraldark" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/PlaceHolder.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/PlaceHolder.png" ARRAY_NAME "g_placeholder" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/pure_dark.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/pure_dark.png" ARRAY_NAME "g_puredark" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/pure_dark_hero.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/pure_dark_hero.png" ARRAY_NAME "g_puredarkhero" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/pure_hero_dark.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/pure_hero_dark.png" ARRAY_NAME "g_pureherodark" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/dark_dark.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/dark_dark.png" ARRAY_NAME "g_darkdark" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/dark_hero.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/dark_hero.png" ARRAY_NAME "g_darkhero" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/hero_dark.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/hero_dark.png" ARRAY_NAME "g_herodark" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/hero_hero.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/hero_hero.png" ARRAY_NAME "g_herohero" COMPRESSION_TYPE "zstd") 
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/A_Rank.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/A_Rank.png" ARRAY_NAME "g_arank" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/key.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/key.png" ARRAY_NAME "g_key" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/Bomb.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/bomb.png" ARRAY_NAME "g_bomb" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/tank.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/tank.png" ARRAY_NAME "g_tank" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/LastStoryComplete.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/LastStoryComplete.png" ARRAY_NAME "g_laststorycomplete" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/overlay.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/overlay.png" ARRAY_NAME "g_overlayAchievements" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/shthR.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/shthR.png" ARRAY_NAME "g_shthR" COMPRESSION_TYPE "zstd")
BIN2C(TARGET_OBJ ShadowRecomp SOURCE_FILE "${RESOURCES_SOURCE_PATH}/images/achievements/Expert.png" DEST_FILE "${RESOURCES_OUTPUT_PATH}/images/achievements/expert.png" ARRAY_NAME "g_expert" COMPRESSION_TYPE "zstd")

if (ANDROID)
    # Copies what the APK needs out of this build and into the Gradle project:
    #
    #   cmake --build <build dir> --target android-jnilibs
    #
    # libmain.so is the game. The four hook libraries belong to libadrenotools and must sit
    # next to it as real files for a custom Vulkan driver to load (see android-apk/app/build.gradle).
    set(SHADOW_RECOMP_ANDROID_JNILIBS_DIR "${CMAKE_SOURCE_DIR}/android-apk/app/src/main/jniLibs/${CMAKE_ANDROID_ARCH_ABI}"
        CACHE PATH "Directory the android-jnilibs target copies the native libraries to")

    set(SHADOW_RECOMP_ANDROID_LIBRARIES ShadowRecomp main_hook file_redirect_hook gsl_alloc_hook hook_impl)

    set(SHADOW_RECOMP_ANDROID_LIBRARY_FILES "")
    set(SHADOW_RECOMP_ANDROID_LIBRARY_COPIES "")
    foreach(library IN LISTS SHADOW_RECOMP_ANDROID_LIBRARIES)
        list(APPEND SHADOW_RECOMP_ANDROID_LIBRARY_FILES $<TARGET_FILE:${library}>)
        list(APPEND SHADOW_RECOMP_ANDROID_LIBRARY_COPIES "${SHADOW_RECOMP_ANDROID_JNILIBS_DIR}/$<TARGET_FILE_NAME:${library}>")
    endforeach()

    # The NDK compiles everything with debug information, which makes libmain.so several
    # times larger than the code in it. Gradle removes it while packaging, but only if it
    # finds the NDK version named in app/build.gradle inside the SDK, so the copies are
    # stripped here and the APK is the same size either way. The libraries in the build
    # tree are left as they are: a crash address is looked up in those (docs/ANDROID.md).
    set(SHADOW_RECOMP_ANDROID_STRIP_COMMAND "")
    if (CMAKE_STRIP)
        set(SHADOW_RECOMP_ANDROID_STRIP_COMMAND
            COMMAND "${CMAKE_STRIP}" --strip-unneeded ${SHADOW_RECOMP_ANDROID_LIBRARY_COPIES})
    else()
        message(STATUS "CMAKE_STRIP is not set: android-jnilibs copies the native libraries with their debug information.")
    endif()

    add_custom_target(android-jnilibs
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADOW_RECOMP_ANDROID_JNILIBS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy ${SHADOW_RECOMP_ANDROID_LIBRARY_FILES} "${SHADOW_RECOMP_ANDROID_JNILIBS_DIR}"
        ${SHADOW_RECOMP_ANDROID_STRIP_COMMAND}
        COMMENT "Copying the native libraries to ${SHADOW_RECOMP_ANDROID_JNILIBS_DIR}"
        VERBATIM
    )

    add_dependencies(android-jnilibs ${SHADOW_RECOMP_ANDROID_LIBRARIES})
endif()
