@echo off
rem crsm\build.bat -- builds SF6_CRSMNative.dll (the SF6 Recording Manager; MSVC, C++20, /EHa, /W4) into build\crsm\
rem Uses the core's header (core\include\sf6core), the REFramework plugin headers (core\third_party\reframework\include)
rem and nlohmann/json (colormod\third_party), as colormod\build.bat does: no copy of them here.
rem Never copies anything into the game folder.
setlocal enabledelayedexpansion
set "VCVARS="
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSW!" (
    echo ERROR: vswhere not found ^(Visual Studio Installer^)
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"!VSW!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
    echo ERROR: MSVC Build Tools 2022 not found ^(vswhere^)
    exit /b 1
)
rem vcvars looks for vswhere.exe on the PATH
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
where cl >nul 2>&1 || call "%VCVARS%" >nul
cd /d "%~dp0"
if not exist ..\build\crsm mkdir ..\build\crsm
set "MODEL="
for %%f in (src\model\*.cpp src\game\*.cpp src\mech\*.cpp src\ui\*.cpp) do set "MODEL=!MODEL! %%f"
for %%f in (src\community\*.cpp) do set "MODEL=!MODEL! %%f"
rem /external:I: the third-party headers are compiled at /W0 so that the 0-warning goal measures our own code
cl /nologo /MP /std:c++20 /EHa /O2 /LD /utf-8 /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /external:I ..\core\include\sf6core /external:I ..\core\third_party\reframework\include /external:I ..\colormod\third_party /external:W0 ^
   /I src ^
   src\main.cpp src\common.cpp src\perf.cpp src\probes.cpp %MODEL% ^
   /Fe:..\build\crsm\SF6_CRSMNative.dll /Fo:..\build\crsm\ ^
   /link /nologo user32.lib
if errorlevel 1 exit /b 1
echo OK: build\crsm\SF6_CRSMNative.dll
