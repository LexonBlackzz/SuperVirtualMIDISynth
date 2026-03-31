@echo off
setlocal

set "ARCH=%~1"
if /I "%ARCH%"=="" set "ARCH=x64"

if /I not "%ARCH%"=="x86" if /I not "%ARCH%"=="x64" (
    echo Usage: test_virtuallysuper.bat [x86^|x64]
    exit /b 1
)

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "OUTDIR=%ROOT%\build\VirtuallySuper\%ARCH%"
set "OBJDIR=%OUTDIR%\obj"

call "%ROOT%\build_virtuallysuper.bat" %ARCH%
if errorlevel 1 exit /b %errorlevel%

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)

if /I "%ARCH%"=="x86" (
    set "VS_ENV=%VS_PATH%\VC\Auxiliary\Build\vcvars32.bat"
) else (
    set "VS_ENV=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
)

call "%VS_ENV%"
if errorlevel 1 exit /b %errorlevel%

set "CXXFLAGS=/nologo /std:c++17 /EHsc /W4 /WX- /permissive- /I\"%ROOT%\src\" /I\"%ROOT%\src\VirtuallySuper\""

cl %CXXFLAGS% /c "%ROOT%\src\VirtuallySuper\VirtuallySuperPrototypeTests.cpp" /Fo"%OBJDIR%\VirtuallySuperPrototypeTests.obj"
if errorlevel 1 exit /b %errorlevel%

link /nologo /OUT:"%OUTDIR%\VirtuallySuperPrototypeTests.exe" "%OBJDIR%\VirtuallySuperPrototypeTests.obj" "%OUTDIR%\VirtuallySuperPrototype.lib"
if errorlevel 1 exit /b %errorlevel%

"%OUTDIR%\VirtuallySuperPrototypeTests.exe"
exit /b %errorlevel%
