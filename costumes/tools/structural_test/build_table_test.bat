@echo off
setlocal
cd /d "%~dp0"
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist out mkdir out
cl /nologo /std:c++17 /O2 /EHa /W3 /D_CRT_SECURE_NO_WARNINGS /I..\..\loader /I..\..\..\common /I..\..\..\common\third_party /Foout\ table_test.cpp ..\..\loader\patch.cpp /Feout\table_test.exe
