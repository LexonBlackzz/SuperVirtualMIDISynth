cmake_minimum_required(VERSION 3.20)
project(SuperVirtualMIDISynthV3 VERSION 0.7.99 LANGUAGES C CXX)

include(CTest)

option(SVMS_XP_COMPAT "Build the 32-bit Windows XP DirectSound variant" OFF)

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
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.."
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
configure_file(SVMSBuildInfo.h.in
    "${SVMS_GENERATED_DIR}/SVMSBuildInfo.h" @ONLY)
include_directories("${SVMS_GENERATED_DIR}")

if(WIN32)
    math(EXPR SVMS_RESOURCE_BUILD_NUMBER "${SVMS_BUILD_NUMBER} % 65536")
    set(SVMS_RESOURCE_FILENAME "winmm.dll")
    set(SVMS_RESOURCE_DESCRIPTION
        "SuperVirtualMIDISynth V3 WinMM/KDMAPI Driver")
    set(SVMS_RESOURCE_FILETYPE "VFT_DLL")
    configure_file(SVMSVersion.rc.in
        "${SVMS_GENERATED_DIR}/SVMSDriverVersion.rc" @ONLY)

    set(SVMS_RESOURCE_FILENAME "SVMSConfigurator.exe")
    set(SVMS_RESOURCE_DESCRIPTION
        "SuperVirtualMIDISynth V3 Configurator")
    set(SVMS_RESOURCE_FILETYPE "VFT_APP")
    configure_file(SVMSVersion.rc.in
        "${SVMS_GENERATED_DIR}/SVMSConfiguratorVersion.rc" @ONLY)
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

