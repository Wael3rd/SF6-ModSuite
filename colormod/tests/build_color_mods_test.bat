@echo off
setlocal
cd /d "%~dp0"

rem Builds and runs color_mods_test.exe (reframework\color_mods: scan, archives, cache, catalogue order).
rem Needs ..\..\build\archive_deps.lib (build.bat archive_deps) and ..\..\build\colormod\miniz.obj (build.bat colormod).
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

if not exist obj mkdir obj
cl /nologo /std:c++20 /O2 /MT /EHa /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I..\..\common /I..\..\common\third_party /Fo"obj\\" /Fe"color_mods_test.exe" color_mods_test.cpp ..\src\engine\color_mods.cpp ..\src\engine\catalog.cpp ..\src\engine\rsz.cpp ..\..\common\archive.cpp ..\..\build\colormod\miniz.obj ..\..\build\archive_deps.lib bcrypt.lib advapi32.lib shell32.lib ole32.lib user32.lib
if errorlevel 1 exit /b 1
.\color_mods_test.exe
