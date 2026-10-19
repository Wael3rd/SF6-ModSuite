@echo off
setlocal
cd /d "%~dp0"

rem Builds and runs rto_test.exe (unit test of plugin\src\rto_data.cpp, the runtime costume options' data side)
rem next to this file. An optional argument names a costume_options.json written by the loader: it is parsed
rem and its rows are listed too.
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

if not exist obj mkdir obj
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /WX /Fo"obj\\" /Fe"rto_test.exe" rto_test.cpp ..\src\rto_data.cpp ..\src\json.cpp
if errorlevel 1 exit /b 1
.\rto_test.exe %1
