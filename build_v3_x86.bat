@echo off
setlocal

set "ROOT=%~dp0"
set "V3_SRC=%ROOT%src\V3"
set "BUILD_DIR=%ROOT%build\V3-x86"

if /I "%~1"=="clean" (
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo Cleaned modern x86 build directory.
    exit /b 0
)

call "%ROOT%find_vs.bat"
if errorlevel 1 exit /b %errorlevel%

call "%VS_VCVARSALL%" x86
if errorlevel 1 (
    echo Failed to initialize the MSVC x86 environment.
    exit /b 1
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DSVMS_XP_COMPAT=OFF -S "%V3_SRC%" -B "%BUILD_DIR%"
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 exit /b 1

echo.
echo Modern x86 build succeeded: %BUILD_DIR%\bin\winmm.dll
exit /b 0
