@echo off
rem crsm\tests\build_community_tests.bat -- builds community_test.exe (the Community client's tests) into build\crsm_tests\
rem Links the DLL's Community sources (protocol, http, client, identity, page, auth, steam_ticket) and the pure model.
rem Never touches the game folder, never calls Steam (steam_ticket runs against a fake steam_api), never reaches the Worker.
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
if not exist "%OUT%\ct" mkdir "%OUT%\ct"
set "MODEL="
for %%f in (src\model\*.cpp) do set "MODEL=!MODEL! %%f"
set "COMM="
for %%f in (protocol http client identity page auth steam_ticket) do set "COMM=!COMM! src\community\%%f.cpp"
cl /nologo /std:c++20 /EHa /utf-8 /W4 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /external:I ..\colormod\third_party /external:W0 /I src ^
   /Od tests\community_test.cpp %COMM% %MODEL% /Fe:"%OUT%\community_test.exe" /Fo:"%OUT%\ct\\" /link /nologo
if errorlevel 1 exit /b 1
echo OK: build\crsm_tests\community_test.exe
