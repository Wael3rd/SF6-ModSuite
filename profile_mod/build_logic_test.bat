@echo off
rem Builds and runs logic_test.exe (steamname_logic.hpp, swap_logic.hpp: no game, no network).
rem Smart App Control can refuse a fresh test build: run it again.
setlocal
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
if not exist obj mkdir obj
cl /nologo /std:c++20 /EHa /O2 /MD /W4 /WX /Fo"obj\\" /Fe"logic_test.exe" logic_test.cpp
if errorlevel 1 exit /b 1
.\logic_test.exe
exit /b %errorlevel%
