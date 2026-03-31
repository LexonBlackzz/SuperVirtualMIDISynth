@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere not found at "%VSWHERE%"
    exit /b 1
)

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)

if not defined VS_PATH (
    echo Visual Studio with C++ tools was not found.
    exit /b 1
)

set "VS_VCVARSALL=%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VS_VCVARSALL%" (
    echo vcvarsall.bat not found in "%VS_PATH%"
    exit /b 1
)

endlocal & set "VS_PATH=%VS_PATH%" & set "VS_VCVARSALL=%VS_VCVARSALL%"
exit /b 0
