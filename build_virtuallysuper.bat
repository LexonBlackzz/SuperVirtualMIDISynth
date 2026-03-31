@echo off
setlocal

set "ARCH=%~1"
if /I "%ARCH%"=="" set "ARCH=x64"

if /I not "%ARCH%"=="x86" if /I not "%ARCH%"=="x64" (
    echo Usage: build_virtuallysuper.bat [x86^|x64]
    exit /b 1
)

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "OUTDIR=%ROOT%\build\VirtuallySuper\%ARCH%"
set "OBJDIR=%OUTDIR%\obj"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere not found at "%VSWHERE%"
    exit /b 1
)

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)

if not defined VS_PATH (
    echo Visual Studio with C++ tools was not found.
    exit /b 1
)

if /I "%ARCH%"=="x86" (
    set "VS_ENV=%VS_PATH%\VC\Auxiliary\Build\vcvars32.bat"
) else (
    set "VS_ENV=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
)

if not exist "%VS_ENV%" (
    echo MSVC environment script not found at "%VS_ENV%"
    exit /b 1
)

call "%VS_ENV%"
if errorlevel 1 exit /b %errorlevel%

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

set "CXXFLAGS=/nologo /std:c++17 /EHsc /W4 /WX- /permissive- /I\"%ROOT%\src\" /I\"%ROOT%\src\VirtuallySuper\""

cl %CXXFLAGS% /c "%ROOT%\src\VirtuallySuper\VirtuallySuperEngine.cpp" /Fo"%OBJDIR%\VirtuallySuperEngine.obj"
if errorlevel 1 exit /b %errorlevel%

cl %CXXFLAGS% /c "%ROOT%\src\VirtuallySuper\VirtuallySuperSamplerEngine.cpp" /Fo"%OBJDIR%\VirtuallySuperSamplerEngine.obj"
if errorlevel 1 exit /b %errorlevel%

cl %CXXFLAGS% /c "%ROOT%\src\VirtuallySuper\VirtuallySuperExact.cpp" /Fo"%OBJDIR%\VirtuallySuperExact.obj"
if errorlevel 1 exit /b %errorlevel%

cl %CXXFLAGS% /c "%ROOT%\src\VirtuallySuper\VirtuallySuperGrouped.cpp" /Fo"%OBJDIR%\VirtuallySuperGrouped.obj"
if errorlevel 1 exit /b %errorlevel%

cl %CXXFLAGS% /c "%ROOT%\src\VirtuallySuper\VirtuallySuperDensity.cpp" /Fo"%OBJDIR%\VirtuallySuperDensity.obj"
if errorlevel 1 exit /b %errorlevel%

cl %CXXFLAGS% /c "%ROOT%\src\VirtuallySuper\VirtuallySuperScheduler.cpp" /Fo"%OBJDIR%\VirtuallySuperScheduler.obj"
if errorlevel 1 exit /b %errorlevel%

lib /nologo /OUT:"%OUTDIR%\VirtuallySuperPrototype.lib" "%OBJDIR%\VirtuallySuperEngine.obj" "%OBJDIR%\VirtuallySuperSamplerEngine.obj" "%OBJDIR%\VirtuallySuperExact.obj" "%OBJDIR%\VirtuallySuperGrouped.obj" "%OBJDIR%\VirtuallySuperDensity.obj" "%OBJDIR%\VirtuallySuperScheduler.obj"
if errorlevel 1 exit /b %errorlevel%

echo Built "%OUTDIR%\VirtuallySuperPrototype.lib"
exit /b 0
