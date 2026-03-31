@echo off
setlocal

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

call "%~dp0find_vs.bat"
if errorlevel 1 exit /b %errorlevel%

if /I "%TARGET%"=="x86" goto build_x86
if /I "%TARGET%"=="win32" goto build_x86
if /I "%TARGET%"=="x64" goto build_x64
if /I "%TARGET%"=="amd64" goto build_x64
if /I "%TARGET%"=="configurator" goto build_configurator
if /I "%TARGET%"=="configurator_v2" goto build_configurator_v2
if /I "%TARGET%"=="all" goto build_all
if /I "%TARGET%"=="xp" goto build_xp_notice

echo Unknown target "%TARGET%". Use x86, x64, configurator, configurator_v2, all, or xp.
exit /b 1

:build_all
call "%~f0" x86
if errorlevel 1 exit /b %errorlevel%
call "%~f0" x64
if errorlevel 1 exit /b %errorlevel%
call "%~f0" configurator
if errorlevel 1 exit /b %errorlevel%
call "%~f0" configurator_v2
exit /b %errorlevel%

:build_xp_notice
echo XP build is intentionally not wired in this clean repo yet.
echo Use the legacy MinGW lane as a later follow-up once the main MSVC path is stable.
exit /b 0

:build_x86
call "%VS_VCVARSALL%" x86 -vcvars_ver=14.1
if errorlevel 1 exit /b %errorlevel%
call "%~dp0write_exports.bat" x86 "%~dp0build\exports_x86.def"
if errorlevel 1 exit /b %errorlevel%
if not exist "%~dp0build\obj\x86" mkdir "%~dp0build\obj\x86"
cl.exe /nologo /O2 /EHsc /LD /MT /DNDEBUG /D_USRDLL /D_WINDLL /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
  /Fe:"%~dp0SuperVirtualMIDISynth.dll" ^
  /Fo:"%~dp0build\obj\x86\\" ^
  /Fd:"%~dp0build\obj\x86\SuperVirtualMIDISynth.pdb" ^
  src\dllmain.cpp src\Config.cpp src\Synth.cpp src\TsfEngine.cpp src\BassMidiEngine.cpp src\SfzEngine.cpp src\WavLoader.cpp src\AudioOutput.cpp src\SystemWinmm.cpp src\LiveRuntime.cpp ^
  src\VirtuallySuper\VirtuallySuperEngine.cpp src\VirtuallySuper\VirtuallySuperScheduler.cpp src\VirtuallySuper\VirtuallySuperScene.cpp src\VirtuallySuper\VirtuallySuperExact.cpp src\VirtuallySuper\VirtuallySuperGrouped.cpp src\VirtuallySuper\VirtuallySuperDensity.cpp src\VirtuallySuper\VirtuallySuperOverload.cpp src\VirtuallySuper\VirtuallySuperRender.cpp src\VirtuallySuper\VirtuallySuperTelemetry.cpp src\VirtuallySuper\VirtuallySuperSamplerEngine.cpp src\VirtuallySuper\VirtuallySuperSoundFontParser.cpp src\VirtuallySuper\VirtuallySuperSoundFontDispatch.cpp src\VirtuallySuper\VirtuallySuperSoundFontRuntime.cpp ^
  user32.lib winmm.lib advapi32.lib shell32.lib dsound.lib dxguid.lib ole32.lib uuid.lib avrt.lib ^
  /link /DEF:"%~dp0build\exports_x86.def" /IMPLIB:"%~dp0build\SuperVirtualMIDISynth.x86.lib"
if errorlevel 1 (
  echo x86 build failed!
  exit /b %errorlevel%
)
echo x86 build succeeded: SuperVirtualMIDISynth.dll
exit /b 0

