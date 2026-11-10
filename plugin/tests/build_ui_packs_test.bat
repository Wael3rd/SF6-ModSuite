@echo off
setlocal
cd /d "%~dp0"

rem Builds and runs ui_packs_test.exe (unit test of plugin\src\ui_packs_logic.hpp and json.cpp's json_with_key) next to this file.
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

if not exist obj mkdir obj
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /WX /Fo"obj\\" /Fe"ui_packs_test.exe" ui_packs_test.cpp ..\src\json.cpp
if errorlevel 1 exit /b 1
.\ui_packs_test.exe
