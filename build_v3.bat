@echo off
setlocal

set "ROOT=%~dp0"
set "V3_SRC=%ROOT%src\V3"
set "BUILD_DIR=%ROOT%build\V3"

if /I "%~1"=="clean" (
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo Cleaned build directory.
    exit /b 0
)

call "%ROOT%find_vs.bat"
if errorlevel 1 exit /b %errorlevel%

call "%VS_VCVARSALL%" x64
if errorlevel 1 (
    echo Failed to initialize MSVC x64 environment.
    exit /b 1
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -S "%V3_SRC%" -B "%BUILD_DIR%"
if errorlevel 1 (
    echo CMake configuration failed.
    exit /b 1
)

cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

if exist "%BUILD_DIR%\bin\svms_v3_configurator.exe" (
    move /y "%BUILD_DIR%\bin\svms_v3_configurator.exe" "%BUILD_DIR%\bin\SVMSConfigurator.exe" >nul
)

echo.
echo Build succeeded: %BUILD_DIR%\bin\winmm.dll
echo Configurator: %BUILD_DIR%\bin\SVMSConfigurator.exe
echo.
echo Copy this DLL next to your MIDI application to use SuperVirtualMIDISynth V3.
exit /b 0