:build_x64
call "%VS_VCVARSALL%" x64 -vcvars_ver=14.1
if errorlevel 1 exit /b %errorlevel%
call "%~dp0write_exports.bat" x64 "%~dp0build\exports_x64.def"
if errorlevel 1 exit /b %errorlevel%
if not exist "%~dp0build\obj\x64" mkdir "%~dp0build\obj\x64"
cl.exe /nologo /O2 /EHsc /LD /MT /DNDEBUG /D_USRDLL /D_WINDLL /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
  /Fe:"%~dp0SuperVirtualMIDISynthx64.dll" ^
  /Fo:"%~dp0build\obj\x64\\" ^
  /Fd:"%~dp0build\obj\x64\SuperVirtualMIDISynthx64.pdb" ^
  src\dllmain.cpp src\Config.cpp src\Synth.cpp src\TsfEngine.cpp src\BassMidiEngine.cpp src\SfzEngine.cpp src\WavLoader.cpp src\AudioOutput.cpp src\SystemWinmm.cpp src\LiveRuntime.cpp ^
  src\VirtuallySuper\VirtuallySuperEngine.cpp src\VirtuallySuper\VirtuallySuperScheduler.cpp src\VirtuallySuper\VirtuallySuperScene.cpp src\VirtuallySuper\VirtuallySuperExact.cpp src\VirtuallySuper\VirtuallySuperGrouped.cpp src\VirtuallySuper\VirtuallySuperDensity.cpp src\VirtuallySuper\VirtuallySuperOverload.cpp src\VirtuallySuper\VirtuallySuperRender.cpp src\VirtuallySuper\VirtuallySuperTelemetry.cpp src\VirtuallySuper\VirtuallySuperSamplerEngine.cpp src\VirtuallySuper\VirtuallySuperSoundFontParser.cpp src\VirtuallySuper\VirtuallySuperSoundFontDispatch.cpp src\VirtuallySuper\VirtuallySuperSoundFontRuntime.cpp ^
  user32.lib winmm.lib advapi32.lib shell32.lib dsound.lib dxguid.lib ole32.lib uuid.lib avrt.lib ^
  /link /DEF:"%~dp0build\exports_x64.def" /IMPLIB:"%~dp0build\SuperVirtualMIDISynth.x64.lib"
if errorlevel 1 (
  echo x64 build failed!
  exit /b %errorlevel%
)
echo x64 build succeeded: SuperVirtualMIDISynthx64.dll
exit /b 0

:build_configurator
call "%VS_VCVARSALL%" x86 -vcvars_ver=14.1
if errorlevel 1 exit /b %errorlevel%
if not exist "%~dp0build\obj\configurator" mkdir "%~dp0build\obj\configurator"
cl.exe /nologo /O2 /EHsc /MT /DNDEBUG /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
  /Fe:"%~dp0SVMSConfigurator.exe" ^
  /Fo:"%~dp0build\obj\configurator\\" ^
  /Fd:"%~dp0build\obj\configurator\SVMSConfigurator.pdb" ^
  src\Configurator.cpp ^
  user32.lib gdi32.lib comdlg32.lib shell32.lib
if errorlevel 1 (
  echo Configurator build failed!
  exit /b %errorlevel%
)
echo Configurator build succeeded: SVMSConfigurator.exe
exit /b 0

:build_configurator_v2
call "%VS_VCVARSALL%" x86 -vcvars_ver=14.1
if errorlevel 1 exit /b %errorlevel%
if not exist "%~dp0build\obj\configurator_v2" mkdir "%~dp0build\obj\configurator_v2"
cl.exe /nologo /O2 /EHsc /MT /DNDEBUG /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
  /Fe:"%~dp0SVMSConfiguratorV2.exe" ^
  /Fo:"%~dp0build\obj\configurator_v2\\" ^
  /Fd:"%~dp0build\obj\configurator_v2\SVMSConfiguratorV2.pdb" ^
  src\ConfiguratorV2.cpp ^
  user32.lib gdi32.lib comdlg32.lib shell32.lib comctl32.lib
if errorlevel 1 (
  echo Configurator V2 build failed!
  exit /b %errorlevel%
)
echo Configurator V2 build succeeded: SVMSConfiguratorV2.exe
exit /b 0
