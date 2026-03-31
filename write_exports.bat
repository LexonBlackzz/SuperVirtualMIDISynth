@echo off
setlocal

set "ARCH=%~1"
set "OUT_FILE=%~2"

if "%ARCH%"=="" (
    echo Missing architecture. Use x86 or x64.
    exit /b 1
)

if "%OUT_FILE%"=="" (
    echo Missing output file path.
    exit /b 1
)

if not exist "%~dp2" mkdir "%~dp2" >nul 2>&1

if /I "%ARCH%"=="x86" goto write_x86
if /I "%ARCH%"=="x64" goto write_x64
if /I "%ARCH%"=="amd64" goto write_x64

echo Unsupported architecture "%ARCH%".
exit /b 1

:write_x64
(
echo LIBRARY SuperVirtualMIDISynthx64
echo EXPORTS
echo     IsKDMAPIAvailable
echo     InitializeKDMAPIStream
echo     TerminateKDMAPIStream
echo     ResetKDMAPIStream
echo     ReturnKDMAPIVer
echo     SendDirectData
echo     SendDirectDataNoBuf
echo     SendCustomEvent
echo     SendDirectLongData
echo     SendDirectLongDataNoBuf
echo     PrepareLongData
echo     UnprepareLongData
echo     DriverSettings
echo     GetDriverDebugInfo
echo     LoadCustomSoundFontsList
echo     timeGetTime64
echo.
echo     midiOutGetNumDevs
echo     midiOutGetDevCapsA
echo     midiOutGetDevCapsW
echo     midiOutOpen
echo     midiOutClose
echo     midiOutPrepareHeader
echo     midiOutUnprepareHeader
echo     midiOutShortMsg
echo     midiOutLongMsg
echo     midiOutReset
echo     midiOutSetVolume
echo     midiOutGetVolume
echo     midiOutGetErrorTextA
echo     midiOutGetErrorTextW
echo.
echo     midiInGetNumDevs
echo     midiInGetDevCapsA
echo     midiInGetDevCapsW
echo     midiInOpen
echo     midiInClose
echo     midiInPrepareHeader
echo     midiInUnprepareHeader
echo     midiInAddBuffer
echo     midiInStart
echo     midiInStop
echo     midiInReset
echo     waveInGetNumDevs
echo     waveInGetDevCapsA
echo     waveInGetDevCapsW
echo     waveInOpen
echo     waveInClose
echo     waveInPrepareHeader
echo     waveInUnprepareHeader
echo     waveInAddBuffer
echo     waveInStart
echo     waveInStop
echo     waveInReset
echo     waveInMessage
echo     waveInGetPosition
echo.
echo     mixerGetNumDevs
echo     mixerGetDevCapsA
echo     mixerGetDevCapsW
echo     mixerOpen
echo     mixerClose
echo     mixerMessage
echo     mixerGetLineInfoA
echo     mixerGetLineInfoW
echo     mixerGetID
echo     mixerGetLineControlsA
echo     mixerGetLineControlsW
echo     mixerGetControlDetailsA
echo     mixerGetControlDetailsW
echo     mixerSetControlDetails
echo.
echo     waveOutGetNumDevs
echo     waveOutGetDevCapsA
echo     waveOutGetDevCapsW
echo     waveOutOpen
echo     waveOutClose
echo     waveOutPrepareHeader
echo     waveOutUnprepareHeader
echo     waveOutWrite
echo     waveOutReset
echo     waveOutRestart
echo     waveOutPause
echo     waveOutBreakLoop
echo     waveOutGetPosition
echo     waveOutGetVolume
echo     waveOutSetVolume
echo     waveOutGetPitch
echo     waveOutSetPitch
echo     waveOutGetPlaybackRate
echo     waveOutSetPlaybackRate
echo     waveOutGetID
echo     waveOutGetErrorTextA
echo     waveOutGetErrorTextW
echo     waveOutMessage
echo.
echo     timeGetTime
echo     timeBeginPeriod
echo     timeEndPeriod
echo     timeGetDevCaps
echo     timeSetEvent
echo     timeKillEvent
) > "%OUT_FILE%"
exit /b 0

