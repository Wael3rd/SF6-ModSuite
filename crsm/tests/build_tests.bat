@echo off
rem crsm\tests\build_tests.bat -- builds the offline benches of the Recording Manager into build\crsm_tests\
rem   p3_bench.exe     the layers files / replay / reversal / record on fakes (src\model\*.cpp, the DLL's pure model)
rem   ui_bench.exe     the pure logic of the DLL's screens (src\ui\logic.cpp)
rem   p5_bench.exe     the Manager page's game-free logic (src\ui\manager_logic.cpp)
rem   integ_bench.exe  the screens + Manager page integration, the owner rule, the standalone claim, settings.json
rem   codecache_bench.exe  the code scan's cache (src\mech\codecheck_cache.hpp)
rem Headers as crsm\build.bat: nlohmann/json from colormod\third_party. No game, no user data; never touches the game folder.
setlocal enabledelayedexpansion
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
for /f "usebackq delims=" %%i in (`"!VSW!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
    echo ERROR: MSVC Build Tools 2022 not found
    exit /b 1
)
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VCVARS%" >nul
cd /d "%~dp0\.."
set "OUT=..\build\crsm_tests"
if not exist "%OUT%" mkdir "%OUT%"
set "MODEL="
for %%f in (src\model\*.cpp) do set "MODEL=!MODEL! %%f"
set "COMMON=/nologo /std:c++20 /EHa /utf-8 /W4 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /external:I ..\colormod\third_party /external:W0 /I src"
rem p3_bench.exe: P3a's name cut on the files engine, the pure tests of replay / reversal / record, slot 8 shared by record and replay
if not exist "%OUT%\p3" mkdir "%OUT%\p3"
cl %COMMON% /Od /I tests tests\p3_bench.cpp tests\p3_tests.cpp %MODEL% /Fe:"%OUT%\p3_bench.exe" /Fo:"%OUT%\p3\\" /link /nologo
if errorlevel 1 exit /b 1
rem ui_bench.exe: the P4 bench (the pure logic of the DLL's screens: windows, cursor, fit, text dialog, Community dialog)
if not exist "%OUT%\ui" mkdir "%OUT%\ui"
cl %COMMON% /Od tests\ui_bench.cpp src\ui\logic.cpp %MODEL% /Fe:"%OUT%\ui_bench.exe" /Fo:"%OUT%\ui\\" /link /nologo
if errorlevel 1 exit /b 1
rem p5_bench.exe: the P5 tests (the Manager page's game-free logic, ui\manager_logic.cpp, on fakes and on the files engine)
if not exist "%OUT%\p5" mkdir "%OUT%\p5"
cl %COMMON% /Od tests\p5_bench.cpp src\ui\manager_logic.cpp %MODEL% /Fe:"%OUT%\p5_bench.exe" /Fo:"%OUT%\p5\\" /link /nologo
if errorlevel 1 exit /b 1
rem integ_bench.exe: the P4 + P5 integration (owner rule, the one key reader, tickers, refresh after a download)
if not exist "%OUT%\integ" mkdir "%OUT%\integ"
cl %COMMON% /Od tests\integ_bench.cpp src\ui\logic.cpp src\ui\manager_logic.cpp %MODEL% /Fe:"%OUT%\integ_bench.exe" /Fo:"%OUT%\integ\\" /link /nologo
if errorlevel 1 exit /b 1
rem codecache_bench.exe: the code scan's cache (mech\codecheck_cache.hpp, header only: no game, no data)
if not exist "%OUT%\cc" mkdir "%OUT%\cc"
cl %COMMON% /Od tests\codecache_bench.cpp /Fe:"%OUT%\codecache_bench.exe" /Fo:"%OUT%\cc\\" /link /nologo
if errorlevel 1 exit /b 1
echo OK: build\crsm_tests\p3_bench.exe ui_bench.exe p5_bench.exe integ_bench.exe codecache_bench.exe
