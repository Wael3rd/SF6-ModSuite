@echo off
rem Builds and runs logic_test.exe (cc_labels.hpp, cc_trials.hpp, cc_rec.hpp, cc_take.hpp) into build\custom_combos\: no game, no data folder.
rem Smart App Control can refuse a fresh test build: run it again.
setlocal
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
if not exist ..\build\custom_combos\test mkdir ..\build\custom_combos\test
cl /nologo /utf-8 /std:c++20 /EHa /O2 /MD /W4 /I ..\colormod\third_party /Fo..\build\custom_combos\test\ /Fe..\build\custom_combos\test\logic_test.exe logic_test.cpp
if errorlevel 1 exit /b 1
..\build\custom_combos\test\logic_test.exe
exit /b %errorlevel%
