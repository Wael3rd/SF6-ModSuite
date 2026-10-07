@echo off
rem colormod\build.bat -- builds SF6_ColorModNative.dll (MSVC, C++20, /EHa) into build\colormod\
rem Uses the core's header (core\include\sf6core) and the REFramework plugin headers (core\third_party\reframework\include).
rem Never copies anything into the game folder.
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul
cd /d "%~dp0"
if not exist ..\build\colormod mkdir ..\build\colormod
cl /nologo /std:c++20 /EHa /O2 /LD /utf-8 /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I third_party /I src /I ..\core\include\sf6core /I ..\core\third_party\reframework\include src\main.cpp src\common.cpp src\perf.cpp src\engine\rsz.cpp src\engine\luajson.cpp src\engine\catalog.cpp src\engine\chips.cpp src\engine\live.cpp src\engine\decide.cpp src\vfx\engine.cpp src\vfx\mech.cpp src\vfx\body.cpp src\vfx\data.cpp src\vfx\scene.cpp src\vfx\memguard.cpp src\vfx\providers.cpp src\vfx\drive.cpp src\vfx\tick.cpp src\vfx\settings.cpp src\vfx\drive_cfg.cpp src\vfx\feed.cpp src\vfx\feed_frame.cpp src\screens\rows.cpp src\screens\dipanel.cpp src\screens\dipanel_mode.cpp src\screens\drive_bridge.cpp src\screens\colour_state.cpp src\screens\colour_track.cpp src\screens\tracker.cpp src\screens\guide.cpp src\dialogs\locale.cpp src\dialogs\camera.cpp src\dialogs\options_driver.cpp src\dialogs\editor_core.cpp src\dialogs\editor.cpp /Fe:..\build\colormod\SF6_ColorModNative.dll /Fo:..\build\colormod\
if errorlevel 1 exit /b 1
echo OK: build\colormod\SF6_ColorModNative.dll
