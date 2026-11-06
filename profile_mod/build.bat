@echo off
rem profile_mod\build.bat -- builds profile_mod.dll (SF6 Profile Mod; MSVC, C++20, /EHa, /W4) into build\profile_mod\
rem Uses the core's header (core\include\sf6core) and the REFramework plugin headers (plugin\include).
rem Never copies anything into the game folder.
setlocal
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
if not exist ..\build\profile_mod mkdir ..\build\profile_mod
cl /nologo /std:c++20 /EHa /O2 /W4 /LD /I ..\plugin\include /I ..\core\include /Fo..\build\profile_mod\ profile_mod.cpp /Fe..\build\profile_mod\profile_mod.dll /link /opt:ref winhttp.lib user32.lib
if errorlevel 1 exit /b 1
echo OK: build\profile_mod\profile_mod.dll
exit /b 0
