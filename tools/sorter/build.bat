@echo off
rem tools\sorter\build.bat -- builds build\SF6_ModSorter.exe (MSVC, C++17, static CRT, Windows subsystem; Direct2D + DirectWrite).
rem The suite's updater (updater\src, C++20) is compiled in with the flags of its own tests: the same code as in the game.
rem Its objects go in build\obj\sorter only: the loaders' sources it shares (common\, costumes\loader\, stages\loader\) are
rem compiled again there with the root build's flags, so building it never rebuilds or overwrites another binary of build\.
rem probe_costume.cpp and probe_stage.cpp include loader_core.cpp and stage_loader.cpp whole (see their header comments).
rem Needs build\archive_deps.lib (the archive decoders, built by "build.bat archive_deps" when missing).
setlocal
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
where cl >nul 2>&1 || call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0..\.."
set B=build
set O=build\obj\sorter
if not exist %O% mkdir %O%
if not exist %B%\archive_deps.lib (
    echo ERROR: build\archive_deps.lib is missing: run "build.bat archive_deps" first
    exit /b 1
)
set C=common
set CO=costumes\loader
set ST=stages\loader
set S=tools\sorter
rem The root build's flags (build.bat CF), so the loaders' code compiled here behaves as in the loader
set CF=/nologo /MP /std:c++17 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS /I"%C%" /I"%C%\third_party" /I"%CO%" /I"%ST%" /I"proxy"

echo === SF6_ModSorter.exe: the loaders' sources ===
cl %CF% /c /Fo%O%\ %C%\third_party\zstddeclib.c %C%\third_party\miniz.c %C%\pak.cpp %C%\archive.cpp %C%\fighter_names.cpp %CO%\patch.cpp %CO%\swap_repairs.cpp %CO%\structural_scene.cpp %CO%\structural_msg.cpp %CO%\vanilla_inventory.cpp %CO%\colour_follow.cpp %ST%\stage_log.cpp %ST%\stage_classify.cpp %ST%\preview_tex.cpp
if errorlevel 1 exit /b 1
echo === SF6_ModSorter.exe: the sorter ===
cl %CF% /c /Fo%O%\ %S%\probe_costume.cpp %S%\probe_stage.cpp %S%\classify.cpp %S%\place.cpp %S%\gamedir.cpp %S%\pictures.cpp %S%\ui.cpp %S%\main.cpp
if errorlevel 1 exit /b 1
echo === SF6_ModSorter.exe: the suite's updater ===
set U=updater\src
cl /nologo /MP /std:c++20 /O2 /MT /EHa /utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /external:I colormod\third_party /external:W0 /c /Fo%O%\ %U%\update_logic.cpp %U%\update_fs.cpp %U%\update_install.cpp %U%\update_net.cpp %U%\update_sys.cpp %U%\update_text.cpp %U%\update_pe.cpp %S%\suite_update.cpp
if errorlevel 1 exit /b 1
link /nologo /INCREMENTAL:NO /MACHINE:X64 /SUBSYSTEM:WINDOWS /OUT:%B%\SF6_ModSorter.exe ^
  %O%\main.obj %O%\ui.obj %O%\classify.obj %O%\place.obj %O%\gamedir.obj %O%\pictures.obj %O%\probe_costume.obj %O%\probe_stage.obj ^
  %O%\zstddeclib.obj %O%\miniz.obj %O%\pak.obj %O%\archive.obj %O%\fighter_names.obj %O%\patch.obj %O%\swap_repairs.obj ^
  %O%\structural_scene.obj %O%\structural_msg.obj %O%\vanilla_inventory.obj %O%\colour_follow.obj ^
  %O%\stage_log.obj %O%\stage_classify.obj %O%\preview_tex.obj ^
  %O%\suite_update.obj %O%\update_logic.obj %O%\update_fs.obj %O%\update_install.obj %O%\update_net.obj %O%\update_sys.obj ^
  %O%\update_text.obj %O%\update_pe.obj ^
  %B%\archive_deps.lib winhttp.lib bcrypt.lib advapi32.lib shell32.lib ole32.lib user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib dwmapi.lib
if errorlevel 1 exit /b 1
echo OK: build\SF6_ModSorter.exe
exit /b 0
