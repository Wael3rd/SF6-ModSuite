@echo off
setlocal
cd /d "%~dp0"

rem Builds and runs random_value_test.exe (engine\random_value.hpp: the Color row's Random value).
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

if not exist obj mkdir obj
cl /nologo /std:c++20 /O2 /MT /EHsc /W3 /utf-8 /Fo"obj\\" /Fe"random_value_test.exe" random_value_test.cpp
if errorlevel 1 exit /b 1
.\random_value_test.exe
