@echo off
setlocal
cd /d "%~dp0"
rem Builds scene_test.exe next to this file. Run: scene_test.exe <oracle folder with in\ and expected\>
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist obj mkdir obj
cl /nologo /std:c++17 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS /I"..\..\loader" /Fo:obj\ /Fe:scene_test.exe scene_test.cpp ..\..\loader\structural_scene.cpp
exit /b %errorlevel%