if(NOT WIN32)
    find_package(Threads REQUIRED)
    find_package(ALSA REQUIRED)

    set(SVMS_PORTABLE_RENDER_SOURCES
        SVMSSoundFont.cpp
        SVMSRenderKernels.cpp
        SVMSRenderWorkersPosix.cpp
        SVMSRenderDispatch.cpp
        SVMSRenderKernelsSSE2.cpp
        SVMSRenderKernelsAVX2.cpp)

    set_source_files_properties(SVMSRenderKernelsSSE2.cpp PROPERTIES
        COMPILE_OPTIONS "-msse2;-mno-avx")
    set_source_files_properties(SVMSRenderKernelsAVX2.cpp PROPERTIES
        COMPILE_OPTIONS "-mavx2;-mno-fma")

    add_executable(svms_v3_render
        SVMSOfflineRenderer.cpp
        SVMSMidiStream.cpp
        ${SVMS_PORTABLE_RENDER_SOURCES})
    target_include_directories(svms_v3_render PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")
    target_compile_options(svms_v3_render PRIVATE
        -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(svms_v3_render PRIVATE Threads::Threads)

    add_executable(svmsd
        SVMSLinuxDaemon.cpp
        ${SVMS_PORTABLE_RENDER_SOURCES})
    target_include_directories(svmsd PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party"
        ${ALSA_INCLUDE_DIRS})
    target_compile_options(svmsd PRIVATE
        -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(svmsd PRIVATE Threads::Threads ${ALSA_LIBRARIES})

    add_library(omnimidi SHARED
        SVMSLinuxKDMAPI.cpp
        ${SVMS_PORTABLE_RENDER_SOURCES})
    target_include_directories(omnimidi PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party"
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
            tests/SVMSNativeHeaderCSmoke.c)
        target_include_directories(svms_v3_native_header_c PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        add_test(NAME svms_v3_native_header_c
            COMMAND svms_v3_native_header_c)

        add_executable(svms_v3_voice_growth_tests
            tests/SVMSVoiceGrowthRegression.cpp
            ${SVMS_PORTABLE_RENDER_SOURCES})
        target_include_directories(svms_v3_voice_growth_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(svms_v3_voice_growth_tests PRIVATE Threads::Threads)
        add_test(NAME svms_v3_voice_growth COMMAND svms_v3_voice_growth_tests)

        add_executable(svms_v3_limiter_tests tests/SVMSLimiterTests.cpp)
        target_include_directories(svms_v3_limiter_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME svms_v3_limiter COMMAND svms_v3_limiter_tests)

        add_executable(svms_v3_midi_stream_tests
            tests/SVMSMidiStreamTests.cpp
            SVMSMidiStream.cpp)
        target_include_directories(svms_v3_midi_stream_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(svms_v3_midi_stream_tests PRIVATE Threads::Threads)
        add_test(NAME svms_v3_midi_stream COMMAND svms_v3_midi_stream_tests)

        add_executable(svms_v3_linux_kdmapi_smoke
            tests/SVMSLinuxKDMAPISmoke.cpp)
        target_include_directories(svms_v3_linux_kdmapi_smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(svms_v3_linux_kdmapi_smoke PRIVATE
            ${CMAKE_DL_LIBS})
        add_test(NAME svms_v3_linux_kdmapi
            COMMAND svms_v3_linux_kdmapi_smoke $<TARGET_FILE:omnimidi>)

        add_executable(svms_v3_linux_native_api_smoke
            tests/SVMSLinuxNativeAPISmoke.cpp)
        target_include_directories(svms_v3_linux_native_api_smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(svms_v3_linux_native_api_smoke PRIVATE
            ${CMAKE_DL_LIBS})
        add_test(NAME svms_v3_linux_native_api
            COMMAND svms_v3_linux_native_api_smoke $<TARGET_FILE:omnimidi>)
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
        add_compile_options(/O2 /Oi /Gy)
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

set(SOURCES
    SVMSConfig.cpp
    SVMSSoundFont.cpp
    SVMSDriver.cpp
    SVMSDiagWindow.cpp
    SVMSRenderKernels.cpp
    SVMSRenderWorkers.cpp
    SVMSRenderDispatch.cpp
    SVMSRenderKernelsSSE2.cpp
)

if(NOT SVMS_XP_COMPAT)
    list(APPEND SOURCES SVMSRenderKernelsAVX2.cpp)
endif()

set(HEADERS
    SVMSTypes.h
    SVMSConfig.h
    SVMSVoiceManager.h
    SVMSEnvelope.h
    SVMSChannelCache.h
    SVMSRenderScalar.h
    SVMSRenderKernels.h
    SVMSRenderWorkers.h
    SVMSAudioOutput.h
    SVMSAudioOutputDirectSound.h
    SVMSSoundFont.h
    SVMSPageAllocator.h
    SVMSDiagWindow.h
    SVMSMPSCQueue.h
    SVMSEventCompile.h
    SVMSEventScheduler.h
    SVMSEventPages.h
    SVMSFrameClock.h
    SVMSPostFilter.h
    SVMSLimiter.h
    SVMSNativeOffline.h
    include/svmsapi.h
)

add_library(svmsynth SHARED ${SOURCES} ${HEADERS}
    "${SVMS_GENERATED_DIR}/SVMSDriverVersion.rc")

if(MSVC)
    set_source_files_properties(SVMSRenderKernelsSSE2.cpp PROPERTIES
        COMPILE_OPTIONS "$<$<EQUAL:${CMAKE_SIZEOF_VOID_P},4>:/arch:SSE2>")
    if(NOT SVMS_XP_COMPAT)
        set_source_files_properties(SVMSRenderKernelsAVX2.cpp PROPERTIES
            COMPILE_OPTIONS "/arch:AVX2")
    endif()
elseif(MINGW)
    set_source_files_properties(SVMSRenderKernelsSSE2.cpp PROPERTIES
        COMPILE_OPTIONS "-msse2;-mno-avx")
    if(NOT SVMS_XP_COMPAT)
        set_source_files_properties(SVMSRenderKernelsAVX2.cpp PROPERTIES
            COMPILE_OPTIONS "-mavx2;-mno-fma")
    endif()
endif()

target_compile_definitions(svmsynth PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
    UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)

if(SVMS_XP_COMPAT)
    target_link_libraries(svmsynth PRIVATE dsound dxguid shell32 user32)
else()
    target_link_libraries(svmsynth PRIVATE winmm ole32 uuid avrt shell32)
endif()

# nlohmann/json is vendored under third_party and intentionally confined to
# the configuration translation unit.
target_include_directories(svmsynth PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")

set(EXPORTS
        PlaySoundA
        PlaySoundW
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

set_target_properties(svmsynth PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(NOT SVMS_XP_COMPAT)
    add_executable(svms_v3_render
        SVMSOfflineRenderer.cpp
        SVMSConfig.cpp
        SVMSMidiStream.cpp
        SVMSSoundFont.cpp
        SVMSRenderKernels.cpp
        SVMSRenderWorkers.cpp
        SVMSRenderDispatch.cpp
        SVMSRenderKernelsSSE2.cpp
        SVMSRenderKernelsAVX2.cpp)
    target_compile_definitions(svms_v3_render PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_include_directories(svms_v3_render PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")
    target_link_libraries(svms_v3_render PRIVATE shell32)
    set_target_properties(svms_v3_render PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/imgui")
    set(IMGUI_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )
    set(CONFIGURATOR_SOURCES
        Configurator/Main.cpp
        Configurator/ConfiguratorApp.cpp
        Configurator/ConfigDocument.cpp
        Configurator/Theme.cpp
        Configurator/Widgets.cpp
        Configurator/WasapiDevices.cpp
        Configurator/EasterEggs.cpp
        Configurator/UpdateService.cpp
        Configurator/PageOverview.cpp
        Configurator/PageAudio.cpp
        Configurator/PagePerformance.cpp
        Configurator/PageMidi.cpp
        Configurator/PageOffline.cpp
        Configurator/PageReverb.cpp
        Configurator/PageLimiter.cpp
        Configurator/PageDiagnostics.cpp
        Configurator/PageAdvanced.cpp
        Configurator/PageAbout.cpp
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
        "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party"
    )
    target_compile_definitions(svms_v3_configurator PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(svms_v3_configurator PRIVATE
        d3d11 dxgi user32 gdi32 shell32 ole32 uuid comdlg32 winhttp)
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

# Native integrations load the canonical SVMSAPI.dll name. Keep the shorter
# SVMS.dll spelling as a compatibility alias. Both are intentionally the same
# runtime as winmm.dll so WinMM, KDMAPI, and SVMS API clients cannot drift into
# separate engine implementations.
add_custom_target(svms_native_alias ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:svmsynth>"
            "$<TARGET_FILE_DIR:svmsynth>/SVMSAPI.dll"
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

option(SVMS_BUILD_TESTS "Build deterministic V3 correctness tests" ON)

if(SVMS_BUILD_TESTS)
    add_executable(svms_v3_native_header_c
        tests/SVMSNativeHeaderCSmoke.c)
    target_include_directories(svms_v3_native_header_c PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_test(NAME svms_v3_native_header_c
        COMMAND svms_v3_native_header_c)

    add_executable(svms_v3_telemetry_abi_smoke
        tests/SVMSTelemetryABISmoke.cpp)
    target_compile_definitions(svms_v3_telemetry_abi_smoke PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_dependencies(svms_v3_telemetry_abi_smoke svmsynth)
    add_test(NAME svms_v3_telemetry_abi_smoke
             COMMAND svms_v3_telemetry_abi_smoke "$<TARGET_FILE:svmsynth>")

    add_executable(svms_v3_runtime_link_abi_smoke
        tests/SVMSRuntimeLinkABISmoke.cpp)
    target_compile_definitions(svms_v3_runtime_link_abi_smoke PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_include_directories(svms_v3_runtime_link_abi_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME svms_v3_runtime_link_abi_smoke
             COMMAND svms_v3_runtime_link_abi_smoke)

    add_executable(svms_v3_native_api_smoke
        tests/SVMSNativeAPISmoke.cpp)
    target_include_directories(svms_v3_native_api_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_dependencies(svms_v3_native_api_smoke svms_native_alias)
    add_test(NAME svms_v3_native_api
             COMMAND svms_v3_native_api_smoke
                     "$<TARGET_FILE_DIR:svmsynth>/SVMSAPI.dll")
    set_tests_properties(svms_v3_native_api PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(svms_v3_tests
        tests/SVMSV3Tests.cpp
        SVMSConfig.cpp
        SVMSSoundFont.cpp
        SVMSRenderKernels.cpp
        SVMSRenderWorkers.cpp
        SVMSRenderDispatch.cpp
        SVMSRenderKernelsSSE2.cpp
    )
    if(NOT SVMS_XP_COMPAT)
        target_sources(svms_v3_tests PRIVATE SVMSRenderKernelsAVX2.cpp)
    endif()
    target_compile_definitions(svms_v3_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS SVMS_ENABLE_REFERENCE_RENDERER=1)
    target_include_directories(svms_v3_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(svms_v3_tests PRIVATE user32)
    target_link_libraries(svms_v3_tests PRIVATE shell32)
    target_include_directories(svms_v3_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")
    add_test(NAME svms_v3_correctness COMMAND svms_v3_tests)

    add_executable(svms_v3_voice_growth_tests
        tests/SVMSVoiceGrowthRegression.cpp
        SVMSRenderKernels.cpp
        SVMSRenderWorkers.cpp
        SVMSRenderDispatch.cpp
        SVMSRenderKernelsSSE2.cpp)
    if(NOT SVMS_XP_COMPAT)
        target_sources(svms_v3_voice_growth_tests PRIVATE SVMSRenderKernelsAVX2.cpp)
    endif()
    target_compile_definitions(svms_v3_voice_growth_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_include_directories(svms_v3_voice_growth_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME svms_v3_voice_growth COMMAND svms_v3_voice_growth_tests)

    add_executable(svms_v3_limiter_tests
        tests/SVMSLimiterTests.cpp)
    target_compile_definitions(svms_v3_limiter_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_include_directories(svms_v3_limiter_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME svms_v3_limiter COMMAND svms_v3_limiter_tests)

    if(SVMS_XP_COMPAT)
        add_executable(svms_v3_xp_directsound_smoke
            tests/SVMSXPDirectSoundSmoke.cpp)
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
            tests/SVMSXPDllSmoke.cpp)
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
    else()
        add_executable(svms_v3_midi_stream_tests
            tests/SVMSMidiStreamTests.cpp SVMSMidiStream.cpp)
        target_compile_definitions(svms_v3_midi_stream_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
        target_include_directories(svms_v3_midi_stream_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME svms_v3_midi_stream COMMAND svms_v3_midi_stream_tests)

        add_executable(svms_v3_live_smoke tests/SVMSLiveSmoke.cpp)
        target_compile_definitions(svms_v3_live_smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_include_directories(svms_v3_live_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(svms_v3_live_smoke PRIVATE ole32 uuid)

        add_executable(svms_v3_runtime_link_smoke
            tests/SVMSRuntimeLinkSmoke.cpp)
        target_compile_definitions(svms_v3_runtime_link_smoke PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        target_include_directories(svms_v3_runtime_link_smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        add_dependencies(svms_v3_runtime_link_smoke svmsynth)
        add_test(NAME svms_v3_runtime_link_smoke
                 COMMAND svms_v3_runtime_link_smoke "$<TARGET_FILE:svmsynth>")
        set_tests_properties(svms_v3_runtime_link_smoke PROPERTIES
            SKIP_RETURN_CODE 77)

        add_executable(svms_v3_runtime_link_probe
            tests/SVMSRuntimeLinkProbe.cpp)
        target_compile_definitions(svms_v3_runtime_link_probe PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        target_include_directories(svms_v3_runtime_link_probe PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")

        add_executable(svms_v3_two_process_smoke
            tests/SVMSTwoProcessSmoke.cpp)
        target_compile_definitions(svms_v3_two_process_smoke PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        target_include_directories(svms_v3_two_process_smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
        add_dependencies(svms_v3_two_process_smoke svmsynth)
        add_test(NAME svms_v3_two_process_smoke
                 COMMAND svms_v3_two_process_smoke
                         "$<TARGET_FILE:svms_v3_runtime_link_probe>"
                         "$<TARGET_FILE:svmsynth>")
        set_tests_properties(svms_v3_two_process_smoke PROPERTIES
            SKIP_RETURN_CODE 77)

        add_executable(svms_v3_bench tests/SVMSV3Bench.cpp SVMSSoundFont.cpp SVMSRenderKernels.cpp SVMSRenderWorkers.cpp
            SVMSRenderDispatch.cpp SVMSRenderKernelsSSE2.cpp SVMSRenderKernelsAVX2.cpp)
        target_compile_definitions(svms_v3_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
            SVMS_ENABLE_REFERENCE_RENDERER=1)
        target_include_directories(svms_v3_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(svms_v3_bench PRIVATE avrt)

        add_executable(svms_v3_event_bench tests/SVMSEventPipelineBench.cpp)
        target_compile_definitions(svms_v3_event_bench PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN)
        target_include_directories(svms_v3_event_bench PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endif()