:write_x86
(
echo LIBRARY SuperVirtualMIDISynth
echo EXPORTS
echo     IsKDMAPIAvailable=_IsKDMAPIAvailable@0
echo     InitializeKDMAPIStream=_InitializeKDMAPIStream@0
echo     TerminateKDMAPIStream=_TerminateKDMAPIStream@0
echo     ResetKDMAPIStream=_ResetKDMAPIStream@0
echo     ReturnKDMAPIVer=_ReturnKDMAPIVer@16
echo     SendDirectData=_SendDirectData@4
echo     SendDirectDataNoBuf=_SendDirectDataNoBuf@4
echo     SendCustomEvent=_SendCustomEvent@12
echo     SendDirectLongData=_SendDirectLongData@8
echo     SendDirectLongDataNoBuf=_SendDirectLongDataNoBuf@8
echo     PrepareLongData=_PrepareLongData@8
echo     UnprepareLongData=_UnprepareLongData@8
echo     DriverSettings=_DriverSettings@16
echo     GetDriverDebugInfo=_GetDriverDebugInfo@0
echo     LoadCustomSoundFontsList=_LoadCustomSoundFontsList@4
echo     timeGetTime64=_timeGetTime64@0
echo.
echo     midiOutGetNumDevs=_midiOutGetNumDevs@0
echo     midiOutGetDevCapsA=_midiOutGetDevCapsA@12
echo     midiOutGetDevCapsW=_midiOutGetDevCapsW@12
echo     midiOutOpen=_midiOutOpen@20
echo     midiOutClose=_midiOutClose@4
echo     midiOutPrepareHeader=_midiOutPrepareHeader@12
echo     midiOutUnprepareHeader=_midiOutUnprepareHeader@12
echo     midiOutShortMsg=_midiOutShortMsg@8
echo     midiOutLongMsg=_midiOutLongMsg@12
echo     midiOutReset=_midiOutReset@4
echo     midiOutSetVolume=_midiOutSetVolume@8
echo     midiOutGetVolume=_midiOutGetVolume@8
echo     midiOutGetErrorTextA=_midiOutGetErrorTextA@12
echo     midiOutGetErrorTextW=_midiOutGetErrorTextW@12
echo.
echo     midiInGetNumDevs=_midiInGetNumDevs@0
echo     midiInGetDevCapsA=_midiInGetDevCapsA@12
echo     midiInGetDevCapsW=_midiInGetDevCapsW@12
echo     midiInOpen=_midiInOpen@20
echo     midiInClose=_midiInClose@4
echo     midiInPrepareHeader=_midiInPrepareHeader@12
echo     midiInUnprepareHeader=_midiInUnprepareHeader@12
echo     midiInAddBuffer=_midiInAddBuffer@12
echo     midiInStart=_midiInStart@4
echo     midiInStop=_midiInStop@4
echo     midiInReset=_midiInReset@4
echo     waveInGetNumDevs=_waveInGetNumDevs@0
echo     waveInGetDevCapsA=_waveInGetDevCapsA@12
echo     waveInGetDevCapsW=_waveInGetDevCapsW@12
echo     waveInOpen=_waveInOpen@24
echo     waveInClose=_waveInClose@4
echo     waveInPrepareHeader=_waveInPrepareHeader@12
echo     waveInUnprepareHeader=_waveInUnprepareHeader@12
echo     waveInAddBuffer=_waveInAddBuffer@12
echo     waveInStart=_waveInStart@4
echo     waveInStop=_waveInStop@4
echo     waveInReset=_waveInReset@4
echo     waveInMessage=_waveInMessage@16
echo     waveInGetPosition=_waveInGetPosition@12
echo.
echo     mixerGetNumDevs=_mixerGetNumDevs@0
echo     mixerGetDevCapsA=_mixerGetDevCapsA@12
echo     mixerGetDevCapsW=_mixerGetDevCapsW@12
echo     mixerOpen=_mixerOpen@20
echo     mixerClose=_mixerClose@4
echo     mixerMessage=_mixerMessage@16
echo     mixerGetLineInfoA=_mixerGetLineInfoA@12
echo     mixerGetLineInfoW=_mixerGetLineInfoW@12
echo     mixerGetID=_mixerGetID@12
echo     mixerGetLineControlsA=_mixerGetLineControlsA@12
echo     mixerGetLineControlsW=_mixerGetLineControlsW@12
echo     mixerGetControlDetailsA=_mixerGetControlDetailsA@12
echo     mixerGetControlDetailsW=_mixerGetControlDetailsW@12
echo     mixerSetControlDetails=_mixerSetControlDetails@12
echo.
echo     waveOutGetNumDevs=_waveOutGetNumDevs@0
echo     waveOutGetDevCapsA=_waveOutGetDevCapsA@12
echo     waveOutGetDevCapsW=_waveOutGetDevCapsW@12
echo     waveOutOpen=_waveOutOpen@24
echo     waveOutClose=_waveOutClose@4
echo     waveOutPrepareHeader=_waveOutPrepareHeader@12
echo     waveOutUnprepareHeader=_waveOutUnprepareHeader@12
echo     waveOutWrite=_waveOutWrite@12
echo     waveOutReset=_waveOutReset@4
echo     waveOutRestart=_waveOutRestart@4
echo     waveOutPause=_waveOutPause@4
echo     waveOutBreakLoop=_waveOutBreakLoop@4
echo     waveOutGetPosition=_waveOutGetPosition@12
echo     waveOutGetVolume=_waveOutGetVolume@8
echo     waveOutSetVolume=_waveOutSetVolume@8
echo     waveOutGetPitch=_waveOutGetPitch@8
echo     waveOutSetPitch=_waveOutSetPitch@8
echo     waveOutGetPlaybackRate=_waveOutGetPlaybackRate@8
echo     waveOutSetPlaybackRate=_waveOutSetPlaybackRate@8
echo     waveOutGetID=_waveOutGetID@8
echo     waveOutGetErrorTextA=_waveOutGetErrorTextA@12
echo     waveOutGetErrorTextW=_waveOutGetErrorTextW@12
echo     waveOutMessage=_waveOutMessage@16
echo.
echo     timeGetTime=_timeGetTime@0
echo     timeBeginPeriod=_timeBeginPeriod@4
echo     timeEndPeriod=_timeEndPeriod@4
echo     timeGetDevCaps=_timeGetDevCaps@8
echo     timeSetEvent=_timeSetEvent@20
echo     timeKillEvent=_timeKillEvent@4
) > "%OUT_FILE%"

exit /b 0
