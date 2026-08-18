cmake_minimum_required(VERSION 3.20)
project(SuperVirtualMIDISynthV3 LANGUAGES CXX)

include(CTest)

option(SVMS_XP_COMPAT "Build the 32-bit Windows XP DirectSound variant" OFF)
if(SVMS_XP_COMPAT AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "SVMS_XP_COMPAT is a 32-bit-only target")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    SVMSFrameClock.h
    SVMSPostFilter.h
)

add_library(svmsynth SHARED ${SOURCES} ${HEADERS})

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
        SVMSMidiStream.cpp
        SVMSSoundFont.cpp
        SVMSRenderKernels.cpp
        SVMSRenderWorkers.cpp
        SVMSRenderDispatch.cpp
        SVMSRenderKernelsSSE2.cpp
        SVMSRenderKernelsAVX2.cpp)
    target_compile_definitions(svms_v3_render PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_include_directories(svms_v3_render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
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
        Configurator/PageOverview.cpp
        Configurator/PageAudio.cpp
        Configurator/PagePerformance.cpp
        Configurator/PageMidi.cpp
        Configurator/PageReverb.cpp
        Configurator/PageLimiter.cpp
        Configurator/PageDiagnostics.cpp
        Configurator/PageAdvanced.cpp
        Configurator/PageAbout.cpp
    )
    add_executable(svms_v3_configurator WIN32
        ${IMGUI_SOURCES}
        ${CONFIGURATOR_SOURCES}
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
        d3d11 dxgi user32 gdi32 shell32 ole32 uuid comdlg32)
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

option(SVMS_BUILD_TESTS "Build deterministic V3 correctness tests" ON)

if(SVMS_BUILD_TESTS)
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
