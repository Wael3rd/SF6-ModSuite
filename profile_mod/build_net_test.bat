@echo off
rem Builds and runs net_test.exe: one HTTPS request to steamcommunity.com (needs the network), checks steamname_net.hpp.
setlocal
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
if not exist obj mkdir obj
cl /nologo /std:c++20 /EHa /O2 /MD /W4 /Fo"obj\\" /Fe"net_test.exe" net_test.cpp /link winhttp.lib
if errorlevel 1 exit /b 1
.\net_test.exe
exit /b %errorlevel%
