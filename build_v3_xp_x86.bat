@echo off
setlocal

set "ROOT=%~dp0"
set "V3_SRC=%ROOT%src\V3"
set "BUILD_DIR=%ROOT%build\V3-XP-x86"
set "TOOLCHAIN_BIN=%ROOT%w64devkit-x86\bin"

if /I "%~1"=="clean" (
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo Cleaned XP x86 build directory.
    exit /b 0
)

if not exist "%TOOLCHAIN_BIN%\g++.exe" (
    echo XP compiler not found: %TOOLCHAIN_BIN%\g++.exe
    exit /b 1
)

set "PATH=%TOOLCHAIN_BIN%;%PATH%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="%TOOLCHAIN_BIN%\g++.exe" -DSVMS_XP_COMPAT=ON -DSVMS_BUILD_TESTS=ON -S "%V3_SRC%" -B "%BUILD_DIR%"
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 exit /b 1

echo.
echo Windows XP x86 build succeeded: %BUILD_DIR%\bin\winmm.dll
exit /b 0
