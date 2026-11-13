@echo off
rem report_mod\build.bat -- builds report_mod.dll ("Report a bug", a small DLL on the SF6 Native Core; MSVC, C++20, /EHa, /W4) into build\report_mod\.
rem Its shared sources are compiled from crsm\src and copied nowhere (src\report_mod.hpp says which). Never copies anything into the game folder.
setlocal enabledelayedexpansion
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
for /f "usebackq delims=" %%i in (`"!VSW!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
    echo ERROR: MSVC Build Tools 2022 not found ^(vswhere^)
    exit /b 1
)
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
where cl >nul 2>&1 || call "%VCVARS%" >nul
cd /d "%~dp0"
set "OUT=..\build\report_mod"
if not "%~1"=="" set "OUT=%~1"
if not exist "%OUT%" mkdir "%OUT%"
set "SRC="
for %%f in (src\*.cpp src\report\*.cpp) do set "SRC=!SRC! %%f"
set "C=..\crsm\src"
set "SHARED=%C%\community\auth.cpp %C%\community\client.cpp %C%\community\http.cpp %C%\community\protocol.cpp %C%\community\steam_ticket.cpp"
set "SHARED=%SHARED% %C%\game\gm.cpp %C%\mech\hooks.cpp %C%\model\chars.cpp %C%\model\labels.cpp %C%\model\locale.cpp %C%\model\luajson.cpp"
set "SHARED=%SHARED% %C%\ui\details_logic.cpp %C%\ui\dialogs.cpp %C%\ui\gx.cpp %C%\ui\lang.cpp %C%\ui\logic.cpp"
cl /nologo /MP /std:c++20 /EHa /O2 /LD /utf-8 /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /external:I ..\core\include\sf6core /external:I ..\core\third_party\reframework\include /external:I ..\colormod\third_party /external:W0 ^
   /I src /I %C% ^
   %SRC% %SHARED% ^
   /Fe:"%OUT%\report_mod.dll" /Fo:"%OUT%\\" ^
   /link /nologo user32.lib windowscodecs.lib bcrypt.lib
if errorlevel 1 exit /b 1
echo OK: %OUT%\report_mod.dll
exit /b 0
