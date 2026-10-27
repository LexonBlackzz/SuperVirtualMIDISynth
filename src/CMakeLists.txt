cmake_minimum_required(VERSION 3.20)
project(SuperVirtualMIDISynthV3 VERSION 0.11.0 LANGUAGES C CXX)

include(CTest)

# Sources include each other by path from src/ (e.g. "synth/Types.h").
include_directories("${CMAKE_CURRENT_SOURCE_DIR}")

option(SVMS_XP_COMPAT "Build the 32-bit Windows XP DirectSound variant" OFF)
option(SVMS_CUDA "Embed the CUDA kernels of the GPU render stage (needs nvcc; point CUDA_PATH at the toolkit)" ON)
option(SVMS_VULKAN "Embed the SPIR-V kernels of the GPU render stage (needs the Vulkan SDK: headers and glslc)" ON)

# CUDA backend of the GPU render stage.  The host half (GpuBackendCUDA.cpp)
# declares the driver API itself and opens nvcuda at run time, so it builds
# anywhere; the kernels are embedded only when nvcc is found, and a build
# without them reports it when the stage is enabled.  --fmad=false keeps the
# scalar path's math step by step (no fused multiply-add).  Architectures: SASS
# for Turing through Blackwell plus PTX for newer GPUs on a new enough driver.
if(NOT SVMS_XP_COMPAT)
    add_library(svms_gpu_cuda OBJECT render/GpuBackendCUDA.cpp render/GpuBackendVulkan.cpp)
    set_target_properties(svms_gpu_cuda PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
if(SVMS_CUDA AND NOT SVMS_XP_COMPAT AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    find_program(SVMS_NVCC nvcc HINTS ENV CUDA_PATH ENV CUDAToolkit_ROOT PATH_SUFFIXES bin)
endif()
if(SVMS_NVCC)
    get_filename_component(SVMS_NVCC_BIN_DIR "${SVMS_NVCC}" DIRECTORY)
    find_program(SVMS_BIN2C bin2c HINTS "${SVMS_NVCC_BIN_DIR}" NO_DEFAULT_PATH)
endif()
if(SVMS_NVCC AND SVMS_BIN2C AND TARGET svms_gpu_cuda)
    set(SVMS_CUDA_GEN "${CMAKE_CURRENT_BINARY_DIR}/gpu_cuda")
    set(SVMS_CUDA_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/render/gpu/VoiceStageCUDA.cu")
    set(SVMS_CUDA_GENCODE)
    foreach(arch 75 80 86 89 90 100 120)
        list(APPEND SVMS_CUDA_GENCODE -gencode arch=compute_${arch},code=sm_${arch})
    endforeach()
    list(APPEND SVMS_CUDA_GENCODE -gencode arch=compute_75,code=compute_75)
    add_custom_command(
        OUTPUT "${SVMS_CUDA_GEN}/VoiceStageCUDA_fatbin.h"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SVMS_CUDA_GEN}"
        COMMAND "${SVMS_NVCC}" -fatbin -O3 --fmad=false -prec-div=true
                -prec-sqrt=true -ftz=false --compress-mode=none
                ${SVMS_CUDA_GENCODE} "${SVMS_CUDA_SOURCE}"
                -o "${SVMS_CUDA_GEN}/VoiceStageCUDA.fatbin"
        COMMAND "${CMAKE_COMMAND}" "-DBIN2C=${SVMS_BIN2C}"
                "-DIN=${SVMS_CUDA_GEN}/VoiceStageCUDA.fatbin"
                "-DOUT=${SVMS_CUDA_GEN}/VoiceStageCUDA_fatbin.h"
                -DNAME=svms_cuda_fatbin
                -P "${CMAKE_CURRENT_SOURCE_DIR}/render/gpu/EmbedFatbin.cmake"
        DEPENDS "${SVMS_CUDA_SOURCE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/render/gpu/EmbedFatbin.cmake"
        COMMENT "Compiling the CUDA kernels of the GPU render stage"
        VERBATIM)
    target_sources(svms_gpu_cuda PRIVATE "${SVMS_CUDA_GEN}/VoiceStageCUDA_fatbin.h")
    target_include_directories(svms_gpu_cuda PRIVATE "${SVMS_CUDA_GEN}")
    target_compile_definitions(svms_gpu_cuda PRIVATE SVMS_HAVE_CUDA_KERNELS=1)
    message(STATUS "SVMS: CUDA kernels enabled (${SVMS_NVCC})")
else()
    message(STATUS "SVMS: CUDA kernels not embedded (no nvcc/bin2c; set CUDA_PATH)")
endif()

# Vulkan backend of the GPU render stage (lives in the same object library).
# Like CUDA it opens its driver (vulkan-1) at run time; the SDK is needed only
# at build time, for the headers and for glslc, which compiles the GLSL of
# render/gpu/VoiceStageGLSL.h to SPIR-V.  Without them the backend reports it.
if(SVMS_VULKAN AND TARGET svms_gpu_cuda AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    find_path(SVMS_VULKAN_INCLUDE vulkan/vulkan.h HINTS ENV VULKAN_SDK PATH_SUFFIXES Include include)
    find_program(SVMS_GLSLC glslc HINTS ENV VULKAN_SDK PATH_SUFFIXES Bin bin)
endif()
if(SVMS_VULKAN_INCLUDE AND SVMS_GLSLC)
    set(SVMS_VULKAN_GEN "${CMAKE_CURRENT_BINARY_DIR}/gpu_vulkan")
    set(SVMS_VULKAN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/render/gpu/VoiceStageGLSL.h")
    add_custom_command(
        OUTPUT "${SVMS_VULKAN_GEN}/VoiceStageVulkan_spv.h"
        COMMAND "${CMAKE_COMMAND}" "-DGLSLC=${SVMS_GLSLC}" "-DSRC=${SVMS_VULKAN_SOURCE}"
                "-DWORK=${SVMS_VULKAN_GEN}" "-DOUT=${SVMS_VULKAN_GEN}/VoiceStageVulkan_spv.h"
                -DGROUP=128 -DTILE=4
                -P "${CMAKE_CURRENT_SOURCE_DIR}/render/gpu/BuildVulkanShaders.cmake"
        DEPENDS "${SVMS_VULKAN_SOURCE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/render/gpu/BuildVulkanShaders.cmake"
        COMMENT "Compiling the Vulkan kernels of the GPU render stage"
        VERBATIM)
    set_source_files_properties(render/GpuBackendVulkan.cpp PROPERTIES
        OBJECT_DEPENDS "${SVMS_VULKAN_GEN}/VoiceStageVulkan_spv.h"
        COMPILE_DEFINITIONS SVMS_HAVE_VULKAN=1
        INCLUDE_DIRECTORIES "${SVMS_VULKAN_GEN};${SVMS_VULKAN_INCLUDE}")
    message(STATUS "SVMS: Vulkan kernels enabled (${SVMS_GLSLC})")
else()
    message(STATUS "SVMS: Vulkan kernels not embedded (no Vulkan SDK; set VULKAN_SDK)")
endif()

if(WIN32)
    enable_language(RC)
endif()

# One generated identity is consumed by every V3 component. Official release
# automation supplies SVMS_BUILD_NUMBER and SVMS_RELEASE_CHANNEL explicitly;
# local builds remain recognizable without depending on mutable git history.
set(SVMS_BUILD_NUMBER "$ENV{GITHUB_RUN_NUMBER}" CACHE STRING
    "Monotonic official build number (0 for local builds)")
if(SVMS_BUILD_NUMBER STREQUAL "")
    set(SVMS_BUILD_NUMBER "0" CACHE STRING
        "Monotonic official build number (0 for local builds)" FORCE)
endif()
if(NOT SVMS_BUILD_NUMBER MATCHES "^[0-9]+$")
    message(FATAL_ERROR "SVMS_BUILD_NUMBER must be an unsigned integer")
endif()
if(SVMS_BUILD_NUMBER GREATER 4294967295)
    message(FATAL_ERROR "SVMS_BUILD_NUMBER must fit in an unsigned 32-bit integer")
endif()

set(SVMS_RELEASE_CHANNEL "" CACHE STRING
    "Release channel: stable, prerelease, or local")
if(SVMS_RELEASE_CHANNEL STREQUAL "")
    if(SVMS_BUILD_NUMBER STREQUAL "0")
        set(SVMS_RELEASE_CHANNEL "local")
    else()
        set(SVMS_RELEASE_CHANNEL "stable")
    endif()
endif()
if(NOT SVMS_RELEASE_CHANNEL MATCHES "^(stable|prerelease|local)$")
    message(FATAL_ERROR
        "SVMS_RELEASE_CHANNEL must be stable, prerelease, or local")
endif()
if(SVMS_RELEASE_CHANNEL STREQUAL "stable")
    set(SVMS_RELEASE_CHANNEL_ID 1)
elseif(SVMS_RELEASE_CHANNEL STREQUAL "prerelease")
    set(SVMS_RELEASE_CHANNEL_ID 2)
else()
    set(SVMS_RELEASE_CHANNEL_ID 0)
endif()

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
    OUTPUT_VARIABLE SVMS_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE SVMS_GIT_RESULT)
if(NOT SVMS_GIT_RESULT EQUAL 0 OR SVMS_GIT_COMMIT STREQUAL "")
    set(SVMS_GIT_COMMIT "unknown")
endif()

if(SVMS_XP_COMPAT)
    set(SVMS_RUNTIME_PROTOCOL_MIN 0)
    set(SVMS_RUNTIME_PROTOCOL_MAX 0)
    set(SVMS_DRIVER_RUNTIME_CAPABILITIES "0ull")
else()
    set(SVMS_RUNTIME_PROTOCOL_MIN 2)
    set(SVMS_RUNTIME_PROTOCOL_MAX 3)
    set(SVMS_DRIVER_RUNTIME_CAPABILITIES
        "CapabilityRuntimeLinkV2 | CapabilityRuntimeLinkV3")
endif()
set(SVMS_NATIVE_ABI_MIN 1)
set(SVMS_NATIVE_ABI_MAX 1)
set(SVMS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SVMS_GENERATED_DIR}")
configure_file(synth/BuildInfo.h.in
    "${SVMS_GENERATED_DIR}/BuildInfo.h" @ONLY)
include_directories("${SVMS_GENERATED_DIR}")

# Geist + Geist Mono (SIL OFL 1.1, configurator/fonts/OFL.txt) embedded in
# the configurator as byte arrays: GeistFonts.h, regenerated when a TTF changes.
if(NOT SVMS_XP_COMPAT)
    set(SVMS_CONFIGURATOR_FONTS Geist-Regular Geist-Medium Geist-SemiBold Geist-Bold
        GeistMono-Regular GeistMono-Medium)
    string(REPEAT "[0-9a-f]" 64 _svms_hex_line)
    set(_svms_font_body "// Generated by CMake from configurator/fonts (SIL OFL 1.1). Do not edit.\n#pragma once\n")
    foreach(_svms_font IN LISTS SVMS_CONFIGURATOR_FONTS)
        set(_svms_font_path "${CMAKE_CURRENT_SOURCE_DIR}/configurator/fonts/${_svms_font}.ttf")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_svms_font_path}")
        file(READ "${_svms_font_path}" _svms_hex HEX)
        string(REGEX REPLACE "(${_svms_hex_line})" "\\1\n" _svms_hex "${_svms_hex}")
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _svms_hex "${_svms_hex}")
        string(MAKE_C_IDENTIFIER "k${_svms_font}" _svms_font_name)
        string(APPEND _svms_font_body
            "alignas(4) static const unsigned char ${_svms_font_name}[] = {\n${_svms_hex}};\n")
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/GeistFonts.h.in" "${_svms_font_body}")
    configure_file("${CMAKE_CURRENT_BINARY_DIR}/GeistFonts.h.in"
        "${SVMS_GENERATED_DIR}/GeistFonts.h" COPYONLY)
endif()

if(WIN32)
    math(EXPR SVMS_RESOURCE_BUILD_NUMBER "${SVMS_BUILD_NUMBER} % 65536")
    set(SVMS_RESOURCE_FILENAME "winmm.dll")
    set(SVMS_RESOURCE_DESCRIPTION
        "SuperVirtualMIDISynth V3 WinMM/KDMAPI Driver")
    set(SVMS_RESOURCE_FILETYPE "VFT_DLL")
    configure_file(synth/Version.rc.in
        "${SVMS_GENERATED_DIR}/SVMSDriverVersion.rc" @ONLY)

    set(SVMS_RESOURCE_FILENAME "SVMSConfigurator.exe")
    set(SVMS_RESOURCE_DESCRIPTION
        "SuperVirtualMIDISynth V3 Configurator")
    set(SVMS_RESOURCE_FILETYPE "VFT_APP")
    configure_file(synth/Version.rc.in
        "${SVMS_GENERATED_DIR}/SVMSConfiguratorVersion.rc" @ONLY)

    set(SVMS_RESOURCE_FILENAME "SVMSAPI.dll")
    set(SVMS_RESOURCE_DESCRIPTION
        "SuperVirtualMIDISynth V3 Standalone Synthesizer API")
    set(SVMS_RESOURCE_FILETYPE "VFT_DLL")
    configure_file(synth/Version.rc.in
        "${SVMS_GENERATED_DIR}/SVMSApiVersion.rc" @ONLY)
endif()

if(SVMS_XP_COMPAT AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "SVMS_XP_COMPAT is a 32-bit-only target")
endif()

if(WIN32)
    set(CMAKE_CXX_STANDARD 20)
else()
    # The portable engine does not require C++20. C++17 keeps the produced
    # ELF binary buildable against Ubuntu 18.04's native GCC/libstdc++.
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Phase-2 canonical synthesis module.  It is intentionally excluded from the
# XP/realtime product for now: the only host integration in this phase is the
# modern standalone offline renderer.
if(NOT SVMS_XP_COMPAT)
    add_library(svms_canonical_engine STATIC
        canonical/CanonicalSampleBank.cpp
        canonical/CanonicalSoundFont.cpp
        canonical/CanonicalChannelInterpreter.cpp
        canonical/CanonicalCore.cpp
        canonical/CanonicalRenderAVX2.cpp
        canonical/CanonicalEngine.cpp)
    target_include_directories(svms_canonical_engine PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/canonical")
    # C++17-clean (canonical/CanonicalCompat.h) so the Linux build keeps its
    # Ubuntu 18.04 / GCC 7 floor; GCC < 9 needs the separate filesystem lib.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
       CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
        target_link_libraries(svms_canonical_engine PUBLIC stdc++fs)
    endif()
    target_compile_definitions(svms_canonical_engine PRIVATE
        MIDISYNTH_HAS_AVX2=1)
    if(MSVC)
        set_source_files_properties(canonical/CanonicalRenderAVX2.cpp PROPERTIES
            COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(canonical/CanonicalRenderAVX2.cpp PROPERTIES
            COMPILE_OPTIONS "-mavx2;-mno-fma")
        target_link_libraries(svms_canonical_engine PUBLIC pthread)
    endif()

    add_executable(svms_canonical_engine_tests
        tests/CanonicalEngineTests.cpp)
    target_link_libraries(svms_canonical_engine_tests PRIVATE
        svms_canonical_engine)
    add_test(NAME svms_canonical_engine
        COMMAND svms_canonical_engine_tests)

    add_executable(svms_canonical_engine_bench
        tests/CanonicalEngineBench.cpp)
    target_link_libraries(svms_canonical_engine_bench PRIVATE
        svms_canonical_engine)
endif()

if(NOT WIN32)
    find_package(Threads REQUIRED)
    set(SVMS_CONTROL_SERVER_SOURCES driver/ControlServer.cpp)
    find_package(ALSA REQUIRED)
    # nlohmann/json for synth/Config.cpp (compiled into every portable target).
    include_directories("${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
    # synth/Config.cpp uses dladdr (module directory); glibc < 2.34 keeps it in libdl.
    link_libraries(${CMAKE_DL_LIBS})
    # GCC < 9 keeps std::(experimental::)filesystem in a separate library;
    # synth/SoundFont.cpp falls back to the Filesystem TS on GCC 7.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
       CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
        link_libraries(stdc++fs)
    endif()

    set(SVMS_PORTABLE_RENDER_SOURCES
        synth/Config.cpp
        synth/SoundFont.cpp
        synth/SfzLoader.cpp
        render/RenderKernels.cpp
        render/RenderWorkersPosix.cpp
        render/RenderDispatch.cpp
        render/RenderKernelsSSE2.cpp
        render/RenderKernelsAVX2.cpp
        render/GpuStage.cpp
        render/GpuBackendGL.cpp $<TARGET_OBJECTS:svms_gpu_cuda>)

    set_source_files_properties(render/RenderKernelsSSE2.cpp PROPERTIES
        COMPILE_OPTIONS "-msse2;-mno-avx")
    set_source_files_properties(render/RenderKernelsAVX2.cpp PROPERTIES
        COMPILE_OPTIONS "-mavx2;-mno-fma")

    add_executable(svms_v3_render
        synth/OfflineRenderer.cpp
        events/MidiStream.cpp
        ${SVMS_PORTABLE_RENDER_SOURCES})
    target_include_directories(svms_v3_render PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
    target_compile_options(svms_v3_render PRIVATE
        -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(svms_v3_render PRIVATE Threads::Threads)
    target_link_libraries(svms_v3_render PRIVATE svms_canonical_engine)

    add_executable(svmsd
        frontends/LinuxDaemon.cpp
        ${SVMS_PORTABLE_RENDER_SOURCES})
    target_include_directories(svmsd PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party"
        ${ALSA_INCLUDE_DIRS})
    target_compile_options(svmsd PRIVATE
        -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(svmsd PRIVATE Threads::Threads ${ALSA_LIBRARIES})

    add_library(omnimidi SHARED
        frontends/LinuxKDMAPI.cpp
        frontends/RemoteClient.cpp
        driver/ControlServer.cpp
        ${SVMS_PORTABLE_RENDER_SOURCES})
    target_include_directories(omnimidi PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party"
        ${ALSA_INCLUDE_DIRS})
    target_compile_options(omnimidi PRIVATE
        -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(omnimidi PRIVATE
        Threads::Threads ${ALSA_LIBRARIES} ${CMAKE_DL_LIBS})
    set_target_properties(omnimidi PROPERTIES OUTPUT_NAME "svmsapi")
    add_custom_command(TARGET omnimidi POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:omnimidi>/libOmniMIDI.so"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                "$<TARGET_FILE_NAME:omnimidi>"
                "$<TARGET_FILE_DIR:omnimidi>/libOmniMIDI.so"
        VERBATIM)

    set_target_properties(svms_v3_render svmsd PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    option(SVMS_BUILD_TESTS "Build deterministic V3 correctness tests" ON)
    if(SVMS_BUILD_TESTS)
        add_executable(svms_v3_native_header_c
            tests/NativeHeaderCSmoke.c)
        target_include_directories(svms_v3_native_header_c PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        add_test(NAME svms_v3_native_header_c
            COMMAND svms_v3_native_header_c)

        add_executable(svms_v3_voice_growth_tests
            tests/VoiceGrowthRegression.cpp
            ${SVMS_PORTABLE_RENDER_SOURCES})
        target_include_directories(svms_v3_voice_growth_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(svms_v3_voice_growth_tests PRIVATE Threads::Threads)
        add_test(NAME svms_v3_voice_growth COMMAND svms_v3_voice_growth_tests)

        add_executable(svms_v3_realtime_block_tests
            tests/RealtimeBlockTests.cpp
            ${SVMS_PORTABLE_RENDER_SOURCES})
        target_include_directories(svms_v3_realtime_block_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(svms_v3_realtime_block_tests PRIVATE Threads::Threads)
        add_test(NAME svms_v3_realtime_block COMMAND svms_v3_realtime_block_tests)
        set_tests_properties(svms_v3_realtime_block PROPERTIES SKIP_RETURN_CODE 77)

        add_executable(svms_v3_limiter_tests tests/LimiterTests.cpp)
        target_include_directories(svms_v3_limiter_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME svms_v3_limiter COMMAND svms_v3_limiter_tests)

        add_executable(svms_v3_control_tests tests/ControlRegistryTests.cpp
            ${SVMS_CONTROL_SERVER_SOURCES})
        target_include_directories(svms_v3_control_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
        if(UNIX)
            target_link_libraries(svms_v3_control_tests PRIVATE Threads::Threads)
        endif()
        add_test(NAME svms_v3_control COMMAND svms_v3_control_tests)



        add_executable(svms_v3_midi_stream_tests
            tests/MidiStreamTests.cpp
            events/MidiStream.cpp)
        target_include_directories(svms_v3_midi_stream_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(svms_v3_midi_stream_tests PRIVATE Threads::Threads)
        add_test(NAME svms_v3_midi_stream COMMAND svms_v3_midi_stream_tests)

        add_executable(svms_v3_linux_kdmapi_smoke
            tests/LinuxKDMAPISmoke.cpp)
        target_include_directories(svms_v3_linux_kdmapi_smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(svms_v3_linux_kdmapi_smoke PRIVATE
            ${CMAKE_DL_LIBS})
        add_test(NAME svms_v3_linux_kdmapi
            COMMAND svms_v3_linux_kdmapi_smoke $<TARGET_FILE:omnimidi>)

        add_executable(svms_v3_linux_native_api_smoke
            tests/LinuxNativeAPISmoke.cpp)
        target_include_directories(svms_v3_linux_native_api_smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(svms_v3_linux_native_api_smoke PRIVATE
            ${CMAKE_DL_LIBS})
        add_test(NAME svms_v3_linux_native_api
            COMMAND svms_v3_linux_native_api_smoke $<TARGET_FILE:omnimidi>)
    endif()

    # ── Linux configurator (SDL2 + OpenGL3 + Dear ImGui) ──────────────────
    # Needs SDL2 and OpenGL development files (libsdl2-dev, libgl1-mesa-dev,
    # pkg-config); skipped with a notice when they are missing.
    option(SVMS_BUILD_CONFIGURATOR "Build the Linux configurator" ON)
    if(SVMS_BUILD_CONFIGURATOR)
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(SVMS_SDL2 QUIET IMPORTED_TARGET sdl2)
        endif()
        find_package(OpenGL QUIET)
        if(SVMS_SDL2_FOUND AND OPENGL_FOUND)
            set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui")
            add_executable(svms_configurator
                configurator/Main_Linux.cpp
                frontends/RemoteClient.cpp
                configurator/ConfiguratorApp.cpp
                configurator/Platform_SDL2.cpp
                configurator/PlatformUtil.cpp
                configurator/ConfigDocument.cpp
                configurator/Theme.cpp
                configurator/Widgets.cpp
                configurator/Anim.cpp
                configurator/ModernWidgets.cpp
                configurator/SettingsRegistry.cpp
                configurator/EasterEggs.cpp
                configurator/UpdateService.cpp
                configurator/PageHome.cpp
                configurator/PageOutput.cpp
                configurator/PageSound.cpp
                configurator/PagePerformance.cpp
                configurator/PageMidi.cpp
                configurator/PageOffline.cpp
                configurator/PageLiveRecording.cpp
                configurator/PageEffects.cpp
                configurator/PageDiagnostics.cpp
                configurator/PageSettings.cpp
                configurator/Tour.cpp
                configurator/PageDriverFeatures.cpp
                driver/ControlServer.cpp
                ${IMGUI_DIR}/imgui.cpp
                ${IMGUI_DIR}/imgui_draw.cpp
                ${IMGUI_DIR}/imgui_tables.cpp
                ${IMGUI_DIR}/imgui_widgets.cpp
                ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
                ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
            target_include_directories(svms_configurator PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}"
                "${IMGUI_DIR}"
                "${IMGUI_DIR}/backends"
                "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
            target_compile_options(svms_configurator PRIVATE -Wall -Wno-unused-parameter)
            target_link_libraries(svms_configurator PRIVATE
                PkgConfig::SVMS_SDL2 OpenGL::GL Threads::Threads ${CMAKE_DL_LIBS})
            set_target_properties(svms_configurator PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
            set(SVMS_CONFIGURATOR_TARGET svms_configurator)
        else()
            message(STATUS "SVMS: Linux configurator skipped (SDL2/OpenGL development files not found)")
        endif()
    endif()

    if(TARGET svms_configurator)
        install(TARGETS svms_configurator RUNTIME DESTINATION bin)
    endif()
    install(TARGETS svmsd svms_v3_render omnimidi
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libOmniMIDI.so"
        DESTINATION lib)
    return()
endif()

if(MSVC)
    add_compile_options(/W4 /WX- /Zc:__cplusplus)
    add_compile_options(/utf-8)
    add_definitions(-DUNICODE -D_UNICODE)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        if(CMAKE_CXX_COMPILER_LAUNCHER)
            # Compiler caches (sccache in CI) can only cache objects that
            # embed their debug info; /Z7 vs /Zi does not change codegen.
            add_compile_options(/O2 /Oi /Gy /Z7)
        else()
            add_compile_options(/O2 /Oi /Gy /Zi /FS)
        endif()
        add_link_options(/DEBUG /OPT:REF /OPT:ICF)
    endif()
endif()

if(SVMS_XP_COMPAT)
    add_compile_definitions(SVMS_XP_COMPAT=1 WINVER=0x0501 _WIN32_WINNT=0x0501)
endif()

if(MINGW)
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    if(SVMS_XP_COMPAT)
        add_compile_options(-O3 -msse2 -mfpmath=sse)
        add_link_options(-static -static-libgcc -static-libstdc++)
    endif()
endif()

# Driver translation units; each starts with driver/DriverInternal.h.
# frontends/FrontWinMM.cpp is deliberately absent: it is the one file compiled per
# DLL (see svms_engine below).
set(SVMS_DRIVER_SOURCES
    driver/Driver.cpp
    driver/DriverRuntimeLink.cpp
    driver/DriverSoundFont.cpp
    driver/DriverBackend.cpp
    driver/DriverIngress.cpp
    driver/DriverShards.cpp
    driver/DriverRender.cpp
    frontends/FrontNative.cpp
    frontends/FrontKDMAPI.cpp
    frontends/FrontBass.cpp
    events/MidiStream.cpp
    driver/DllMain.cpp
)

set(SOURCES
    synth/Config.cpp
    synth/SoundFont.cpp
    synth/SfzLoader.cpp
    ${SVMS_DRIVER_SOURCES}
    driver/DiagWindow.cpp
    render/RenderKernels.cpp
    render/RenderWorkers.cpp
    render/RenderDispatch.cpp
    render/RenderKernelsSSE2.cpp
)

if(NOT SVMS_XP_COMPAT)
    list(APPEND SOURCES render/RenderKernelsAVX2.cpp synth/LiveRecorder.cpp
         render/GpuSynth.cpp render/GpuDevice.cpp
         render/GpuStage.cpp render/GpuBackendGL.cpp)
endif()

# Steinberg ASIO SDK host-side glue (GPLv3 — see asio-main/). Compiled into
# svmsynth only for non-XP builds; the actual drivers are vendor COM DLLs.
set(SVMS_ASIO_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../asio-main/common/asio.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../asio-main/common/combase.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../asio-main/common/debugmessage.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../asio-main/host/asiodrivers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../asio-main/host/pc/asiolist.cpp"
)
set(SVMS_ASIO_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../asio-main/common")
set(SVMS_ASIO_INCLUDE_HOST "${CMAKE_CURRENT_SOURCE_DIR}/../asio-main/host")
set(SVMS_ASIO_INCLUDE_PC "${CMAKE_CURRENT_SOURCE_DIR}/../asio-main/host/pc")
if(NOT SVMS_XP_COMPAT AND EXISTS "${SVMS_ASIO_INCLUDE}/asio.h")
    list(APPEND SOURCES ${SVMS_ASIO_SOURCES})
    set(SVMS_WITH_ASIO_DEF SVMS_WITH_ASIO=1)
    if(MSVC)
        set_source_files_properties(${SVMS_ASIO_SOURCES} PROPERTIES
            COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/driver/ASIOForceIncludes.h")
    else()
        set_source_files_properties(${SVMS_ASIO_SOURCES} PROPERTIES
            COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/driver/ASIOForceIncludes.h")
    endif()
endif()

set(HEADERS
    synth/Types.h
    synth/Config.h
    synth/VoiceManager.h
    render/Envelope.h
    synth/ChannelCache.h
    render/RenderScalar.h
    render/RenderEvent.h
    driver/DriverInternal.h
    render/HilbertPair.h
    render/RenderKernels.h
    render/RenderWorkers.h
    driver/AudioOutput.h
    driver/AudioOutputDirectSound.h
    synth/SoundFont.h
    events/PageAllocator.h
    driver/DiagWindow.h
    events/MPSCQueue.h
    events/EventCompile.h
    events/EventScheduler.h
    events/EventPages.h
    events/FrameClock.h
    render/PostFilter.h
    render/Limiter.h
    render/Reverb.h
    frontends/NativeOffline.h
    synth/LiveRecorder.h
    include/svmsapi.h
)

# ISA selection must be applied before either engine target is created; source
# properties are directory-scoped and consumed by every target built from
# these files, so svmsynth and svmsapi cannot drift apart here.
if(MSVC)
    set_source_files_properties(render/RenderKernelsSSE2.cpp PROPERTIES
        COMPILE_OPTIONS "$<$<EQUAL:${CMAKE_SIZEOF_VOID_P},4>:/arch:SSE2>")
    if(NOT SVMS_XP_COMPAT)
        set_source_files_properties(render/RenderKernelsAVX2.cpp PROPERTIES
            COMPILE_OPTIONS "/arch:AVX2")
    endif()
elseif(MINGW)
    set_source_files_properties(render/RenderKernelsSSE2.cpp PROPERTIES
        COMPILE_OPTIONS "-msse2;-mno-avx")
    if(NOT SVMS_XP_COMPAT)
        set_source_files_properties(render/RenderKernelsAVX2.cpp PROPERTIES
            COMPILE_OPTIONS "-mavx2;-mno-fma")
    endif()
endif()

# The engine is compiled once and linked into both DLLs. The only per-DLL
# translation unit is the WinMM shim file, because svmsapi builds it with
# _WINMM_ (see below); no other file defines a WinMM SDK function.
add_library(svms_engine OBJECT ${SOURCES} ${HEADERS})

# Precompile driver/DriverInternal.h for the driver sources. Each of them includes
# it first anyway, so this only removes the repeated parse of the big engine
# headers. Off by default under a compiler cache, which cannot cache PCH users.
if(CMAKE_CXX_COMPILER_LAUNCHER)
    set(SVMS_DRIVER_PCH_DEFAULT OFF)
else()
    set(SVMS_DRIVER_PCH_DEFAULT ON)
endif()
option(SVMS_DRIVER_PCH "Precompile driver/DriverInternal.h for the driver sources"
    ${SVMS_DRIVER_PCH_DEFAULT})
if(SVMS_DRIVER_PCH)
    target_precompile_headers(svms_engine PRIVATE driver/DriverInternal.h)
    set(SVMS_ENGINE_NON_DRIVER_SOURCES ${SOURCES})
    list(REMOVE_ITEM SVMS_ENGINE_NON_DRIVER_SOURCES ${SVMS_DRIVER_SOURCES})
    set_source_files_properties(${SVMS_ENGINE_NON_DRIVER_SOURCES} PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON)
endif()

add_library(svmsynth SHARED frontends/FrontWinMM.cpp
    "${SVMS_GENERATED_DIR}/SVMSDriverVersion.rc")

# Standalone native-API runtime. Built from the same engine sources as
# winmm.dll (one implementation, two frontends), but with its own export
# surface: the permanent SVMS_GetInterface bootstrap plus the KDMAPI facade.
# Deliberately no WinMM shim exports - SVMSAPI.dll is not a midiOutOpen
# drop-in. Players integrate via LoadLibrary + GetProcAddress only.
add_library(svmsapi SHARED frontends/FrontWinMM.cpp frontends/RemoteClient.cpp
    "${SVMS_GENERATED_DIR}/SVMSApiVersion.rc")

set(SVMS_DLL_TARGETS svms_engine svmsynth svmsapi)
if(NOT SVMS_XP_COMPAT)
    # bass.dll: the winmm runtime (same exports) plus the offline note-table
    # renderer, which the KMC/BASSMIDI imitation mode runs in process.
    add_library(svms_offline_render OBJECT synth/OfflineRenderer.cpp)
    target_compile_definitions(svms_offline_render PRIVATE
        SVMS_OFFLINE_AS_LIBRARY NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
        _CRT_SECURE_NO_WARNINGS)
    target_include_directories(svms_offline_render PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
    add_library(svmsbass SHARED frontends/FrontWinMM.cpp
        $<TARGET_OBJECTS:svms_offline_render>
        "${SVMS_GENERATED_DIR}/SVMSDriverVersion.rc")
    target_link_libraries(svmsbass PRIVATE svms_canonical_engine)
    list(APPEND SVMS_DLL_TARGETS svmsbass)
endif()

foreach(svms_engine_target ${SVMS_DLL_TARGETS})
    target_compile_definitions(${svms_engine_target} PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
        _CRT_SECURE_NO_WARNINGS ${SVMS_WITH_ASIO_DEF})
    if(svms_engine_target STREQUAL svmsapi)
        # The system multimedia headers declare the WinMM shims dllimport
        # (WINMMAPI = DECLSPEC_IMPORT in mmsyscom.h). The local shim
        # definitions in frontends/FrontWinMM.cpp then conflict (C4273) and MSVC turns
        # each one into a linker /EXPORT directive, re-exporting the whole
        # WinMM surface. _WINMM_ is the SDK's official "building winmm"
        # switch: it empties WINMMAPI, so no conflict and no phantom exports.
        # The shims stay as private dead code (/OPT:REF strips them) and the
        # export surface is exactly the SVMSAPI_EXPORTS list below. Only the
        # standalone target gets this; winmm.dll keeps its exact export table.
        # It only reaches frontends/FrontWinMM.cpp: svms_engine never sees it.
        target_compile_definitions(${svms_engine_target} PRIVATE _WINMM_)
    endif()
    target_include_directories(${svms_engine_target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
    if(svms_engine_target STREQUAL svms_engine)
        continue()
    endif()
    target_link_libraries(${svms_engine_target} PRIVATE svms_engine)
    if(TARGET svms_gpu_cuda)
        target_link_libraries(${svms_engine_target} PRIVATE svms_gpu_cuda)
    endif()
    if(SVMS_XP_COMPAT)
        target_link_libraries(${svms_engine_target} PRIVATE
            dsound dxguid shell32 user32)
    else()
        target_link_libraries(${svms_engine_target} PRIVATE
            winmm ole32 uuid avrt shell32 d3d11 dxgi d3dcompiler)
    endif()
endforeach()

if(NOT SVMS_XP_COMPAT)
    option(SVMS_ASIO "Build the ASIO output backend (requires the ASIO SDK)" ON)
    if(SVMS_ASIO AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../asio-main/common/asio.h")
        set(ASIO_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../asio-main")
        target_sources(svms_engine PRIVATE
            "${ASIO_SDK_DIR}/common/asio.cpp"
            "${ASIO_SDK_DIR}/common/combase.cpp"
            "${ASIO_SDK_DIR}/common/debugmessage.cpp"
            "${ASIO_SDK_DIR}/host/asiodrivers.cpp"
            "${ASIO_SDK_DIR}/host/pc/asiolist.cpp")
        foreach(svms_engine_target ${SVMS_DLL_TARGETS})
            target_compile_definitions(${svms_engine_target} PRIVATE SVMS_WITH_ASIO)
            target_include_directories(${svms_engine_target} PRIVATE
                "${ASIO_SDK_DIR}/common" "${ASIO_SDK_DIR}/host"
                "${ASIO_SDK_DIR}/host/pc")
        endforeach()
        # The SDK is not warning-clean under /W3; it is vendor code.
        if(MSVC)
            set(ASIO_SDK_SOURCES
                "${ASIO_SDK_DIR}/common/asio.cpp"
                "${ASIO_SDK_DIR}/common/combase.cpp"
                "${ASIO_SDK_DIR}/common/debugmessage.cpp"
                "${ASIO_SDK_DIR}/host/asiodrivers.cpp"
                "${ASIO_SDK_DIR}/host/pc/asiolist.cpp")
            set_source_files_properties(${ASIO_SDK_SOURCES} PROPERTIES
                COMPILE_OPTIONS "/w;/FI${CMAKE_CURRENT_SOURCE_DIR}/driver/AsioSdkPrefix.h")
        endif()
        message(STATUS "SVMS: ASIO backend enabled (${ASIO_SDK_DIR})")
    else()
        message(STATUS "SVMS: ASIO backend disabled (set SVMS_ASIO=ON with the SDK at asio-main/)")
    endif()
endif()

# nlohmann/json is vendored under third_party and intentionally confined to
# the configuration translation unit (both engine targets inherit it above).

set(EXPORTS
        PlaySoundA
        PlaySoundW
        midiInGetErrorTextW
        sndPlaySoundA
        joyGetNumDevs
        joyGetDevCapsA
        joyGetPosEx
        mmioOpenA
        mmioClose
        mmioRead
        mmioSeek
        mmioDescend
        mmioAscend
        mmioStringToFOURCCA
        waveInGetErrorTextA
        waveInGetErrorTextW
        waveInGetID
        midiInGetErrorTextA
        sndPlaySoundW
        timeGetSystemTime
        mmsystemGetVersion
        mmioOpenW
        mmioWrite
        mmioAdvance
        mmioFlush
        mmioGetInfo
        mmioSetInfo
        mmioSetBuffer
        mmioCreateChunk
        mmioStringToFOURCCW
        mmioRenameA
        mmioRenameW
        mmioSendMessage
        mmioInstallIOProcA
        mmioInstallIOProcW
        auxGetNumDevs
        auxGetDevCapsA
        auxGetDevCapsW
        auxGetVolume
        auxSetVolume
        auxOutMessage
        joyGetPos
        joyGetDevCapsW
        joyGetThreshold
        joySetThreshold
        joyReleaseCapture
        joySetCapture
        mciSendCommandA
        mciSendCommandW
        mciSendStringA
        mciSendStringW
        mciGetErrorStringA
        mciGetErrorStringW
        mciGetDeviceIDA
        mciGetDeviceIDW
        BASS_Init
        BASS_Free
        BASS_SetConfig
        BASS_GetConfig
        BASS_GetVersion
        BASS_ErrorGetCode
        BASS_MIDI_FontInit
        BASS_MIDI_FontLoad
        BASS_MIDI_FontFree
        BASS_MIDI_StreamSetFonts
        BASS_MIDI_StreamCreate
        BASS_MIDI_StreamEvents
        BASS_MIDI_StreamEvent
        BASS_MIDI_StreamGetEvent
        BASS_MIDI_GetVersion
        BASS_MIDI_StreamLoadSamples
        BASS_MIDI_StreamSetFilter
        BASS_MIDI_FontFlags
        BASS_MIDI_FontSetVolume
        BASS_MIDI_FontGetVolume
        BASS_MIDI_FontCompact
        BASS_MIDI_FontUnload
        BASS_MIDI_StreamCreateFile
        BASS_MIDI_StreamCreateURL
        BASS_MIDI_StreamCreateFileUser
        BASS_MIDI_StreamCreateEvents
        BASS_MIDI_ConvertEvents
        BASS_MIDI_StreamGetEvents
        BASS_MIDI_StreamGetEventsEx
        BASS_MIDI_StreamGetMark
        BASS_MIDI_StreamGetMarks
        BASS_MIDI_StreamGetPreset
        BASS_MIDI_StreamGetFonts
        BASS_MIDI_StreamGetChannel
        BASS_MIDI_FontInitUser
        BASS_MIDI_FontLoadEx
        BASS_MIDI_FontPack
        BASS_MIDI_FontUnpack
        BASS_MIDI_FontGetInfo
        BASS_MIDI_FontGetPreset
        BASS_MIDI_FontGetPresets
        BASS_MIDI_InInit
        BASS_MIDI_InFree
        BASS_MIDI_InStart
        BASS_MIDI_InStop
        BASS_MIDI_InGetDeviceInfo
        BASS_ChannelGetData
        BASS_ChannelGetLength
        BASS_ChannelGetPosition
        BASS_ChannelSetPosition
        BASS_ChannelIsActive
        BASS_ChannelSeconds2Bytes
        BASS_ChannelBytes2Seconds
        BASS_ChannelSetAttribute
        BASS_ChannelGetAttribute
        BASS_ChannelFlags
        BASS_ChannelGetInfo
        BASS_StreamFree
        BASS_FXReset
        BASS_FXFree
        BASS_FXSetParameters
        BASS_FXGetParameters
        BASS_FXSetPriority
        BASS_FXVersion
        BASS_ChannelSetSync
        BASS_ChannelRemoveSync
        BASS_ChannelSetDSP
        BASS_ChannelRemoveDSP
        BASS_ChannelSetFX
        BASS_ChannelRemoveFX
        BASS_GetConfigPtr
        BASS_ChannelGetTags
        BASS_MIDI_StreamGetTrackCount
        midiOutGetNumDevs
        midiOutGetDevCapsA
        midiOutGetDevCapsW
        midiOutOpen
        midiOutClose
        midiOutShortMsg
        midiOutLongMsg
        midiOutReset
        midiOutPrepareHeader
        midiOutUnprepareHeader
        midiOutGetVolume
        midiOutSetVolume
        midiOutGetErrorTextA
        midiOutGetErrorTextW
        midiOutGetID
        midiOutMessage
        IsKDMAPIAvailable
        InitializeKDMAPIStream
        TerminateKDMAPIStream
        ResetKDMAPIStream
        ReturnKDMAPIVer
        SendDirectData
        SendDirectDataNoBuf
        SendCustomEvent
        SendDirectLongData
        SendDirectLongDataNoBuf
        PrepareLongData
        UnprepareLongData
        DriverSettings
        GetDriverDebugInfo
        GetRenderingTime
        GetVoiceCount
        GetVoiceStatistics
        SVMSGetDriverDebugInfoV1
        SVMS_GetInterface
        LoadCustomSoundFontsList
        timeGetTime64
        midiInGetNumDevs
        midiInGetDevCapsA
        midiInGetDevCapsW
        midiInOpen
        midiInClose
        midiInPrepareHeader
        midiInUnprepareHeader
        midiInAddBuffer
        midiInStart
        midiInStop
        midiInReset
        midiInGetID
        midiInMessage
        waveInGetNumDevs
        waveInGetDevCapsA
        waveInGetDevCapsW
        waveInOpen
        waveInClose
        waveInPrepareHeader
        waveInUnprepareHeader
        waveInAddBuffer
        waveInStart
        waveInStop
        waveInReset
        waveInMessage
        waveInGetPosition
        mixerGetNumDevs
        mixerGetDevCapsA
        mixerGetDevCapsW
        mixerOpen
        mixerClose
        mixerMessage
        mixerGetLineInfoA
        mixerGetLineInfoW
        mixerGetID
        mixerGetLineControlsA
        mixerGetLineControlsW
        mixerGetControlDetailsA
        mixerGetControlDetailsW
        mixerSetControlDetails
        waveOutGetNumDevs
        waveOutGetDevCapsA
        waveOutGetDevCapsW
        waveOutOpen
        waveOutClose
        waveOutPrepareHeader
        waveOutUnprepareHeader
        waveOutWrite
        waveOutReset
        waveOutRestart
        waveOutPause
        waveOutBreakLoop
        waveOutGetPosition
        waveOutGetVolume
        waveOutSetVolume
        waveOutGetPitch
        waveOutSetPitch
        waveOutGetPlaybackRate
        waveOutSetPlaybackRate
        waveOutGetID
        waveOutGetErrorTextA
        waveOutGetErrorTextW
        waveOutMessage
        timeGetTime
        timeBeginPeriod
        timeEndPeriod
        timeGetDevCaps
        timeSetEvent
        timeKillEvent
)
if(MSVC)
    foreach(exp ${EXPORTS})
        target_link_options(svmsynth PRIVATE /EXPORT:${exp})
        if(TARGET svmsbass)
            target_link_options(svmsbass PRIVATE /EXPORT:${exp})
        endif()
    endforeach()
elseif(MINGW)
    set(SVMS_EXPORT_FILE "${CMAKE_CURRENT_BINARY_DIR}/SVMSExports.def")
    file(WRITE "${SVMS_EXPORT_FILE}" "EXPORTS\n")
    foreach(exp ${EXPORTS})
        file(APPEND "${SVMS_EXPORT_FILE}" "    ${exp}\n")
    endforeach()
    target_sources(svmsynth PRIVATE "${SVMS_EXPORT_FILE}")
    target_link_options(svmsynth PRIVATE -Wl,--kill-at)
endif()

if(TARGET svmsbass)
    set_target_properties(svmsbass PROPERTIES
        OUTPUT_NAME "bass"
        PREFIX ""
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

set_target_properties(svmsynth PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# SVMSAPI.dll export surface: the permanent SVMS_GetInterface bootstrap plus
# the KDMAPI facade (OmniMIDI-conventional entry points, which the native API
# smoke test also verifies). No WinMM shims.
set(SVMSAPI_EXPORTS
        SVMS_GetInterface
        SVMS_Remote_EnumerateHosts
        SVMS_Remote_Attach
        SVMS_Remote_IsAlive
        SVMS_Remote_SetTimeout
        IsKDMAPIAvailable
        InitializeKDMAPIStream
        TerminateKDMAPIStream
        ResetKDMAPIStream
        ReturnKDMAPIVer
        SendDirectData
        SendDirectDataNoBuf
        SendCustomEvent
        SendDirectLongData
        SendDirectLongDataNoBuf
        PrepareLongData
        UnprepareLongData
        DriverSettings
        GetDriverDebugInfo
        GetRenderingTime
        GetVoiceCount
        GetVoiceStatistics
        SVMSGetDriverDebugInfoV1
        LoadCustomSoundFontsList
        timeGetTime64
)
if(MSVC)
    foreach(exp ${SVMSAPI_EXPORTS})
        target_link_options(svmsapi PRIVATE /EXPORT:${exp})
    endforeach()
elseif(MINGW)
    set(SVMS_API_EXPORT_FILE "${CMAKE_CURRENT_BINARY_DIR}/SVMSApiExports.def")
    file(WRITE "${SVMS_API_EXPORT_FILE}" "EXPORTS\n")
    foreach(exp ${SVMSAPI_EXPORTS})
        file(APPEND "${SVMS_API_EXPORT_FILE}" "    ${exp}\n")
    endforeach()
    target_sources(svmsapi PRIVATE "${SVMS_API_EXPORT_FILE}")
    target_link_options(svmsapi PRIVATE -Wl,--kill-at)
endif()

set_target_properties(svmsapi PROPERTIES
    OUTPUT_NAME "SVMSAPI"
    PREFIX ""
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
# Map file: lets crash-stack RVAs be resolved to static/internal functions
# even when dbghelp's DIA refuses the newest toolset's PDB format.
target_link_options(svmsapi PRIVATE
    "/MAP:${CMAKE_BINARY_DIR}/bin/SVMSAPI.map")

if(NOT SVMS_XP_COMPAT)
    add_executable(svms_v3_render
        synth/OfflineRenderer.cpp
        synth/Config.cpp
        events/MidiStream.cpp
        synth/SoundFont.cpp
        synth/SfzLoader.cpp
        render/RenderKernels.cpp
        render/RenderWorkers.cpp
        render/RenderDispatch.cpp
        render/RenderKernelsSSE2.cpp
        render/RenderKernelsAVX2.cpp
        render/GpuSynth.cpp
        render/GpuDevice.cpp
        render/GpuStage.cpp
        render/GpuBackendGL.cpp $<TARGET_OBJECTS:svms_gpu_cuda>)
    target_compile_definitions(svms_v3_render PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    if(MINGW)
        # wmain entry point requires the unicode startup on MinGW.
        target_compile_options(svms_v3_render PRIVATE -municode)
        target_link_options(svms_v3_render PRIVATE -municode)
    endif()
    target_include_directories(svms_v3_render PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
    target_link_libraries(svms_v3_render PRIVATE shell32 d3d11 dxgi d3dcompiler)
    target_link_libraries(svms_v3_render PRIVATE svms_canonical_engine)
    set_target_properties(svms_v3_render PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    add_executable(svms_v3_realtime_block_tests
        tests/RealtimeBlockTests.cpp
        synth/Config.cpp
        synth/SoundFont.cpp
        synth/SfzLoader.cpp
        render/RenderKernels.cpp
        render/RenderWorkers.cpp
        render/RenderDispatch.cpp
        render/RenderKernelsSSE2.cpp
        render/RenderKernelsAVX2.cpp
        render/GpuSynth.cpp
        render/GpuDevice.cpp
        render/GpuStage.cpp
        render/GpuBackendGL.cpp $<TARGET_OBJECTS:svms_gpu_cuda>)
    target_compile_definitions(svms_v3_realtime_block_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_include_directories(svms_v3_realtime_block_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
    target_link_libraries(svms_v3_realtime_block_tests PRIVATE d3d11 dxgi d3dcompiler)
    add_test(NAME svms_v3_realtime_block COMMAND svms_v3_realtime_block_tests
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/..")
    set_tests_properties(svms_v3_realtime_block PROPERTIES SKIP_RETURN_CODE 77)

    # GPU synthesis proof spike (D3D11 compute). Offline-only, not XP. Proves
    # compute-shader sample synthesis matches the CPU scalar reference to the
    # agreed -85 dB gate before it is wired into RenderBlock.
    add_executable(svms_v3_gpu_spike
        render/GpuSpike.cpp
        render/GpuDevice.cpp)
    target_compile_definitions(svms_v3_gpu_spike PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_include_directories(svms_v3_gpu_spike PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
    target_link_libraries(svms_v3_gpu_spike PRIVATE
        d3d11 dxgi d3dcompiler)
    set_target_properties(svms_v3_gpu_spike PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui")
    set(IMGUI_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )
    set(CONFIGURATOR_SOURCES
        configurator/Main.cpp
        frontends/RemoteClient.cpp
        configurator/ConfiguratorApp.cpp
        configurator/Platform_Win32.cpp
        configurator/PlatformUtil.cpp
        configurator/ConfigDocument.cpp
        configurator/Theme.cpp
        configurator/Widgets.cpp
        configurator/Anim.cpp
        configurator/ModernWidgets.cpp
        configurator/SettingsRegistry.cpp
        configurator/WasapiDevices.cpp
        configurator/EasterEggs.cpp
        configurator/UpdateService.cpp
        configurator/PageHome.cpp
        configurator/PageOutput.cpp
        configurator/PageSound.cpp
        configurator/PagePerformance.cpp
        configurator/PageMidi.cpp
        configurator/PageOffline.cpp
        configurator/PageLiveRecording.cpp
        configurator/PageEffects.cpp
        configurator/PageDiagnostics.cpp
        configurator/PageSettings.cpp
        configurator/Tour.cpp
    )
    add_executable(svms_v3_configurator WIN32
        ${IMGUI_SOURCES}
        ${CONFIGURATOR_SOURCES}
        "${SVMS_GENERATED_DIR}/SVMSConfiguratorVersion.rc"
    )
    target_include_directories(svms_v3_configurator PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${IMGUI_DIR}"
        "${IMGUI_DIR}/backends"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party"
    )
    target_compile_definitions(svms_v3_configurator PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(svms_v3_configurator PRIVATE
        d3d11 dxgi d3dcompiler dwmapi user32 gdi32 shell32 ole32 uuid comdlg32 winhttp)
    set_target_properties(svms_v3_configurator PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# Ziggy's direct-synth loader probes SnappySynth.dll and then discovers the
# SSV2-compatible telemetry/KDMAPI exports. Keep winmm.dll as the canonical
# drop-in output and emit a byte-identical alias for that host integration.
add_custom_target(svms_ziggy_alias ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:svmsynth>"
            "$<TARGET_FILE_DIR:svmsynth>/SnappySynth.dll"
    DEPENDS svmsynth
    VERBATIM)

# The standalone SVMSAPI.dll is a real target above (same engine sources, its
# own export surface). Keep the shorter SVMS.dll spelling as a compatibility
# alias of the winmm runtime. WinMM and native-API clients now intentionally
# load different modules; the engine implementation is shared at the source
# level and both export surfaces are exercised by the ctest suite.
add_custom_target(svms_native_alias ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:svmsynth>"
            "$<TARGET_FILE_DIR:svmsynth>/SVMS.dll"
    DEPENDS svmsynth
    VERBATIM)

# Legacy KDMAPI applications conventionally load OmniMIDI.dll directly.
# Publish the same runtime under that name so the facade and native API share
# scheduling, SoundFont state, telemetry, and ownership.
add_custom_target(svms_kdmapi_alias ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:svmsynth>"
            "$<TARGET_FILE_DIR:svmsynth>/OmniMIDI.dll"
    DEPENDS svmsynth
    VERBATIM)

# BASS compatibility: bass.dll is the hosting alias of the runtime (the
# full BASS/BASSMIDI prerender surface lives in svmsynth); bassmidi.dll is
# a thin forwarder to it so a player loading both module names sees one
# engine instance.
if(NOT TARGET svmsbass)
    add_custom_target(svms_bass_alias ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:svmsynth>"
                "$<TARGET_FILE_DIR:svmsynth>/bass.dll"
        DEPENDS svmsynth
        VERBATIM)
endif()

add_library(svms_bassmidi_forwarder SHARED frontends/BassMidiForwarder.cpp)
set_target_properties(svms_bassmidi_forwarder PROPERTIES
    OUTPUT_NAME "bassmidi"
    PREFIX ""
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(MSVC)
    foreach(exp
            BASS_Init BASS_Free BASS_SetConfig BASS_GetConfig
            BASS_GetVersion BASS_ErrorGetCode BASS_MIDI_FontInit
            BASS_MIDI_FontLoad BASS_MIDI_FontFree
            BASS_MIDI_StreamSetFonts BASS_MIDI_StreamCreate
            BASS_MIDI_StreamEvents BASS_MIDI_StreamEvent
            BASS_MIDI_StreamGetEvent BASS_MIDI_GetVersion
            BASS_MIDI_StreamLoadSamples BASS_MIDI_StreamSetFilter
            BASS_MIDI_FontFlags BASS_MIDI_FontSetVolume
            BASS_MIDI_FontGetVolume BASS_MIDI_FontCompact
            BASS_MIDI_FontUnload BASS_MIDI_StreamCreateFile
            BASS_MIDI_StreamCreateURL BASS_MIDI_StreamCreateFileUser
            BASS_MIDI_StreamCreateEvents BASS_MIDI_ConvertEvents
            BASS_MIDI_StreamGetEvents BASS_MIDI_StreamGetEventsEx
            BASS_MIDI_StreamGetMark BASS_MIDI_StreamGetMarks
            BASS_MIDI_StreamGetPreset BASS_MIDI_StreamGetFonts
            BASS_MIDI_StreamGetChannel BASS_MIDI_FontInitUser
            BASS_MIDI_FontLoadEx BASS_MIDI_FontPack BASS_MIDI_FontUnpack
            BASS_MIDI_FontGetInfo BASS_MIDI_FontGetPreset
            BASS_MIDI_FontGetPresets BASS_MIDI_InInit BASS_MIDI_InFree
            BASS_MIDI_InStart BASS_MIDI_InStop BASS_MIDI_InGetDeviceInfo
            BASS_ChannelGetData
            BASS_ChannelGetLength BASS_ChannelGetPosition
            BASS_ChannelSetPosition BASS_ChannelIsActive
            BASS_ChannelSeconds2Bytes BASS_ChannelBytes2Seconds
            BASS_ChannelSetAttribute BASS_ChannelGetAttribute
            BASS_ChannelFlags BASS_ChannelGetInfo BASS_StreamFree
            BASS_FXReset BASS_FXFree BASS_FXSetParameters
            BASS_FXGetParameters BASS_FXSetPriority BASS_FXVersion
            BASS_MIDI_StreamGetTrackCount)
        target_link_options(svms_bassmidi_forwarder PRIVATE /EXPORT:${exp})
    endforeach()
endif()

# bassenc.dll replacement: real bassenc imports an undocumented table from the
# real bass.dll, so conversions (KMC's WAV/OGG/MP3 export) get this small
# shim that encodes from the SVMS bass.dll's channel DSP chain.
add_library(svms_bassenc_shim SHARED frontends/BassEncShim.cpp)
set_target_properties(svms_bassenc_shim PROPERTIES
    OUTPUT_NAME "bassenc"
    PREFIX ""
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(MSVC)
    foreach(exp
            BASS_Encode_GetVersion BASS_Encode_Start BASS_Encode_Stop
            BASS_Encode_StopEx BASS_Encode_IsActive BASS_Encode_SetPaused
            BASS_Encode_Write BASS_Encode_GetChannel BASS_Encode_GetCount
            BASS_Encode_SetNotify)
        target_link_options(svms_bassenc_shim PRIVATE /EXPORT:${exp})
    endforeach()
endif()

# bass_vst.dll stand-in (see frontends/BassVstShim.cpp).
add_library(svms_bassvst_shim SHARED frontends/BassVstShim.cpp)
set_target_properties(svms_bassvst_shim PROPERTIES
    OUTPUT_NAME "bass_vst"
    PREFIX ""
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# basswasapi.dll stand-in (see frontends/BassWasapiShim.cpp).
add_library(svms_basswasapi_shim SHARED frontends/BassWasapiShim.cpp)
set_target_properties(svms_basswasapi_shim PROPERTIES
    OUTPUT_NAME "basswasapi"
    PREFIX ""
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(SVMS_BUILD_TESTS "Build deterministic V3 correctness tests" ON)

if(SVMS_BUILD_TESTS)
    add_executable(svms_v3_native_header_c
        tests/NativeHeaderCSmoke.c)
    target_include_directories(svms_v3_native_header_c PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_test(NAME svms_v3_native_header_c
        COMMAND svms_v3_native_header_c)

    add_executable(svms_v3_telemetry_abi_smoke
        tests/TelemetryABISmoke.cpp)
    target_compile_definitions(svms_v3_telemetry_abi_smoke PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_dependencies(svms_v3_telemetry_abi_smoke svmsynth)
    add_test(NAME svms_v3_telemetry_abi_smoke
             COMMAND svms_v3_telemetry_abi_smoke "$<TARGET_FILE:svmsynth>")

    add_executable(svms_v3_runtime_link_abi_smoke
        tests/RuntimeLinkABISmoke.cpp)
    target_compile_definitions(svms_v3_runtime_link_abi_smoke PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_include_directories(svms_v3_runtime_link_abi_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME svms_v3_runtime_link_abi_smoke
             COMMAND svms_v3_runtime_link_abi_smoke)

    add_executable(svms_v3_native_api_smoke
        tests/NativeAPISmoke.cpp)
    target_include_directories(svms_v3_native_api_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_dependencies(svms_v3_native_api_smoke svmsapi)
    add_test(NAME svms_v3_native_api
             COMMAND svms_v3_native_api_smoke
                     "$<TARGET_FILE:svmsapi>")
    set_tests_properties(svms_v3_native_api PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(svms_v3_offline_stack_smoke
        tests/OfflineStackSmoke.cpp)
    target_include_directories(svms_v3_offline_stack_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(svms_v3_offline_stack_smoke svmsapi)
    add_test(NAME svms_v3_offline_stack
             COMMAND svms_v3_offline_stack_smoke
                     "$<TARGET_FILE:svmsapi>")
    set_tests_properties(svms_v3_offline_stack PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(svms_v3_remote_client_smoke
        tests/RemoteClientSmoke.cpp)
    target_compile_definitions(svms_v3_remote_client_smoke PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(svms_v3_remote_client_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    if(SVMS_XP_COMPAT)
        target_compile_definitions(svms_v3_remote_client_smoke PRIVATE SVMS_XP_COMPAT)
    endif()
    add_dependencies(svms_v3_remote_client_smoke svmsapi)
    add_test(NAME svms_v3_remote_client
             COMMAND svms_v3_remote_client_smoke "$<TARGET_FILE:svmsapi>")
    set_tests_properties(svms_v3_remote_client PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 120)

    add_executable(svms_v3_engine_link_smoke
        tests/EngineLinkSmoke.cpp
        frontends/RemoteClient.cpp)
    target_compile_definitions(svms_v3_engine_link_smoke PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_include_directories(svms_v3_engine_link_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
    if(SVMS_XP_COMPAT)
        target_compile_definitions(svms_v3_engine_link_smoke PRIVATE SVMS_XP_COMPAT)
    endif()
    add_dependencies(svms_v3_engine_link_smoke svmsapi)
    add_test(NAME svms_v3_engine_link
             COMMAND svms_v3_engine_link_smoke "$<TARGET_FILE:svmsapi>")
    set_tests_properties(svms_v3_engine_link PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 120)

    # The driver-schema code only builds against the Linux socket client; this
    # test compiles it against an in-memory fake so every platform runs it.
    add_executable(svms_v3_remote_schema_test
        tests/RemoteSchemaTest.cpp
        frontends/RemoteClient.cpp)
    target_compile_definitions(svms_v3_remote_schema_test PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
        SVMS_REMOTE_USE_FAKE_CLIENT)
    target_include_directories(svms_v3_remote_schema_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${SVMS_GENERATED_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
    add_test(NAME svms_v3_remote_schema COMMAND svms_v3_remote_schema_test)

    add_executable(svms_v3_tests
        tests/V3Tests.cpp
        synth/Config.cpp
        synth/SoundFont.cpp
        synth/SfzLoader.cpp
        render/RenderKernels.cpp
        render/RenderWorkers.cpp
        render/RenderDispatch.cpp
        render/RenderKernelsSSE2.cpp
    )
    if(NOT SVMS_XP_COMPAT)
        target_sources(svms_v3_tests PRIVATE render/RenderKernelsAVX2.cpp
            render/GpuStage.cpp render/GpuBackendGL.cpp $<TARGET_OBJECTS:svms_gpu_cuda>)
    endif()
    target_compile_definitions(svms_v3_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS SVMS_ENABLE_REFERENCE_RENDERER=1)
    target_include_directories(svms_v3_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(svms_v3_tests PRIVATE user32)
    target_link_libraries(svms_v3_tests PRIVATE shell32)
    target_include_directories(svms_v3_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
    add_test(NAME svms_v3_correctness COMMAND svms_v3_tests)

    add_executable(svms_v3_voice_growth_tests
        tests/VoiceGrowthRegression.cpp
        render/RenderKernels.cpp
        render/RenderWorkers.cpp
        render/RenderDispatch.cpp
        render/RenderKernelsSSE2.cpp)
    if(NOT SVMS_XP_COMPAT)
        target_sources(svms_v3_voice_growth_tests PRIVATE render/RenderKernelsAVX2.cpp)
    endif()
    target_compile_definitions(svms_v3_voice_growth_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_include_directories(svms_v3_voice_growth_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME svms_v3_voice_growth COMMAND svms_v3_voice_growth_tests)

    add_executable(svms_v3_limiter_tests
        tests/LimiterTests.cpp)
    target_compile_definitions(svms_v3_limiter_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_include_directories(svms_v3_limiter_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME svms_v3_limiter COMMAND svms_v3_limiter_tests)

        add_executable(svms_v3_control_tests tests/ControlRegistryTests.cpp
            ${SVMS_CONTROL_SERVER_SOURCES})
        target_include_directories(svms_v3_control_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
        if(UNIX)
            target_link_libraries(svms_v3_control_tests PRIVATE Threads::Threads)
        endif()
        add_test(NAME svms_v3_control COMMAND svms_v3_control_tests)
    if(SVMS_XP_COMPAT)
        add_executable(svms_v3_xp_directsound_smoke


            tests/XPDirectSoundSmoke.cpp)
        target_compile_definitions(svms_v3_xp_directsound_smoke PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        target_include_directories(svms_v3_xp_directsound_smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(svms_v3_xp_directsound_smoke PRIVATE
            dsound dxguid user32)
        add_test(NAME svms_v3_xp_directsound_smoke
                 COMMAND svms_v3_xp_directsound_smoke)
        set_tests_properties(svms_v3_xp_directsound_smoke PROPERTIES
            SKIP_RETURN_CODE 77)
        add_test(NAME svms_v3_xp_waveout_smoke
                 COMMAND svms_v3_xp_directsound_smoke --waveout)
        set_tests_properties(svms_v3_xp_waveout_smoke PROPERTIES
            SKIP_RETURN_CODE 77)

        add_executable(svms_v3_xp_dll_smoke
            tests/XPDllSmoke.cpp)
        target_compile_definitions(svms_v3_xp_dll_smoke PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        target_link_libraries(svms_v3_xp_dll_smoke PRIVATE user32)
        add_dependencies(svms_v3_xp_dll_smoke svmsynth)
        add_test(NAME svms_v3_xp_dll_smoke
                 COMMAND svms_v3_xp_dll_smoke
                         "$<TARGET_FILE:svmsynth>")
        set_tests_properties(svms_v3_xp_dll_smoke PROPERTIES
            SKIP_RETURN_CODE 77)
        add_test(NAME svms_v3_xp_waveout_dll_smoke
                 COMMAND svms_v3_xp_dll_smoke
                         "$<TARGET_FILE:svmsynth>" --waveout)
        set_tests_properties(svms_v3_xp_waveout_dll_smoke PROPERTIES
            SKIP_RETURN_CODE 77)
        add_test(NAME svms_v3_xp_diag_window_smoke
                 COMMAND svms_v3_xp_dll_smoke
                         "$<TARGET_FILE:svmsynth>" --diag-window)
        set_tests_properties(svms_v3_xp_diag_window_smoke PROPERTIES
            SKIP_RETURN_CODE 77)

        # XP gets the same offline MIDI-song regression gate (no GPU sources).
        add_executable(svms_v3_midi_song
            tests/V3MidiSong.cpp
            frontends/RemoteClient.cpp
            synth/Config.cpp
            events/MidiStream.cpp
            synth/SoundFont.cpp
            synth/SfzLoader.cpp
            render/RenderKernels.cpp
            render/RenderWorkers.cpp
            render/RenderDispatch.cpp
            render/RenderKernelsSSE2.cpp
            render/RenderKernelsAVX2.cpp)
        target_compile_definitions(svms_v3_midi_song PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
            _CRT_SECURE_NO_WARNINGS)
        target_include_directories(svms_v3_midi_song PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
        target_link_libraries(svms_v3_midi_song PRIVATE winmm shell32)
        add_test(NAME svms_v3_midi_song
                 COMMAND svms_v3_midi_song --quiet)
        set_tests_properties(svms_v3_midi_song PROPERTIES
            SKIP_RETURN_CODE 77 TIMEOUT 600)
    else()
        add_executable(svms_v3_midi_stream_tests
            tests/MidiStreamTests.cpp events/MidiStream.cpp)
        target_compile_definitions(svms_v3_midi_stream_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
        target_include_directories(svms_v3_midi_stream_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        if(MINGW)
            # Windows entry point is wmain; MinGW needs the unicode startup.
            target_compile_options(svms_v3_midi_stream_tests PRIVATE -municode)
            target_link_options(svms_v3_midi_stream_tests PRIVATE -municode)
        endif()
        add_test(NAME svms_v3_midi_stream COMMAND svms_v3_midi_stream_tests)

        add_executable(svms_v3_unsafe_exit_tests
            tests/UnsafeExitTests.cpp)
        target_compile_definitions(svms_v3_unsafe_exit_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(svms_v3_unsafe_exit_tests PRIVATE winmm)
        add_test(NAME svms_v3_unsafe_exit
                 COMMAND svms_v3_unsafe_exit_tests)
        set_tests_properties(svms_v3_unsafe_exit PROPERTIES
            SKIP_RETURN_CODE 77 TIMEOUT 300)

        if(EXISTS "${SVMS_ASIO_INCLUDE}/asio.h")
            add_executable(svms_v3_asio_stress_tests
                tests/AsioStressTests.cpp driver/AudioOutputASIO.h
                ${SVMS_ASIO_SOURCES})
            target_compile_definitions(svms_v3_asio_stress_tests PRIVATE
                NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
                ${SVMS_WITH_ASIO_DEF})
            target_include_directories(svms_v3_asio_stress_tests PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}"
                "${SVMS_ASIO_INCLUDE}"
                "${SVMS_ASIO_INCLUDE_HOST}"
                "${SVMS_ASIO_INCLUDE_PC}")
            add_test(NAME svms_v3_asio_stress
                     COMMAND svms_v3_asio_stress_tests)
            set_tests_properties(svms_v3_asio_stress PROPERTIES
                SKIP_RETURN_CODE 77 TIMEOUT 180)
        endif()

        add_executable(svms_v3_config_profile_tests
            tests/ConfiguratorProfileTests.cpp
            configurator/ConfigDocument.cpp)
        target_compile_definitions(svms_v3_config_profile_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
            _CRT_SECURE_NO_WARNINGS)
        target_include_directories(svms_v3_config_profile_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
        add_test(NAME svms_v3_config_profiles
            COMMAND svms_v3_config_profile_tests)

        add_executable(svms_v3_live_recorder_tests
            tests/LiveRecorderTests.cpp
            synth/LiveRecorder.cpp)
        target_compile_definitions(svms_v3_live_recorder_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
            _CRT_SECURE_NO_WARNINGS)
        target_include_directories(svms_v3_live_recorder_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME svms_v3_live_recorder
            COMMAND svms_v3_live_recorder_tests)

        add_executable(svms_v3_live_smoke tests/LiveSmoke.cpp)
        target_compile_definitions(svms_v3_live_smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MINGW)
            # wmain entry point requires the unicode startup on MinGW.
            target_compile_options(svms_v3_live_smoke PRIVATE -municode)
            target_link_options(svms_v3_live_smoke PRIVATE -municode)
        endif()
        target_include_directories(svms_v3_live_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(svms_v3_live_smoke PRIVATE ole32 uuid)

        # Live smoke of the engine link (EngineLink = SVMSAPI remote client)
        # against the real winmm.dll driver running inside this process.
        add_executable(svms_v3_runtime_link_smoke
            tests/RuntimeLinkSmoke.cpp
            frontends/RemoteClient.cpp)
        target_compile_definitions(svms_v3_runtime_link_smoke PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
            _CRT_SECURE_NO_WARNINGS)
        target_include_directories(svms_v3_runtime_link_smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
        add_dependencies(svms_v3_runtime_link_smoke svmsynth)
        add_test(NAME svms_v3_runtime_link_smoke
                 COMMAND svms_v3_runtime_link_smoke "$<TARGET_FILE:svmsynth>")
        set_tests_properties(svms_v3_runtime_link_smoke PROPERTIES
            SKIP_RETURN_CODE 77)

        # The two-process flow against the shipping winmm.dll as the host (the
        # old TwoProcessSmoke + probe): same test as svms_v3_remote_client, with
        # the host engine taken from winmm.dll instead of SVMSAPI.dll.
        add_test(NAME svms_v3_two_process_smoke
                 COMMAND svms_v3_remote_client_smoke
                         "$<TARGET_FILE:svmsapi>" "$<TARGET_FILE:svmsynth>")
        set_tests_properties(svms_v3_two_process_smoke PROPERTIES
            SKIP_RETURN_CODE 77 TIMEOUT 120)
        add_dependencies(svms_v3_remote_client_smoke svmsynth)

        add_executable(svms_v3_bench tests/V3Bench.cpp synth/SoundFont.cpp synth/SfzLoader.cpp render/RenderKernels.cpp render/RenderWorkers.cpp
            render/RenderDispatch.cpp render/RenderKernelsSSE2.cpp render/RenderKernelsAVX2.cpp
            render/GpuStage.cpp render/GpuBackendGL.cpp $<TARGET_OBJECTS:svms_gpu_cuda>)
        target_compile_definitions(svms_v3_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
            SVMS_ENABLE_REFERENCE_RENDERER=1)
        target_include_directories(svms_v3_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(svms_v3_bench PRIVATE avrt)

        # "Proper MIDI" playback test: offline render of a real SMF through the
        # production synthesis chain (StandaloneSynth), silent by construction,
        # plus an optional --realtime mode that drives the live winmm.dll
        # (muted unless --audible).  Telemetry/verbose flags make it a
        # performance-development harness as well as a regression gate.
        set(SVMS_MIDI_SONG_SOURCES
            tests/V3MidiSong.cpp
            frontends/RemoteClient.cpp
            synth/Config.cpp
            events/MidiStream.cpp
            synth/SoundFont.cpp
            synth/SfzLoader.cpp
            render/RenderKernels.cpp
            render/RenderWorkers.cpp
            render/RenderDispatch.cpp
            render/RenderKernelsSSE2.cpp
            render/RenderKernelsAVX2.cpp)
        add_executable(svms_v3_midi_song ${SVMS_MIDI_SONG_SOURCES})
        target_compile_definitions(svms_v3_midi_song PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
            _CRT_SECURE_NO_WARNINGS)
        target_include_directories(svms_v3_midi_song PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
        target_link_libraries(svms_v3_midi_song PRIVATE winmm shell32)
        if(NOT SVMS_XP_COMPAT)
            target_sources(svms_v3_midi_song PRIVATE
                render/GpuSynth.cpp render/GpuDevice.cpp
                render/GpuStage.cpp render/GpuBackendGL.cpp $<TARGET_OBJECTS:svms_gpu_cuda>)
            target_link_libraries(svms_v3_midi_song PRIVATE d3d11 dxgi d3dcompiler)
        endif()
        add_test(NAME svms_v3_midi_song
                 COMMAND svms_v3_midi_song --quiet)
        set_tests_properties(svms_v3_midi_song PROPERTIES
            SKIP_RETURN_CODE 77 TIMEOUT 600)

        add_executable(svms_v3_direct_ingest_test tests/DirectIngestTest.cpp)
        target_compile_definitions(svms_v3_direct_ingest_test PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        target_include_directories(svms_v3_direct_ingest_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME svms_v3_direct_ingest COMMAND svms_v3_direct_ingest_test)
        # Needs a live audio device (set SVMS_TEST_CONFIG_PATH to a master_volume 0 config); not a ctest.
        add_executable(svms_v3_direct_e2e tests/DirectE2E.cpp)
        target_compile_definitions(svms_v3_direct_e2e PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

        add_executable(svms_v3_event_bench tests/EventPipelineBench.cpp)
        target_compile_definitions(svms_v3_event_bench PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN)
        target_include_directories(svms_v3_event_bench PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")

        add_executable(svms_policy_lab tests/PolicyLab.cpp events/MidiStream.cpp)
        target_compile_definitions(svms_policy_lab PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
        target_include_directories(svms_policy_lab PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endif()
