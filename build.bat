@echo off
setlocal
rem Usage: build.bat [x64|x86|xp|xp-mingw|player|clean]   (default: x64)
rem   x64        MSVC x64 Release            -> build\V3\bin\winmm.dll
rem   x86        MSVC x86 Release            -> build\V3-x86\bin\winmm.dll
rem   xp         MSVC x86, XP-compat + tests -> build\V3_XP
rem   xp-mingw   MinGW x86, XP-compat        -> build\V3-XP-x86 (set W64DEVKIT_X86, default .\w64devkit-x86)
rem   player     reference MIDI player       -> build\player-local\svms_player.exe
rem   clean      delete build\V3
rem Cap build parallelism with: set CMAKE_BUILD_PARALLEL_LEVEL=2

set "ROOT=%~dp0"
set "V3_SRC=%ROOT%src"
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=x64"

if /I "%TARGET%"=="clean" (
    if exist "%ROOT%build\V3" rmdir /s /q "%ROOT%build\V3"
    echo Cleaned build\V3.
    exit /b 0
)

if /I "%TARGET%"=="xp-mingw" goto :xp_mingw

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere not found at "%VSWHERE%"
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo Visual Studio with C++ tools was not found.
    exit /b 1
)
set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    echo vcvarsall.bat not found in "%VS_PATH%"
    exit /b 1
)

set "ARCH=x64"
if /I "%TARGET%"=="x86" set "ARCH=x86"
if /I "%TARGET%"=="xp" set "ARCH=x86"
call "%VCVARS%" %ARCH% >nul
if errorlevel 1 (
    echo Failed to initialize the MSVC %ARCH% environment.
    exit /b 1
)

if /I "%TARGET%"=="player" goto :player
if /I "%TARGET%"=="x64" ( set "BUILD_DIR=%ROOT%build\V3" & set "OPTS=" )
if /I "%TARGET%"=="x86" ( set "BUILD_DIR=%ROOT%build\V3-x86" & set "OPTS=-DSVMS_XP_COMPAT=OFF" )
if /I "%TARGET%"=="xp"  ( set "BUILD_DIR=%ROOT%build\V3_XP" & set "OPTS=-DSVMS_XP_COMPAT=ON" )
if not defined BUILD_DIR (
    echo Unknown target "%TARGET%". See the usage line at the top of build.bat.
    exit /b 1
)

cmake -G Ninja -DCMAKE_BUILD_TYPE=Release %OPTS% -S "%V3_SRC%" -B "%BUILD_DIR%"
if errorlevel 1 exit /b 1
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 exit /b 1

if /I "%TARGET%"=="xp" (
    ctest --test-dir "%BUILD_DIR%" --output-on-failure --timeout 180
    exit /b %errorlevel%
)
if exist "%BUILD_DIR%\bin\svms_v3_configurator.exe" (
    move /y "%BUILD_DIR%\bin\svms_v3_configurator.exe" "%BUILD_DIR%\bin\SVMSConfigurator.exe" >nul
)
echo.
echo Build succeeded: %BUILD_DIR%\bin\winmm.dll
echo Copy this DLL next to your MIDI application to use SuperVirtualMIDISynth V3.
exit /b 0

:xp_mingw
if not defined W64DEVKIT_X86 set "W64DEVKIT_X86=%ROOT%w64devkit-x86"
if not exist "%W64DEVKIT_X86%\bin\g++.exe" (
    echo XP compiler not found: %W64DEVKIT_X86%\bin\g++.exe
    exit /b 1
)
set "PATH=%W64DEVKIT_X86%\bin;%PATH%"
set "BUILD_DIR=%ROOT%build\V3-XP-x86"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="%W64DEVKIT_X86%\bin\g++.exe" -DSVMS_XP_COMPAT=ON -DSVMS_BUILD_TESTS=ON -S "%V3_SRC%" -B "%BUILD_DIR%"
if errorlevel 1 exit /b 1
cmake --build "%BUILD_DIR%" --config Release
exit /b %errorlevel%

:player
cd /d "%ROOT%"
if not exist build\player-local mkdir build\player-local
cl /nologo /std:c++17 /EHsc /O2 /W3 /Zi /D_CRT_SECURE_NO_WARNINGS /I src /I third_party\imgui /Fe:build\player-local\svms_player.exe /Fo:build\player-local\ /Fd:build\player-local\ src\player\SVMSPlayer.cpp src\events\SVMSMidiStream.cpp third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\backends\imgui_impl_win32.cpp third_party\imgui\backends\imgui_impl_dx11.cpp /link dbghelp.lib user32.lib winmm.lib version.lib d3d11.lib dxgi.lib comdlg32.lib
exit /b %errorlevel%
