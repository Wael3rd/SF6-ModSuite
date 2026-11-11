@echo off
rem updater\tests\build_update_tests.bat -- builds the updater's tests into build\update_tests\ (not shipped)
rem   update_test.exe   the guards: versions, the manifest as hostile input, the plan, zip slip, the all-or-nothing swap, the journal,
rem                     the clean-up, the "ignored" memory, the restart's waiter (run on a short process). --work <folder>: its files there
rem   update_e2e.exe    a whole update on a fake game folder (a release's zips in, the next release's out), and one real HTTPS fetch
rem No game, never the game folder.
setlocal enabledelayedexpansion
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
for /f "usebackq delims=" %%i in (`"!VSW!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
    echo ERROR: MSVC Build Tools 2022 not found
    exit /b 1
)
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
where cl >nul 2>&1 || call "%VCVARS%" >nul
cd /d "%~dp0\.."
set "OUT=..\build\update_tests"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /c /O2 /W0 /MT /TC ..\common\third_party\miniz.c /Fo:"%OUT%\obj\miniz.obj"
if errorlevel 1 exit /b 1
set "COMMON=/nologo /std:c++20 /EHa /utf-8 /W4 /MT /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /external:I ..\colormod\third_party /external:W0"
set "SRC=src\update_logic.cpp src\update_fs.cpp src\update_install.cpp src\update_net.cpp src\update_sys.cpp src\update_text.cpp src\update_pe.cpp"
cl %COMMON% /Od tests\update_test.cpp %SRC% "%OUT%\obj\miniz.obj" /Fe:"%OUT%\update_test.exe" /Fo:"%OUT%\obj\\" /link /nologo
if errorlevel 1 exit /b 1
cl %COMMON% /O2 /external:I ..\plugin\include tests\update_e2e.cpp src\update_game.cpp %SRC% "%OUT%\obj\miniz.obj" /Fe:"%OUT%\update_e2e.exe" /Fo:"%OUT%\obj\\" /link /nologo
if errorlevel 1 exit /b 1
echo OK: build\update_tests\update_test.exe, update_e2e.exe
exit /b 0
