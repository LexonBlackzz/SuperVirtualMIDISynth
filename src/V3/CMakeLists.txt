cmake_minimum_required(VERSION 3.20)
project(SuperVirtualMIDISynthV3 LANGUAGES CXX)

include(CTest)

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

set(SOURCES
    SVMSConfig.cpp
    SVMSSoundFont.cpp
    SVMSDriver.cpp
    SVMSDiagWindow.cpp
    SVMSRenderKernels.cpp
)

set(HEADERS
    SVMSTypes.h
    SVMSConfig.h
    SVMSVoiceManager.h
    SVMSEnvelope.h
    SVMSChannelCache.h
    SVMSRenderScalar.h
    SVMSRenderKernels.h
    SVMSAudioOutput.h
    SVMSSoundFont.h
    SVMSPageAllocator.h
    SVMSDiagWindow.h
    SVMSMPSCQueue.h
    SVMSFrameClock.h
)

add_library(svmsynth SHARED ${SOURCES} ${HEADERS})

target_compile_definitions(svmsynth PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

target_link_libraries(svmsynth PRIVATE
    winmm
    ole32
    uuid
    avrt
    shell32
)

# nlohmann/json is vendored under third_party and intentionally confined to
# the configuration translation unit.
target_include_directories(svmsynth PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")

if(MSVC)
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
    foreach(exp ${EXPORTS})
        target_link_options(svmsynth PRIVATE /EXPORT:${exp})
    endforeach()
endif()

set_target_properties(svmsynth PROPERTIES
    OUTPUT_NAME "winmm"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

option(SVMS_BUILD_TESTS "Build deterministic V3 correctness tests" ON)

if(SVMS_BUILD_TESTS)
    add_executable(svms_v3_tests
        tests/SVMSV3Tests.cpp
        SVMSConfig.cpp
        SVMSSoundFont.cpp
        SVMSRenderKernels.cpp
    )
    target_compile_definitions(svms_v3_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS SVMS_ENABLE_REFERENCE_RENDERER=1)
    target_include_directories(svms_v3_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(svms_v3_tests PRIVATE user32)
    target_link_libraries(svms_v3_tests PRIVATE shell32)
    target_include_directories(svms_v3_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")
    add_test(NAME svms_v3_correctness COMMAND svms_v3_tests)

    add_executable(svms_v3_live_smoke tests/SVMSLiveSmoke.cpp)
    target_compile_definitions(svms_v3_live_smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(svms_v3_live_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(svms_v3_live_smoke PRIVATE ole32 uuid)

    add_executable(svms_v3_bench tests/SVMSV3Bench.cpp SVMSRenderKernels.cpp)
    target_compile_definitions(svms_v3_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
        SVMS_ENABLE_REFERENCE_RENDERER=1)
    target_include_directories(svms_v3_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(svms_v3_bench PRIVATE avrt)
endif()
