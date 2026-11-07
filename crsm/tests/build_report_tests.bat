@echo off
rem crsm\tests\build_report_tests.bat -- builds the tests of "Report a bug" into build\crsm_tests\
rem   report_test.exe          the pure logic (cleaning, caps, costume key, menu, session), the gathering on a fake game folder
rem   report_capture_test.exe  the picture on a D3D12 swap chain of its own (WARP by default: it never touches the GPU of a game running beside it)
rem   report_e2e_test.exe      a whole report through the real client code, to a server on this machine named on its command line (it refuses any other)
rem No game, no user data; never touches the game folder.
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
if not exist "%OUT%\rp" mkdir "%OUT%\rp"
if not exist "%OUT%\rc" mkdir "%OUT%\rc"
set "COMMON=/nologo /std:c++20 /EHa /utf-8 /W4 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /external:I ..\colormod\third_party /external:W0 /I src"
cl %COMMON% /Od tests\report_test.cpp src\report\report_logic.cpp src\report\gather.cpp /Fe:"%OUT%\report_test.exe" /Fo:"%OUT%\rp\\" /link /nologo
if errorlevel 1 exit /b 1
cl %COMMON% /O2 tests\report_capture_test.cpp src\report\capture.cpp src\report\report_logic.cpp /Fe:"%OUT%\report_capture_test.exe" /Fo:"%OUT%\rc\\" /link /nologo windowscodecs.lib
if errorlevel 1 exit /b 1
if not exist "%OUT%\re" mkdir "%OUT%\re"
set "MODEL="
for %%f in (src\model\*.cpp) do set "MODEL=!MODEL! %%f"
cl %COMMON% /Od tests\report_e2e_test.cpp src\report\capture.cpp src\report\report_logic.cpp src\report\gather.cpp src\community\protocol.cpp src\community\http.cpp src\community\client.cpp src\community\auth.cpp %MODEL% /Fe:"%OUT%\report_e2e_test.exe" /Fo:"%OUT%\re\\" /link /nologo
if errorlevel 1 exit /b 1
echo OK: build\crsm_tests\report_test.exe, report_capture_test.exe, report_e2e_test.exe
