@echo off
rem custom_combos\build.bat -- builds SF6_CustomCombos.dll (SF6 Custom Combos; MSVC, C++20, /EHa, /W4) into build\custom_combos\
rem Uses the core's header (core\include\sf6core), the REFramework plugin headers (plugin\include) and nlohmann/json
rem (colormod\third_party). Never copies anything into the game folder.
setlocal
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
if not exist ..\build\custom_combos mkdir ..\build\custom_combos
cl /nologo /utf-8 /std:c++20 /EHa /O2 /W4 /LD /I ..\plugin\include /I ..\core\include /I ..\colormod\third_party /Fo..\build\custom_combos\ custom_combos.cpp /Fe..\build\custom_combos\SF6_CustomCombos.dll /link /opt:ref user32.lib
if errorlevel 1 exit /b 1
echo OK: build\custom_combos\SF6_CustomCombos.dll
exit /b 0
